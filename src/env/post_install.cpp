// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

#include "post_install.h"

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <map>
#include <set>
#include <system_error>

namespace den {

namespace {

using json = nlohmann::json;

/// `p` lies strictly beneath `root` (both lexically normal, absolute).
bool lexically_under(const fs::path& p, const fs::path& root) {
    auto rel = p.lexically_relative(root);
    if (rel.empty() || rel == ".") {
        return false;
    }
    return *rel.begin() != "..";
}

std::map<std::string, fs::path> token_paths(const PostInstallContext& ctx) {
    const auto& p = ctx.prefix;
    const auto& k = ctx.keg;
    // The subset of Homebrew's install-step tokens den resolves. `prefix`
    // is the versioned keg, as Homebrew's runner sets
    // prefix_returns_versioned_prefix for the steps.
    return {
        {"HOMEBREW_PREFIX", p}, {"HOMEBREW_CELLAR", p / "Cellar"},
        {"prefix", k},          {"opt_prefix", p / "opt" / ctx.name},
        {"bin", k / "bin"},     {"sbin", k / "sbin"},
        {"lib", k / "lib"},     {"libexec", k / "libexec"},
        {"share", k / "share"}, {"pkgshare", k / "share" / ctx.name},
        {"etc", p / "etc"},     {"pkgetc", p / "etc" / ctx.name},
        {"var", p / "var"},
    };
}

} // namespace

bool expand_post_install_path(const std::string& path, const PostInstallContext& ctx, fs::path& out,
                              std::string* unknown_token) {
    const auto tokens = token_paths(ctx);
    std::string result;
    size_t i = 0;
    while (i < path.size()) {
        auto open = path.find("{{", i);
        if (open == std::string::npos) {
            result.append(path, i, std::string::npos);
            break;
        }
        auto close = path.find("}}", open + 2);
        if (close == std::string::npos) {
            if (unknown_token) {
                *unknown_token = path.substr(open);
            }
            return false;
        }
        result.append(path, i, open - i);
        const auto token = path.substr(open + 2, close - open - 2);
        if (token == "formula_name" || token == "name") {
            result += ctx.name;
        } else if (auto it = tokens.find(token); it != tokens.end()) {
            result += it->second.string();
        } else {
            if (unknown_token) {
                *unknown_token = "{{" + token + "}}";
            }
            return false;
        }
        i = close + 2;
    }
    out = fs::path(result);
    return true;
}

PostInstallPlan plan_post_install(const std::string& steps_json, const PostInstallContext& ctx) {
    PostInstallPlan plan;
    if (steps_json.empty()) {
        return plan;
    }
    auto refuse = [&](std::string why) {
        plan.links.clear();
        plan.unsupported = std::move(why);
        return plan;
    };

    json steps = json::parse(steps_json, nullptr, /*allow_exceptions=*/false);
    if (!steps.is_array()) {
        return refuse("post_install_steps is not a list");
    }
    const auto prefix = ctx.prefix.lexically_normal();
    const auto etc = (prefix / "etc").lexically_normal();
    const auto var = (prefix / "var").lexically_normal();

    static const std::set<std::string> kStepKeys = {"type", "source", "target", "force",
                                                    "overwrite"};
    for (const auto& step : steps) {
        if (!step.is_object()) {
            return refuse("a post_install step is not an object");
        }
        const auto type = step.value("type", std::string{});
        if (type != "symlink") {
            return refuse("post_install step type '" + type + "' is not supported");
        }
        for (auto it = step.begin(); it != step.end(); ++it) {
            if (!kStepKeys.count(it.key())) {
                return refuse("symlink step option '" + it.key() + "' is not supported");
            }
        }
        fs::path ends[2];
        const char* names[2] = {"source", "target"};
        for (int e = 0; e < 2; ++e) {
            const auto& spec = step.contains(names[e]) ? step[names[e]] : json();
            if (!spec.is_object() || !spec.contains("path") || !spec["path"].is_string()) {
                return refuse(std::string("symlink step has no ") + names[e] + " path");
            }
            for (auto it = spec.begin(); it != spec.end(); ++it) {
                const bool absolute_base = it.key() == "base" && it.value() == "absolute";
                if (it.key() != "path" && !absolute_base) {
                    return refuse(std::string("symlink ") + names[e] + " option '" + it.key() +
                                  "' is not supported");
                }
            }
            std::string unknown;
            if (!expand_post_install_path(spec["path"].get<std::string>(), ctx, ends[e],
                                          &unknown)) {
                return refuse("placeholder " + unknown + " is not supported");
            }
            if (!ends[e].is_absolute()) {
                return refuse(std::string("symlink ") + names[e] + " is not an absolute path");
            }
            ends[e] = ends[e].lexically_normal();
        }
        const auto& points_to = ends[0];
        const auto& link = ends[1];
        if (!lexically_under(points_to, prefix)) {
            return refuse("symlink source " + points_to.string() + " is outside " +
                          prefix.string());
        }
        if (!lexically_under(link, etc) && !lexically_under(link, var)) {
            return refuse("symlink " + link.string() + " is outside " + etc.string() + " and " +
                          var.string());
        }
        const bool force = (step.contains("force") && step["force"] == true) ||
                           (step.contains("overwrite") && step["overwrite"] == true);
        plan.links.push_back({link, points_to, force});
    }
    return plan;
}

PostInstallResult apply_post_install(const PostInstallPlan& plan, const fs::path& prefix,
                                     bool replace) {
    PostInstallResult result;
    std::error_code ec;
    for (const auto& step : plan.links) {
        const auto& link = step.link;
        const auto shown = link.string();

        // Every directory between the prefix and the link must be a real
        // directory (or not exist yet): a symlinked one, such as a
        // brew-folded <prefix>/etc/<name> -> keg link, would make den write
        // into whatever it points at.
        bool through_link = false;
        auto rel = link.parent_path().lexically_relative(prefix.lexically_normal());
        fs::path walk = prefix.lexically_normal();
        for (const auto& part : rel) {
            walk /= part;
            const auto st = fs::symlink_status(walk, ec);
            if (st.type() == fs::file_type::not_found) {
                break;
            }
            if (st.type() != fs::file_type::directory) {
                through_link = true;
                break;
            }
        }
        if (through_link) {
            result.skipped.push_back(shown + ": a parent directory is not a real directory");
            continue;
        }

        const auto st = fs::symlink_status(link, ec);
        if (st.type() == fs::file_type::symlink) {
            const auto current = fs::read_symlink(link, ec);
            if (!ec && current == step.points_to) {
                continue; // already right
            }
        }
        if (st.type() != fs::file_type::not_found) {
            if (st.type() == fs::file_type::directory) {
                result.skipped.push_back(shown + ": a directory is in the way");
                continue;
            }
            if (!replace || !step.force) {
                result.skipped.push_back(shown + ": already exists");
                continue;
            }
            fs::remove(link, ec);
            if (ec) {
                result.skipped.push_back(shown + ": cannot replace: " + ec.message());
                continue;
            }
        }
        fs::create_directories(link.parent_path(), ec);
        if (ec) {
            result.skipped.push_back(shown + ": " + ec.message());
            continue;
        }
        fs::create_symlink(step.points_to, link, ec);
        if (ec) {
            result.skipped.push_back(shown + ": " + ec.message());
            continue;
        }
        SPDLOG_INFO("post_install: {} -> {}", shown, step.points_to.string());
        ++result.created;
    }
    return result;
}

} // namespace den
