#include <huxerui/archive.h>

#include <archive.h>
#include <archive_entry.h>

#ifdef CreateDirectory
#undef CreateDirectory
#endif

#include <array>
#include <cstdlib>
#include <iostream>
#include <span>
#include <string>

using namespace huxerui;
using namespace huxerui::archive;

void Require(bool condition, const char* message) {
  if (!condition) {
    std::cerr << message << '\n';
    std::exit(1);
  }
}

template <class T> T Value(ArchiveResult<T> result) {
  if (!result.Succeeded()) {
    std::cerr << result.Error().message << '\n';
    std::exit(1);
  }
  if constexpr (!std::is_void_v<T>)
    return std::move(result).Value();
}

template <class T> T IoValue(IoResult<T> result) {
  if (!result.Succeeded()) {
    std::cerr << result.Error().message << '\n';
    std::exit(1);
  }
  return std::move(result).Value();
}

void WriteTarFixture(const File& file, std::span<const std::string_view> names, int type, bool hardlink = false) {
  std::array<std::byte, 8192> data;
  std::size_t size = 0;
  auto* writer = archive_write_new();
  Require(archive_write_set_format_pax_restricted(writer) == ARCHIVE_OK, "Set fixture format");
  Require(archive_write_open_memory(writer, data.data(), data.size(), &size) == ARCHIVE_OK, "Open fixture");
  for (const auto name : names) {
    auto* entry = archive_entry_new();
    archive_entry_set_pathname(entry, std::string(name).c_str());
    archive_entry_set_filetype(entry, type);
    archive_entry_set_perm(entry, 0644);
    archive_entry_set_size(entry, 0);
    if (type == AE_IFLNK)
      archive_entry_set_symlink(entry, "other");
    if (hardlink)
      archive_entry_set_hardlink(entry, "other");
    Require(archive_write_header(writer, entry) == ARCHIVE_OK, "Write fixture entry");
    archive_entry_free(entry);
  }
  Require(archive_write_free(writer) == ARCHIVE_OK, "Finish fixture");
  Require(file.WriteBytes(std::span(data).first(size)), "Save fixture");
}

int main(int argc, char** argv) {
  Require(argc == 2, "Missing test directory");
  File root(argv[1]);
  File source(root, "source");
  File plain(source, "file");
  File empty(source, "empty");
  File output(root, "output");
  Require(source.CreateDirectory() && empty.CreateDirectory() && output.CreateDirectory(), "Create directories");

  Bytes payload(320 * 1024);
  std::uint32_t random = 1;
  for (auto& byte : payload) {
    random = random * 1664525 + 1013904223;
    byte = static_cast<std::byte>(random >> 24);
  }
  Require(plain.WriteBytes(payload), "Write test payload");

  File default_archive(root, "default-archive");
  Value(CreateArchive(plain, default_archive));
  const auto zip_bytes = IoValue(default_archive.ReadBytes());
  Require(zip_bytes.size() >= 2 && zip_bytes[0] == std::byte{'P'} && zip_bytes[1] == std::byte{'K'},
          "Create ZIP by default");
  const auto single_file = Value(Value(ArchiveReader::Open(default_archive)).ListEntries());
  Require(single_file.size() == 1 && single_file.front().path == plain.Name(), "Keep source file name");

  File explicit_archive(root, "explicit-archive");
  Value(CreateArchive(source, explicit_archive, ArchiveFormat::TarGzip));
  const auto gzip_bytes = IoValue(explicit_archive.ReadBytes());
  Require(gzip_bytes.size() >= 2 && gzip_bytes[0] == std::byte{0x1f} && gzip_bytes[1] == std::byte{0x8b},
          "Select gzip-compressed TAR explicitly");
  Value(ExtractArchive(explicit_archive, output));
  Require(File(File(output, source.Name()), empty.Name()).IsDirectory(),
          "Keep the source directory and empty children");
  const File unpacked(File(output, source.Name()), plain.Name());
  const auto extract_conflict = ExtractArchive(explicit_archive, output);
  Require(!extract_conflict.Succeeded() && extract_conflict.Error().io_error &&
              extract_conflict.Error().io_error->code == IoErrorCode::AlreadyExists &&
              IoValue(unpacked.ReadBytes()) == payload,
          "Preserve existing files when extracting an archive");
  const auto missing_archive = ExtractArchive(File(root, "missing-archive"), output);
  Require(!missing_archive.Succeeded() && missing_archive.Error().io_error &&
              missing_archive.Error().io_error->code == IoErrorCode::NotFound,
          "Report an archive open failure");

  File bounded_output(root, "bounded-extraction");
  Require(bounded_output.CreateDirectory(), "Create bounded extraction destination");
  const auto too_many = ExtractArchive(default_archive, bounded_output, {.max_entries = 0});
  Require(!too_many.Succeeded() && too_many.Error().code == ArchiveErrorCode::LimitExceeded,
          "Apply the entry limit to whole-archive extraction");
  const auto too_large = ExtractArchive(default_archive, bounded_output, {.max_output_bytes = payload.size() - 1});
  Require(!too_large.Succeeded() && too_large.Error().code == ArchiveErrorCode::LimitExceeded &&
              !File(bounded_output, plain.Name()).Exists(),
          "Apply the output limit and remove the incomplete file");
  auto existing = CreateArchive(source, default_archive);
  Require(!existing.Succeeded() && existing.Error().io_error &&
              existing.Error().io_error->code == IoErrorCode::AlreadyExists &&
              IoValue(default_archive.ReadBytes()) == zip_bytes,
          "Preserve an existing destination");
  File incomplete(root, "incomplete.zip");
  auto missing_source = CreateArchive(File(root, "missing-source"), incomplete);
  Require(!missing_source.Succeeded() && !incomplete.Exists(), "Clean up an incomplete archive");

  constexpr std::string_view name = "entries/\xe8\xb5\x84\xe6\x96\x99.bin";
  const std::array formats{ArchiveFormat::Zip, ArchiveFormat::Tar, ArchiveFormat::TarGzip, ArchiveFormat::TarZstd};
  for (std::size_t index = 0; index < formats.size(); ++index) {
    File empty_archive(root, "empty-" + std::to_string(index));
    Value(Value(ArchiveWriter::Create(empty_archive, formats[index])).Finish());
    Require(Value(Value(ArchiveReader::Open(empty_archive)).ListEntries()).empty(), "List an empty archive");

    File archive(root, "archive-" + std::to_string(index));
    auto writer = Value(ArchiveWriter::Create(archive, formats[index]));
    Value(writer.AddEntry(name, payload));
    auto duplicate = writer.AddEntry(name, {});
    Require(!duplicate.Succeeded() && duplicate.Error().code == ArchiveErrorCode::DuplicateEntryPath,
            "Reject duplicate");
    Value(writer.AddDirectory("explicit-empty/"));
    Value(writer.AddEntry("empty-file", {}));
    Value(writer.AddPath("source", source));
    Value(writer.Finish());
    Value(writer.Finish());
    auto replacement = ArchiveWriter::Create(archive, formats[index]);
    Require(!replacement.Succeeded() && replacement.Error().io_error->code == IoErrorCode::AlreadyExists,
            "Preserve archive");

    auto reader = Value(ArchiveReader::Open(archive));
    const auto entries = Value(reader.ListEntries());
    Require(entries.size() == 6 && entries.front().path == name, "List file and directory entries");
    Require(Value(reader.ReadEntry(name)) == payload, "Read decompressed bytes");
    Require(Value(reader.ReadEntry("empty-file")).empty(), "Read an empty file");
    auto missing_read = reader.ReadEntry("missing");
    Require(!missing_read.Succeeded() && missing_read.Error().code == ArchiveErrorCode::EntryNotFound,
            "Report a missing entry when reading");
    auto directory_read = reader.ReadEntry(entries[1].path);
    Require(entries[1].kind == EntryKind::Directory && !directory_read.Succeeded() &&
                directory_read.Error().code == ArchiveErrorCode::UnsafeEntry,
            "Reject directories when reading");
    File extracted(root, "entry-" + std::to_string(index));
    Value(reader.ExtractEntry(name, extracted));
    Require(IoValue(extracted.ReadBytes()) == payload, "Single-entry byte round trip");
    auto conflict = reader.ExtractEntry(name, extracted);
    Require(!conflict.Succeeded() && conflict.Error().io_error->code == IoErrorCode::AlreadyExists,
            "Preserve existing entry");
    Require(IoValue(extracted.ReadBytes()) == payload, "Existing output unchanged");
    auto missing = reader.ExtractEntry("missing", extracted);
    Require(!missing.Succeeded() && missing.Error().code == ArchiveErrorCode::EntryNotFound, "Report missing entry");

    File directory(output, std::to_string(index));
    Require(directory.CreateDirectory(), "Create extraction destination");
    Value(reader.ExtractAll(directory));
    File subtree(directory, "source");
    Require(File(subtree, "empty").IsDirectory(), "Extract empty directory");
    Require(IoValue(File(directory, "empty-file").ReadBytes()).empty(), "Extract empty file");
    Require(IoValue(File(subtree, "file").ReadBytes()) == payload, "Full archive byte round trip");

    auto limited = Value(ArchiveReader::Open(archive, {.max_output_bytes = 100}));
    File limited_destination(root, "limited-" + std::to_string(index));
    auto over_limit = limited.ExtractEntry(name, limited_destination);
    Require(!over_limit.Succeeded() && over_limit.Error().code == ArchiveErrorCode::LimitExceeded,
            "Enforce extraction limit");
    Require(!limited_destination.Exists(), "Remove incomplete extraction");
    auto bounded = Value(ArchiveReader::Open(archive, {.max_output_bytes = payload.size() - 1}));
    auto oversized = bounded.ReadEntry(name);
    Require(!oversized.Succeeded() && oversized.Error().code == ArchiveErrorCode::LimitExceeded,
            "Enforce the cumulative read limit");
    auto exact = Value(ArchiveReader::Open(archive, {.max_output_bytes = payload.size()}));
    Require(Value(exact.ReadEntry(name)) == payload, "Read an entry exactly at the limit");
  }

  for (const auto format : {CompressionFormat::Gzip, CompressionFormat::Zstd}) {
    const auto suffix = format == CompressionFormat::Gzip ? ".gz" : ".zst";
    File compressed(root, std::string("compressed") + suffix);
    File uncompressed(root, std::string("uncompressed") + suffix);
    Value(CompressStream(IoValue(plain.OpenRead()), IoValue(compressed.OpenWrite()), format));
    Require(Value(DecompressStream(IoValue(compressed.OpenRead()), IoValue(uncompressed.OpenWrite()), format)) ==
                payload.size(),
            "Report decompressed size");
    Require(IoValue(uncompressed.ReadBytes()) == payload, "Stream byte round trip");
    auto limited = DecompressStream(IoValue(compressed.OpenRead()), IoValue(uncompressed.OpenWrite()), format, 100);
    Require(!limited.Succeeded() && limited.Error().code == ArchiveErrorCode::LimitExceeded, "Enforce decoder limit");
    auto bytes = IoValue(compressed.ReadBytes());
    const auto frame = bytes;
    bytes.insert(bytes.end(), frame.begin(), frame.end());
    Require(compressed.WriteBytes(bytes), "Write concatenated frames");
    Require(Value(DecompressStream(IoValue(compressed.OpenRead()), IoValue(uncompressed.OpenWrite()), format)) ==
                2 * payload.size(),
            "Decode concatenated frames");
    bytes = frame;
    bytes.back() ^= std::byte{1};
    Require(compressed.WriteBytes(bytes), "Write corrupt checksum");
    auto corrupt = DecompressStream(IoValue(compressed.OpenRead()), IoValue(uncompressed.OpenWrite()), format);
    Require(!corrupt.Succeeded() && corrupt.Error().code == ArchiveErrorCode::InvalidData, "Reject corrupt checksum");
    bytes = frame;
    bytes.pop_back();
    Require(compressed.WriteBytes(bytes), "Write truncated compressed stream");
    auto truncated = DecompressStream(IoValue(compressed.OpenRead()), IoValue(uncompressed.OpenWrite()), format);
    Require(!truncated.Succeeded() && truncated.Error().code == ArchiveErrorCode::InvalidData, "Reject truncation");
  }

  File fixture(root, "fixture.tar");
  const std::array<std::string_view, 2> repeated{"duplicate", "duplicate"};
  WriteTarFixture(fixture, repeated, AE_IFREG);
  {
    auto reader = Value(ArchiveReader::Open(fixture));
    Require(Value(reader.ListEntries()).size() == repeated.size(), "Retain repeated paths in listings");
    File destination(output, "duplicate");
    auto extracted = reader.ExtractEntry(repeated.front(), destination);
    Require(!extracted.Succeeded() && extracted.Error().code == ArchiveErrorCode::DuplicateEntryPath &&
                !destination.Exists(),
            "Ambiguous extraction must leave the destination untouched");
    auto read = reader.ReadEntry(repeated.front());
    Require(!read.Succeeded() && read.Error().code == ArchiveErrorCode::DuplicateEntryPath,
            "Reject ambiguous paths when reading");
  }
  auto rejects_fixture = [&](std::string_view path, int type, bool hardlink = false) {
    WriteTarFixture(fixture, std::span(&path, 1), type, hardlink);
    auto reader = Value(ArchiveReader::Open(fixture));
    auto result = reader.ExtractAll(output);
    Require(!result.Succeeded() && result.Error().code == ArchiveErrorCode::UnsafeEntry, "Reject unsafe entry");
    auto read = reader.ReadEntry(path);
    Require(!read.Succeeded() && read.Error().code == ArchiveErrorCode::UnsafeEntry, "Reject unsafe reads");
  };
  rejects_fixture("../outside", AE_IFREG);
  rejects_fixture("symbolic", AE_IFLNK);
  rejects_fixture("hard", AE_IFREG, true);
  Require(!File(root, "outside").Exists(), "Extraction stayed under the destination");

  std::cout << "Archive and compression checks passed.\n";
}
