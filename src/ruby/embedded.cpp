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
constexpr const char* FORMULA_EXTRACTOR = R"RUBY(
module DenFormulaExtractor
  def self.extract(klass)
    f = klass.new(klass.name.downcase)

    result = {
      name: f.name,
      version: f.respond_to?(:version) ? f.version.to_s : "",
      description: klass.respond_to?(:desc) ? (klass.desc || "") : "",
      homepage: klass.respond_to?(:homepage) ? (klass.homepage || "") : "",
      license: klass.respond_to?(:license) ? (klass.license.to_s rescue "") : "",
      keg_only: klass.respond_to?(:keg_only?) ? klass.keg_only? : false,
      dependencies: [],
      build_dependencies: [],
    }

    if klass.respond_to?(:stable) && klass.stable
      spec = klass.stable
      result[:url] = spec.url.to_s rescue ""
      result[:sha256] = spec.checksum.to_s rescue ""
    end

    if klass.respond_to?(:deps)
      klass.deps.each do |dep|
        if dep.build?
          result[:build_dependencies] << dep.name
        else
          result[:dependencies] << dep.name
        end
      end
    end

    result
  rescue => e
    { error: e.message, backtrace: e.backtrace&.first(5)&.join("\n") }
  end
end
)RUBY";

// Convert a Ruby hash VALUE to a C++ FormulaRecipe.
FormulaRecipe value_to_recipe(VALUE hash) {
    FormulaRecipe recipe;

    auto get_string = [&](const char* key) -> std::string {
        VALUE sym = rb_id2sym(rb_intern(key));
        VALUE val = rb_hash_aref(hash, sym);
        if (NIL_P(val)) return "";
        return StringValueCStr(val);
    };

    auto get_bool = [&](const char* key) -> bool {
        VALUE sym = rb_id2sym(rb_intern(key));
        VALUE val = rb_hash_aref(hash, sym);
        return RTEST(val);
    };

    auto get_string_array = [&](const char* key) -> std::vector<std::string> {
        VALUE sym = rb_id2sym(rb_intern(key));
        VALUE arr = rb_hash_aref(hash, sym);
        std::vector<std::string> result;
        if (!NIL_P(arr) && RB_TYPE_P(arr, RUBY_T_ARRAY)) {
            long len = RARRAY_LEN(arr);
            for (long i = 0; i < len; ++i) {
                VALUE elem = rb_ary_entry(arr, i);
                result.push_back(StringValueCStr(elem));
            }
        }
        return result;
    };

    // Check for error.
    VALUE error_sym = rb_id2sym(rb_intern("error"));
    VALUE error_val = rb_hash_aref(hash, error_sym);
    if (!NIL_P(error_val)) {
        SPDLOG_ERROR("formula evaluation error: {}", StringValueCStr(error_val));
        return recipe;
    }

    recipe.name = get_string("name");
    recipe.version = get_string("version");
    recipe.description = get_string("description");
    recipe.homepage = get_string("homepage");
    recipe.license = get_string("license");
    recipe.url = get_string("url");
    recipe.sha256 = get_string("sha256");
    recipe.keg_only = get_bool("keg_only");
    recipe.dependencies = get_string_array("dependencies");
    recipe.build_dependencies = get_string_array("build_dependencies");

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

    if (!RB_TYPE_P(result, RUBY_T_HASH)) {
        SPDLOG_ERROR("formula extractor returned non-hash for {}", filename);
        return std::nullopt;
    }

    return value_to_recipe(result);
}

bool RubyRuntime::is_initialized() const { return m->initialized; }

} // namespace den
