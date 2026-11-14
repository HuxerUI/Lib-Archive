#include "archive_file.h"

#include <algorithm>

#if defined(_WIN32)
#include <windows.h>
#endif

namespace huxerui::archive::detail {
namespace {

bool IsUtf8(std::string_view text) {
  std::uint32_t value = 0;
  std::uint32_t minimum = 0;
  unsigned remaining = 0;
  for (unsigned char byte : text) {
    if (remaining) {
      if ((byte & 0xc0) != 0x80)
        return false;
      value = (value << 6) | (byte & 0x3f);
      if (--remaining == 0 && (value < minimum || value > 0x10ffff || (value >= 0xd800 && value <= 0xdfff)))
        return false;
    } else if (byte >= 0x80) {
      if (byte < 0xc2 || byte > 0xf4)
        return false;
      remaining = byte < 0xe0 ? 1 : byte < 0xf0 ? 2 : 3;
      value = byte & (0x7f >> (remaining + 1));
      minimum = remaining == 1 ? 0x80 : remaining == 2 ? 0x800 : 0x10000;
    }
  }
  return remaining == 0;
}

bool IsPortableSegment(std::string_view part) {
  if (part == ".." || part.back() == '.' || part.back() == ' ' ||
      part.find_first_of("<>:\"|?*") != std::string_view::npos ||
      std::ranges::any_of(part, [](unsigned char c) { return c < 32; }))
    return false;
  std::string stem(part.substr(0, part.find('.')));
  while (!stem.empty() && stem.back() == ' ')
    stem.pop_back();
  std::ranges::transform(stem, stem.begin(), [](char c) { return c >= 'a' && c <= 'z' ? c - 'a' + 'A' : c; });
  if (stem == "CON" || stem == "PRN" || stem == "AUX" || stem == "NUL")
    return false;
  if (stem.starts_with("COM") || stem.starts_with("LPT")) {
    const std::string_view suffix(stem.data() + 3, stem.size() - 3);
    if ((suffix.size() == 1 && suffix.front() >= '0' && suffix.front() <= '9') || suffix == "\xc2\xb9" ||
        suffix == "\xc2\xb2" || suffix == "\xc2\xb3")
      return false;
  }
  return true;
}

} // namespace

Utf8Locale::Utf8Locale() {
#if defined(_WIN32)
  previous_mode_ = _configthreadlocale(_ENABLE_PER_THREAD_LOCALE);
  if (previous_mode_ != -1) {
    if (const char* current = std::setlocale(LC_CTYPE, nullptr))
      previous_locale_ = current;
    std::setlocale(LC_CTYPE, ".UTF8");
  }
#else
  locale_ = newlocale(LC_CTYPE_MASK, "C.UTF-8", nullptr);
  if (!locale_)
    locale_ = newlocale(LC_CTYPE_MASK, "en_US.UTF-8", nullptr);
  if (locale_)
    previous_locale_ = uselocale(locale_);
#endif
}

Utf8Locale::~Utf8Locale() {
#if defined(_WIN32)
  if (previous_mode_ != -1) {
    if (!previous_locale_.empty())
      std::setlocale(LC_CTYPE, previous_locale_.c_str());
    _configthreadlocale(previous_mode_);
  }
#else
  if (previous_locale_)
    uselocale(previous_locale_);
  if (locale_)
    freelocale(locale_);
#endif
}

std::filesystem::path Utf8Path(std::string_view path) {
  return std::filesystem::path(std::u8string(path.begin(), path.end()));
}

std::filesystem::path FilePath(const File& file) {
  return Utf8Path(file.Path());
}

ArchiveError FileSystemError(const std::filesystem::path& path, std::error_code error) {
  auto code = IoErrorCode::Io;
  if (error == std::errc::no_such_file_or_directory)
    code = IoErrorCode::NotFound;
  if (error == std::errc::permission_denied)
    code = IoErrorCode::PermissionDenied;
  if (error == std::errc::not_a_directory)
    code = IoErrorCode::NotDirectory;
  if (error == std::errc::file_exists)
    code = IoErrorCode::AlreadyExists;
  if (error == std::errc::no_space_on_device)
    code = IoErrorCode::NoSpace;
  const auto name = path.u8string();
  return IoFailure({code, std::string(name.begin(), name.end()) + ": " + error.message()});
}

ArchiveResult<std::filesystem::file_status> PathStatus(const std::filesystem::path& path) {
  std::error_code error;
  const auto status = std::filesystem::symlink_status(path, error);
  if (error && error != std::errc::no_such_file_or_directory)
    return ArchiveResult<std::filesystem::file_status>::Failure(FileSystemError(path, error));
#if defined(_WIN32)
  if (std::filesystem::exists(status)) {
    const DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
      return ArchiveResult<std::filesystem::file_status>::Failure(
          FileSystemError(path, {static_cast<int>(GetLastError()), std::system_category()}));
    }
    if (attributes & FILE_ATTRIBUTE_REPARSE_POINT) {
      return ArchiveResult<std::filesystem::file_status>::Success(
          std::filesystem::file_status(std::filesystem::file_type::symlink));
    }
  }
#endif
  return ArchiveResult<std::filesystem::file_status>::Success(status);
}

ArchiveResult<std::filesystem::path> ResolveDirectory(const std::filesystem::path& path) {
  using Result = ArchiveResult<std::filesystem::path>;
  std::error_code error;
  auto resolved = std::filesystem::canonical(path, error);
  if (error)
    return Result::Failure(FileSystemError(path, error));
  if (!std::filesystem::is_directory(resolved, error)) {
    return Result::Failure(FileSystemError(path, error ? error : std::make_error_code(std::errc::not_a_directory)));
  }
  return Result::Success(std::move(resolved));
}

ArchiveResult<void> CreateEntryDirectories(std::filesystem::path root, const std::filesystem::path& relative) {
  for (const auto& part : relative) {
    root /= part;
    auto status = PathStatus(root);
    if (!status.Succeeded())
      return ArchiveResult<void>::Failure(std::move(status).Error());
    if (std::filesystem::is_symlink(status.Value())) {
      return ArchiveResult<void>::Failure(Error(ArchiveErrorCode::UnsafeEntry, "Directory path crosses a link"));
    }
    if (std::filesystem::is_directory(status.Value()))
      continue;
    if (std::filesystem::exists(status.Value())) {
      return ArchiveResult<void>::Failure(FileSystemError(root, std::make_error_code(std::errc::not_a_directory)));
    }
    std::error_code error;
    if (!std::filesystem::create_directory(root, error)) {
      if (error)
        return ArchiveResult<void>::Failure(FileSystemError(root, error));
      status = PathStatus(root);
      if (!status.Succeeded())
        return ArchiveResult<void>::Failure(std::move(status).Error());
      if (!std::filesystem::is_directory(status.Value())) {
        return ArchiveResult<void>::Failure(
            Error(ArchiveErrorCode::UnsafeEntry, "Directory path changed during extraction"));
      }
    }
  }
  return ArchiveResult<void>::Success();
}

std::optional<std::string> SafeEntryPath(std::string_view path) {
  if (path.empty() || path.front() == '/' || path.find('\\') != std::string_view::npos || !IsUtf8(path)) {
    return std::nullopt;
  }
  std::string normalized;
  while (!path.empty()) {
    const auto end = path.find('/');
    const auto part = path.substr(0, end);
    if (!part.empty() && part != ".") {
      if (!IsPortableSegment(part))
        return std::nullopt;
      if (!normalized.empty())
        normalized += '/';
      normalized.append(part);
    }
    if (end == std::string_view::npos)
      break;
    path.remove_prefix(end + 1);
  }
  return normalized;
}

EntryKind GetEntryKind(archive_entry* entry) {
  if (archive_entry_hardlink(entry) || archive_entry_symlink(entry))
    return EntryKind::Other;
  switch (archive_entry_filetype(entry)) {
  case AE_IFREG:
    return EntryKind::File;
  case AE_IFDIR:
    return EntryKind::Directory;
  default:
    return EntryKind::Other;
  }
}

} // namespace huxerui::archive::detail
