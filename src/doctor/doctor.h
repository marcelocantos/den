// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "../core/config.h"

#include <string>
#include <vector>

namespace den {

enum class Severity {
    Warning,
    Error,
};

struct Finding {
    Severity severity;
    std::string message;
};

/// Run all health checks and print a summary.
/// Returns the list of findings so callers can set a non-zero exit code
/// when there are errors.
std::vector<Finding> doctor(const Config& config);

} // namespace den
