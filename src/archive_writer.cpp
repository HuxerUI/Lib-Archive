#include "archive_file.h"

#include <algorithm>
#include <limits>
#include <map>

namespace huxerui::archive {
namespace {

using namespace detail;
using WriteHandle = std::unique_ptr<struct archive, decltype(&archive_write_free)>;
using EntryHandle = std::unique_ptr<archive_entry, decltype(&archive_entry_free)>;

ArchiveResult<void> ConfigureWriter(struct archive* handle, ArchiveFormat format) {
  int status;
  switch (format) {
  case ArchiveFormat::Zip:
    status = archive_write_set_format_zip(handle);
    break;
  case ArchiveFormat::Tar:
  case ArchiveFormat::TarGzip:
  case ArchiveFormat::TarZstd:
    status = archive_write_set_format_pax_restricted(handle);
    break;
  default:
    return ArchiveResult<void>::Failure(Error(ArchiveErrorCode::InvalidArgument, "Invalid archive format"));
  }
  if (status == ARCHIVE_OK && format == ArchiveFormat::TarGzip)
    status = archive_write_add_filter_gzip(handle);
  if (status == ARCHIVE_OK && format == ArchiveFormat::TarZstd)
    status = archive_write_add_filter_zstd(handle);
  if (status != ARCHIVE_OK)
    return ArchiveResult<void>::Failure(
        ArchiveFailure(handle, ArchiveErrorCode::UnsupportedFormat, "Archive format unavailable"));
  if (archive_write_set_bytes_in_last_block(handle, 1) != ARCHIVE_OK) {
    return ArchiveResult<void>::Failure(
        ArchiveFailure(handle, ArchiveErrorCode::InvalidState, "Could not configure archive output"));
  }
  return ArchiveResult<void>::Success();
}

ArchiveError DuplicatePath() {
  return Error(ArchiveErrorCode::DuplicateEntryPath, "Archive path conflicts with an existing entry");
}

} // namespace

struct ArchiveWriter::Impl {
  enum class State { Writing, Failed, Finished };

  File destination;
  std::optional<RandomAccessStream> output;
  std::uint64_t offset = 0;
  std::optional<IoError> error;
  std::map<std::string, EntryKind, std::less<>> entries;
  State state = State::Writing;
  WriteHandle handle{archive_write_new(), archive_write_free};

  explicit Impl(File file) : destination(std::move(file)) {}

  ~Impl() {
    if (handle && state != State::Finished)
      archive_write_fail(handle.get());
  }

  static la_ssize_t Write(struct archive*, void* data, const void* bytes, std::size_t size) {
    auto& writer = *static_cast<Impl*>(data);
    if (size > std::numeric_limits<std::uint64_t>::max() - writer.offset) {
      writer.error = IoError{IoErrorCode::TooLarge, "Archive output exceeds the supported file size"};
      return -1;
    }
    auto result = writer.output->WriteAt(writer.offset, {static_cast<const std::byte*>(bytes), size});
    if (!result.Succeeded()) {
      writer.error = std::move(result).Error();
      return -1;
    }
    writer.offset += size;
    return static_cast<la_ssize_t>(size);
  }

  ArchiveError Failure() {
    state = State::Failed;
    if (error)
      return IoFailure(*error);
    return ArchiveFailure(handle.get(), ArchiveErrorCode::Io, "Could not write the archive");
  }

  ArchiveResult<void> Begin(const std::string& path, EntryKind kind, std::uint64_t size) {
    if (entries.contains(path))
      return ArchiveResult<void>::Failure(DuplicatePath());
    for (auto slash = path.find('/'); slash != std::string::npos; slash = path.find('/', slash + 1)) {
      const auto parent = entries.find(std::string_view(path).substr(0, slash));
      if (parent != entries.end() && parent->second == EntryKind::File)
        return ArchiveResult<void>::Failure(DuplicatePath());
    }
    if (kind == EntryKind::File) {
      const std::string prefix = path + '/';
      const auto child = entries.lower_bound(prefix);
      if (child != entries.end() && child->first.starts_with(prefix))
        return ArchiveResult<void>::Failure(DuplicatePath());
    }
    if (size > static_cast<std::uint64_t>(std::numeric_limits<la_int64_t>::max())) {
      return ArchiveResult<void>::Failure(
          Error(ArchiveErrorCode::LimitExceeded, "Entry exceeds the supported file size"));
    }

    const Utf8Locale locale;
    EntryHandle entry(archive_entry_new(), archive_entry_free);
    if (!entry)
      return ArchiveResult<void>::Failure(Error(ArchiveErrorCode::Io, "Could not allocate an archive entry"));
    archive_entry_set_pathname_utf8(entry.get(), path.c_str());
    archive_entry_set_filetype(entry.get(), kind == EntryKind::Directory ? AE_IFDIR : AE_IFREG);
    archive_entry_set_perm(entry.get(), kind == EntryKind::Directory ? 0755 : 0644);
    archive_entry_set_size(entry.get(), static_cast<la_int64_t>(size));
    if (archive_write_header(handle.get(), entry.get()) != ARCHIVE_OK)
      return ArchiveResult<void>::Failure(Failure());
    entries.emplace(path, kind);
    return ArchiveResult<void>::Success();
  }

  ArchiveResult<void> WriteBytes(std::span<const std::byte> bytes) {
    while (!bytes.empty()) {
      const auto data = bytes.first(std::min(bytes.size(), ChunkSize));
      const auto written = archive_write_data(handle.get(), data.data(), data.size());
      if (written <= 0 || static_cast<std::size_t>(written) > data.size())
        return ArchiveResult<void>::Failure(Failure());
      bytes = bytes.subspan(static_cast<std::size_t>(written));
    }
    return ArchiveResult<void>::Success();
  }

  ArchiveResult<void> End() {
    if (archive_write_finish_entry(handle.get()) != ARCHIVE_OK)
      return ArchiveResult<void>::Failure(Failure());
    return ArchiveResult<void>::Success();
  }

  ArchiveResult<void> AddSource(const std::string& path, const File& source, std::span<std::byte> buffer) {
    const auto file_path = FilePath(source);
    auto status = PathStatus(file_path);
    if (!status.Succeeded())
      return ArchiveResult<void>::Failure(std::move(status).Error());
    if (std::filesystem::is_directory(status.Value())) {
      auto children = source.ListChildren();
      if (!children.Succeeded())
        return ArchiveResult<void>::Failure(IoFailure(std::move(children).Error()));
      auto added = Begin(path, EntryKind::Directory, 0);
      if (!added.Succeeded())
        return added;
      auto ended = End();
      if (!ended.Succeeded())
        return ended;

      std::ranges::sort(children.Value(), {}, &File::Path);
      for (const auto& child : children.Value()) {
        auto name = SafeEntryPath(path + '/' + child.Name());
        if (!name || name->empty()) {
          return ArchiveResult<void>::Failure(Error(ArchiveErrorCode::UnsafeEntry, "Unsafe source entry name"));
        }
        auto result = AddSource(*name, child, buffer);
        if (!result.Succeeded())
          return result;
      }
      return ArchiveResult<void>::Success();
    }
    if (!std::filesystem::exists(status.Value())) {
      return ArchiveResult<void>::Failure(
          FileSystemError(file_path, std::make_error_code(std::errc::no_such_file_or_directory)));
    }
    if (!std::filesystem::is_regular_file(status.Value())) {
      return ArchiveResult<void>::Failure(
          Error(ArchiveErrorCode::UnsafeEntry, "Source is a symbolic link or a special file"));
    }
    std::error_code error;
    if (std::filesystem::equivalent(file_path, FilePath(destination), error)) {
      return ArchiveResult<void>::Failure(
          Error(ArchiveErrorCode::InvalidArgument, "Source contains the output archive"));
    }
    if (error)
      return ArchiveResult<void>::Failure(FileSystemError(file_path, error));
    auto input = source.OpenRandomAccess();
    if (!input.Succeeded())
      return ArchiveResult<void>::Failure(IoFailure(std::move(input).Error()));
    auto size = input.Value().Size();
    if (!size.Succeeded())
      return ArchiveResult<void>::Failure(IoFailure(std::move(size).Error()));
    if (!size.Value()) {
      return ArchiveResult<void>::Failure(Error(ArchiveErrorCode::Io, "Could not determine source file size"));
    }

    auto added = Begin(path, EntryKind::File, *size.Value());
    if (!added.Succeeded())
      return added;
    std::uint64_t position = 0;
    while (position < *size.Value()) {
      auto result = input.Value().ReadAt(
          position,
          buffer.first(static_cast<std::size_t>(std::min<std::uint64_t>(buffer.size(), *size.Value() - position))));
      if (!result.Succeeded())
        return ArchiveResult<void>::Failure(IoFailure(std::move(result).Error()));
      if (result.Value() == 0) {
        return ArchiveResult<void>::Failure(Error(ArchiveErrorCode::Io, "Source ended before its declared size"));
      }
      auto written = WriteBytes(buffer.first(result.Value()));
      if (!written.Succeeded())
        return written;
      position += result.Value();
    }
    return End();
  }
};

ArchiveResult<ArchiveWriter> ArchiveWriter::Create(const File& destination, ArchiveFormat format) {
  const Utf8Locale locale;
  auto impl = std::make_unique<Impl>(destination);
  if (!impl->handle)
    return ArchiveResult<ArchiveWriter>::Failure(Error(ArchiveErrorCode::Io, "Could not allocate an archive writer"));
  auto configured = ConfigureWriter(impl->handle.get(), format);
  if (!configured.Succeeded())
    return ArchiveResult<ArchiveWriter>::Failure(std::move(configured).Error());
  auto created = destination.OpenRandomAccess(FileAccessMode::WriteOnly, FileOpenMode::CreateNew);
  if (!created.Succeeded())
    return ArchiveResult<ArchiveWriter>::Failure(IoFailure(std::move(created).Error()));
  impl->output.emplace(std::move(created).Value());
  if (archive_write_open(impl->handle.get(), impl.get(), nullptr, Impl::Write, nullptr) != ARCHIVE_OK) {
    auto error = impl->Failure();
    impl.reset();
    (void)destination.Delete();
    return ArchiveResult<ArchiveWriter>::Failure(std::move(error));
  }
  return ArchiveResult<ArchiveWriter>::Success(ArchiveWriter(std::move(impl)));
}

ArchiveWriter::ArchiveWriter(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
ArchiveWriter::ArchiveWriter(ArchiveWriter&&) noexcept = default;
ArchiveWriter& ArchiveWriter::operator=(ArchiveWriter&&) noexcept = default;
ArchiveWriter::~ArchiveWriter() = default;

ArchiveResult<void> ArchiveWriter::AddEntry(std::string_view entry_path, std::span<const std::byte> bytes) {
  if (!impl_ || impl_->state != Impl::State::Writing) {
    return ArchiveResult<void>::Failure(Error(ArchiveErrorCode::InvalidState, "Archive writer is not writable"));
  }
  const auto path = SafeEntryPath(entry_path);
  if (!path || path->empty() || entry_path.ends_with('/')) {
    return ArchiveResult<void>::Failure(Error(ArchiveErrorCode::UnsafeEntry, "Unsafe archive file path"));
  }
  auto added = impl_->Begin(*path, EntryKind::File, bytes.size());
  if (!added.Succeeded())
    return added;
  auto written = impl_->WriteBytes(bytes);
  if (!written.Succeeded())
    return written;
  return impl_->End();
}

ArchiveResult<void> ArchiveWriter::AddPath(std::string_view entry_path, const File& source) {
  if (!impl_ || impl_->state != Impl::State::Writing) {
    return ArchiveResult<void>::Failure(Error(ArchiveErrorCode::InvalidState, "Archive writer is not writable"));
  }
  const auto path = SafeEntryPath(entry_path);
  if (!path || path->empty()) {
    return ArchiveResult<void>::Failure(Error(ArchiveErrorCode::UnsafeEntry, "Unsafe archive entry path"));
  }

  const auto source_path = FilePath(source);
  auto parent = ResolveDirectory(source_path.parent_path());
  if (!parent.Succeeded())
    return ArchiveResult<void>::Failure(std::move(parent).Error());
  std::error_code error;
  const auto destination = std::filesystem::canonical(FilePath(impl_->destination), error);
  if (error)
    return ArchiveResult<void>::Failure(FileSystemError(FilePath(impl_->destination), error));
  for (auto candidate = destination; !candidate.empty();) {
    if (std::filesystem::equivalent(source_path, candidate, error)) {
      return ArchiveResult<void>::Failure(
          Error(ArchiveErrorCode::InvalidArgument, "Source contains the output archive"));
    }
    const auto next = candidate.parent_path();
    if (next == candidate)
      break;
    candidate = next;
  }

  Bytes buffer(ChunkSize);
  const auto before = impl_->entries.size();
  auto result = impl_->AddSource(*path, source, buffer);
  if (!result.Succeeded() && impl_->entries.size() != before)
    impl_->state = Impl::State::Failed;
  return result;
}

ArchiveResult<void> ArchiveWriter::AddDirectory(std::string_view entry_path) {
  if (!impl_ || impl_->state != Impl::State::Writing) {
    return ArchiveResult<void>::Failure(Error(ArchiveErrorCode::InvalidState, "Archive writer is not writable"));
  }
  const auto path = SafeEntryPath(entry_path);
  if (!path || path->empty()) {
    return ArchiveResult<void>::Failure(Error(ArchiveErrorCode::UnsafeEntry, "Unsafe archive directory path"));
  }
  auto added = impl_->Begin(*path, EntryKind::Directory, 0);
  if (!added.Succeeded())
    return added;
  return impl_->End();
}

ArchiveResult<void> ArchiveWriter::Finish() {
  if (!impl_ || impl_->state == Impl::State::Failed) {
    return ArchiveResult<void>::Failure(Error(ArchiveErrorCode::InvalidState, "Archive writer cannot be finished"));
  }
  if (impl_->state == Impl::State::Finished)
    return ArchiveResult<void>::Success();
  if (archive_write_close(impl_->handle.get()) != ARCHIVE_OK)
    return ArchiveResult<void>::Failure(impl_->Failure());
  auto closed = impl_->output->Close();
  if (!closed.Succeeded()) {
    impl_->state = Impl::State::Failed;
    return ArchiveResult<void>::Failure(IoFailure(std::move(closed).Error()));
  }
  impl_->state = Impl::State::Finished;
  return ArchiveResult<void>::Success();
}

ArchiveResult<void> CreateArchive(const File& source, const File& destination, ArchiveFormat format) {
  const auto name = source.Name();
  if (name.empty()) {
    return ArchiveResult<void>::Failure(Error(ArchiveErrorCode::InvalidArgument, "Archive source must have a name"));
  }

  auto created = ArchiveWriter::Create(destination, format);
  if (!created.Succeeded())
    return ArchiveResult<void>::Failure(std::move(created).Error());

  auto result = [&]() {
    auto writer = std::move(created).Value();
    auto added = writer.AddPath(name, source);
    if (!added.Succeeded())
      return added;
    return writer.Finish();
  }();
  if (!result.Succeeded())
    (void)destination.Delete();
  return result;
}

} // namespace huxerui::archive
