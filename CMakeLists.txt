cmake_minimum_required(VERSION 3.24)
project(den VERSION 0.2.0 LANGUAGES C CXX)

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

# --- Portable Ruby (for formula evaluation) ---
# Look for Homebrew's Portable Ruby first, then system Ruby.
set(RUBY_PREFIX "" CACHE PATH "Path to Portable Ruby installation")
if(NOT RUBY_PREFIX)
    # Try Homebrew's vendored Portable Ruby.
    set(_brew_ruby "/opt/homebrew/Library/Homebrew/vendor/portable-ruby/current")
    if(EXISTS "${_brew_ruby}/lib/libruby.4.0-static.a")
        set(RUBY_PREFIX "${_brew_ruby}")
    endif()
endif()

if(RUBY_PREFIX)
    message(STATUS "Using Portable Ruby at: ${RUBY_PREFIX}")
    add_library(ruby STATIC IMPORTED)
    # Find the static library (name varies by version).
    file(GLOB _ruby_static "${RUBY_PREFIX}/lib/libruby.*-static.a")
    if(_ruby_static)
        list(GET _ruby_static 0 _ruby_lib)
        set_target_properties(ruby PROPERTIES IMPORTED_LOCATION "${_ruby_lib}")
    endif()
    # Find include directories.
    file(GLOB _ruby_inc "${RUBY_PREFIX}/include/ruby-*")
    if(_ruby_inc)
        list(GET _ruby_inc 0 _ruby_inc_dir)
        # Ruby has platform-specific headers too.
        file(GLOB _ruby_arch "${_ruby_inc_dir}/*-darwin*" "${_ruby_inc_dir}/*-linux*")
        set(_ruby_includes "${_ruby_inc_dir}")
        if(_ruby_arch)
            list(APPEND _ruby_includes ${_ruby_arch})
        endif()
        target_include_directories(ruby INTERFACE ${_ruby_includes})
    endif()
    set(HAVE_RUBY TRUE)
else()
    message(STATUS "Portable Ruby not found — formula evaluation disabled")
    set(HAVE_RUBY FALSE)
endif()

# --- den library (all modules) ---
file(GLOB_RECURSE DEN_SOURCES src/*.cpp)
# Exclude main.cpp from the library.
list(FILTER DEN_SOURCES EXCLUDE REGEX "main\\.cpp$")
# Exclude Ruby embedding module when Portable Ruby is not available.
if(NOT HAVE_RUBY)
    list(FILTER DEN_SOURCES EXCLUDE REGEX "ruby/")
endif()

add_library(den_lib STATIC ${DEN_SOURCES})
target_include_directories(den_lib PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(den_lib PUBLIC
    vendor_headers
    spdlog::spdlog_header_only
    CURL::libcurl
    LibArchive::LibArchive
)

# Ruby embedding (optional).
if(HAVE_RUBY)
    target_link_libraries(den_lib PUBLIC ruby)
    target_compile_definitions(den_lib PUBLIC DEN_HAVE_RUBY=1)
    if(APPLE)
        target_link_libraries(den_lib PUBLIC "-framework CoreFoundation" "-framework Foundation")
    endif()
endif()

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

file(GLOB_RECURSE TEST_SOURCES tests/*.cpp)
if(TEST_SOURCES)
    add_executable(den_tests ${TEST_SOURCES})
    target_link_libraries(den_tests PRIVATE den_lib vendor_headers)
    target_include_directories(den_tests PRIVATE ${CMAKE_SOURCE_DIR}/vendor/include)
    add_test(NAME den_tests COMMAND den_tests)
endif()

# --- Install ---
install(TARGETS den RUNTIME DESTINATION bin)
