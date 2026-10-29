# Copyright 2026 Marcelo Cantos
# SPDX-License-Identifier: Apache-2.0
#
# ctest corpus_pin: fails when the DEN_CORPUS_DIR override is not at the
# commit the source tree pins now (e.g. after merging a submodule bump
# without updating the shared checkout).
#
#   cmake -DCORPUS=<dir> -DSRC=<source dir> -P cmake/check_corpus.cmake

include("${CMAKE_CURRENT_LIST_DIR}/corpus_dir.cmake")
den_check_corpus("${CORPUS}" "${SRC}" FATAL_ERROR)
message(STATUS "DEN_CORPUS_DIR=${CORPUS} matches the commit ${SRC} pins")
