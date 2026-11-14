include(FetchContent)

if (POLICY CMP0135)
    cmake_policy(SET CMP0135 NEW)
endif ()

FetchContent_Declare(huxerui_archive_zlib
    URL https://codeload.github.com/madler/zlib/tar.gz/refs/tags/v1.4
    URL_HASH SHA256=4331355048E286908260FDBEAA0B8CF75CEAF2907C75855A093A6487C7032AAA
)
FetchContent_Declare(huxerui_archive_zstd
    URL https://codeload.github.com/facebook/zstd/tar.gz/refs/tags/v1.5.7
    URL_HASH SHA256=37D7284556B20954E56E1CA85B80226768902E2EDABD3B649E9E72C0C9012EE3
    SOURCE_SUBDIR build/cmake
)
FetchContent_Declare(huxerui_archive_libarchive
    URL https://codeload.github.com/libarchive/libarchive/tar.gz/refs/tags/v3.8.11
    URL_HASH SHA256=6E9560B2E0659FF688C5EFB7717A68BB06B28EF65A69ADDCD6C424EAF9C319B1
)

function(huxerui_archive_add_dependencies)
    if (POLICY CMP0126)
        set(CMAKE_POLICY_DEFAULT_CMP0126 NEW)
    endif ()
    set(ZLIB_BUILD_SHARED OFF)
    set(ZLIB_BUILD_TESTING OFF)
    set(ZLIB_INSTALL OFF)
    set(ZSTD_BUILD_SHARED OFF)
    set(ZSTD_BUILD_PROGRAMS OFF)
    set(ZSTD_BUILD_TESTS OFF)
    set(ZSTD_MULTITHREAD_SUPPORT OFF)
    set(ZSTD_LEGACY_SUPPORT OFF)
    set(ZSTD_BUILD_DICTBUILDER OFF)
    set(CMAKE_POSITION_INDEPENDENT_CODE ON)
    set(CMAKE_POLICY_DEFAULT_CMP0077 NEW)
    FetchContent_MakeAvailable(huxerui_archive_zlib huxerui_archive_zstd)

    # libarchive's checks expect installed codec libraries. Supply the source-built
    # targets and the features guaranteed by their pinned versions.
    set(ZLIB_INCLUDE_DIR "${huxerui_archive_zlib_SOURCE_DIR}")
    set(ZLIB_LIBRARY zlibstatic)
    set(ZLIB_LIBRARIES zlibstatic)
    set(ZSTD_INCLUDE_DIR "${huxerui_archive_zstd_SOURCE_DIR}/lib")
    set(ZSTD_LIBRARY libzstd_static)
    set(HAVE_LIBZSTD ON)
    set(HAVE_ZSTD_compressStream ON)
    set(HAVE_ZSTD_minCLevel ON)
    set(BUILD_SHARED_LIBS OFF)
    set(BUILD_TESTING OFF)
    set(ENABLE_WERROR OFF)
    set(ENABLE_ZLIB ON)
    set(ENABLE_ZSTD ON)
    set(POSIX_REGEX_LIB NONE)
    foreach(feature IN ITEMS OPENSSL MD LIBB2 LZ4 LZO LZMA BZip2 LIBXML2 BSDXML EXPAT
            WIN32_XMLLITE PCREPOSIX PCRE2POSIX LIBGCC CNG TAR CPIO CAT UNZIP XATTR ACL
            ICONV TEST INSTALL)
        set(ENABLE_${feature} OFF)
    endforeach ()
    FetchContent_MakeAvailable(huxerui_archive_libarchive)
endfunction()

huxerui_archive_add_dependencies()
set(archive_dependency_targets archive_static zlibstatic libzstd_static)
target_link_libraries(huxerui_archive PRIVATE ${archive_dependency_targets})
if (IOS)
    # The SDK assembles the iOS core from its registered static library inputs.
    set_property(TARGET huxerui_archive APPEND PROPERTY HUXERUI_LIBRARIES ${archive_dependency_targets})
    set_property(TARGET huxerui_archive APPEND PROPERTY HUXERUI_REQUESTED_LIBRARY_TARGETS ${archive_dependency_targets})
endif ()
target_compile_definitions(huxerui_archive PRIVATE LIBARCHIVE_STATIC)
if (WIN32)
    target_compile_definitions(huxerui_archive PRIVATE NOMINMAX)
endif ()
