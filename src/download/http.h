// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <string>

namespace den {

/// Fetch a URL and return the response body as a string.
/// Throws DownloadError on failure.
std::string fetch_url(const std::string& url);

} // namespace den
