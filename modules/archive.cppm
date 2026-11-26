// Hand-maintained module interface (`cpp_modules_gen = false` under [package.metadata.huxerui]).
// HuxerUI's `strip_noncode()` misparses the `100'000` digit separator in include/huxerui/archive.h:64
// as a character literal, omitting ArchiveReader from auto-generation.
//
// `import huxerui.archive;` and including <huxerui/archive.h> reach the same 16 public entities.

module;

#include <typeinfo>
#include <huxerui/archive.h>

export module huxerui.archive;

export import huxerui;

export namespace huxerui::archive {
    using huxerui::archive::ArchiveEntry;
    using huxerui::archive::ArchiveError;
    using huxerui::archive::ArchiveErrorCode;
    using huxerui::archive::ArchiveFormat;
    using huxerui::archive::ArchiveReadOptions;
    using huxerui::archive::ArchiveReader;
    using huxerui::archive::ArchiveResult;
    using huxerui::archive::ArchiveWriter;
    using huxerui::archive::CompressStream;
    using huxerui::archive::CompressStreamAsync;
    using huxerui::archive::CompressionFormat;
    using huxerui::archive::CreateArchive;
    using huxerui::archive::DecompressStream;
    using huxerui::archive::DecompressStreamAsync;
    using huxerui::archive::EntryKind;
    using huxerui::archive::ExtractArchive;
}
