#pragma once

#include "archive_internal.h"

#include <archive.h>
#include <archive_entry.h>

#include <clocale>
#include <filesystem>

namespace huxerui::archive::detail {

/// libarchive converts some path metadata through LC_CTYPE on the calling thread.
class Utf8Locale {
public:
  Utf8Locale();
  ~Utf8Locale();
  Utf8Locale(const Utf8Locale&) = delete;
  Utf8Locale& operator=(const Utf8Locale&) = delete;

private:
#if defined(_WIN32)
  int previous_mode_ = 0;
  std::string previous_locale_;
#else
  locale_t locale_ = nullptr;
  locale_t previous_locale_ = nullptr;
#endif
};

std::filesystem::path Utf8Path(std::string_view path);
std::filesystem::path FilePath(const File& file);
ArchiveError FileSystemError(const std::filesystem::path& path, std::error_code error);
ArchiveResult<std::filesystem::file_status> PathStatus(const std::filesystem::path& path);
ArchiveResult<std::filesystem::path> ResolveDirectory(const std::filesystem::path& path);
ArchiveResult<void> CreateEntryDirectories(std::filesystem::path root, const std::filesystem::path& relative);
std::optional<std::string> SafeEntryPath(std::string_view path);
EntryKind GetEntryKind(archive_entry* entry);

inline ArchiveError ArchiveFailure(struct archive* handle, ArchiveErrorCode code, const char* fallback) {
  const char* message = archive_error_string(handle);
  return Error(code, message ? message : fallback);
}

} // namespace huxerui::archive::detail
