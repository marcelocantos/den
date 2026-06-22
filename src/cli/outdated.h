// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "../daemon/daemon.h"

#include <ostream>

namespace den {

// Render the daemon's view of outdated packages — pending upgrades plus any
// upgrades the daemon has deferred because a managed binary is in use
// (🎯T51 / 🎯T72). Deferred entries are flagged so the user sees why an
// available upgrade has not yet applied. When nothing is deferred the word
// "deferred" does not appear.
void format_outdated(std::ostream& out, const DaemonState& state);

} // namespace den
