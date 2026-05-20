// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

#include "plist_import.h"

#include <spdlog/spdlog.h>

#include <fstream>
#include <sstream>

namespace den {

namespace {

// Minimal recursive plist scanner. Operates over a single string (the file
// contents) using a cursor. Supports: <dict>, <array>, <string>, <key>,
// <true/>, <false/>. Everything else is skipped at the value position.

struct Scanner {
    const std::string& s;
    size_t pos = 0;

    explicit Scanner(const std::string& s_) : s(s_) {}

    void skip_ws() {
        while (pos < s.size() &&
               (s[pos] == ' ' || s[pos] == '\t' || s[pos] == '\n' || s[pos] == '\r'))
            ++pos;
    }

    // Find the next "<...>" tag starting at or after pos.
    // Returns the tag's inner content (e.g. "dict", "/dict", "true/",
    // "key", "string", "?xml...", "!DOCTYPE..."). Advances pos past the '>'.
    // Returns nullopt at EOF.
    std::optional<std::string> next_tag() {
        while (true) {
            while (pos < s.size() && s[pos] != '<')
                ++pos;
            if (pos >= s.size())
                return std::nullopt;
            size_t start = pos + 1;
            // Skip CDATA / comments defensively.
            if (start + 3 < s.size() && s.compare(start, 3, "!--") == 0) {
                auto end = s.find("-->", start + 3);
                if (end == std::string::npos)
                    return std::nullopt;
                pos = end + 3;
                continue;
            }
            size_t end = s.find('>', start);
            if (end == std::string::npos)
                return std::nullopt;
            std::string tag = s.substr(start, end - start);
            pos = end + 1;
            return tag;
        }
    }

    // After observing an opening <tag> (e.g. "key" or "string"), read until
    // the matching </tag>, returning the un-escaped character data.
    std::string read_text(const std::string& tag_name) {
        std::string close = "</" + tag_name + ">";
        auto end = s.find(close, pos);
        if (end == std::string::npos) {
            std::string out = s.substr(pos);
            pos = s.size();
            return out;
        }
        std::string raw = s.substr(pos, end - pos);
        pos = end + close.size();
        return xml_unescape(raw);
    }

    static std::string xml_unescape(const std::string& in) {
        std::string out;
        out.reserve(in.size());
        for (size_t i = 0; i < in.size();) {
            if (in[i] == '&') {
                if (in.compare(i, 5, "&amp;") == 0) {
                    out += '&';
                    i += 5;
                    continue;
                }
                if (in.compare(i, 4, "&lt;") == 0) {
                    out += '<';
                    i += 4;
                    continue;
                }
                if (in.compare(i, 4, "&gt;") == 0) {
                    out += '>';
                    i += 4;
                    continue;
                }
                if (in.compare(i, 6, "&quot;") == 0) {
                    out += '"';
                    i += 6;
                    continue;
                }
                if (in.compare(i, 6, "&apos;") == 0) {
                    out += '\'';
                    i += 6;
                    continue;
                }
            }
            out += in[i++];
        }
        return out;
    }
};

// Forward decls for the value-shape parsers.
struct PlistValue {
    enum Kind { String, Bool, Array, Dict, Other } kind = Other;
    std::string str;
    bool boolean = false;
    std::vector<PlistValue> array;
    std::vector<std::pair<std::string, PlistValue>> dict;
};

PlistValue parse_value(Scanner& sc, const std::string& opening_tag);

PlistValue parse_dict(Scanner& sc) {
    PlistValue out;
    out.kind = PlistValue::Dict;
    while (true) {
        auto tag = sc.next_tag();
        if (!tag)
            break;
        if (*tag == "/dict")
            break;
        if (*tag == "key") {
            std::string key = sc.read_text("key");
            // Read value.
            auto vtag = sc.next_tag();
            if (!vtag)
                break;
            if ((*vtag).front() == '/') // unexpected close
                break;
            PlistValue val = parse_value(sc, *vtag);
            out.dict.emplace_back(std::move(key), std::move(val));
        }
        // Ignore other tags inside a dict (whitespace, comments).
    }
    return out;
}

PlistValue parse_array(Scanner& sc) {
    PlistValue out;
    out.kind = PlistValue::Array;
    while (true) {
        auto tag = sc.next_tag();
        if (!tag)
            break;
        if (*tag == "/array")
            break;
        if ((*tag).front() == '/')
            break;
        out.array.push_back(parse_value(sc, *tag));
    }
    return out;
}

PlistValue parse_value(Scanner& sc, const std::string& opening_tag) {
    // Strip trailing '/' for self-closing checks.
    bool self_closing = !opening_tag.empty() && opening_tag.back() == '/';
    std::string name = opening_tag;
    if (self_closing)
        name.pop_back();
    // Trim trailing whitespace.
    while (!name.empty() && (name.back() == ' ' || name.back() == '\t'))
        name.pop_back();

    PlistValue v;
    if (name == "string") {
        v.kind = PlistValue::String;
        v.str = sc.read_text("string");
    } else if (name == "true") {
        v.kind = PlistValue::Bool;
        v.boolean = true;
    } else if (name == "false") {
        v.kind = PlistValue::Bool;
        v.boolean = false;
    } else if (name == "dict") {
        v = parse_dict(sc);
    } else if (name == "array") {
        v = parse_array(sc);
    } else {
        // Skip until the matching close tag. If self-closing, nothing to skip.
        if (!self_closing) {
            std::string close = "</" + name + ">";
            auto end = sc.s.find(close, sc.pos);
            if (end != std::string::npos)
                sc.pos = end + close.size();
        }
    }
    return v;
}

const PlistValue* dict_lookup(const PlistValue& d, const std::string& key) {
    if (d.kind != PlistValue::Dict)
        return nullptr;
    for (const auto& [k, v] : d.dict) {
        if (k == key)
            return &v;
    }
    return nullptr;
}

} // namespace

std::optional<ServiceSpec> parse_homebrew_plist(const fs::path& path, const std::string& name) {
    std::ifstream f(path);
    if (!f.is_open())
        return std::nullopt;
    std::ostringstream ss;
    ss << f.rdbuf();
    std::string content = ss.str();

    Scanner sc(content);
    // Skip header tags until we hit the top-level <dict>.
    PlistValue root;
    bool got_dict = false;
    while (true) {
        auto tag = sc.next_tag();
        if (!tag)
            break;
        if (*tag == "dict") {
            root = parse_dict(sc);
            got_dict = true;
            break;
        }
    }
    if (!got_dict) {
        SPDLOG_WARN("plist: no top-level dict in {}", path.string());
        return std::nullopt;
    }

    const PlistValue* args = dict_lookup(root, "ProgramArguments");
    if (!args || args->kind != PlistValue::Array || args->array.empty()) {
        SPDLOG_WARN("plist: no ProgramArguments array in {}", path.string());
        return std::nullopt;
    }

    ServiceSpec spec;
    spec.name = name;
    for (const auto& v : args->array) {
        if (v.kind == PlistValue::String)
            spec.argv.push_back(v.str);
    }
    if (spec.argv.empty())
        return std::nullopt;

    if (const auto* env = dict_lookup(root, "EnvironmentVariables")) {
        if (env->kind == PlistValue::Dict) {
            for (const auto& [k, v] : env->dict) {
                if (v.kind == PlistValue::String)
                    spec.env.emplace_back(k, v.str);
            }
        }
    }
    if (const auto* wd = dict_lookup(root, "WorkingDirectory")) {
        if (wd->kind == PlistValue::String)
            spec.working_dir = wd->str;
    }
    // Map KeepAlive to a restart policy.
    if (const auto* ka = dict_lookup(root, "KeepAlive")) {
        if (ka->kind == PlistValue::Bool && ka->boolean) {
            spec.restart = RestartPolicy::Always;
        } else if (ka->kind == PlistValue::Dict) {
            // The "SuccessfulExit=false" idiom means "restart on failure".
            for (const auto& [k, v] : ka->dict) {
                if (k == "SuccessfulExit" && v.kind == PlistValue::Bool && !v.boolean) {
                    spec.restart = RestartPolicy::OnFailure;
                    break;
                }
            }
        }
    }

    return spec;
}

} // namespace den
