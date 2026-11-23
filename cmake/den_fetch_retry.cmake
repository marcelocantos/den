# Bounded-retry download of one pinned archive (cmake -P script).
#
#   cmake -DURL=<url> -DSHA256=<hex> -DDEST=<file> -P den_fetch_retry.cmake
#
# Run as an ExternalProject step just before the stock `download` step (see
# DenFetchRetry.cmake). It leaves DEST in place only once its SHA-256 matches;
# the stock download step then finds the file with a matching hash and skips
# its own (single-attempt) download, while URL_HASH verification and
# extraction stay ExternalProject's own.
#
# Retried (bounded): transport failures that a second attempt can fix —
# curl 6/7 (resolve/connect), 18 (partial file), 28 (timeout), 35 (TLS
# handshake), 52 (empty reply), 55/56 (send/recv failure) — and HTTP errors
# (curl 22) whose FINAL response status, after any redirects, is 408, 429 or
# 5xx. Anything else (404, 403, other 4xx, unclassified errors) fails at once,
# as does a SHA-256 mismatch on a completed download.
#
# Optional:
#   -DMAX_ATTEMPTS=<n>       default 3 (env DEN_FETCH_MAX_ATTEMPTS overrides)
#   -DBACKOFF_SECONDS=<n>    first backoff, doubled each retry; default 2
#                            (env DEN_FETCH_BACKOFF_SECONDS overrides)
#   -DTIMEOUT=<s>            per attempt, default 180
#   -DINACTIVITY_TIMEOUT=<s> default 30
cmake_minimum_required(VERSION 3.24)

foreach(_var URL SHA256 DEST)
    if(NOT DEFINED ${_var} OR "${${_var}}" STREQUAL "")
        message(FATAL_ERROR "den_fetch_retry: ${_var} is required")
    endif()
endforeach()

if(DEFINED ENV{DEN_FETCH_MAX_ATTEMPTS})
    set(MAX_ATTEMPTS "$ENV{DEN_FETCH_MAX_ATTEMPTS}")
elseif(NOT DEFINED MAX_ATTEMPTS)
    set(MAX_ATTEMPTS 3)
endif()
if(DEFINED ENV{DEN_FETCH_BACKOFF_SECONDS})
    set(BACKOFF_SECONDS "$ENV{DEN_FETCH_BACKOFF_SECONDS}")
elseif(NOT DEFINED BACKOFF_SECONDS)
    set(BACKOFF_SECONDS 2)
endif()
if(NOT DEFINED TIMEOUT)
    set(TIMEOUT 180)
endif()
if(NOT DEFINED INACTIVITY_TIMEOUT)
    set(INACTIVITY_TIMEOUT 30)
endif()

string(TOLOWER "${SHA256}" SHA256)
get_filename_component(_name "${DEST}" NAME)

# Verified cache: a file already at DEST with the pinned hash needs no network.
if(EXISTS "${DEST}")
    file(SHA256 "${DEST}" _have)
    if(_have STREQUAL SHA256)
        message(STATUS "den_fetch_retry: ${_name}: cached, SHA-256 verified")
        return()
    endif()
    message(STATUS "den_fetch_retry: ${_name}: cached file has the wrong hash; re-downloading")
    file(REMOVE "${DEST}")
endif()

get_filename_component(_dir "${DEST}" DIRECTORY)
file(MAKE_DIRECTORY "${_dir}")
set(_partial "${DEST}.partial")

set(_retry_curl_codes 6 7 18 28 35 52 55 56)
set(_backoff ${BACKOFF_SECONDS})
set(_attempt 1)
while(TRUE)
    file(REMOVE "${_partial}")
    file(DOWNLOAD "${URL}" "${_partial}"
        STATUS _status
        LOG _log
        TIMEOUT ${TIMEOUT}
        INACTIVITY_TIMEOUT ${INACTIVITY_TIMEOUT}
        TLS_VERIFY ON)
    list(GET _status 0 _code)
    list(GET _status 1 _msg)

    # Final HTTP status: the last response status line in curl's log, so a
    # 30x redirect is never mistaken for the result.
    set(_http "")
    string(REGEX MATCHALL "HTTP/[0-9.]+ [0-9][0-9][0-9]" _lines "${_log}")
    if(_lines)
        list(GET _lines -1 _last)
        string(REGEX REPLACE "^HTTP/[0-9.]+ " "" _http "${_last}")
    endif()
    set(_what "curl ${_code} (${_msg})")
    if(NOT _http STREQUAL "")
        string(APPEND _what ", HTTP ${_http}")
    endif()

    if(_code EQUAL 0)
        file(SHA256 "${_partial}" _got)
        if(NOT _got STREQUAL SHA256)
            file(REMOVE "${_partial}")
            message(FATAL_ERROR "den_fetch_retry: ${_name}: SHA-256 mismatch from ${URL}\n"
                "  expected ${SHA256}\n  actual   ${_got}")
        endif()
        file(RENAME "${_partial}" "${DEST}")
        message(STATUS "den_fetch_retry: ${_name}: downloaded on attempt ${_attempt}/${MAX_ATTEMPTS}, SHA-256 verified")
        return()
    endif()

    set(_transient FALSE)
    if(_code IN_LIST _retry_curl_codes)
        set(_transient TRUE)
    elseif(_code EQUAL 22 AND _http MATCHES "^(408|429|5[0-9][0-9])$")
        set(_transient TRUE)
    endif()

    file(REMOVE "${_partial}")
    if(NOT _transient)
        message(FATAL_ERROR "den_fetch_retry: ${_name}: ${_what} from ${URL}; not retryable "
            "(attempt ${_attempt}/${MAX_ATTEMPTS})")
    endif()
    if(_attempt GREATER_EQUAL MAX_ATTEMPTS)
        message(FATAL_ERROR "den_fetch_retry: ${_name}: ${_what} from ${URL}; "
            "giving up after ${_attempt} attempts")
    endif()
    message(STATUS "den_fetch_retry: ${_name}: ${_what}; retrying in ${_backoff}s "
        "(attempt ${_attempt}/${MAX_ATTEMPTS})")
    if(_backoff GREATER 0)
        execute_process(COMMAND "${CMAKE_COMMAND}" -E sleep ${_backoff})
    endif()
    math(EXPR _backoff "${_backoff} * 2")
    math(EXPR _attempt "${_attempt} + 1")
endwhile()
