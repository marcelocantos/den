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

void RubyRuntime::init_with_bundle(const fs::path& bundle_dir) {
    std::lock_guard lock(m->mutex);
    if (m->initialized)
        return;

    SPDLOG_INFO("initializing Ruby VM with bundle at {}", bundle_dir.string());

    RUBY_INIT_STACK;
    ruby_init();

    // Set up load paths: Ruby stdlib, Homebrew library, Sorbet runtime.
    auto ruby_lib = bundle_dir / "ruby" / "lib" / "4.0.0";
    auto homebrew_lib = bundle_dir / "homebrew";
    auto sorbet_lib = bundle_dir / "gems" / "sorbet-runtime";

    // Add to $LOAD_PATH.
    VALUE load_path = rb_gv_get("$LOAD_PATH");
    rb_ary_unshift(load_path, rb_str_new_cstr(homebrew_lib.c_str()));
    rb_ary_unshift(load_path, rb_str_new_cstr(sorbet_lib.c_str()));
    rb_ary_unshift(load_path, rb_str_new_cstr(ruby_lib.c_str()));

    // Also add the platform-specific stdlib dir.
    for (const auto& entry : fs::directory_iterator(ruby_lib)) {
        if (entry.is_directory() && entry.path().filename().string().find("darwin") != std::string::npos) {
            rb_ary_push(load_path, rb_str_new_cstr(entry.path().c_str()));
            break;
        }
    }

    // Set HOMEBREW env vars that the library expects.
    auto homebrew_prefix = std::string("/opt/homebrew"); // nominal, not actually used
    rb_eval_string_protect(
        ("ENV['HOMEBREW_PREFIX'] ||= '" + homebrew_prefix + "'\n"
         "ENV['HOMEBREW_CELLAR'] ||= '" + homebrew_prefix + "/Cellar'\n"
         "ENV['HOMEBREW_REPOSITORY'] ||= '" + homebrew_prefix + "'\n"
         "ENV['HOMEBREW_NO_ANALYTICS'] = '1'\n"
         "ENV['HOMEBREW_NO_AUTO_UPDATE'] = '1'\n"
         "ENV['HOMEBREW_REQUIRED_RUBY_VERSION'] ||= '4.0'\n"
         "ENV['HOMEBREW_BUNDLER_VERSION'] ||= '2.6.2'\n"
         "ENV['HOMEBREW_OS_VERSION'] ||= '26.0'\n")
            .c_str(),
        nullptr);

    // Load Homebrew's startup sequence, then the formula infrastructure.
    int state = 0;
    rb_eval_string_protect(
        "require 'standalone/sorbet'\n"
        "require 'extend/blank'\n"
        "require 'os'\n"
        "require 'formula'\n"
        "require 'formulary'\n",
        &state);
    if (state) {
        VALUE err = rb_errinfo();
        VALUE msg = rb_funcall(err, rb_intern("message"), 0);
        SPDLOG_ERROR("failed to load Homebrew library: {}", StringValueCStr(msg));
        VALUE bt = rb_funcall(err, rb_intern("backtrace"), 0);
        if (!NIL_P(bt)) {
            VALUE bt_str = rb_funcall(bt, rb_intern("first"), 1, INT2FIX(5));
            VALUE joined = rb_funcall(bt_str, rb_intern("join"), 1, rb_str_new_cstr("\n"));
            SPDLOG_ERROR("  backtrace:\n{}", StringValueCStr(joined));
        }
        rb_set_errinfo(Qnil);
        // Don't return — partial initialization may still be useful.
        // The build_formula call will fail gracefully.
    }

    m->initialized = true;
    SPDLOG_INFO("Ruby VM initialized with Homebrew library");
}

bool RubyRuntime::build_formula(const std::string& formula_source, const std::string& name,
                                const std::string& version, const fs::path& prefix,
                                const fs::path& src_dir, const fs::path& store) {
    std::lock_guard lock(m->mutex);
    if (!m->initialized)
        return false;

    SPDLOG_INFO("building {} {} via embedded Ruby", name, version);

    // Build the Ruby script that will:
    // 1. Eval the formula source
    // 2. Allocate an instance with den's prefix
    // 3. Run the install method
    std::string script =
        "begin\n"
        // Eval the formula to define the class.
        "  eval($den_formula_source, TOPLEVEL_BINDING, 'formula.rb')\n"
        "  klass = ObjectSpace.each_object(Class).select { |c| c < Formula }.last\n"
        "  raise 'no Formula subclass found' unless klass\n"
        "\n"
        "  f = klass.allocate\n"
        "  f.instance_variable_set(:@name, $den_name)\n"
        "  dest = Pathname.new($den_prefix)\n"
        "  f.define_singleton_method(:prefix) { dest }\n"
        "  f.define_singleton_method(:opt_prefix) { dest }\n"
        "  f.define_singleton_method(:cellar) { dest }\n"
        "  f.define_singleton_method(:bin) { dest / 'bin' }\n"
        "  f.define_singleton_method(:sbin) { dest / 'sbin' }\n"
        "  f.define_singleton_method(:lib) { dest / 'lib' }\n"
        "  f.define_singleton_method(:include) { dest / 'include' }\n"
        "  f.define_singleton_method(:share) { dest / 'share' }\n"
        "  f.define_singleton_method(:man) { dest / 'share' / 'man' }\n"
        "  f.define_singleton_method(:man1) { dest / 'share' / 'man' / 'man1' }\n"
        "  f.define_singleton_method(:libexec) { dest / 'libexec' }\n"
        "  f.define_singleton_method(:pkgshare) { dest / 'share' / $den_name }\n"
        "  f.define_singleton_method(:etc) { dest / 'etc' }\n"
        "  f.define_singleton_method(:var) { dest / 'var' }\n"
        "  f.define_singleton_method(:name) { $den_name }\n"
        "  f.define_singleton_method(:rpath) { |**opts| '@loader_path/../lib' }\n"
        "  f.define_singleton_method(:buildpath) { Pathname.new($den_src_dir) }\n"
        "  f.instance_variable_set(:@buildpath, Pathname.new($den_src_dir))\n"
        "\n"
        "  Dir.chdir($den_src_dir) { f.install }\n"
        "  $den_result = 'ok'\n"
        "rescue => e\n"
        "  $den_result = \"error: #{e.message}\"\n"
        "  $stderr.puts e.backtrace&.first(5)&.join(\"\\n\")\n"
        "end\n";

    // Set global variables for the script.
    rb_gv_set("$den_formula_source", rb_str_new_cstr(formula_source.c_str()));
    rb_gv_set("$den_name", rb_str_new_cstr(name.c_str()));
    rb_gv_set("$den_prefix", rb_str_new_cstr(prefix.c_str()));
    rb_gv_set("$den_src_dir", rb_str_new_cstr(src_dir.c_str()));
    rb_gv_set("$den_result", rb_str_new_cstr("pending"));

    int state = 0;
    rb_eval_string_protect(script.c_str(), &state);
    if (state) {
        VALUE err = rb_errinfo();
        VALUE msg = rb_funcall(err, rb_intern("message"), 0);
        SPDLOG_ERROR("Ruby build error: {}", StringValueCStr(msg));
        rb_set_errinfo(Qnil);
        return false;
    }

    VALUE result = rb_gv_get("$den_result");
    std::string result_str(RSTRING_PTR(result), RSTRING_LEN(result));

    if (result_str == "ok") {
        SPDLOG_INFO("Ruby build succeeded for {} {}", name, version);
        return true;
    }

    SPDLOG_ERROR("Ruby build failed: {}", result_str);
    return false;
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
