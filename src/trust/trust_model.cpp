// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

#include "trust_model.h"

#include "../core/error.h"
#include "../core/self_exe.h"
#include "../download/http.h"
#include "../download/sha256.h"

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <array>
#include <fstream>
#include <vector>

namespace den {

namespace {

// den's replica snapshot lives in the source repo and is served from
// raw.githubusercontent.com — a SEPARATE CDN/origin from formulae.brew.sh.
// This is the second independent root of trust required by 🎯T42.
constexpr const char* kReplicaUrl =
    "https://raw.githubusercontent.com/marcelocantos/den/master/data/known_hashes.json";

// A SHA256 hex digest is exactly 64 lowercase hex characters.  We validate
// every hash crossing the trust boundary (replica file, network response)
// rather than trusting the source — a malformed digest is a corrupt/forged
// replica and must fail closed.
constexpr size_t kSha256HexLen = 64;

bool is_sha256_hex(const std::string& s) {
    if (s.size() != kSha256HexLen)
        return false;
    for (char c : s) {
        const bool hex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
        if (!hex)
            return false;
    }
    return true;
}

} // namespace

TrustCheckResult cross_check_hashes(const MaybeHash& homebrew_hash, const MaybeHash& replica_hash) {
    // Neither source reachable: cannot verify anything.
    if (!homebrew_hash && !replica_hash) {
        return {TrustDecision::Refuse,
                "both hash sources unreachable — cannot verify bottle integrity"};
    }

    // Both reachable: require agreement.
    if (homebrew_hash && replica_hash) {
        if (*homebrew_hash == *replica_hash) {
            return {TrustDecision::Proceed, "hashes agree"};
        }
        return {TrustDecision::Refuse,
                "hash mismatch: formulae.brew.sh=" + *homebrew_hash + " replica=" + *replica_hash};
    }

    // Exactly one reachable: degraded-trust fallback.
    return {TrustDecision::WarnProceed,
            homebrew_hash ? "replica unreachable — using Homebrew hash only"
                          : "formulae.brew.sh unreachable — using replica hash only"};
}

std::string replica_key(const std::string& formula, const std::string& version,
                        const std::string& platform_tag) {
    return formula + "--" + version + "--" + platform_tag;
}

std::map<std::string, std::string> parse_replica_document(const std::string& json_text) {
    nlohmann::json doc;
    try {
        doc = nlohmann::json::parse(json_text);
    } catch (const nlohmann::json::parse_error& e) {
        throw UserError(std::string("replica snapshot is not valid JSON: ") + e.what());
    }

    if (!doc.is_object()) {
        throw UserError("replica snapshot must be a JSON object");
    }

    // The document carries a top-level "hashes" object; everything else
    // (schema_version, generated_at, source) is informational metadata.
    if (!doc.contains("hashes")) {
        throw UserError("replica snapshot missing required 'hashes' object");
    }
    const auto& hashes = doc.at("hashes");
    if (!hashes.is_object()) {
        throw UserError("replica snapshot 'hashes' field must be a JSON object");
    }

    std::map<std::string, std::string> out;
    for (const auto& [key, value] : hashes.items()) {
        if (!value.is_string()) {
            throw UserError("replica snapshot entry '" + key + "' is not a string");
        }
        std::string sha = value.get<std::string>();
        // Fail closed on a malformed digest — a forged/corrupt entry must not
        // be allowed to masquerade as a verifiable hash.
        if (!is_sha256_hex(sha)) {
            throw UserError("replica snapshot entry '" + key + "' is not a 64-char hex SHA256");
        }
        out.emplace(key, std::move(sha));
    }
    return out;
}

std::string ReplicaSource::describe() const {
    switch (kind) {
    case Kind::File:
        return path.string();
    case Kind::Embedded:
        return "embedded snapshot";
    case Kind::None:
        break;
    }
    return "none";
}

ReplicaSource locate_local_replica(const Config& config, const std::optional<fs::path>& exe,
                                   std::string_view embedded) {
    auto exists = [](const fs::path& p) {
        std::error_code ec;
        return fs::is_regular_file(p, ec) && !ec;
    };

    std::vector<fs::path> candidates{config.den_home / "trust" / "known_hashes.json"};
    if (exe && !exe->empty()) {
        // Both the path as invoked (e.g. <prefix>/bin/den, a symlink into a
        // keg) and its resolved target (the keg's own bin/den).
        std::vector<fs::path> dirs{exe->parent_path()};
        std::error_code ec;
        auto real = fs::canonical(*exe, ec);
        if (!ec && real.parent_path() != dirs.front()) {
            dirs.push_back(real.parent_path());
        }
        for (const auto& d : dirs) {
            candidates.push_back(d / ".." / "share" / "den" / "known_hashes.json");
        }
        for (const auto& d : dirs) {
            candidates.push_back(d / "share" / "den" / "known_hashes.json");
        }
        for (const auto& d : dirs) {
            candidates.push_back(d / ".." / "data" / "known_hashes.json");
        }
    }
    for (const auto& c : candidates) {
        if (exists(c)) {
            return {ReplicaSource::Kind::File, c.lexically_normal()};
        }
    }
    if (!embedded.empty()) {
        return {ReplicaSource::Kind::Embedded, {}};
    }
    return {};
}

ReplicaSource locate_local_replica(const Config& config) {
    return locate_local_replica(config, current_executable_path(), embedded_replica_json());
}

fs::path local_replica_path(const Config& config) {
    auto src = locate_local_replica(config);
    if (src.kind == ReplicaSource::Kind::File) {
        return src.path;
    }
    // No on-disk copy: return the canonical den_home location for diagnostics.
    return config.den_home / "trust" / "known_hashes.json";
}

std::map<std::string, std::string> load_replica(const ReplicaSource& source,
                                                std::string_view embedded) {
    switch (source.kind) {
    case ReplicaSource::Kind::None:
        SPDLOG_DEBUG("local replica snapshot absent");
        return {};
    case ReplicaSource::Kind::Embedded:
        SPDLOG_DEBUG("using embedded replica snapshot");
        return parse_replica_document(std::string(embedded));
    case ReplicaSource::Kind::File:
        break;
    }

    std::ifstream in(source.path, std::ios::binary);
    if (!in) {
        // Present but unreadable: fail closed rather than silently falling
        // back to an older embedded snapshot.
        throw UserError("cannot read local replica snapshot: " + source.path.string());
    }
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    SPDLOG_DEBUG("using replica snapshot {}", source.path.string());

    // A malformed local snapshot is a hard error rather than a silent empty
    // map: an attacker who can corrupt the file should not be able to disable
    // the replica check by making the JSON unparseable.
    return parse_replica_document(text);
}

std::map<std::string, std::string> load_local_replica(const Config& config) {
    return load_replica(locate_local_replica(config), embedded_replica_json());
}

MaybeHash replica_lookup(const std::map<std::string, std::string>& replica,
                         const std::string& formula, const std::string& version,
                         const std::string& platform_tag) {
    auto it = replica.find(replica_key(formula, version, platform_tag));
    if (it == replica.end())
        return std::nullopt;
    return it->second;
}

std::optional<std::map<std::string, std::string>> fetch_replica_hashes() {
    std::string text;
    try {
        SPDLOG_INFO("fetching trust replica from {}", kReplicaUrl);
        text = fetch_url(kReplicaUrl);
    } catch (const std::exception& e) {
        SPDLOG_WARN("trust replica fetch failed: {}", e.what());
        return std::nullopt;
    }
    try {
        return parse_replica_document(text);
    } catch (const std::exception& e) {
        SPDLOG_WARN("trust replica parse failed: {}", e.what());
        return std::nullopt;
    }
}

DiffVerifyOutcome verify_diff_entry(const ReplicaDiffEntry& entry, const fs::path& bottle_path) {
    // A claimed hash that is not even a well-formed SHA256 is a fabrication.
    if (!is_sha256_hex(entry.claimed_sha256)) {
        SPDLOG_WARN("diff entry {} has a malformed claimed SHA256", entry.formula_name);
        return DiffVerifyOutcome::Mismatch;
    }

    std::string actual_sha;
    try {
        actual_sha = hash_file(bottle_path);
    } catch (const std::exception&) {
        // If we cannot hash the file, treat it as a mismatch — fail closed.
        return DiffVerifyOutcome::Mismatch;
    }
    return (actual_sha == entry.claimed_sha256) ? DiffVerifyOutcome::Match
                                                : DiffVerifyOutcome::Mismatch;
}

} // namespace den
