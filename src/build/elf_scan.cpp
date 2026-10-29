// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

#include "elf_scan.h"

#include <cstdint>
#include <cstring>
#include <fstream>
#include <optional>

namespace den {

namespace {

constexpr uint32_t kPtLoad = 1;
constexpr uint32_t kPtDynamic = 2;
constexpr int64_t kDtNull = 0;
constexpr int64_t kDtNeeded = 1;
constexpr int64_t kDtStrtab = 5;
constexpr int64_t kDtStrsz = 10;
constexpr int64_t kDtRpath = 15;
constexpr int64_t kDtRunpath = 29;

template <typename T> T le(const unsigned char* p) {
    T v = 0;
    for (size_t i = 0; i < sizeof(T); ++i) {
        v |= static_cast<T>(p[i]) << (8 * i);
    }
    return v;
}

bool read_at(std::ifstream& in, uint64_t off, void* buf, size_t n) {
    in.clear();
    in.seekg(static_cast<std::streamoff>(off));
    in.read(static_cast<char*>(buf), static_cast<std::streamsize>(n));
    return static_cast<size_t>(in.gcount()) == n;
}

struct Segment {
    uint32_t type;
    uint64_t offset, vaddr, filesz;
};

} // namespace

ElfDynamic elf_dynamic(const fs::path& file) {
    ElfDynamic out;
    std::ifstream in(file, std::ios::binary);
    if (!in) {
        return out;
    }
    unsigned char eh[64];
    if (!read_at(in, 0, eh, sizeof eh) ||
        std::memcmp(eh,
                    "\x7f"
                    "ELF",
                    4) != 0 ||
        eh[4] != 2 /* ELFCLASS64 */ || eh[5] != 1 /* ELFDATA2LSB */) {
        return out;
    }
    out.is_elf = true;
    const auto phoff = le<uint64_t>(eh + 32);
    const auto phentsize = le<uint16_t>(eh + 54);
    const auto phnum = le<uint16_t>(eh + 56);
    if (phentsize < 56 || phnum == 0 || phnum > 4096) {
        return out;
    }
    std::vector<Segment> segs;
    std::optional<Segment> dyn;
    for (uint16_t i = 0; i < phnum; ++i) {
        unsigned char ph[56];
        if (!read_at(in, phoff + uint64_t{i} * phentsize, ph, sizeof ph)) {
            return out;
        }
        Segment s{le<uint32_t>(ph), le<uint64_t>(ph + 8), le<uint64_t>(ph + 16),
                  le<uint64_t>(ph + 32)};
        if (s.type == kPtLoad) {
            segs.push_back(s);
        } else if (s.type == kPtDynamic) {
            dyn = s;
        }
    }
    if (!dyn || dyn->filesz > (1u << 24)) {
        return out;
    }
    std::vector<unsigned char> d(dyn->filesz);
    if (!read_at(in, dyn->offset, d.data(), d.size())) {
        return out;
    }
    uint64_t strtab = 0, strsz = 0;
    std::vector<std::pair<int64_t, uint64_t>> entries;
    for (size_t off = 0; off + 16 <= d.size(); off += 16) {
        const auto tag = le<int64_t>(d.data() + off);
        const auto val = le<uint64_t>(d.data() + off + 8);
        if (tag == kDtNull) {
            break;
        }
        if (tag == kDtStrtab) {
            strtab = val;
        } else if (tag == kDtStrsz) {
            strsz = val;
        } else if (tag == kDtNeeded || tag == kDtRpath || tag == kDtRunpath) {
            entries.emplace_back(tag, val);
        }
    }
    // DT_STRTAB is a virtual address; find the file offset through PT_LOAD.
    std::optional<uint64_t> str_off;
    for (const auto& s : segs) {
        if (strtab >= s.vaddr && strtab < s.vaddr + s.filesz) {
            str_off = s.offset + (strtab - s.vaddr);
            break;
        }
    }
    if (!str_off || strsz == 0 || strsz > (1u << 24)) {
        return out;
    }
    std::vector<char> strs(strsz);
    if (!read_at(in, *str_off, strs.data(), strs.size())) {
        return out;
    }
    auto str_at = [&](uint64_t i) -> std::string {
        if (i >= strs.size()) {
            return {};
        }
        const char* b = strs.data() + i;
        return std::string(b, strnlen(b, strs.size() - i));
    };
    for (const auto& [tag, val] : entries) {
        auto s = str_at(val);
        if (tag == kDtNeeded) {
            out.needed.push_back(std::move(s));
            continue;
        }
        size_t start = 0;
        while (start <= s.size()) {
            const auto colon = s.find(':', start);
            const auto part = s.substr(start, colon == std::string::npos ? colon : colon - start);
            if (!part.empty()) {
                out.search_paths.push_back(part);
            }
            if (colon == std::string::npos) {
                break;
            }
            start = colon + 1;
        }
    }
    return out;
}

} // namespace den
