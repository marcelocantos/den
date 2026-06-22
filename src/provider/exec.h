// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <map>
#include <optional>
#include <string>
#include <vector>

namespace den {

/// Result of running an external command via `run_tool`.
struct ToolResult {
    int exit_code = -1;   // process exit status; -1 if it never ran.
    std::string output;   // combined stdout + stderr.
    bool spawned = false; // false if the executable could not be launched.
};

/// Run `argv[0]` with arguments `argv[1..]`, capturing combined stdout+stderr.
///
/// The command is launched directly via `posix_spawn` with an explicit argv
/// vector — never a shell — so package names and version hints sourced from
/// user input cannot inject shell metacharacters. `argv` must be non-empty;
/// `argv[0]` is resolved against PATH (plus any `extra_path` prepended).
///
/// `extra_env` entries are layered onto the inherited environment (later keys
/// win). `extra_path`, when set, is prepended to PATH for the child only —
/// providers use this so a tool installed into the env can find peer tools.
///
/// Never throws; a failure to spawn is reported via `ToolResult::spawned ==
/// false` so callers can degrade gracefully (e.g. skip a smoke probe when the
/// toolchain is absent).
ToolResult run_tool(const std::vector<std::string>& argv,
                    const std::map<std::string, std::string>& extra_env = {},
                    const std::optional<std::string>& extra_path = std::nullopt);

/// Is `tool` an executable resolvable on PATH? Used by providers to detect a
/// missing toolchain and surface a clear, actionable error rather than an
/// opaque spawn failure.
bool tool_available(const std::string& tool);

/// Run `argv` with stdio inherited from the current process (no capture),
/// layering `extra_env` and optionally prepending `extra_path` to PATH.
/// Returns the child's exit code, or -1 if it could not be launched. Used by
/// `den run` to exec a package binary transparently.
int run_tool_interactive(const std::vector<std::string>& argv,
                         const std::map<std::string, std::string>& extra_env = {},
                         const std::optional<std::string>& extra_path = std::nullopt);

} // namespace den
