// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

#include "self_exe.h"

#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

#include <cstdint>
#include <string>
#include <system_error>

namespace den {

std::optional<std::filesystem::path> current_executable_path() {
#ifdef __APPLE__
    uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);
    std::string buf(size, '\0');
    if (_NSGetExecutablePath(buf.data(), &size) != 0) {
        return std::nullopt;
    }
    buf.resize(buf.find('\0') == std::string::npos ? buf.size() : buf.find('\0'));
    return std::filesystem::path(buf).lexically_normal();
#else
    std::error_code ec;
    auto p = std::filesystem::read_symlink("/proc/self/exe", ec);
    if (ec) {
        return std::nullopt;
    }
    return p;
#endif
}

} // namespace den
