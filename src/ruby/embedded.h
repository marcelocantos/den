// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace den {

namespace fs = std::filesystem;

/// Extracted formula metadata — the structured result of evaluating a
/// Homebrew formula Ruby file.
struct FormulaRecipe {
    std::string name;
    std::string version;
    std::string description;
    std::string homepage;
    std::string license;
    std::string url;          // source tarball URL
    std::string sha256;       // source tarball SHA256
    std::vector<std::string> dependencies;
    std::vector<std::string> build_dependencies;
    bool keg_only = false;

    // Build instructions (extracted from the install method).
    struct BuildStep {
        std::string command; // "system", "ENV", etc.
        std::vector<std::string> args;
    };
    std::vector<BuildStep> build_steps;
};

/// The embedded Ruby runtime. Manages the Ruby VM lifecycle and
/// provides a virtual filesystem for formula evaluation.
///
/// Usage:
///   RubyRuntime rt;
///   rt.init();
///   rt.register_source("formula", formula_rb_content);
///   auto recipe = rt.evaluate_formula(formula_source);
///   rt.shutdown();
class RubyRuntime {
  public:
    RubyRuntime();
    ~RubyRuntime();

    RubyRuntime(const RubyRuntime&) = delete;
    RubyRuntime& operator=(const RubyRuntime&) = delete;

    /// Initialize the Ruby VM and install the virtual FS layer.
    void init();

    /// Shut down the Ruby VM.
    void shutdown();

    /// Register a Ruby source file in the virtual filesystem.
    /// After this, `require "name"` in Ruby will load from memory.
    void register_source(const std::string& name, const std::string& source);

    /// Bulk-register all .rb files from a directory tree.
    /// Files are registered with paths relative to root_dir.
    void register_directory(const fs::path& root_dir);

    /// Evaluate a formula Ruby source string and extract its metadata.
    std::optional<FormulaRecipe> evaluate_formula(const std::string& source,
                                                  const std::string& filename = "<formula>");

    /// Return true if the VM is initialized.
    bool is_initialized() const;

  private:
    struct M;
    std::unique_ptr<M> m;
};

} // namespace den
