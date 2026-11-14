# Lib-Archive

Lib-Archive provides C++20 archive and stream compression APIs for HuxerUI applications.
Create archives from files, directories, or in-memory data. List entries, read individual
files into memory, or extract them to disk.

## Formats

| Format | Operations |
| --- | --- |
| ZIP (`.zip`), TAR (`.tar`), compressed TAR (`.tar.gz`, `.tar.zst`) | Create, list, read an entry, extract an entry, or extract the full archive |
| gzip (`.gz`), Zstandard (`.zst`) | Compress or decompress a byte stream using synchronous or asynchronous HuxerUI streams |

## Installation

Use the HuxerUI SDK, CMake 3.20 or newer, and a toolchain with C and C++20 support.
Add one of the following `huxerui_use_library()` calls after `huxerui_add_app()` in
your application's `CMakeLists.txt`. Replace `my_app` with your application target.

### Pin a commit

Set `LIB_ARCHIVE_COMMIT` to the full 40-character SHA of the Lib-Archive revision you want to build.

```cmake
huxerui_use_library(my_app
    TARGET HuxerUI::Archive
    URL "https://github.com/HuxerUI/Lib-Archive.git"
    COMMIT "${LIB_ARCHIVE_COMMIT}"
)
```

### Pin a tag

Use `v0.1.0` to pin the first Lib-Archive release.

```cmake
huxerui_use_library(my_app
    TARGET HuxerUI::Archive
    URL "https://github.com/HuxerUI/Lib-Archive.git"
    TAG "v0.1.0"
)
```

### Use a local checkout

```cmake
huxerui_use_library(my_app
    TARGET HuxerUI::Archive
    PATH "${CMAKE_CURRENT_SOURCE_DIR}/../Lib-Archive"
)
```

Include `<huxerui/archive.h>` to use the `huxerui::archive` namespace. The first
configure downloads the pinned libarchive, zlib, and Zstandard sources.

## Quick start

Package a file or directory as a ZIP. `CreateArchive()` stores the source under
its original name. Pass an `ArchiveFormat` as the third argument to select
another format.

```cpp
#include <huxerui/archive.h>

using namespace huxerui;

auto PackAssets() {
  return archive::CreateArchive(File("assets"), File("assets.zip"));
}
```

Extract an archive into an existing directory. `ExtractArchive()` recognizes
the format from the file contents.

```cpp
auto UnpackAssets() {
  return archive::ExtractArchive(File("assets.zip"), File("output"));
}
```

Check `Succeeded()` before accessing `Value()` or `Error()` on an `ArchiveResult`.

## Read and write entries

Open a local `File` with `ArchiveReader::Open()` to detect the archive format. Use
`ListEntries()` to obtain the paths accepted by `ReadEntry()` and `ExtractEntry()`.
`ReadEntry(path)` returns the decompressed contents as `Bytes`, within the same
output limit as extraction. Paths match case-sensitively. Both operations report
`DuplicateEntryPath` when multiple entries match, before reading or writing the
selected entry. Compressed TAR may require a full scan followed by a second pass.

Use `ArchiveWriter::Create(destination, format)` to choose the entries in the archive.
Add in-memory content with `AddEntry("config/settings.json", bytes)`, local files and
directory trees with `AddPath()`, or an explicit directory with `AddDirectory()`.
Check `Finish()` to ensure the archive trailer and output have been finalized.

## Extraction rules and limits

Extraction preserves existing files. It rejects archive links, special files, unsafe
paths, and links encountered below the extraction root. The caller controls the
destination tree for the duration of the operation. Completed files remain if a
later entry fails.

`ArchiveReadOptions` defaults to 100,000 entries per traversal and 1 GiB of actual
decompressed output per call. Whole-archive extraction shares the byte limit
across all files. Pass custom options to `ArchiveReader::Open()` or
`ExtractArchive()` to change either limit. `ReadEntry()` holds the full output in memory.

## Stream compression

Use `CompressStream()` and `DecompressStream()` with synchronous HuxerUI streams.
Use `CompressStreamAsync()` and `DecompressStreamAsync()` with asynchronous streams.
Select gzip or Zstandard with `CompressionFormat`. Each operation takes ownership
of both streams and closes the destination on success. The result contains the
number of bytes written. The stream decompression functions default to a 256 MiB
output limit.

## Web

Use HuxerUI's `FileReference::OpenReadAsync()` and application temporary storage for
archives picked in a browser. The preview copies the input in chunks and caps it at
64 MiB before calling the synchronous archive API. The complete temporary file still
occupies browser memory. Archive creation and extraction can also target temporary
storage, then use `FilePicker::SaveFileAsync()` where available.

## Build and test

With a configured HuxerUI SDK, configure the library and run CTest on a desktop build.

```sh
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

The [preview application](examples/preview) opens and lists an archive, saves a
selected entry, and creates a sample ZIP from in-memory data.

## Third-party dependencies

The downloaded sources retain their upstream notices.
[libarchive 3.8.11](https://github.com/libarchive/libarchive/blob/v3.8.11/COPYING),
[zlib 1.4](https://github.com/madler/zlib/blob/v1.4/LICENSE), and
[Zstandard 1.5.7](https://github.com/facebook/zstd/blob/v1.5.7/LICENSE) are linked
as private dependencies.
