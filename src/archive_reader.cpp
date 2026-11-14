#include "archive_file.h"

#include <algorithm>
#include <cstdio>
#include <limits>
#include <type_traits>

namespace huxerui::archive {
namespace {

using namespace detail;
using ReadHandle = std::unique_ptr<struct archive, decltype(&archive_read_free)>;

struct ReaderInput {
  ReaderInput(RandomAccessStream source, la_int64_t length) : stream(std::move(source)), size(length) {}

  RandomAccessStream stream;
  la_int64_t size;
  la_int64_t position = 0;
  Bytes buffer = Bytes(ChunkSize);
  std::optional<IoError> error;
};

la_ssize_t ReadInput(struct archive*, void* data, const void** buffer) {
  auto& input = *static_cast<ReaderInput*>(data);
  const auto available = input.size - input.position;
  if (available == 0)
    return 0;
  const auto size = static_cast<std::size_t>(std::min<la_int64_t>(available, input.buffer.size()));
  auto result = input.stream.ReadAt(input.position, std::span(input.buffer).first(size));
  if (!result.Succeeded()) {
    input.error = std::move(result).Error();
    return -1;
  }
  *buffer = input.buffer.data();
  input.position += static_cast<la_int64_t>(result.Value());
  return static_cast<la_ssize_t>(result.Value());
}

la_int64_t SkipInput(struct archive*, void* data, la_int64_t count) {
  if (count <= 0)
    return 0;
  auto& input = *static_cast<ReaderInput*>(data);
  const auto skipped = std::min(count, input.size - input.position);
  input.position += skipped;
  return skipped;
}

la_int64_t SeekInput(struct archive*, void* data, la_int64_t offset, int origin) {
  auto& input = *static_cast<ReaderInput*>(data);
  la_int64_t base = 0;
  switch (origin) {
  case SEEK_SET:
    break;
  case SEEK_CUR:
    base = input.position;
    break;
  case SEEK_END:
    base = input.size;
    break;
  default:
    return ARCHIVE_FAILED;
  }
  if (offset < -base || offset > input.size - base)
    return ARCHIVE_FAILED;
  input.position = base + offset;
  return input.position;
}

bool SupportedArchive(struct archive* handle) {
  const int format = archive_format(handle) & ARCHIVE_FORMAT_BASE_MASK;
  const int filter = archive_filter_code(handle, 0);
  if (archive_filter_count(handle) != (filter == ARCHIVE_FILTER_NONE ? 1 : 2))
    return false;
  if (format == ARCHIVE_FORMAT_ZIP)
    return filter == ARCHIVE_FILTER_NONE;
  return format == ARCHIVE_FORMAT_TAR &&
         (filter == ARCHIVE_FILTER_NONE || filter == ARCHIVE_FILTER_GZIP || filter == ARCHIVE_FILTER_ZSTD);
}

struct ReaderSession {
  ReaderInput& input;
  ReadHandle handle{archive_read_new(), archive_read_free};

  ArchiveError Failure(ArchiveErrorCode code = ArchiveErrorCode::InvalidData) const {
    if (input.error)
      return IoFailure(*input.error);
    return ArchiveFailure(handle.get(), code, "Could not read the archive");
  }

  ArchiveResult<void> Open() {
    input.position = 0;
    input.error.reset();
    if (!handle)
      return ArchiveResult<void>::Failure(Error(ArchiveErrorCode::Io, "Could not allocate an archive reader"));
    if (archive_read_support_format_zip_seekable(handle.get()) != ARCHIVE_OK ||
        archive_read_support_format_tar(handle.get()) != ARCHIVE_OK ||
        archive_read_support_filter_gzip(handle.get()) != ARCHIVE_OK ||
        archive_read_support_filter_zstd(handle.get()) != ARCHIVE_OK ||
        archive_read_set_seek_callback(handle.get(), SeekInput) != ARCHIVE_OK) {
      return ArchiveResult<void>::Failure(Failure(ArchiveErrorCode::UnsupportedFormat));
    }
    if (archive_read_open2(handle.get(), &input, nullptr, ReadInput, SkipInput, nullptr) != ARCHIVE_OK) {
      return ArchiveResult<void>::Failure(Failure(
          archive_format(handle.get()) == 0 ? ArchiveErrorCode::UnsupportedFormat : ArchiveErrorCode::InvalidData));
    }
    return ArchiveResult<void>::Success();
  }

  ArchiveResult<archive_entry*> Next() {
    archive_entry* entry = nullptr;
    const int status = archive_read_next_header(handle.get(), &entry);
    if (status != ARCHIVE_OK && status != ARCHIVE_EOF)
      return ArchiveResult<archive_entry*>::Failure(Failure());
    if (!SupportedArchive(handle.get())) {
      return ArchiveResult<archive_entry*>::Failure(
          Error(ArchiveErrorCode::UnsupportedFormat, "The archive format or filter is unsupported"));
    }
    if (status == ARCHIVE_OK && archive_entry_is_encrypted(entry)) {
      return ArchiveResult<archive_entry*>::Failure(
          Error(ArchiveErrorCode::UnsupportedFormat, "Encrypted entries are unsupported"));
    }
    return ArchiveResult<archive_entry*>::Success(status == ARCHIVE_EOF ? nullptr : entry);
  }
};

// The returned view borrows the current entry and must stay within its visit.
ArchiveResult<std::string_view> EntryPath(archive_entry* entry) {
  const char* path = archive_entry_pathname_utf8(entry);
  if (!path || !*path)
    return ArchiveResult<std::string_view>::Failure(Error(ArchiveErrorCode::InvalidData, "Entry has no UTF-8 path"));
  return ArchiveResult<std::string_view>::Success(path);
}

template <class Sink> ArchiveResult<void> CopyEntryData(ReaderSession& session, std::uint64_t& remaining, Sink&& sink) {
  Bytes buffer(ChunkSize);
  std::uint64_t offset = 0;
  while (true) {
    const auto count = archive_read_data(session.handle.get(), buffer.data(), buffer.size());
    if (count < 0)
      return ArchiveResult<void>::Failure(session.Failure());
    if (count == 0)
      return ArchiveResult<void>::Success();
    const auto size = static_cast<std::size_t>(count);
    if (size > remaining)
      return ArchiveResult<void>::Failure(Error(ArchiveErrorCode::LimitExceeded, "Archive output limit exceeded"));
    auto written = sink(offset, std::span<const std::byte>(buffer).first(size));
    if (!written.Succeeded())
      return written;
    offset += size;
    remaining -= size;
  }
}

ArchiveResult<void> WriteExtractedFile(ReaderSession& session, const File& destination, std::uint64_t& remaining) {
  const auto path = FilePath(destination);
  auto status = PathStatus(path);
  if (!status.Succeeded())
    return ArchiveResult<void>::Failure(std::move(status).Error());
  if (std::filesystem::exists(status.Value())) {
    return ArchiveResult<void>::Failure(FileSystemError(path, std::make_error_code(std::errc::file_exists)));
  }
  auto opened = destination.OpenRandomAccess(FileAccessMode::WriteOnly, FileOpenMode::CreateNew);
  if (!opened.Succeeded())
    return ArchiveResult<void>::Failure(IoFailure(std::move(opened).Error()));

  auto result = [&]() -> ArchiveResult<void> {
    auto output = std::move(opened).Value();
    auto copied = CopyEntryData(session, remaining, [&](std::uint64_t offset, std::span<const std::byte> bytes) {
      auto written = output.WriteAt(offset, bytes);
      if (!written.Succeeded())
        return ArchiveResult<void>::Failure(IoFailure(std::move(written).Error()));
      return ArchiveResult<void>::Success();
    });
    if (!copied.Succeeded())
      return copied;
    auto closed = output.Close();
    if (!closed.Succeeded())
      return ArchiveResult<void>::Failure(IoFailure(std::move(closed).Error()));
    return ArchiveResult<void>::Success();
  }();
  if (!result.Succeeded())
    (void)destination.Delete();
  return result;
}

} // namespace

struct ArchiveReader::Impl {
  Impl(RandomAccessStream stream, la_int64_t size, ArchiveReadOptions read_options)
      : input(std::move(stream), size), options(read_options) {}

  ReaderInput input;
  ArchiveReadOptions options;

  template <class Operation> ArchiveResult<void> Visit(Operation&& operation) {
    const Utf8Locale locale;
    ReaderSession session{input};
    auto opened = session.Open();
    if (!opened.Succeeded())
      return opened;
    for (std::uint64_t count = 0;; ++count) {
      auto next = session.Next();
      if (!next.Succeeded())
        return ArchiveResult<void>::Failure(std::move(next).Error());
      auto* entry = next.Value();
      if (!entry)
        return ArchiveResult<void>::Success();
      if (count == options.max_entries) {
        return ArchiveResult<void>::Failure(
            Error(ArchiveErrorCode::LimitExceeded, "Archive entry count limit exceeded"));
      }
      auto path = EntryPath(entry);
      if (!path.Succeeded())
        return ArchiveResult<void>::Failure(std::move(path).Error());
      if constexpr (std::is_void_v<decltype(operation(session, entry, path.Value()))>) {
        operation(session, entry, path.Value());
      } else {
        auto result = operation(session, entry, path.Value());
        if (!result.Succeeded())
          return ArchiveResult<void>::Failure(std::move(result).Error());
        if (result.Value())
          return ArchiveResult<void>::Success();
      }
    }
  }

  template <class Operation> ArchiveResult<void> WithEntry(std::string_view entry_path, Operation&& operation) {
    if (entry_path.empty() || entry_path.find('\0') != std::string_view::npos) {
      return ArchiveResult<void>::Failure(Error(ArchiveErrorCode::InvalidArgument, "Invalid archive entry path"));
    }
    std::optional<EntryKind> match;
    auto scan = Visit([&](ReaderSession&, archive_entry* entry, std::string_view path) -> ArchiveResult<bool> {
      if (path == entry_path) {
        if (match)
          return ArchiveResult<bool>::Failure(
              Error(ArchiveErrorCode::DuplicateEntryPath, "Archive contains multiple entries at this path"));
        match = GetEntryKind(entry);
      }
      return ArchiveResult<bool>::Success(false);
    });
    if (!scan.Succeeded())
      return scan;
    if (!match)
      return ArchiveResult<void>::Failure(Error(ArchiveErrorCode::EntryNotFound, "Archive entry was not found"));
    const auto safe = SafeEntryPath(entry_path);
    if (*match != EntryKind::File || !safe || safe->empty()) {
      return ArchiveResult<void>::Failure(Error(ArchiveErrorCode::UnsafeEntry, "Entry is not a safe ordinary file"));
    }

    auto remaining = options.max_output_bytes;
    bool found = false;
    auto result =
        Visit([&](ReaderSession& session, archive_entry* entry, std::string_view path) -> ArchiveResult<bool> {
          if (path != entry_path)
            return ArchiveResult<bool>::Success(false);
          if (GetEntryKind(entry) != EntryKind::File)
            return ArchiveResult<bool>::Failure(Error(ArchiveErrorCode::UnsafeEntry, "Entry kind changed"));
          auto processed = operation(session, remaining);
          if (!processed.Succeeded())
            return ArchiveResult<bool>::Failure(std::move(processed).Error());
          found = true;
          return ArchiveResult<bool>::Success(true);
        });
    if (result.Succeeded() && !found)
      return ArchiveResult<void>::Failure(Error(ArchiveErrorCode::EntryNotFound, "Archive entry was not found"));
    return result;
  }
};

ArchiveResult<ArchiveReader> ArchiveReader::Open(const File& source, ArchiveReadOptions options) {
  auto opened = source.OpenRandomAccess();
  if (!opened.Succeeded())
    return ArchiveResult<ArchiveReader>::Failure(IoFailure(std::move(opened).Error()));
  auto size = opened.Value().Size();
  if (!size.Succeeded())
    return ArchiveResult<ArchiveReader>::Failure(IoFailure(std::move(size).Error()));
  if (!size.Value() || *size.Value() > static_cast<std::uint64_t>(std::numeric_limits<la_int64_t>::max())) {
    return ArchiveResult<ArchiveReader>::Failure(
        Error(ArchiveErrorCode::LimitExceeded, "Archive input size is unavailable or out of range"));
  }
  auto impl = std::make_unique<Impl>(std::move(opened).Value(), static_cast<la_int64_t>(*size.Value()), options);
  const Utf8Locale locale;
  ReaderSession session{impl->input};
  auto ready = session.Open();
  if (!ready.Succeeded())
    return ArchiveResult<ArchiveReader>::Failure(std::move(ready).Error());
  auto first = session.Next();
  if (!first.Succeeded())
    return ArchiveResult<ArchiveReader>::Failure(std::move(first).Error());
  return ArchiveResult<ArchiveReader>::Success(ArchiveReader(std::move(impl)));
}

ArchiveReader::ArchiveReader(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
ArchiveReader::ArchiveReader(ArchiveReader&&) noexcept = default;
ArchiveReader& ArchiveReader::operator=(ArchiveReader&&) noexcept = default;
ArchiveReader::~ArchiveReader() = default;

ArchiveResult<std::vector<ArchiveEntry>> ArchiveReader::ListEntries() {
  if (!impl_)
    return ArchiveResult<std::vector<ArchiveEntry>>::Failure(
        Error(ArchiveErrorCode::InvalidState, "Archive reader has been moved"));
  std::vector<ArchiveEntry> entries;
  auto result = impl_->Visit([&](ReaderSession&, archive_entry* entry, std::string_view path) {
    std::optional<std::uint64_t> size;
    if (archive_entry_size_is_set(entry) && archive_entry_size(entry) >= 0) {
      size = static_cast<std::uint64_t>(archive_entry_size(entry));
    }
    entries.push_back({std::string(path), GetEntryKind(entry), size});
  });
  if (!result.Succeeded())
    return ArchiveResult<std::vector<ArchiveEntry>>::Failure(std::move(result).Error());
  return ArchiveResult<std::vector<ArchiveEntry>>::Success(std::move(entries));
}

ArchiveResult<Bytes> ArchiveReader::ReadEntry(std::string_view entry_path) {
  if (!impl_)
    return ArchiveResult<Bytes>::Failure(Error(ArchiveErrorCode::InvalidState, "Archive reader has been moved"));
  Bytes bytes;
  auto result = impl_->WithEntry(entry_path, [&](ReaderSession& session, std::uint64_t& remaining) {
    return CopyEntryData(session, remaining, [&](std::uint64_t, std::span<const std::byte> chunk) {
      if (chunk.size() > bytes.max_size() - bytes.size())
        return ArchiveResult<void>::Failure(Error(ArchiveErrorCode::LimitExceeded, "Entry cannot fit in memory"));
      bytes.insert(bytes.end(), chunk.begin(), chunk.end());
      return ArchiveResult<void>::Success();
    });
  });
  if (!result.Succeeded())
    return ArchiveResult<Bytes>::Failure(std::move(result).Error());
  return ArchiveResult<Bytes>::Success(std::move(bytes));
}

ArchiveResult<void> ArchiveReader::ExtractEntry(std::string_view entry_path, const File& destination) {
  if (!impl_)
    return ArchiveResult<void>::Failure(Error(ArchiveErrorCode::InvalidState, "Archive reader has been moved"));
  return impl_->WithEntry(entry_path, [&](ReaderSession& session, std::uint64_t& remaining) {
    const auto output_path = FilePath(destination);
    auto parent = ResolveDirectory(output_path.parent_path());
    if (!parent.Succeeded())
      return ArchiveResult<void>::Failure(std::move(parent).Error());
    const File output((parent.Value() / output_path.filename()).u8string());
    return WriteExtractedFile(session, output, remaining);
  });
}

ArchiveResult<void> ArchiveReader::ExtractAll(const File& destination_directory) {
  if (!impl_)
    return ArchiveResult<void>::Failure(Error(ArchiveErrorCode::InvalidState, "Archive reader has been moved"));
  auto directory = ResolveDirectory(FilePath(destination_directory));
  if (!directory.Succeeded())
    return ArchiveResult<void>::Failure(std::move(directory).Error());
  const auto& root = directory.Value();
  auto remaining = impl_->options.max_output_bytes;
  return impl_->Visit([&](ReaderSession& session, archive_entry* entry, std::string_view path) -> ArchiveResult<bool> {
    const auto kind = GetEntryKind(entry);
    const auto safe = SafeEntryPath(path);
    if (!safe || kind == EntryKind::Other || (safe->empty() && kind != EntryKind::Directory)) {
      return ArchiveResult<bool>::Failure(
          Error(ArchiveErrorCode::UnsafeEntry, std::string("Unsafe archive entry: ").append(path)));
    }
    if (safe->empty())
      return ArchiveResult<bool>::Success(false);

    const auto relative = Utf8Path(*safe);
    auto result = CreateEntryDirectories(root, kind == EntryKind::Directory ? relative : relative.parent_path());
    if (!result.Succeeded())
      return ArchiveResult<bool>::Failure(std::move(result).Error());
    if (kind == EntryKind::File) {
      result = WriteExtractedFile(session, File((root / relative).u8string()), remaining);
      if (!result.Succeeded())
        return ArchiveResult<bool>::Failure(std::move(result).Error());
    }
    return ArchiveResult<bool>::Success(false);
  });
}

ArchiveResult<void> ExtractArchive(const File& source, const File& destination_directory, ArchiveReadOptions options) {
  auto reader = ArchiveReader::Open(source, options);
  if (!reader.Succeeded())
    return ArchiveResult<void>::Failure(std::move(reader).Error());
  return reader.Value().ExtractAll(destination_directory);
}

} // namespace huxerui::archive
