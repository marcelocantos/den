// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

#include "version.h"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <vector>

namespace den {

namespace {

struct Token {
    bool numeric = false;
    long long number = 0;
    std::string text; // lower-cased, for alphabetic runs
};

// Split off a trailing "_<digits>" Homebrew revision.
void split_revision(const std::string& v, std::string& base, long long& revision) {
    base = v;
    revision = 0;
    auto us = v.rfind('_');
    if (us == std::string::npos || us + 1 >= v.size())
        return;
    long long r = 0;
    for (std::size_t i = us + 1; i < v.size(); ++i) {
        unsigned char c = static_cast<unsigned char>(v[i]);
        if (!std::isdigit(c))
            return; // not a revision suffix
        if (r < 1'000'000'000'000LL)
            r = r * 10 + (c - '0');
    }
    base = v.substr(0, us);
    revision = r;
}

std::vector<Token> tokenize(const std::string& s) {
    std::vector<Token> out;
    std::size_t i = 0;
    while (i < s.size()) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        if (std::isdigit(c)) {
            Token t;
            t.numeric = true;
            while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i]))) {
                if (t.number < 1'000'000'000'000'000LL)
                    t.number = t.number * 10 + (s[i] - '0');
                ++i;
            }
            out.push_back(std::move(t));
        } else if (std::isalpha(c)) {
            Token t;
            while (i < s.size() && std::isalpha(static_cast<unsigned char>(s[i]))) {
                t.text.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(s[i]))));
                ++i;
            }
            out.push_back(std::move(t));
        } else {
            ++i; // separator
        }
    }
    return out;
}

bool is_prerelease(const Token& t) {
    if (t.numeric)
        return false;
    return t.text == "alpha" || t.text == "beta" || t.text == "pre" || t.text == "rc" ||
           t.text == "dev" || t.text == "preview";
}

// Compare a present token against a missing one: >0 if the token makes the
// version newer than its absence.
int against_missing(const Token& t) {
    if (t.numeric)
        return t.number > 0 ? 1 : 0;
    return is_prerelease(t) ? -1 : 1;
}

int compare_tokens(const std::vector<Token>& a, const std::vector<Token>& b) {
    std::size_t n = std::max(a.size(), b.size());
    for (std::size_t i = 0; i < n; ++i) {
        if (i >= a.size()) {
            int c = against_missing(b[i]);
            if (c != 0)
                return -c;
            continue;
        }
        if (i >= b.size()) {
            int c = against_missing(a[i]);
            if (c != 0)
                return c;
            continue;
        }
        const Token& x = a[i];
        const Token& y = b[i];
        if (x.numeric && y.numeric) {
            if (x.number != y.number)
                return x.number < y.number ? -1 : 1;
        } else if (x.numeric != y.numeric) {
            // A number beats an alphabetic run at the same position
            // ("1.0.1" > "1.0rc1").
            return x.numeric ? 1 : -1;
        } else {
            bool px = is_prerelease(x), py = is_prerelease(y);
            if (px != py)
                return px ? -1 : 1;
            if (x.text != y.text)
                return x.text < y.text ? -1 : 1;
        }
    }
    return 0;
}

} // namespace

int compare_pkg_versions(const std::string& a, const std::string& b) {
    std::string base_a, base_b;
    long long rev_a = 0, rev_b = 0;
    split_revision(a, base_a, rev_a);
    split_revision(b, base_b, rev_b);
    int c = compare_tokens(tokenize(base_a), tokenize(base_b));
    if (c != 0)
        return c;
    if (rev_a != rev_b)
        return rev_a < rev_b ? -1 : 1;
    return 0;
}

int compare_versioned(int scheme_a, const std::string& a, int scheme_b, const std::string& b) {
    if (scheme_a != scheme_b)
        return scheme_a < scheme_b ? -1 : 1;
    return compare_pkg_versions(a, b);
}

VersionDelta version_delta(const std::string& installed, int installed_scheme,
                           const std::string& available, int available_scheme) {
    const int by_version = compare_pkg_versions(available, installed);
    if (by_version == 0)
        return VersionDelta::Same;
    // Never downgrade from an upgrade path, even when the index's
    // version_scheme outranks the installed keg's (homebrew-core moved
    // openssl@3 from 3.6.x to 3.5.9 LTS with version_scheme 1).
    if (by_version < 0 && available_scheme > installed_scheme)
        return VersionDelta::Held;
    int c = compare_versioned(available_scheme, available, installed_scheme, installed);
    if (c > 0)
        return VersionDelta::Newer;
    if (c < 0)
        return VersionDelta::Older;
    return VersionDelta::Same;
}

std::string version_hold_reason(const std::string& installed, const std::string& available,
                                int available_scheme) {
    return "held: index pins " + available + " (version_scheme " +
           std::to_string(available_scheme) + "), below installed " + installed;
}

VersionDelta version_delta(const std::string& installed, const std::string& available) {
    int c = compare_pkg_versions(available, installed);
    if (c > 0)
        return VersionDelta::Newer;
    if (c < 0)
        return VersionDelta::Older;
    return VersionDelta::Same;
}

} // namespace den
