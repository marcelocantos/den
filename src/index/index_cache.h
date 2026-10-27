// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

#pragma once

// Binary lookup cache for the package index.
//
// index.json is the source of truth. Parsing all ~25 MB of it costs ~140 ms,
// which dominated every index-backed command even though most of them (info,
// install, deps, upgrade) only touch a handful of packages. The cache is a
// derived, disposable sidecar (`index.json.idx`) that is mmap'd read-only and
// holds a sorted name→record table, so a lookup is a binary search plus
// decoding one small record.
//
// Layout (native endian; the file never leaves the machine that wrote it):
//
//   Header (80 bytes)   magic "DENIDX\0\0", format version, endian tag,
//                       source stamp (size, mtime, fingerprint), entry count,
//                       file size, table hash, blob offset, header hash
//   Entries             count × {name_off, name_len, rec_off, rec_len, rec_hash}
//                       sorted by name (bytewise)
//   Names               concatenated package names
//   Records             one compact binary Package encoding per entry
//
// Safety: the cache is keyed on index.json's size, mtime and a fingerprint of
// its first and last 64 KiB. Any mismatch, a bad magic/version, a size or
// hash mismatch in the header or table, or a per-record hash/bounds failure
// means the cache is ignored (or, if discovered mid-run, deleted) and den falls
// back to parsing index.json. The cache is (re)written by `den update` and,
// lazily, by the first command that had to parse index.json.

#include "package.h"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>

namespace den::index_cache {

namespace fs = std::filesystem;

/// Identity of an index.json at a point in time.
struct SourceStamp {
    uint64_t size = 0;
    int64_t mtime = 0;
    uint64_t fingerprint = 0;

    bool operator==(const SourceStamp&) const = default;
};

/// Path of the cache sidecar for a given index.json.
fs::path cache_path_for(const fs::path& json_path);

/// Stamp the source file, or nullopt if it cannot be read.
std::optional<SourceStamp> stamp_source(const fs::path& json_path);

/// Write the cache for `idx`, which must be exactly what parsing `json_path`
/// yields. Best effort: returns false (and logs) on any failure.
bool write(const PackageIndex& idx, const fs::path& json_path);

/// Open and validate the cache for `json_path`. Returns nullptr when the
/// cache is missing, stale or corrupt; `why` (if given) receives the reason.
std::shared_ptr<const IndexBacking> open(const fs::path& json_path, std::string* why = nullptr);

/// True when DEN_NO_INDEX_CACHE is set to a non-empty value other than "0".
bool disabled_by_env();

/// Record codec, exposed for tests.
std::string encode_package(const Package& pkg);
Package decode_package(const char* data, size_t len); // throws on malformed input

} // namespace den::index_cache
