// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

#include "embedded.h"

#include <spdlog/spdlog.h>

#include <ruby.h>

#include <fstream>
#include <mutex>
#include <sstream>

namespace den {

namespace {

// Ruby source for the virtual filesystem require hook.
constexpr const char* VIRTUAL_FS_BOOTSTRAP = R"RUBY(
module DenVFS
  @files = {}
  @loaded = {}

  def self.register(name, source)
    @files[name] = source
  end

  def self.files
    @files
  end

  def self.loaded
    @loaded
  end

  def self.resolve(name)
    # Try exact name, then with .rb extension.
    return @files[name] if @files.key?(name)
    with_rb = name.end_with?('.rb') ? name : "#{name}.rb"
    return @files[with_rb] if @files.key?(with_rb)
    # Try without leading path components for stdlib.
    nil
  end
end

# Override require to check the virtual filesystem first.
module Kernel
  alias_method :den_original_require, :require

  def require(name)
    # Check virtual FS first.
    if (src = DenVFS.resolve(name))
      # Prevent double-loading.
      key = name.end_with?('.rb') ? name : "#{name}.rb"
      return false if DenVFS.loaded[key]
      DenVFS.loaded[key] = true
      eval(src, TOPLEVEL_BINDING, key, 1)
      return true
    end

    # Fall through to original require for C extensions and builtins.
    den_original_require(name)
  rescue LoadError => e
    # If the original require also fails, log and re-raise.
    raise
  end
end

# Also override load for explicit file loads.
module Kernel
  alias_method :den_original_load, :load

  def load(name, wrap = false)
    if (src = DenVFS.resolve(name))
      if wrap
        Module.new.module_eval(src, name, 1)
      else
        eval(src, TOPLEVEL_BINDING, name, 1)
      end
      return true
    end
    den_original_load(name, wrap)
  end
end
)RUBY";

// Ruby source for the formula extraction helper.
// Uses a simple approach: call class methods that the DSL defined,
// reading class-level instance variables set by desc/homepage/etc.
constexpr const char* FORMULA_EXTRACTOR = R"RUBY(
module DenFormulaExtractor
  def self.extract(klass)
    # klass.name can segfault in embedded Ruby 4.0 — use empty string
    # and let the C++ caller supply the name from the index.
    name = ""

    # Read DSL-set values via the class methods.
    description = (klass.desc rescue nil) || ""
    home = (klass.homepage rescue nil) || ""
    lic = (klass.license.to_s rescue nil) || ""
    is_keg_only = (klass.keg_only? rescue false)

    # Dependencies — if the class tracks them.
    deps = []
    build_deps = []
    if klass.respond_to?(:deps)
      (klass.deps || []).each do |d|
        if d.is_a?(String)
          deps << d
        elsif d.respond_to?(:name)
          if d.respond_to?(:build?) && d.build?
            build_deps << d.name
          else
            deps << d.name
          end
        end
      end
    end

    # Return as a simple delimited string to avoid needing JSON gem.
    # Format: key=value lines, arrays as comma-separated.
    lines = []
    lines << "name=#{name}"
    lines << "version="
    lines << "description=#{description}"
    lines << "homepage=#{home}"
    lines << "license=#{lic}"
    lines << "keg_only=#{is_keg_only}"
    lines << "dependencies=#{deps.join(',')}"
    lines << "build_dependencies=#{build_deps.join(',')}"
    lines.join("\n")
  rescue => e
    "error=#{e.message}\nbacktrace=#{(e.backtrace || []).first(5).join(';')}"
  end
end
)RUBY";

// Parse the line-based key=value format from the Ruby extractor.
FormulaRecipe parse_extractor_output(const std::string& output) {
    FormulaRecipe recipe;
    std::istringstream ss(output);
    std::string line;

    auto split_csv = [](const std::string& s) -> std::vector<std::string> {
        std::vector<std::string> result;
        std::istringstream cs(s);
        std::string item;
        while (std::getline(cs, item, ',')) {
            if (!item.empty())
                result.push_back(item);
        }
        return result;
    };

    while (std::getline(ss, line)) {
        auto eq = line.find('=');
        if (eq == std::string::npos)
            continue;
        auto key = line.substr(0, eq);
        auto val = line.substr(eq + 1);

        if (key == "error") {
            SPDLOG_ERROR("formula extraction error: {}", val);
            return recipe;
        } else if (key == "backtrace") {
            SPDLOG_ERROR("  backtrace: {}", val);
        } else if (key == "name") {
            recipe.name = val;
        } else if (key == "version") {
            recipe.version = val;
        } else if (key == "description") {
            recipe.description = val;
        } else if (key == "homepage") {
            recipe.homepage = val;
        } else if (key == "license") {
            recipe.license = val;
        } else if (key == "url") {
            recipe.url = val;
        } else if (key == "sha256") {
            recipe.sha256 = val;
        } else if (key == "keg_only") {
            recipe.keg_only = (val == "true");
        } else if (key == "dependencies") {
            recipe.dependencies = split_csv(val);
        } else if (key == "build_dependencies") {
            recipe.build_dependencies = split_csv(val);
        }
    }
    return recipe;
}

} // namespace

struct RubyRuntime::M {
    bool initialized = false;
    std::mutex mutex;
};

RubyRuntime::RubyRuntime() : m(std::make_unique<M>()) {}

RubyRuntime::~RubyRuntime() {
    if (m->initialized) {
        shutdown();
    }
}

void RubyRuntime::init() {
    std::lock_guard lock(m->mutex);
    if (m->initialized) return;

    SPDLOG_INFO("initializing embedded Ruby VM");

    // Initialize the Ruby VM.
    // RUBY_INIT_STACK must be called from the main thread.
    RUBY_INIT_STACK;
    ruby_init();
    ruby_init_loadpath();

    // Install the virtual filesystem layer.
    int state = 0;
    rb_eval_string_protect(VIRTUAL_FS_BOOTSTRAP, &state);
    if (state) {
        VALUE err = rb_errinfo();
        VALUE msg = rb_funcall(err, rb_intern("message"), 0);
        SPDLOG_ERROR("failed to install VFS bootstrap: {}", StringValueCStr(msg));
        rb_set_errinfo(Qnil);
        return;
    }

    // Install the formula extractor.
    rb_eval_string_protect(FORMULA_EXTRACTOR, &state);
    if (state) {
        VALUE err = rb_errinfo();
        VALUE msg = rb_funcall(err, rb_intern("message"), 0);
        SPDLOG_ERROR("failed to install formula extractor: {}", StringValueCStr(msg));
        rb_set_errinfo(Qnil);
        return;
    }

    m->initialized = true;
    SPDLOG_INFO("Ruby VM initialized");
}

void RubyRuntime::shutdown() {
    std::lock_guard lock(m->mutex);
    if (!m->initialized) return;

    SPDLOG_INFO("shutting down Ruby VM");
    ruby_cleanup(0);
    m->initialized = false;
}

void RubyRuntime::register_source(const std::string& name, const std::string& source) {
    std::lock_guard lock(m->mutex);
    if (!m->initialized) return;

    VALUE den_vfs = rb_const_get(rb_cObject, rb_intern("DenVFS"));
    VALUE rb_name = rb_str_new_cstr(name.c_str());
    VALUE rb_source = rb_str_new(source.c_str(), static_cast<long>(source.size()));
    rb_funcall(den_vfs, rb_intern("register"), 2, rb_name, rb_source);

    SPDLOG_DEBUG("registered VFS source: {}", name);
}

void RubyRuntime::register_directory(const fs::path& root_dir) {
    if (!fs::is_directory(root_dir)) return;

    for (const auto& entry : fs::recursive_directory_iterator(root_dir)) {
        if (!entry.is_regular_file()) continue;
        auto ext = entry.path().extension().string();
        if (ext != ".rb") continue;

        auto rel = fs::relative(entry.path(), root_dir).string();
        // Read the file content.
        std::ifstream f(entry.path());
        if (!f) continue;
        std::ostringstream ss;
        ss << f.rdbuf();

        register_source(rel, ss.str());
    }
}

std::optional<FormulaRecipe> RubyRuntime::evaluate_formula(const std::string& source,
                                                           const std::string& filename) {
    std::lock_guard lock(m->mutex);
    if (!m->initialized) return std::nullopt;

    SPDLOG_DEBUG("evaluating formula: {}", filename);

    // Evaluate the formula source to define the class.
    int state = 0;
    rb_eval_string_protect(source.c_str(), &state);
    if (state) {
        VALUE err = rb_errinfo();
        VALUE msg = rb_funcall(err, rb_intern("message"), 0);
        SPDLOG_ERROR("formula eval error ({}): {}", filename, StringValueCStr(msg));
        rb_set_errinfo(Qnil);
        return std::nullopt;
    }

    // Find the last defined Formula subclass.
    // Homebrew formulas define a class like `class Tree < Formula`.
    // We look for the most recently defined subclass of Formula.
    VALUE formula_class = rb_const_get_at(rb_cObject, rb_intern("Formula"));
    if (NIL_P(formula_class)) {
        SPDLOG_ERROR("Formula class not found — Homebrew library not loaded?");
        return std::nullopt;
    }

    // Find the last-defined Formula subclass.
    // Use a simple iteration approach compatible with Ruby 4.0.
    VALUE klass = rb_eval_string_protect(R"(
        result = nil
        ObjectSpace.each_object(Class) do |c|
            result = c if c < Formula
        end
        result
    )", &state);
    if (state || NIL_P(klass)) {
        VALUE err = rb_errinfo();
        if (!NIL_P(err)) {
            VALUE msg = rb_funcall(err, rb_intern("message"), 0);
            SPDLOG_ERROR("failed to find formula class: {}", StringValueCStr(msg));
            rb_set_errinfo(Qnil);
        }
        return std::nullopt;
    }

    // Extract metadata using our extractor.
    VALUE extractor = rb_const_get(rb_cObject, rb_intern("DenFormulaExtractor"));
    VALUE result = rb_funcall(extractor, rb_intern("extract"), 1, klass);

    if (!RB_TYPE_P(result, RUBY_T_STRING)) {
        SPDLOG_ERROR("formula extractor returned non-string for {}", filename);
        return std::nullopt;
    }

    std::string output(RSTRING_PTR(result), RSTRING_LEN(result));
    SPDLOG_DEBUG("extractor output:\n{}", output);
    return parse_extractor_output(output);
}

bool RubyRuntime::is_initialized() const { return m->initialized; }

} // namespace den
