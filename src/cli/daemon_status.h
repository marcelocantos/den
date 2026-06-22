// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "../daemon/daemon.h"

#include <ostream>

namespace den {

// Render the daemon-status report from persisted DaemonState: last check,
// pending upgrades, and any deferred upgrades held back because a managed
// binary is in use (🎯T51 / 🎯T72). When nothing is deferred no deferral
// section is emitted.
void format_daemon_status(std::ostream& out, const DaemonState& state);

} // namespace den
