// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

#include "index_cache.h"

#include "index.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <mutex>
#include <stdexcept>
#include <string_view>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace den::index_cache {

namespace {

constexpr std::array<char, 8> kMagic = {'D', 'E', 'N', 'I', 'D', 'X', '\0', '\0'};
// v2: records carry Package::cask_uninstall (🎯T12 uninstall stanzas).
// v3: records carry Package::revision (Homebrew formula revision).
// v4: archives carry Package::Archive::cellar (fixed-cellar bottles).
// v5: records carry Package::old_names (renamed casks/formulae).
// v6: cask artifacts may be CaskArtifactKind::Font.
// v7: cask artifacts may be CaskArtifactKind::Wrapper (command_wrapper).
constexpr uint32_t kFormatVersion = 7;
constexpr uint32_t kEndianTag = 0x01020304;
constexpr size_t kHeaderSize = 80;
constexpr size_t kEntrySize = 24;
constexpr size_t kFingerprintSpan = 64 * 1024;

// Header field offsets.
constexpr size_t kOffVersion = 8;
constexpr size_t kOffEndian = 12;
constexpr size_t kOffSrcSize = 16;
constexpr size_t kOffSrcMtime = 24;
constexpr size_t kOffSrcFingerprint = 32;
constexpr size_t kOffCount = 40;
constexpr size_t kOffFileSize = 48;
constexpr size_t kOffTableHash = 56;
constexpr size_t kOffBlob = 64;
constexpr size_t kOffHeaderHash = 72;

struct CorruptCache : std::runtime_error {
    using std::runtime_error::runtime_error;
};

/// Fast non-cryptographic 64-bit hash (word-at-a-time multiply/xorshift).
/// Only guards against accidental corruption, not tampering.
uint64_t hash_bytes(const void* data, size_t n, uint64_t seed = 0) {
    auto* p = static_cast<const unsigned char*>(data);
    uint64_t h = 0x9E3779B97F4A7C15ull ^ seed ^ (n * 0xff51afd7ed558ccdull);
    while (n >= 8) {
        uint64_t w;
        std::memcpy(&w, p, 8);
        h = (h ^ w) * 0xff51afd7ed558ccdull;
        h ^= h >> 32;
        p += 8;
        n -= 8;
    }
    uint64_t w = 0;
    std::memcpy(&w, p, n);
    h = (h ^ w ^ (static_cast<uint64_t>(n) << 56)) * 0xc4ceb9fe1a85ec53ull;
    h ^= h >> 29;
    return h;
}

template <typename T> T load(const char* p) {
    T v;
    std::memcpy(&v, p, sizeof v);
    return v;
}

template <typename T> void store(std::string& buf, size_t off, T v) {
    std::memcpy(buf.data() + off, &v, sizeof v);
}

template <typename T> void append(std::string& buf, T v) {
    char tmp[sizeof v];
    std::memcpy(tmp, &v, sizeof v);
    buf.append(tmp, sizeof v);
}

// --- record codec -----------------------------------------------------------

void put_str(std::string& b, const std::string& s) {
    append<uint32_t>(b, static_cast<uint32_t>(s.size()));
    b.append(s);
}

void put_strs(std::string& b, const std::vector<std::string>& v) {
    append<uint32_t>(b, static_cast<uint32_t>(v.size()));
    for (const auto& s : v) {
        put_str(b, s);
    }
}

void put_opt(std::string& b, const std::optional<std::string>& s) {
    append<uint8_t>(b, s ? 1 : 0);
    if (s) {
        put_str(b, *s);
    }
}

class Reader {
  public:
    Reader(const char* p, size_t n) : p_(p), end_(p + n) {}

    void need(size_t n) const {
        if (static_cast<size_t>(end_ - p_) < n) {
            throw CorruptCache("truncated record");
        }
    }
    template <typename T> T num() {
        need(sizeof(T));
        T v = load<T>(p_);
        p_ += sizeof(T);
        return v;
    }
    uint8_t enum_byte(uint8_t max) {
        auto v = num<uint8_t>();
        if (v > max) {
            throw CorruptCache("enum out of range");
        }
        return v;
    }
    bool flag() { return enum_byte(1) != 0; }
    std::string str() {
        auto n = num<uint32_t>();
        need(n);
        std::string s(p_, n);
        p_ += n;
        return s;
    }
    std::vector<std::string> strs() {
        auto n = num<uint32_t>();
        std::vector<std::string> v;
        v.reserve(std::min<size_t>(n, 1024));
        for (uint32_t i = 0; i < n; ++i) {
            v.push_back(str());
        }
        return v;
    }
    std::optional<std::string> opt() {
        if (!flag()) {
            return std::nullopt;
        }
        return str();
    }
    bool done() const { return p_ == end_; }

  private:
    const char* p_;
    const char* end_;
};

// --- mapped cache -----------------------------------------------------------

class MappedIndex final : public IndexBacking {
  public:
    MappedIndex(void* map, size_t len, fs::path json_path)
        : map_(static_cast<const char*>(map)), len_(len), json_path_(std::move(json_path)) {
        count_ = load<uint64_t>(map_ + kOffCount);
        entries_ = map_ + kHeaderSize;
        names_ = entries_ + count_ * kEntrySize;
        blob_ = map_ + load<uint64_t>(map_ + kOffBlob);
        names_len_ = static_cast<size_t>(blob_ - names_);
    }

    ~MappedIndex() override { ::munmap(const_cast<char*>(map_), len_); }

    std::optional<Package> lookup(const std::string& name) const override {
        std::lock_guard lock(mu_);
        if (fallback_) {
            auto it = fallback_->find(name);
            return it != fallback_->end() ? std::optional<Package>(it->second) : std::nullopt;
        }
        try {
            size_t lo = 0;
            size_t hi = count_;
            while (lo < hi) {
                size_t mid = lo + (hi - lo) / 2;
                int c = name_at(mid).compare(name);
                if (c == 0) {
                    return decode_entry(mid);
                }
                if (c < 0) {
                    lo = mid + 1;
                } else {
                    hi = mid;
                }
            }
            return std::nullopt;
        } catch (const CorruptCache& e) {
            fall_back(e.what());
            auto it = fallback_->find(name);
            return it != fallback_->end() ? std::optional<Package>(it->second) : std::nullopt;
        }
    }

    size_t size() const override {
        std::lock_guard lock(mu_);
        return fallback_ ? fallback_->size() : count_;
    }

    void load_all(std::map<std::string, Package>& out) const override {
        std::lock_guard lock(mu_);
        if (!fallback_) {
            try {
                std::map<std::string, Package> tmp;
                auto hint = tmp.end();
                for (size_t i = 0; i < count_; ++i) {
                    Package pkg = decode_entry(i);
                    std::string key(name_at(i));
                    hint = tmp.emplace_hint(hint, std::move(key), std::move(pkg));
                }
                for (auto& [k, v] : tmp) {
                    out.try_emplace(k, std::move(v));
                }
                return;
            } catch (const CorruptCache& e) {
                fall_back(e.what());
            }
        }
        for (const auto& [k, v] : *fallback_) {
            out.try_emplace(k, v);
        }
    }

  private:
    std::string_view name_at(size_t i) const {
        const char* e = entries_ + i * kEntrySize;
        auto off = load<uint32_t>(e);
        auto len = load<uint32_t>(e + 4);
        if (static_cast<size_t>(off) + len > names_len_) {
            throw CorruptCache("name out of bounds");
        }
        return {names_ + off, len};
    }

    Package decode_entry(size_t i) const {
        const char* e = entries_ + i * kEntrySize;
        auto rec_off = load<uint64_t>(e + 8);
        auto rec_len = load<uint32_t>(e + 16);
        auto rec_hash = load<uint32_t>(e + 20);
        size_t blob_start = static_cast<size_t>(blob_ - map_);
        if (rec_off < blob_start || rec_off > len_ || rec_len > len_ - rec_off) {
            throw CorruptCache("record out of bounds");
        }
        const char* rec = map_ + rec_off;
        if (static_cast<uint32_t>(hash_bytes(rec, rec_len)) != rec_hash) {
            throw CorruptCache("record hash mismatch");
        }
        Package pkg = decode_package(rec, rec_len);
        if (pkg.name != name_at(i)) {
            throw CorruptCache("record name mismatch");
        }
        return pkg;
    }

    // Called with mu_ held.
    void fall_back(const char* why) const {
        SPDLOG_WARN("index cache {} is corrupt ({}); falling back to {}",
                    cache_path_for(json_path_).string(), why, json_path_.string());
        std::error_code ec;
        fs::remove(cache_path_for(json_path_), ec);
        auto idx = load_index_json(json_path_);
        fallback_ = std::make_unique<std::map<std::string, Package>>(std::move(idx.packages));
    }

    const char* map_;
    size_t len_;
    fs::path json_path_;
    size_t count_ = 0;
    const char* entries_ = nullptr;
    const char* names_ = nullptr;
    const char* blob_ = nullptr;
    size_t names_len_ = 0;

    mutable std::mutex mu_;
    mutable std::unique_ptr<std::map<std::string, Package>> fallback_;
};

/// RAII fd.
struct Fd {
    int fd = -1;
    explicit Fd(int f) : fd(f) {}
    ~Fd() {
        if (fd >= 0) {
            ::close(fd);
        }
    }
    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;
};

bool read_at(int fd, char* buf, size_t n, off_t off) {
    size_t got = 0;
    while (got < n) {
        ssize_t r = ::pread(fd, buf + got, n - got, off + static_cast<off_t>(got));
        if (r <= 0) {
            return false;
        }
        got += static_cast<size_t>(r);
    }
    return true;
}

} // namespace

fs::path cache_path_for(const fs::path& json_path) {
    fs::path p = json_path;
    p += ".idx";
    return p;
}

bool disabled_by_env() {
    const char* v = std::getenv("DEN_NO_INDEX_CACHE");
    return v && *v && std::string_view(v) != "0";
}

std::optional<SourceStamp> stamp_source(const fs::path& json_path) {
    Fd f(::open(json_path.c_str(), O_RDONLY | O_CLOEXEC));
    if (f.fd < 0) {
        return std::nullopt;
    }
    struct stat st = {};
    if (::fstat(f.fd, &st) != 0) {
        return std::nullopt;
    }
    SourceStamp s;
    s.size = static_cast<uint64_t>(st.st_size);
#if defined(__APPLE__)
    s.mtime =
        static_cast<int64_t>(st.st_mtimespec.tv_sec) * 1'000'000'000 + st.st_mtimespec.tv_nsec;
#else
    s.mtime = static_cast<int64_t>(st.st_mtim.tv_sec) * 1'000'000'000 + st.st_mtim.tv_nsec;
#endif
    // Fingerprint the head and tail so a same-size rewrite within the mtime
    // granularity is still caught without reading the whole file.
    size_t head = static_cast<size_t>(std::min<uint64_t>(s.size, kFingerprintSpan));
    size_t tail = static_cast<size_t>(std::min<uint64_t>(s.size - head, kFingerprintSpan));
    std::string buf(head + tail, '\0');
    if (!read_at(f.fd, buf.data(), head, 0) ||
        !read_at(f.fd, buf.data() + head, tail, static_cast<off_t>(s.size - tail))) {
        return std::nullopt;
    }
    s.fingerprint = hash_bytes(buf.data(), buf.size(), s.size);
    return s;
}

std::string encode_package(const Package& pkg) {
    std::string b;
    b.reserve(256);
    put_str(b, pkg.name);
    put_str(b, pkg.version);
    append<uint32_t>(b, static_cast<uint32_t>(pkg.revision));
    put_str(b, pkg.description);
    put_str(b, pkg.homepage);
    put_str(b, pkg.license);
    append<uint8_t>(b, static_cast<uint8_t>(pkg.artifact_type));
    append<uint8_t>(b, static_cast<uint8_t>(pkg.stability));
    append<uint8_t>(b, pkg.deprecated ? 1 : 0);
    append<uint8_t>(b, pkg.disabled ? 1 : 0);
    append<uint8_t>(b, pkg.keg_only ? 1 : 0);
    put_str(b, pkg.keg_only_reason);
    put_strs(b, pkg.dependencies);
    put_strs(b, pkg.build_dependencies);
    put_strs(b, pkg.conflicts_with);
    append<uint32_t>(b, static_cast<uint32_t>(pkg.archives.size()));
    for (const auto& [tag, ar] : pkg.archives) {
        put_str(b, tag);
        put_str(b, ar.url);
        put_str(b, ar.sha256);
        append<uint8_t>(b, static_cast<uint8_t>(ar.relocation));
        put_str(b, ar.cellar);
    }
    append<uint32_t>(b, static_cast<uint32_t>(pkg.cask_artifacts.size()));
    for (const auto& a : pkg.cask_artifacts) {
        append<uint8_t>(b, static_cast<uint8_t>(a.kind));
        put_str(b, a.source);
        put_str(b, a.target);
    }
    const auto& u = pkg.cask_uninstall;
    put_strs(b, u.quit);
    append<uint32_t>(b, static_cast<uint32_t>(u.signal.size()));
    for (const auto& [sig, bundle] : u.signal) {
        put_str(b, sig);
        put_str(b, bundle);
    }
    put_strs(b, u.launchctl);
    put_strs(b, u.pkgutil);
    put_strs(b, u.delete_paths);
    put_strs(b, u.trash);
    put_strs(b, u.rmdir);
    put_strs(b, u.unsupported);
    put_opt(b, pkg.source_url);
    put_opt(b, pkg.source_sha256);
    put_opt(b, pkg.ruby_source_path);
    put_strs(b, pkg.old_names);
    return b;
}

Package decode_package(const char* data, size_t len) {
    Reader r(data, len);
    Package pkg;
    pkg.name = r.str();
    pkg.version = r.str();
    pkg.revision = static_cast<int>(r.num<uint32_t>());
    pkg.description = r.str();
    pkg.homepage = r.str();
    pkg.license = r.str();
    pkg.artifact_type = static_cast<ArtifactType>(r.enum_byte(1));
    pkg.stability = static_cast<Stability>(r.enum_byte(4));
    pkg.deprecated = r.flag();
    pkg.disabled = r.flag();
    pkg.keg_only = r.flag();
    pkg.keg_only_reason = r.str();
    pkg.dependencies = r.strs();
    pkg.build_dependencies = r.strs();
    pkg.conflicts_with = r.strs();
    auto n_archives = r.num<uint32_t>();
    for (uint32_t i = 0; i < n_archives; ++i) {
        std::string tag = r.str();
        Package::Archive ar;
        ar.url = r.str();
        ar.sha256 = r.str();
        ar.relocation = static_cast<RelocationType>(r.enum_byte(2));
        ar.cellar = r.str();
        pkg.archives.emplace(std::move(tag), std::move(ar));
    }
    auto n_arts = r.num<uint32_t>();
    for (uint32_t i = 0; i < n_arts; ++i) {
        CaskArtifact a;
        a.kind = static_cast<CaskArtifactKind>(r.enum_byte(4));
        a.source = r.str();
        a.target = r.str();
        pkg.cask_artifacts.push_back(std::move(a));
    }
    auto& u = pkg.cask_uninstall;
    u.quit = r.strs();
    auto n_signal = r.num<uint32_t>();
    for (uint32_t i = 0; i < n_signal; ++i) {
        std::string sig = r.str();
        std::string bundle = r.str();
        u.signal.emplace_back(std::move(sig), std::move(bundle));
    }
    u.launchctl = r.strs();
    u.pkgutil = r.strs();
    u.delete_paths = r.strs();
    u.trash = r.strs();
    u.rmdir = r.strs();
    u.unsupported = r.strs();
    pkg.source_url = r.opt();
    pkg.source_sha256 = r.opt();
    pkg.ruby_source_path = r.opt();
    pkg.old_names = r.strs();
    if (!r.done()) {
        throw CorruptCache("trailing bytes in record");
    }
    return pkg;
}

bool write(const PackageIndex& idx, const fs::path& json_path) {
    auto stamp = stamp_source(json_path);
    if (!stamp) {
        SPDLOG_WARN("index cache: cannot stamp {}", json_path.string());
        return false;
    }
    const auto& pkgs = idx.packages; // std::map: already sorted bytewise
    const size_t count = pkgs.size();

    std::string names;
    std::vector<std::string> records;
    records.reserve(count);
    for (const auto& [name, pkg] : pkgs) {
        names.append(name);
        records.push_back(encode_package(pkg));
    }
    size_t table_end = kHeaderSize + count * kEntrySize + names.size();
    size_t blob_off = (table_end + 7) & ~size_t{7};

    std::string buf(blob_off, '\0');
    size_t name_off = 0;
    size_t rec_off = blob_off;
    size_t i = 0;
    for (const auto& [name, pkg] : pkgs) {
        size_t e = kHeaderSize + i * kEntrySize;
        const auto& rec = records[i];
        store<uint32_t>(buf, e, static_cast<uint32_t>(name_off));
        store<uint32_t>(buf, e + 4, static_cast<uint32_t>(name.size()));
        store<uint64_t>(buf, e + 8, static_cast<uint64_t>(rec_off));
        store<uint32_t>(buf, e + 16, static_cast<uint32_t>(rec.size()));
        store<uint32_t>(buf, e + 20, static_cast<uint32_t>(hash_bytes(rec.data(), rec.size())));
        name_off += name.size();
        rec_off += rec.size();
        ++i;
    }
    std::memcpy(buf.data() + kHeaderSize + count * kEntrySize, names.data(), names.size());
    buf.reserve(rec_off);
    for (const auto& rec : records) {
        buf.append(rec);
    }

    std::memcpy(buf.data(), kMagic.data(), kMagic.size());
    store<uint32_t>(buf, kOffVersion, kFormatVersion);
    store<uint32_t>(buf, kOffEndian, kEndianTag);
    store<uint64_t>(buf, kOffSrcSize, stamp->size);
    store<int64_t>(buf, kOffSrcMtime, stamp->mtime);
    store<uint64_t>(buf, kOffSrcFingerprint, stamp->fingerprint);
    store<uint64_t>(buf, kOffCount, count);
    store<uint64_t>(buf, kOffFileSize, buf.size());
    store<uint64_t>(buf, kOffTableHash,
                    hash_bytes(buf.data() + kHeaderSize, blob_off - kHeaderSize));
    store<uint64_t>(buf, kOffBlob, blob_off);
    store<uint64_t>(buf, kOffHeaderHash, hash_bytes(buf.data(), kOffHeaderHash));

    auto dest = cache_path_for(json_path);
    fs::path tmp = dest;
    tmp += ".tmp." + std::to_string(::getpid());
    try {
        {
            std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
            if (!out) {
                throw std::runtime_error("cannot open " + tmp.string());
            }
            out.write(buf.data(), static_cast<std::streamsize>(buf.size()));
            if (!out) {
                throw std::runtime_error("write failed for " + tmp.string());
            }
        }
        // If the source changed while we were encoding, don't publish a
        // cache stamped with the old identity under the new file's name.
        auto after = stamp_source(json_path);
        if (!after || !(*after == *stamp)) {
            throw std::runtime_error("source changed during cache build");
        }
        fs::rename(tmp, dest);
    } catch (const std::exception& e) {
        std::error_code ec;
        fs::remove(tmp, ec);
        SPDLOG_WARN("index cache: not written ({})", e.what());
        return false;
    }
    SPDLOG_INFO("wrote index cache {} ({} packages, {} bytes)", dest.string(), count, buf.size());
    return true;
}

std::shared_ptr<const IndexBacking> open(const fs::path& json_path, std::string* why) {
    auto fail = [&](const std::string& reason) -> std::shared_ptr<const IndexBacking> {
        SPDLOG_INFO("index cache unusable: {}", reason);
        if (why) {
            *why = reason;
        }
        return nullptr;
    };

    auto path = cache_path_for(json_path);
    Fd f(::open(path.c_str(), O_RDONLY | O_CLOEXEC));
    if (f.fd < 0) {
        return fail("missing");
    }
    struct stat st = {};
    if (::fstat(f.fd, &st) != 0 || st.st_size < static_cast<off_t>(kHeaderSize)) {
        return fail("too small");
    }
    size_t len = static_cast<size_t>(st.st_size);
    void* map = ::mmap(nullptr, len, PROT_READ, MAP_PRIVATE, f.fd, 0);
    if (map == MAP_FAILED) {
        return fail("mmap failed");
    }
    struct Unmap {
        void* p;
        size_t n;
        bool armed = true;
        ~Unmap() {
            if (armed) {
                ::munmap(p, n);
            }
        }
    } guard{map, len};
    const char* m = static_cast<const char*>(map);

    if (std::memcmp(m, kMagic.data(), kMagic.size()) != 0) {
        return fail("bad magic");
    }
    if (load<uint32_t>(m + kOffVersion) != kFormatVersion) {
        return fail("format version mismatch");
    }
    if (load<uint32_t>(m + kOffEndian) != kEndianTag) {
        return fail("endianness mismatch");
    }
    if (load<uint64_t>(m + kOffHeaderHash) != hash_bytes(m, kOffHeaderHash)) {
        return fail("header hash mismatch");
    }
    if (load<uint64_t>(m + kOffFileSize) != len) {
        return fail("file size mismatch (truncated?)");
    }
    auto count = load<uint64_t>(m + kOffCount);
    auto blob_off = load<uint64_t>(m + kOffBlob);
    if (count > len / kEntrySize || kHeaderSize + count * kEntrySize > blob_off || blob_off > len) {
        return fail("bad table bounds");
    }

    auto stamp = stamp_source(json_path);
    if (!stamp) {
        return fail("cannot stamp source");
    }
    SourceStamp cached{load<uint64_t>(m + kOffSrcSize), load<int64_t>(m + kOffSrcMtime),
                       load<uint64_t>(m + kOffSrcFingerprint)};
    if (!(cached == *stamp)) {
        return fail("stale (index.json changed)");
    }
    if (load<uint64_t>(m + kOffTableHash) != hash_bytes(m + kHeaderSize, blob_off - kHeaderSize)) {
        return fail("table hash mismatch");
    }

    guard.armed = false;
    return std::make_shared<MappedIndex>(map, len, json_path);
}

} // namespace den::index_cache
