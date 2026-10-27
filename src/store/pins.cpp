// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

#include "pins.h"

#include "../core/error.h"

#include <nlohmann/json.hpp>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>

namespace den {

using json = nlohmann::json;

fs::path pins_path(const fs::path& den_home) {
    return den_home / "pins.json";
}

PinMap read_pins(const fs::path& den_home) {
    const auto path = pins_path(den_home);
    std::error_code ec;
    if (!fs::exists(path, ec) || ec) {
        return {};
    }
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        throw UserError("cannot read " + path.string());
    }
    std::stringstream ss;
    ss << in.rdbuf();

    json doc;
    try {
        doc = json::parse(ss.str());
    } catch (const json::parse_error& e) {
        throw UserError(path.string() + " is not valid JSON (" + e.what() +
                        "); fix or remove it to continue");
    }
    if (!doc.is_object() || !doc.contains("pins") || !doc.at("pins").is_object()) {
        throw UserError(path.string() + " is missing its 'pins' object");
    }
    PinMap out;
    for (const auto& [name, entry] : doc.at("pins").items()) {
        if (name.empty() || !entry.is_object() || !entry.contains("version") ||
            !entry.at("version").is_string()) {
            throw UserError(path.string() + ": malformed pin entry '" + name + "'");
        }
        out.emplace(name, entry.at("version").get<std::string>());
    }
    return out;
}

void write_pins(const fs::path& den_home, const PinMap& pins) {
    const auto path = pins_path(den_home);
    fs::create_directories(path.parent_path());
    json doc;
    doc["schema_version"] = 1;
    doc["pins"] = json::object();
    for (const auto& [name, version] : pins) {
        doc["pins"][name] = {{"version", version}};
    }
    const auto tmp = path.string() + ".tmp";
    {
        std::ofstream out(tmp, std::ios::trunc);
        if (!out) {
            throw InternalError("failed to open " + tmp);
        }
        out << doc.dump(2) << '\n';
        out.flush();
        if (!out) {
            throw InternalError("failed to write " + tmp);
        }
    }
    if (std::rename(tmp.c_str(), path.c_str()) != 0) {
        throw InternalError("failed to rename " + tmp + ": " + std::strerror(errno));
    }
}

PinOutcome pin_package(const fs::path& den_home, const std::string& name,
                       const std::string& version) {
    auto pins = read_pins(den_home);
    if (pins.count(name)) {
        return PinOutcome::AlreadyPinned;
    }
    pins.emplace(name, version);
    write_pins(den_home, pins);
    return PinOutcome::Pinned;
}

bool unpin_package(const fs::path& den_home, const std::string& name) {
    auto pins = read_pins(den_home);
    if (pins.erase(name) == 0) {
        return false;
    }
    write_pins(den_home, pins);
    return true;
}

PinPartition partition_pinned(const std::vector<UpgradeCandidate>& candidates, const PinMap& pins) {
    PinPartition out;
    for (const auto& c : candidates) {
        (pins.count(c.name) ? out.held : out.upgrade).push_back(c);
    }
    return out;
}

} // namespace den
