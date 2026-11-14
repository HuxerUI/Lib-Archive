#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <huxerui/data.h>
#include <huxerui/file.h>
#include <huxerui/stream.h>
#include <huxerui/task.h>

namespace huxerui::archive {

/// Archive containers available for reading and creation.
enum class ArchiveFormat { Zip, Tar, TarGzip, TarZstd };

/// Formats for a single compressed byte stream.
enum class CompressionFormat { Gzip, Zstd };

/// The kind of entry recorded in an archive. Other includes links and special files.
enum class EntryKind { File, Directory, Other };

/// Stable failure categories for archive and compression operations.
enum class ArchiveErrorCode {
  UnsupportedFormat,  ///< The requested or detected format or feature is unsupported.
  InvalidArgument,    ///< A caller supplied an invalid path, format, or option.
  InvalidState,       ///< The reader or writer is moved from, finished, or unusable after failure.
  InvalidData,        ///< The archive or compressed stream is malformed or truncated.
  EntryNotFound,      ///< No archive entry has the requested path.
  DuplicateEntryPath, ///< A path is ambiguous in the input or conflicts with an entry being written.
  UnsafeEntry,        ///< A path or entry kind cannot be handled safely.
  LimitExceeded,      ///< An entry count, output size, or decoder memory limit was exceeded.
  Io,                 ///< A file or byte stream operation failed.
};

struct ArchiveError {
  ArchiveErrorCode code;
  /// Diagnostic text for display or logging.
  std::string message;
  /// The original I/O category and message when available.
  std::optional<IoError> io_error;
};

/// An owned value or an archive error. Check Succeeded() before accessing Value() or Error().
template <class T> using ArchiveResult = huxerui::Result<T, ArchiveError>;

/// Metadata reported by the archive. Use the returned path to select an entry.
struct ArchiveEntry {
  std::string path;
  EntryKind kind;
  /// Uncompressed size reported in the archive, when available.
  std::optional<std::uint64_t> size;
};

/// Limits for listing, reading, and extracting archive entries. Exceeding a limit returns LimitExceeded.
struct ArchiveReadOptions {
  /// Maximum entries per traversal, including directories and entries skipped during a path lookup.
  /// Defaults to 100,000.
  std::uint64_t max_entries = 100'000;
  /// Maximum bytes returned by ReadEntry() or written by ExtractEntry() per call.
  /// ExtractAll() and ExtractArchive() share one budget across all files in the call.
  /// Counts actual decompressed output, including zero-filled sparse ranges. Defaults to 1 GiB.
  std::uint64_t max_output_bytes = 1ull << 30;
};

/// A move-only reader over an open local archive. Serialize operations on each reader.
/// On Web, copy a picked FileReference into an application temporary file before opening it.
class ArchiveReader final {
public:
  /// @brief Opens a local archive and identifies its format from its contents.
  /// @param source Archive file whose contents must stay unchanged while the reader is in use.
  /// @param options Entry count and decompressed output limits for the reader.
  static ArchiveResult<ArchiveReader> Open(const File& source, ArchiveReadOptions options = {});

  ArchiveReader(const ArchiveReader&) = delete;
  ArchiveReader& operator=(const ArchiveReader&) = delete;
  ArchiveReader(ArchiveReader&&) noexcept;
  ArchiveReader& operator=(ArchiveReader&&) noexcept;
  ~ArchiveReader();

  /// Lists entries in archive order, including repeated paths and special entry kinds.
  ArchiveResult<std::vector<ArchiveEntry>> ListEntries();

  /// @brief Reads an ordinary file into owned memory using the same entry validation as ExtractEntry().
  /// @param entry_path Exact, case-sensitive path returned by ListEntries(). The path must identify one entry.
  /// @return The complete decompressed contents or an error. An empty file returns empty Bytes.
  /// Use a lower output limit when memory is constrained.
  ArchiveResult<Bytes> ReadEntry(std::string_view entry_path);

  /// @brief Extracts one ordinary file. A name collision in the archive is reported before opening the destination.
  /// An existing destination file is preserved and reported as IoErrorCode::AlreadyExists.
  /// This can scan the full archive before writing, including for compressed TAR.
  /// @param entry_path Exact, case-sensitive path returned by ListEntries().
  /// @param destination Complete output file path. Its parent must exist and the caller must
  /// control the path throughout extraction.
  ArchiveResult<void> ExtractEntry(std::string_view entry_path, const File& destination);

  /// @brief Extracts ordinary files and directories.
  /// Entries with unsafe paths, links, or special file kinds cause failure. Existing files are preserved
  /// and reported as IoErrorCode::AlreadyExists. Completed entries remain on failure.
  /// @param destination_directory Existing directory whose tree remains under the caller's control during extraction.
  ArchiveResult<void> ExtractAll(const File& destination_directory);

private:
  struct Impl;
  explicit ArchiveReader(std::unique_ptr<Impl> impl) noexcept;

  std::unique_ptr<Impl> impl_;
};

/// A move-only writer for a new local archive. Serialize operations on each writer.
/// Call Finish() to finalize the archive. Destruction releases resources, and an unfinished
/// writer can leave a partial archive.
class ArchiveWriter final {
public:
  /// @brief Exclusively creates a new archive. An existing destination is preserved.
  /// On Web, use an application temporary file for synchronous writes.
  /// @param destination Complete archive file path whose parent directory must exist.
  /// @param format Archive format to write.
  static ArchiveResult<ArchiveWriter> Create(const File& destination, ArchiveFormat format);

  ArchiveWriter(const ArchiveWriter&) = delete;
  ArchiveWriter& operator=(const ArchiveWriter&) = delete;
  ArchiveWriter(ArchiveWriter&&) noexcept;
  ArchiveWriter& operator=(ArchiveWriter&&) noexcept;
  ~ArchiveWriter();

  /// @brief Adds a regular file from memory.
  /// An existing entry at the same path causes DuplicateEntryPath.
  /// @param entry_path UTF-8, slash-separated relative path within the archive.
  /// @param bytes File contents borrowed until return. An empty span adds an empty file.
  ArchiveResult<void> AddEntry(std::string_view entry_path, std::span<const std::byte> bytes);

  /// @brief Adds an ordinary local file or recursively adds a directory and its descendants.
  /// Files are read incrementally. Empty directories are included. Symbolic links and special files are rejected.
  /// @param entry_path UTF-8, slash-separated relative path for the file or the root of the directory tree.
  /// @param source Local file or directory. It must remain unchanged during the call and exclude the output archive.
  ArchiveResult<void> AddPath(std::string_view entry_path, const File& source);

  /// @brief Adds an explicit directory entry.
  /// @param entry_path UTF-8, slash-separated relative path within the archive.
  ArchiveResult<void> AddDirectory(std::string_view entry_path);

  /// Writes archive trailers and closes the output, reporting errors from both operations.
  /// Repeated calls after success also succeed. A writer that fails during a write cannot be resumed.
  ArchiveResult<void> Finish();

private:
  struct Impl;
  explicit ArchiveWriter(std::unique_ptr<Impl> impl) noexcept;

  std::unique_ptr<Impl> impl_;
};

/// @brief Creates an archive containing one local file or directory under its own name.
/// An existing destination is preserved. On failure after creation, attempts to remove the incomplete archive.
/// @param source Local file or directory to package. Includes all descendants and empty directories.
/// @param destination Complete archive file path whose parent directory must exist.
/// @param format Archive format to write. Defaults to ZIP.
ArchiveResult<void> CreateArchive(const File& source, const File& destination,
                                  ArchiveFormat format = ArchiveFormat::Zip);

/// @brief Extracts all entries according to ArchiveReader::ExtractAll() rules.
/// @param source Local archive file. The format is detected from its contents.
/// @param destination_directory Existing directory whose tree remains under the caller's control during extraction.
/// @param options Entry count and total decompressed output limits.
ArchiveResult<void> ExtractArchive(const File& source, const File& destination_directory,
                                   ArchiveReadOptions options = {});

/// @brief Compresses through EOF. An error can leave partial output.
/// @param input Source stream transferred to the operation, including on failure.
/// @param output Destination stream transferred to the operation and closed on success.
/// @param format Compression format to write.
/// @return The number of bytes written or an error.
ArchiveResult<std::uint64_t> CompressStream(InputStream input, OutputStream output, CompressionFormat format);

/// @brief Decompresses a complete stream with a byte limit on the output.
/// Malformed or truncated input returns InvalidData. An error can leave partial output.
/// @param input Compressed source stream transferred to the operation, including on failure.
/// @param output Destination stream transferred to the operation and closed on success.
/// @param format Expected compression format.
/// @param max_output_bytes Maximum actual bytes written. Defaults to 256 MiB.
/// @return The number of bytes written or an error.
ArchiveResult<std::uint64_t> DecompressStream(InputStream input, OutputStream output, CompressionFormat format,
                                              std::uint64_t max_output_bytes = 256ull << 20);

/// @brief Asynchronously compresses through EOF. Failure or cancellation can leave partial output.
/// @param input Source stream transferred to the task.
/// @param output Destination stream transferred to the task and closed on success.
/// @param format Compression format to write.
/// @return A task yielding the number of bytes written or an error.
Task<ArchiveResult<std::uint64_t>> CompressStreamAsync(AsyncInputStream input, AsyncOutputStream output,
                                                       CompressionFormat format);

/// @brief Asynchronously decompresses a complete stream with a byte limit on the output.
/// Malformed or truncated input returns InvalidData. Failure or cancellation can leave partial output.
/// @param input Compressed source stream transferred to the task.
/// @param output Destination stream transferred to the task and closed on success.
/// @param format Expected compression format.
/// @param max_output_bytes Maximum actual bytes written. Defaults to 256 MiB.
/// @return A task yielding the number of bytes written or an error.
Task<ArchiveResult<std::uint64_t>> DecompressStreamAsync(AsyncInputStream input, AsyncOutputStream output,
                                                         CompressionFormat format,
                                                         std::uint64_t max_output_bytes = 256ull << 20);

} // namespace huxerui::archive
