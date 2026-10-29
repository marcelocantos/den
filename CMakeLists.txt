cmake_minimum_required(VERSION 3.24)

# Release binaries target macOS 14 (Sonoma) and later. Must be set before
# project(); ignored on other platforms.
set(CMAKE_OSX_DEPLOYMENT_TARGET "14.0" CACHE STRING "Minimum macOS version")

project(den VERSION 0.15.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Parallel builds.
include(ProcessorCount)
ProcessorCount(NPROC)
if(NPROC EQUAL 0)
    set(NPROC 4)
endif()
set(CMAKE_BUILD_PARALLEL_LEVEL ${NPROC} CACHE STRING "")

# --- Vendored header-only dependencies ---
add_library(vendor_headers INTERFACE)
target_include_directories(vendor_headers INTERFACE
    ${CMAKE_SOURCE_DIR}/vendor/include
)

# spdlog (header-only mode).
set(SPDLOG_FMT_EXTERNAL OFF CACHE BOOL "" FORCE)
add_subdirectory(vendor/github.com/gabime/spdlog EXCLUDE_FROM_ALL)

# --- System dependencies ---
find_package(CURL REQUIRED)

# libarchive: vendored and statically linked by default so release binaries
# need nothing from a package manager (see cmake/VendorLibarchive.cmake).
# -DDEN_VENDOR_LIBARCHIVE=OFF falls back to a system libarchive.
option(DEN_VENDOR_LIBARCHIVE "Build libarchive (+xz/zstd/lz4) from pinned sources and link statically" ON)
if(DEN_VENDOR_LIBARCHIVE)
    include(${CMAKE_SOURCE_DIR}/cmake/VendorLibarchive.cmake)
else()
    find_package(LibArchive REQUIRED)
endif()

# --- den library (all modules) ---
file(GLOB_RECURSE DEN_SOURCES src/*.cpp)

# Add the embedded Ruby bundle data (C source). macOS only — Linux
# lazy-downloads Portable Ruby on first source build (🎯T47), so it has
# no need for the embedded bundle and we keep it out of the binary.
if(APPLE AND EXISTS "${CMAKE_SOURCE_DIR}/src/ruby/bundle_data.c")
    list(APPEND DEN_SOURCES "${CMAKE_SOURCE_DIR}/src/ruby/bundle_data.c")
endif()

# 🎯T42/T64: compile the trust replica snapshot into the binary as the
# last-resort fallback, so a bare `den` (release tarball, install.sh,
# self-update) can still cross-check bottle hashes. configure_file re-runs
# whenever data/known_hashes.json changes.
file(READ "${CMAKE_SOURCE_DIR}/data/known_hashes.json" DEN_REPLICA_JSON)
string(FIND "${DEN_REPLICA_JSON}" ")den_replica\"" _den_replica_delim)
if(NOT _den_replica_delim EQUAL -1)
    message(FATAL_ERROR "data/known_hashes.json contains the raw-string delimiter")
endif()
configure_file(
    "${CMAKE_SOURCE_DIR}/cmake/embedded_replica.cpp.in"
    "${CMAKE_BINARY_DIR}/generated/embedded_replica.cpp"
    @ONLY
)
list(APPEND DEN_SOURCES "${CMAKE_BINARY_DIR}/generated/embedded_replica.cpp")

# Compile src/ruby/den_build.rb into the binary so the unpacked Ruby bundle
# always runs the current script (the bundle's own copy can be stale).
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${CMAKE_SOURCE_DIR}/src/ruby/den_build.rb")
file(READ "${CMAKE_SOURCE_DIR}/src/ruby/den_build.rb" DEN_BUILD_RB)
string(FIND "${DEN_BUILD_RB}" ")den_build\"" _den_build_delim)
if(NOT _den_build_delim EQUAL -1)
    message(FATAL_ERROR "src/ruby/den_build.rb contains the raw-string delimiter")
endif()
configure_file(
    "${CMAKE_SOURCE_DIR}/cmake/embedded_den_build.cpp.in"
    "${CMAKE_BINARY_DIR}/generated/embedded_den_build.cpp"
    @ONLY
)
list(APPEND DEN_SOURCES "${CMAKE_BINARY_DIR}/generated/embedded_den_build.cpp")

add_library(den_lib STATIC ${DEN_SOURCES})
target_include_directories(den_lib PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(den_lib PUBLIC
    vendor_headers
    spdlog::spdlog_header_only
    CURL::libcurl
    LibArchive::LibArchive
)

# Platform-specific libraries.
if(APPLE)
    # macOS frameworks for cask/app handling.
elseif(UNIX)
    # Linux: may need pthread explicitly.
    find_package(Threads REQUIRED)
    target_link_libraries(den_lib PUBLIC Threads::Threads)
    # Don't depend on the host's libstdc++/libgcc_s versions.
    target_link_options(den_lib PUBLIC -static-libstdc++ -static-libgcc)
endif()
if(DEN_VENDOR_LIBARCHIVE)
    add_dependencies(den_lib den_vendor_libarchive)
endif()

# Version injection.
target_compile_definitions(den_lib PUBLIC
    DEN_VERSION="${PROJECT_VERSION}"
    DEN_VERSION_MAJOR=${PROJECT_VERSION_MAJOR}
    DEN_VERSION_MINOR=${PROJECT_VERSION_MINOR}
    DEN_VERSION_PATCH=${PROJECT_VERSION_PATCH}
)

# --- den binary ---
add_executable(den src/main.cpp)
target_link_libraries(den PRIVATE den_lib)

# --- Tests ---
enable_testing()

# The built binary must report exactly the CMake project version, so a
# release cut from this tree can never ship a stale `den --version`.
add_test(
    NAME den_version_matches_cmake
    COMMAND sh "${CMAKE_SOURCE_DIR}/scripts/check-version.sh"
            --binary "$<TARGET_FILE:den>" "${PROJECT_VERSION}"
)

# Only pick up top-level tests/*.cpp, not anything that might land under
# tests/corpus/** (e.g. the homebrew-core submodule). Use a non-recursive
# glob so the submodule's contents are ignored even if it grows .cpp
# files in the future.
file(GLOB TEST_SOURCES tests/*.cpp)

# homebrew-core for the oracle tests: the submodule, or a shared checkout of
# the same commit named by DEN_CORPUS_DIR (see cmake/corpus_dir.cmake).
include(cmake/corpus_dir.cmake)
set(DEN_CORPUS_DIR "" CACHE PATH
    "Shared homebrew-core checkout for the tests (default: $ENV{DEN_CORPUS_DIR}, else the tests/corpus/homebrew-core submodule)")
den_resolve_corpus_dir(DEN_CORPUS_DIR_RESOLVED "${CMAKE_SOURCE_DIR}" "${DEN_CORPUS_DIR}")

if(TEST_SOURCES)
    add_executable(den_tests ${TEST_SOURCES})
    target_link_libraries(den_tests PRIVATE den_lib vendor_headers)
    target_include_directories(den_tests PRIVATE ${CMAKE_SOURCE_DIR}/vendor/include)
    # Oracle tests (🎯T18) read formula Ruby source from homebrew-core.
    # Pass the path as a compile-time constant so the tests can locate the
    # corpus and skip gracefully if it isn't initialised. In-repo fixtures
    # (tests/corpus/*.txt, golden/, ...) are found via DEN_SOURCE_DIR, never
    # relative to DEN_CORPUS_DIR, which may live outside the tree.
    target_compile_definitions(den_tests PRIVATE
        DEN_CORPUS_DIR="${DEN_CORPUS_DIR_RESOLVED}"
        DEN_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
    add_test(NAME den_tests COMMAND den_tests)
endif()

# A shared corpus must track the submodule pin as it moves (merges re-pin it
# without re-running cmake).
if(NOT DEN_CORPUS_DIR_RESOLVED STREQUAL "${CMAKE_SOURCE_DIR}/tests/corpus/homebrew-core")
    add_test(NAME corpus_pin
        COMMAND ${CMAKE_COMMAND} -DCORPUS=${DEN_CORPUS_DIR_RESOLVED}
                -DSRC=${CMAKE_SOURCE_DIR} -P ${CMAKE_SOURCE_DIR}/cmake/check_corpus.cmake)
endif()

# Harness helper regression: installed_pkg_runs must reject wrong-arch /
# loader-error binaries and still accept a real host-native binary.
add_test(
    NAME harness_installed_pkg_runs
    COMMAND sh "${CMAKE_SOURCE_DIR}/tests/harness/test_installed_pkg_runs.sh"
)

# macOS harness sandbox helper: toolchain caches/TMPDIR/Go telemetry are kept
# inside the sandbox, and cleanup handles read-only trees and fails loudly.
add_test(
    NAME harness_macos_sandbox
    COMMAND sh "${CMAKE_SOURCE_DIR}/tests/harness/macos/test_sandbox.sh"
)

# Shell-script regression tests for the release tooling under scripts/.
# Each tests/scripts/test_<name>.sh is registered as script_test_<name>.
file(GLOB SCRIPT_TESTS ${CMAKE_SOURCE_DIR}/tests/scripts/test_*.sh)
foreach(script_test ${SCRIPT_TESTS})
    get_filename_component(script_test_name "${script_test}" NAME_WE)
    add_test(NAME script_${script_test_name} COMMAND sh "${script_test}")
    # Lets test_install_sh.sh exercise the real den's config.json handling.
    set_tests_properties(script_${script_test_name} PROPERTIES
        ENVIRONMENT "DEN_TEST_BIN=$<TARGET_FILE:den>")
endforeach()

# --- Install ---
install(TARGETS den RUNTIME DESTINATION bin)

# 🎯T42/T64: ship the independent trust replica snapshot alongside the binary
# so `den install` and `den doctor` can cross-check bottle hashes offline. The
# runtime resolves it via <exe_dir>/../share/den/known_hashes.json (see
# locate_local_replica); the copy compiled into the binary is the fallback.
install(FILES ${CMAKE_SOURCE_DIR}/data/known_hashes.json DESTINATION share/den)
