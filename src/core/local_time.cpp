// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

#include "local_time.h"

#include <ctime>

namespace den {

std::string format_local_time(uint64_t secs, const std::string& fallback) {
    std::time_t t = static_cast<std::time_t>(secs);
    std::tm tm{};
    if (!localtime_r(&t, &tm))
        return fallback;
    char buf[32];
    return std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M", &tm) > 0 ? std::string(buf) : fallback;
}

} // namespace den
