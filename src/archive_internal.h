#pragma once

#include <huxerui/archive.h>

#include <string>
#include <utility>

namespace huxerui::archive::detail {

inline constexpr std::size_t ChunkSize = 64 * 1024;

inline ArchiveError Error(ArchiveErrorCode code, std::string message) {
  return {code, std::move(message), std::nullopt};
}

inline ArchiveError IoFailure(IoError error) {
  return {ArchiveErrorCode::Io, error.message, std::move(error)};
}

} // namespace huxerui::archive::detail
