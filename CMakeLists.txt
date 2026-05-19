cmake_minimum_required(VERSION 3.24)
project(den VERSION 0.11.0 LANGUAGES C CXX)

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
find_package(LibArchive REQUIRED)

# --- den library (all modules) ---
file(GLOB_RECURSE DEN_SOURCES src/*.cpp)

# Add the embedded Ruby bundle data (C source). macOS only — Linux
# lazy-downloads Portable Ruby on first source build (🎯T47), so it has
# no need for the embedded bundle and we keep it out of the binary.
if(APPLE AND EXISTS "${CMAKE_SOURCE_DIR}/src/ruby/bundle_data.c")
    list(APPEND DEN_SOURCES "${CMAKE_SOURCE_DIR}/src/ruby/bundle_data.c")
endif()

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

# Only pick up top-level tests/*.cpp, not anything that might land under
# tests/corpus/** (e.g. the homebrew-core submodule). Use a non-recursive
# glob so the submodule's contents are ignored even if it grows .cpp
# files in the future.
file(GLOB TEST_SOURCES tests/*.cpp)
if(TEST_SOURCES)
    add_executable(den_tests ${TEST_SOURCES})
    target_link_libraries(den_tests PRIVATE den_lib vendor_headers)
    target_include_directories(den_tests PRIVATE ${CMAKE_SOURCE_DIR}/vendor/include)
    # Oracle tests (🎯T18) read formula Ruby source from the homebrew-core
    # submodule. Pass the path as a compile-time constant so the tests can
    # locate the corpus and skip gracefully if it isn't initialised.
    target_compile_definitions(den_tests PRIVATE
        DEN_CORPUS_DIR="${CMAKE_SOURCE_DIR}/tests/corpus/homebrew-core")
    add_test(NAME den_tests COMMAND den_tests)
endif()

# --- Install ---
install(TARGETS den RUNTIME DESTINATION bin)
