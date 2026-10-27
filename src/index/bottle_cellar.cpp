// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

#include "bottle_cellar.h"

namespace den {

namespace fs = std::filesystem;

namespace {

bool ends_with(const std::string& s, const std::string& suffix) {
    return s.size() >= suffix.size() &&
           s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

std::string normalised(const fs::path& p) {
    auto s = p.lexically_normal().string();
    while (s.size() > 1 && s.back() == '/')
        s.pop_back();
    return s;
}

} // namespace

std::string standard_bottle_cellar(const std::string& tag) {
    if (ends_with(tag, "_linux"))
        return "/home/linuxbrew/.linuxbrew/Cellar";
    if (tag.rfind("arm64_", 0) == 0)
        return "/opt/homebrew/Cellar";
    return "/usr/local/Cellar";
}

std::string required_bottle_cellar(const std::string& tag, const Package::Archive& archive) {
    if (archive.relocation != RelocationType::HardcodedPrefix)
        return {};
    return archive.cellar.empty() ? standard_bottle_cellar(tag) : archive.cellar;
}

bool bottle_pours_into(const std::string& tag, const Package::Archive& archive,
                       const fs::path& store) {
    auto required = required_bottle_cellar(tag, archive);
    return required.empty() || normalised(required) == normalised(store);
}

std::string fixed_cellar_refusal(const Package& pkg, const std::string& tag,
                                 const std::string& required, const fs::path& store) {
    std::string msg = "cannot pour " + pkg.name + " " + pkg.version + ": its " + tag +
                      " bottle was built for the fixed Cellar " + required +
                      " and can't be relocated to den's Cellar " + store.string() + ".";
    const bool source_build = pkg.source_url.has_value() || pkg.ruby_source_path.has_value();
    if (source_build) {
        msg += "\nBuild it from source instead: den install --build-from-source " + pkg.name;
    } else {
        msg += "\nNo source build is available for " + pkg.name + ".";
    }
    msg += "\nOr pour into the prefix the bottle was built for (DEN_PREFIX=" +
           fs::path(required).parent_path().string() + ", shared with Homebrew).";
    return msg;
}

} // namespace den
