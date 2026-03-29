// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <spdlog/spdlog.h>

#include <stdexcept>
#include <string>

namespace den {

// Base exception for all den errors.
class Error : public std::runtime_error {
  public:
    using std::runtime_error::runtime_error;
};

// User-facing error (displays message without stack trace).
class UserError : public Error {
  public:
    using Error::Error;
};

// Internal error (should not happen — indicates a bug).
class InternalError : public Error {
  public:
    using Error::Error;
};

// Network/download error.
class DownloadError : public Error {
  public:
    using Error::Error;
};

// Archive extraction error.
class ArchiveError : public Error {
  public:
    using Error::Error;
};

} // namespace den
