// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

#include "cask_uninstall_json.h"

#include <algorithm>

namespace den {

namespace {

using json = nlohmann::json;

void add_strings(const json& v, std::vector<std::string>& out) {
    auto push = [&](const std::string& s) {
        if (!s.empty() && std::find(out.begin(), out.end(), s) == out.end())
            out.push_back(s);
    };
    if (v.is_string()) {
        push(v.get<std::string>());
    } else if (v.is_array()) {
        for (const auto& e : v)
            if (e.is_string())
                push(e.get<std::string>());
    }
}

void add_signals(const json& v, std::vector<std::pair<std::string, std::string>>& out) {
    auto pair_of = [&](const json& p) {
        if (p.is_array() && p.size() == 2 && p[0].is_string() && p[1].is_string())
            out.emplace_back(p[0].get<std::string>(), p[1].get<std::string>());
    };
    if (v.is_array() && v.size() == 2 && v[0].is_string()) {
        pair_of(v); // single ["TERM", "com.foo"]
    } else if (v.is_array()) {
        for (const auto& p : v)
            pair_of(p);
    }
}

void note_unsupported(CaskUninstall& out, const std::string& name) {
    if (std::find(out.unsupported.begin(), out.unsupported.end(), name) == out.unsupported.end())
        out.unsupported.push_back(name);
}

} // namespace

void merge_uninstall_directives(const json& obj, CaskUninstall& out) {
    if (!obj.is_object())
        return;
    for (auto it = obj.begin(); it != obj.end(); ++it) {
        const std::string& k = it.key();
        const json& v = it.value();
        if (k == "quit")
            add_strings(v, out.quit);
        else if (k == "signal")
            add_signals(v, out.signal);
        else if (k == "launchctl")
            add_strings(v, out.launchctl);
        else if (k == "pkgutil")
            add_strings(v, out.pkgutil);
        else if (k == "delete")
            add_strings(v, out.delete_paths);
        else if (k == "trash")
            add_strings(v, out.trash);
        else if (k == "rmdir")
            add_strings(v, out.rmdir);
        else if (k == "unsupported") {
            std::vector<std::string> names;
            add_strings(v, names);
            for (const auto& n : names)
                note_unsupported(out, n);
        } else if (k == "on_upgrade") {
            // Option, not a directive: den always runs quit/signal/launchctl
            // before an upgrade swap.
        } else {
            note_unsupported(out, k); // script, early_script, kext, login_item, …
        }
    }
}

void parse_cask_uninstall(const json& artifacts, CaskUninstall& out) {
    if (!artifacts.is_array())
        return;
    for (const auto& a : artifacts) {
        if (!a.is_object() || !a.contains("uninstall"))
            continue;
        const auto& u = a["uninstall"];
        if (u.is_array()) {
            for (const auto& d : u)
                merge_uninstall_directives(d, out);
        } else {
            merge_uninstall_directives(u, out);
        }
    }
}

json uninstall_to_json(const CaskUninstall& u) {
    json j = json::object();
    auto put = [&](const char* k, const std::vector<std::string>& v) {
        if (!v.empty())
            j[k] = v;
    };
    put("quit", u.quit);
    if (!u.signal.empty()) {
        json s = json::array();
        for (const auto& [sig, bid] : u.signal)
            s.push_back(json::array({sig, bid}));
        j["signal"] = std::move(s);
    }
    put("launchctl", u.launchctl);
    put("pkgutil", u.pkgutil);
    put("delete", u.delete_paths);
    put("trash", u.trash);
    put("rmdir", u.rmdir);
    put("unsupported", u.unsupported);
    return j;
}

} // namespace den
