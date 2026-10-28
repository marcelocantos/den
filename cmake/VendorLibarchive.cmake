# Vendored, statically linked libarchive (plus its xz/zstd/lz4 codecs).
#
# Release binaries must be self-contained: nothing from a package manager
# prefix (e.g. /opt/homebrew) may appear in the binary's load commands.
# We therefore build libarchive and the compression libraries it needs from
# pinned upstream release tarballs and link them statically.
#
# Kept from the system (always present on the target OS):
#   macOS: zlib, bzip2, iconv, expat (xar), CommonCrypto (libSystem)
#   Linux: glibc only; zlib and bzip2 are linked from their static archives.
#
# Deliberately disabled: OpenSSL, mbedTLS, Nettle, libb2, libxml2, LZO,
# PCRE, ACLs (Linux), xattrs (Linux) and all libarchive tools/tests.
#
# Bumping a version: update the *_VERSION / *_URL / *_SHA256 triple below.
# Hashes are of the upstream release asset at that URL.

include(ExternalProject)

set(DEN_XZ_VERSION 5.8.4)
set(DEN_XZ_URL
    "https://github.com/tukaani-project/xz/releases/download/v${DEN_XZ_VERSION}/xz-${DEN_XZ_VERSION}.tar.gz")
set(DEN_XZ_SHA256 0014c7886930454fe8bd4228665b51af55eeae560ea135c9c4cd33f55b2591d9)

set(DEN_ZSTD_VERSION 1.5.7)
set(DEN_ZSTD_URL
    "https://github.com/facebook/zstd/releases/download/v${DEN_ZSTD_VERSION}/zstd-${DEN_ZSTD_VERSION}.tar.gz")
set(DEN_ZSTD_SHA256 eb33e51f49a15e023950cd7825ca74a4a2b43db8354825ac24fc1b7ee09e6fa3)

set(DEN_LZ4_VERSION 1.10.0)
set(DEN_LZ4_URL
    "https://github.com/lz4/lz4/releases/download/v${DEN_LZ4_VERSION}/lz4-${DEN_LZ4_VERSION}.tar.gz")
set(DEN_LZ4_SHA256 537512904744b35e232912055ccf8ec66d768639ff3abe5788d90d792ec5f48b)

set(DEN_LIBARCHIVE_VERSION 3.8.10)
set(DEN_LIBARCHIVE_URL
    "https://github.com/libarchive/libarchive/releases/download/v${DEN_LIBARCHIVE_VERSION}/libarchive-${DEN_LIBARCHIVE_VERSION}.tar.xz")
set(DEN_LIBARCHIVE_SHA256 56cad765459d5ee25a3b761f90c47d99379d28297fbfb72d908880d6b875fb96)

set(DEN_VENDOR_PREFIX "${CMAKE_BINARY_DIR}/_vendor/install")
set(DEN_VENDOR_LIBDIR "${DEN_VENDOR_PREFIX}/lib")
set(DEN_VENDOR_INCDIR "${DEN_VENDOR_PREFIX}/include")
file(MAKE_DIRECTORY "${DEN_VENDOR_INCDIR}")

if(CMAKE_BUILD_TYPE)
    set(_den_vendor_build_type "${CMAKE_BUILD_TYPE}")
else()
    set(_den_vendor_build_type Release)
endif()

# Common arguments for every vendored sub-build. The ignore list keeps a
# package manager's copies of zlib/bzip2/iconv/etc. from being picked up.
set(_den_vendor_cmake_args
    -DCMAKE_BUILD_TYPE=${_den_vendor_build_type}
    -DCMAKE_INSTALL_PREFIX=${DEN_VENDOR_PREFIX}
    -DCMAKE_INSTALL_LIBDIR=lib
    -DCMAKE_PREFIX_PATH=${DEN_VENDOR_PREFIX}
    "-DCMAKE_IGNORE_PREFIX_PATH=/opt/homebrew$<SEMICOLON>/usr/local$<SEMICOLON>/opt/local$<SEMICOLON>/home/linuxbrew/.linuxbrew"
    -DCMAKE_FIND_FRAMEWORK=LAST
    -DCMAKE_POSITION_INDEPENDENT_CODE=ON
    -DCMAKE_POLICY_VERSION_MINIMUM=3.5
    -DBUILD_SHARED_LIBS=OFF
    -DBUILD_TESTING=OFF
    -DCMAKE_C_COMPILER=${CMAKE_C_COMPILER}
)
if(CMAKE_C_COMPILER_LAUNCHER)
    list(APPEND _den_vendor_cmake_args -DCMAKE_C_COMPILER_LAUNCHER=${CMAKE_C_COMPILER_LAUNCHER})
endif()
if(APPLE)
    list(APPEND _den_vendor_cmake_args
        -DCMAKE_OSX_DEPLOYMENT_TARGET=${CMAKE_OSX_DEPLOYMENT_TARGET}
        "-DCMAKE_OSX_ARCHITECTURES=${CMAKE_OSX_ARCHITECTURES}"
    )
    if(CMAKE_OSX_SYSROOT)
        list(APPEND _den_vendor_cmake_args -DCMAKE_OSX_SYSROOT=${CMAKE_OSX_SYSROOT})
    endif()
endif()

set(_den_lib_prefix "${CMAKE_STATIC_LIBRARY_PREFIX}")
set(_den_lib_suffix "${CMAKE_STATIC_LIBRARY_SUFFIX}")
set(DEN_LZMA_LIB "${DEN_VENDOR_LIBDIR}/${_den_lib_prefix}lzma${_den_lib_suffix}")
set(DEN_ZSTD_LIB "${DEN_VENDOR_LIBDIR}/${_den_lib_prefix}zstd${_den_lib_suffix}")
set(DEN_LZ4_LIB "${DEN_VENDOR_LIBDIR}/${_den_lib_prefix}lz4${_den_lib_suffix}")
set(DEN_ARCHIVE_LIB "${DEN_VENDOR_LIBDIR}/${_den_lib_prefix}archive${_den_lib_suffix}")

ExternalProject_Add(den_vendor_xz
    URL "${DEN_XZ_URL}"
    URL_HASH SHA256=${DEN_XZ_SHA256}
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    PREFIX "${CMAKE_BINARY_DIR}/_vendor/xz"
    CMAKE_ARGS ${_den_vendor_cmake_args}
        -DXZ_NLS=OFF
        -DXZ_DOC=OFF
        -DXZ_TOOL_XZ=OFF
        -DXZ_TOOL_XZDEC=OFF
        -DXZ_TOOL_LZMADEC=OFF
        -DXZ_TOOL_LZMAINFO=OFF
        -DXZ_TOOL_SCRIPTS=OFF
    BUILD_BYPRODUCTS "${DEN_LZMA_LIB}"
)

ExternalProject_Add(den_vendor_zstd
    URL "${DEN_ZSTD_URL}"
    URL_HASH SHA256=${DEN_ZSTD_SHA256}
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    PREFIX "${CMAKE_BINARY_DIR}/_vendor/zstd"
    SOURCE_SUBDIR build/cmake
    CMAKE_ARGS ${_den_vendor_cmake_args}
        -DZSTD_BUILD_SHARED=OFF
        -DZSTD_BUILD_STATIC=ON
        -DZSTD_BUILD_PROGRAMS=OFF
        -DZSTD_BUILD_TESTS=OFF
        -DZSTD_BUILD_CONTRIB=OFF
        -DZSTD_MULTITHREAD_SUPPORT=OFF
    BUILD_BYPRODUCTS "${DEN_ZSTD_LIB}"
)

ExternalProject_Add(den_vendor_lz4
    URL "${DEN_LZ4_URL}"
    URL_HASH SHA256=${DEN_LZ4_SHA256}
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    PREFIX "${CMAKE_BINARY_DIR}/_vendor/lz4"
    SOURCE_SUBDIR build/cmake
    CMAKE_ARGS ${_den_vendor_cmake_args}
        -DBUILD_STATIC_LIBS=ON
        -DLZ4_BUILD_CLI=OFF
    BUILD_BYPRODUCTS "${DEN_LZ4_LIB}"
)

set(_den_libarchive_args
    -DENABLE_TAR=OFF
    -DENABLE_CPIO=OFF
    -DENABLE_CAT=OFF
    -DENABLE_UNZIP=OFF
    -DENABLE_TEST=OFF
    -DENABLE_COVERAGE=OFF
    -DENABLE_WERROR=OFF
    -DENABLE_INSTALL=ON
    # Formats/filters den relies on: tar(.gz/.bz2/.xz/.zst/.lz4) and zip.
    -DENABLE_ZLIB=ON
    -DENABLE_BZip2=ON
    -DENABLE_LZMA=ON
    -DENABLE_ZSTD=ON
    -DENABLE_LZ4=ON
    # Optional extras we do not want to drag in.
    -DENABLE_OPENSSL=OFF
    -DENABLE_MBEDTLS=OFF
    -DENABLE_NETTLE=OFF
    -DENABLE_CNG=OFF
    -DENABLE_LIBB2=OFF
    -DENABLE_LZO=OFF
    -DENABLE_LIBXML2=OFF
    -DENABLE_BSDXML=OFF
    -DENABLE_PCREPOSIX=OFF
    -DENABLE_PCRE2POSIX=OFF
    -DENABLE_LIBGCC=OFF
    -DENABLE_ICONV=ON
)
if(APPLE)
    # All of these live in libSystem / /usr/lib on macOS.
    list(APPEND _den_libarchive_args -DENABLE_EXPAT=ON -DENABLE_ACL=ON -DENABLE_XATTR=ON)
else()
    # Keep the binary's NEEDED list to glibc + libcurl: no libexpat, libacl,
    # libattr, libmd. zlib and bzip2 come from their static archives.
    list(APPEND _den_libarchive_args
        -DENABLE_EXPAT=OFF -DENABLE_ACL=OFF -DENABLE_XATTR=OFF -DENABLE_MD=OFF)
    # Prefer static zlib/bzip2 so they don't show up in NEEDED.
    find_library(DEN_STATIC_ZLIB NAMES libz.a REQUIRED)
    find_library(DEN_STATIC_BZ2 NAMES libbz2.a REQUIRED)
    list(APPEND _den_libarchive_args
        -DZLIB_LIBRARY=${DEN_STATIC_ZLIB}
        -DZLIB_LIBRARY_RELEASE=${DEN_STATIC_ZLIB}
        -DBZIP2_LIBRARIES=${DEN_STATIC_BZ2}
        -DBZIP2_LIBRARY_RELEASE=${DEN_STATIC_BZ2})
endif()
# Pin the codec libraries to the vendored static archives (never a system
# or package-manager copy).
list(APPEND _den_libarchive_args
    -DLIBLZMA_LIBRARY=${DEN_LZMA_LIB}
    -DLIBLZMA_LIBRARY_RELEASE=${DEN_LZMA_LIB}
    -DLIBLZMA_INCLUDE_DIR=${DEN_VENDOR_INCDIR}
    -DZSTD_LIBRARY=${DEN_ZSTD_LIB}
    -DZSTD_INCLUDE_DIR=${DEN_VENDOR_INCDIR}
    -DLZ4_LIBRARY=${DEN_LZ4_LIB}
    -DLZ4_INCLUDE_DIR=${DEN_VENDOR_INCDIR})

ExternalProject_Add(den_vendor_libarchive
    URL "${DEN_LIBARCHIVE_URL}"
    URL_HASH SHA256=${DEN_LIBARCHIVE_SHA256}
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    PREFIX "${CMAKE_BINARY_DIR}/_vendor/libarchive"
    DEPENDS den_vendor_xz den_vendor_zstd den_vendor_lz4
    CMAKE_ARGS ${_den_vendor_cmake_args} ${_den_libarchive_args}
    BUILD_BYPRODUCTS "${DEN_ARCHIVE_LIB}"
)

# System libraries the static libarchive needs at link time.
if(APPLE)
    # Resolve these against the SDK, never a package manager's keg.
    set(_den_saved_ignore "${CMAKE_IGNORE_PREFIX_PATH}")
    list(APPEND CMAKE_IGNORE_PREFIX_PATH /opt/homebrew /usr/local /opt/local)
    find_package(ZLIB REQUIRED)
    find_package(BZip2 REQUIRED)
    find_package(Iconv REQUIRED)
    find_package(EXPAT REQUIRED)
    set(CMAKE_IGNORE_PREFIX_PATH "${_den_saved_ignore}")
    set(_den_archive_sys_libs ZLIB::ZLIB BZip2::BZip2 Iconv::Iconv EXPAT::EXPAT)
else()
    set(_den_archive_sys_libs ${DEN_STATIC_ZLIB} ${DEN_STATIC_BZ2})
endif()

add_library(den_vendored_archive INTERFACE)
add_dependencies(den_vendored_archive den_vendor_libarchive)
target_include_directories(den_vendored_archive INTERFACE "${DEN_VENDOR_INCDIR}")
target_link_libraries(den_vendored_archive INTERFACE
    "${DEN_ARCHIVE_LIB}"
    "${DEN_LZMA_LIB}"
    "${DEN_ZSTD_LIB}"
    "${DEN_LZ4_LIB}"
    ${_den_archive_sys_libs}
)
add_library(LibArchive::LibArchive ALIAS den_vendored_archive)

message(STATUS "den: vendoring libarchive ${DEN_LIBARCHIVE_VERSION} "
    "(xz ${DEN_XZ_VERSION}, zstd ${DEN_ZSTD_VERSION}, lz4 ${DEN_LZ4_VERSION}), static")
