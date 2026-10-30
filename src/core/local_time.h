// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstdint>
#include <string>

namespace den {

/// `secs` (epoch seconds) in local time as "YYYY-MM-DD HH:MM", the one
/// human-readable time den prints: `den history`, `den trust review`, the
/// trust ledger's "since" and the daemon's disabled_blocked entries.
/// `fallback` when it cannot be shown.
std::string format_local_time(uint64_t secs, const std::string& fallback = "?");

} // namespace den
