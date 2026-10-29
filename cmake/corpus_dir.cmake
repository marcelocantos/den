# Copyright 2026 Marcelo Cantos
# SPDX-License-Identifier: Apache-2.0
#
# Where the oracle tests find homebrew-core (tests/corpus/homebrew-core is a
# submodule pinned to one commit). A worktree can point DEN_CORPUS_DIR at a
# shared checkout of that commit instead of cloning the submodule again:
#
#   cmake -B build -DDEN_CORPUS_DIR=$HOME/src/den-corpus/homebrew-core
#   DEN_CORPUS_DIR=... cmake -B build          # read at configure time
#
# The -D cache value wins, then the environment, then the submodule. An
# override must hold Formula/ and, when its commit can be read, be checked out
# at the commit the source tree pins; anything else fails the configure,
# since stale formulae make the oracle goldens fail in confusing ways.
#
# Used by CMakeLists.txt and, in script mode, by tests/scripts/test_corpus_dir.sh
# and the corpus_pin ctest (check_corpus.cmake).

set(DEN_CORPUS_SUBMODULE "tests/corpus/homebrew-core")

# The homebrew-core commit `src` pins (its index entry for the gitlink, so a
# staged submodule bump counts), or "" when `src` is not a git checkout.
function(den_corpus_pin out src)
    execute_process(
        COMMAND git -C "${src}" ls-files -s -- "${DEN_CORPUS_SUBMODULE}"
        OUTPUT_VARIABLE listing
        RESULT_VARIABLE rc
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET)
    if(rc EQUAL 0 AND listing MATCHES "^160000 ([0-9a-f]+) ")
        set(${out} "${CMAKE_MATCH_1}" PARENT_SCOPE)
    else()
        set(${out} "" PARENT_SCOPE)
    endif()
endfunction()

# The commit checked out at `dir`, or "" when it is not a git checkout.
function(den_corpus_head out dir)
    execute_process(
        COMMAND git -C "${dir}" rev-parse --verify -q HEAD
        OUTPUT_VARIABLE head
        RESULT_VARIABLE rc
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET)
    if(rc EQUAL 0)
        set(${out} "${head}" PARENT_SCOPE)
    else()
        set(${out} "" PARENT_SCOPE)
    endif()
endfunction()

# Fail (or warn, with `level` WARNING) unless `dir` is a homebrew-core
# checkout at the commit `src` pins. An unreadable pin or head only warns.
function(den_check_corpus dir src level)
    if(NOT IS_DIRECTORY "${dir}/Formula")
        message(${level} "DEN_CORPUS_DIR=${dir} has no Formula/ directory; point it "
                         "at a homebrew-core checkout or unset it to use the "
                         "${DEN_CORPUS_SUBMODULE} submodule.")
        return()
    endif()
    den_corpus_pin(pin "${src}")
    den_corpus_head(head "${dir}")
    if(pin STREQUAL "" OR head STREQUAL "")
        message(WARNING "DEN_CORPUS_DIR=${dir}: cannot compare its commit "
                        "('${head}') with the one ${src} pins ('${pin}'); using it as is.")
        return()
    endif()
    if(NOT pin STREQUAL head)
        message(${level} "DEN_CORPUS_DIR=${dir} is at ${head}, but ${src} pins "
                         "homebrew-core at ${pin}. Update it:\n"
                         "  git -C ${dir} fetch --depth 1 origin ${pin}\n"
                         "  git -C ${dir} checkout --detach FETCH_HEAD")
    endif()
endfunction()

# Resolve the corpus for `src` into `out`: `override` (the -D cache value),
# else $ENV{DEN_CORPUS_DIR}, else the submodule. Overrides are checked with
# den_check_corpus; the submodule is used as is (the tests skip an
# uninitialised one).
function(den_resolve_corpus_dir out src override)
    set(dir "${override}")
    set(origin "DEN_CORPUS_DIR cache variable")
    if(dir STREQUAL "" AND NOT "$ENV{DEN_CORPUS_DIR}" STREQUAL "")
        set(dir "$ENV{DEN_CORPUS_DIR}")
        set(origin "DEN_CORPUS_DIR environment variable")
    endif()
    if(dir STREQUAL "")
        set(${out} "${src}/${DEN_CORPUS_SUBMODULE}" PARENT_SCOPE)
        return()
    endif()
    if(dir MATCHES "^~/")
        string(REGEX REPLACE "^~" "$ENV{HOME}" dir "${dir}")
    endif()
    get_filename_component(dir "${dir}" ABSOLUTE BASE_DIR "${src}")
    message(STATUS "homebrew-core corpus: ${dir} (from the ${origin})")
    den_check_corpus("${dir}" "${src}" FATAL_ERROR)
    set(${out} "${dir}" PARENT_SCOPE)
endfunction()
