# den_vendor_fetch_retry(<ep-target> URL <url> SHA256 <hex> DOWNLOAD_NAME <file>)
#
# Adds a `fetch_retry` step to an existing URL-based ExternalProject, between
# its `mkdir` and `download` steps. The step runs cmake/den_fetch_retry.cmake
# (bounded retries on transient transport errors and on final HTTP 408/429/
# 5xx) to place the archive, hash-verified, at <DOWNLOAD_DIR>/<file>. The
# stock download step then sees a file with a matching URL_HASH and skips its
# own download; hash verification and extraction are unchanged.
#
# Because it is a build-time step of each ExternalProject, the vendored
# downloads still run in parallel at build time, as before. Pass the same
# URL, hash and DOWNLOAD_NAME the ExternalProject_Add call uses.
#
# DEN_VENDOR_FETCH_RETRY=OFF drops the step (stock single-attempt download).
option(DEN_VENDOR_FETCH_RETRY
    "Retry transient failures when downloading vendored source archives" ON)
set(_den_fetch_retry_script "${CMAKE_CURRENT_LIST_DIR}/den_fetch_retry.cmake")

function(den_vendor_fetch_retry target)
    cmake_parse_arguments(PARSE_ARGV 1 arg "" "URL;SHA256;DOWNLOAD_NAME" "")
    if(NOT DEN_VENDOR_FETCH_RETRY)
        return()
    endif()
    foreach(_req URL SHA256 DOWNLOAD_NAME)
        if(NOT arg_${_req})
            message(FATAL_ERROR "den_vendor_fetch_retry(${target}): ${_req} is required")
        endif()
    endforeach()
    ExternalProject_Get_Property(${target} DOWNLOAD_DIR)
    ExternalProject_Add_Step(${target} fetch_retry
        COMMENT "Fetching ${arg_DOWNLOAD_NAME} (bounded retries) for '${target}'"
        COMMAND "${CMAKE_COMMAND}"
            "-DURL=${arg_URL}"
            "-DSHA256=${arg_SHA256}"
            "-DDEST=${DOWNLOAD_DIR}/${arg_DOWNLOAD_NAME}"
            -P "${_den_fetch_retry_script}"
        DEPENDEES mkdir
        DEPENDERS download
        # The stock download step is INDEPENDENT (no deps on other targets),
        # so the step it now depends on must be too.
        INDEPENDENT TRUE
    )
endfunction()
