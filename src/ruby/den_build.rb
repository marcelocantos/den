#!/usr/bin/env ruby
# Copyright 2026 Marcelo Cantos
# SPDX-License-Identifier: Apache-2.0
#
# Build a Homebrew formula into den's store.
# Works in two modes:
#   1. Via `brew ruby` (Homebrew already loaded)
#   2. Standalone with bundled Portable Ruby (den's embedded bundle)
#
# The formula Ruby source is passed as a file (from `brew cat`).
#
# Usage: den_build.rb <formula_file> <store_path> <den_home> [<version> [<name>]]
# den passes the version and keg name it expects, so the keg lands exactly
# where den looks for it.

# Bootstrap: if Homebrew's library isn't loaded yet, load it.
unless defined?(Formula)
  # Load env vars from the bundled snapshot (generated from a real brew
  # invocation) — only when den did not supply the environment itself. The
  # snapshot's paths name the machine that captured it (/opt/homebrew, its
  # home directory), so den derives those from its own configuration and sets
  # DEN_RUBY_ENV=1; backfilling here would reintroduce them.
  env_file = File.expand_path("homebrew_env.txt", File.dirname(__FILE__))
  if ENV["DEN_RUBY_ENV"] != "1" && File.exist?(env_file)
    File.readlines(env_file).each do |line|
      line.strip!
      next if line.empty? || line.start_with?("#")
      key, value = line.split("=", 2)
      ENV[key] ||= value if key && value
    end
  end

  # Override paths to point at den's locations.
  bundle_dir = File.dirname(__FILE__)
  ENV["HOMEBREW_LIBRARY"] = File.expand_path("homebrew", bundle_dir)
  ENV["HOMEBREW_BREW_FILE"] ||= "/dev/null"
  ENV["HOMEBREW_ORIGINAL_BREW_FILE"] ||= "/dev/null"
  ENV["HOMEBREW_NO_ANALYTICS"] = "1"
  ENV["HOMEBREW_NO_AUTO_UPDATE"] = "1"
  ENV["HOMEBREW_NO_BOOTSNAP"] = "1" # Skip startup accelerator — not needed.

  # Disable gem loading to avoid hardcoded gem paths.
  ENV["GEM_HOME"] = File.expand_path("gems", bundle_dir)
  ENV["GEM_PATH"] = ENV["GEM_HOME"]

  # Load Homebrew's full library via its own entry point.
  require "startup"
  require "global"
  require "formula"
  require "formulary"

  # 🎯T45: no xcrun during a source build. A failed Formula#system dumps
  # Homebrew's system config into the build log, and on arm64 that runs
  # `xcrun --find metal`. A failed install step can be retried (an install
  # step that ignores DESTDIR), so leave the Metal toolchain out of the dump.
  require "system_config"
  if SystemConfig.respond_to?(:metal_toolchain)
    SystemConfig.singleton_class.prepend(Module.new do
      def metal_toolchain = nil
    end)
  end
end

formula_file = ARGV[0] or abort "usage: den_build.rb <formula_file> <store_path> <den_home> [<version> [<name>]]"
store_path = Pathname.new(ARGV[1])
den_home = Pathname.new(ARGV[2])
want_version = ARGV[3].to_s.empty? ? nil : ARGV[3]
want_name = ARGV[4].to_s.empty? ? nil : ARGV[4]

# Build sandbox (docs/source-builds.md): den passes a Seatbelt profile that
# denies writes to its prefix and $DEN_HOME outside this build's directories.
# Every command the formula runs through Formula#system executes under it;
# the formula's own Ruby file operations and the stage merge do not.
den_sandbox_profile = ENV.delete("DEN_BUILD_SANDBOX_PROFILE").to_s
den_sandbox_profile = nil if den_sandbox_profile.empty?

# --- Scan den's store for installed packages ---

installed = {} # name -> Pathname (latest version dir)
if store_path.directory?
  store_path.children.select(&:directory?).each do |pkg_dir|
    name = pkg_dir.basename.to_s
    versions = pkg_dir.children.select(&:directory?).sort
    installed[name] = versions.last if versions.any?
  end
end

# den's prefix: the Cellar's parent, as for ensure_prefix_opt_link.
prefix_root = store_path.parent

# --- Patch Formula[] to resolve deps from den's store ---
#
# A dependency's opt paths are its stable <prefix>/opt/<dep> link, so what a
# build bakes in (rpaths, pkg-config, wrapper scripts) survives the dep's
# upgrades. A dep without an opt link falls back to its keg.

original_bracket = Formula.method(:[])
Formula.define_singleton_method(:[]) do |name|
  f = original_bracket.call(name)
  den_path = installed[f.name] || installed[name.to_s]
  if den_path
    dep_name = den_path.parent.basename.to_s
    dep_opt = prefix_root / "opt" / dep_name
    dep_opt = den_path unless dep_opt.directory?
    f.define_singleton_method(:opt_prefix) { dep_opt }
    f.define_singleton_method(:prefix) { den_path }
    f.define_singleton_method(:opt_bin) { dep_opt / "bin" }
    f.define_singleton_method(:opt_sbin) { dep_opt / "sbin" }
    f.define_singleton_method(:opt_lib) { dep_opt / "lib" }
    f.define_singleton_method(:opt_include) { dep_opt / "include" }
    f.define_singleton_method(:opt_share) { dep_opt / "share" }
    f.define_singleton_method(:opt_libexec) { dep_opt / "libexec" }
    f.define_singleton_method(:opt_pkgshare) { dep_opt / "share" / dep_name }
    f.define_singleton_method(:opt_frameworks) { dep_opt / "Frameworks" }
    f.define_singleton_method(:cellar) { den_path }
    f.define_singleton_method(:bin) { den_path / "bin" }
    f.define_singleton_method(:lib) { den_path / "lib" }
    f.define_singleton_method(:include) { den_path / "include" }
    f.define_singleton_method(:share) { den_path / "share" }
  end
  f
end

# --- Formula DSL newer than the bundled Homebrew library ---
#
# post_install_steps declares the steps brew applies after pouring (openssl's
# cert.pem symlink). den reads them from the formula API and applies them
# itself when the keg is linked (src/env/post_install.cpp), so the build only
# needs the class body to accept the block.
unless Formula.respond_to?(:post_install_steps)
  Formula.define_singleton_method(:post_install_steps) { |*_args, **_opts, &_block| nil }
end

# --- Load and evaluate the formula source ---

source = File.read(formula_file)
eval(source, TOPLEVEL_BINDING, formula_file)

klass = ObjectSpace.each_object(Class).select { |c| c < Formula }.last
abort "error: no Formula subclass found in #{formula_file}" unless klass

# Get metadata from the API-loaded formula (has version info).
name = want_name || klass.name.gsub(/([a-z\d])([A-Z])/, '\\1-\\2').downcase
api_formula = want_version ? nil : (Formulary.factory(name) rescue nil)
version = want_version || api_formula&.pkg_version&.to_s || klass.stable&.version&.to_s || "0.0.0"

dest = store_path / name / version
$stderr.puts "==> #{name} #{version} -> #{dest}"

# Opt-prefix mode (docs/source-builds.md): the build is configured for the
# stable opt path, and install steps stage through DESTDIR into the keg.
opt_path = prefix_root / "opt" / name
stage = den_home / "cache" / "build" / "#{name}-#{version}.destdir"

# --- Create formula instance via allocate (bypass abstract! check) ---

f = klass.allocate
f.instance_variable_set(:@name, name)

# Override all path methods to point at den's store.
f.define_singleton_method(:prefix) { dest }
f.define_singleton_method(:opt_prefix) { opt_path }
f.define_singleton_method(:cellar) { dest }
f.define_singleton_method(:bin) { dest / "bin" }
f.define_singleton_method(:sbin) { dest / "sbin" }
f.define_singleton_method(:lib) { dest / "lib" }
f.define_singleton_method(:include) { dest / "include" }
f.define_singleton_method(:share) { dest / "share" }
f.define_singleton_method(:man) { dest / "share" / "man" }
f.define_singleton_method(:man1) { dest / "share" / "man" / "man1" }
f.define_singleton_method(:man3) { dest / "share" / "man" / "man3" }
f.define_singleton_method(:man5) { dest / "share" / "man" / "man5" }
f.define_singleton_method(:man7) { dest / "share" / "man" / "man7" }
f.define_singleton_method(:man8) { dest / "share" / "man" / "man8" }
f.define_singleton_method(:libexec) { dest / "libexec" }
f.define_singleton_method(:pkgshare) { dest / "share" / name }
# etc and var are shared prefix directories, as in Homebrew.
f.define_singleton_method(:etc) { prefix_root / "etc" }
f.define_singleton_method(:var) { prefix_root / "var" }
f.define_singleton_method(:elisp) { dest / "share" / "emacs" / "site-lisp" / name }
f.define_singleton_method(:frameworks) { dest / "Frameworks" }
f.define_singleton_method(:kext_prefix) { dest / "Library" / "Extensions" }
f.define_singleton_method(:name) { name }
# As in Homebrew, version is the Version without the revision (formulas
# compare it with a resource's version), and pkg_version carries the revision.
f.define_singleton_method(:version) { PkgVersion.parse(version).version rescue version }
f.define_singleton_method(:pkg_version) { PkgVersion.parse(version) rescue version }

# rpath for macOS dylibs.
f.define_singleton_method(:rpath) { |**opts|
  origin = opts[:source] || "bin"
  target = opts[:target] || "lib"
  "@loader_path/#{Pathname.new(origin).relative_path_from(Pathname.new(target))}/../lib" rescue "@loader_path/../lib"
}

# Path helpers newer than the bundled Homebrew (Utils::Path::FormulaHelpers:
# formula_opt_prefix and friends). Current homebrew-core formulae call them
# (gettext passes formula_opt_prefix("libunistring") to configure), so give
# the formula the same definitions when its Homebrew lacks them.
unless f.respond_to?(:formula_opt_prefix, true)
  den_opt = ->(n) { prefix_root / "opt" / n.to_s.split("/").last }
  f.define_singleton_method(:formula_opt_prefix) { |n| den_opt.call(n) }
  f.define_singleton_method(:formula_opt_bin) { |n| den_opt.call(n) / "bin" }
  f.define_singleton_method(:formula_opt_lib) { |n| den_opt.call(n) / "lib" }
  f.define_singleton_method(:formula_opt_libexec) { |n| den_opt.call(n) / "libexec" }
  f.define_singleton_method(:formula_opt_include) { |n| den_opt.call(n) / "include" }
end

# --- Opt-prefix build mode (G1/G2) ---
#
# The formula's own Ruby file operations (bin.install, rm, inreplace of an
# installed file) work on the keg, so prefix and friends stay the keg path.
# What a build command sees is rewritten to the opt path instead:
#   * system arguments naming the keg (--prefix=, prefix=, std_*_args, ...);
#   * environment values naming the keg, for the duration of the command;
#   * inreplace replacement text.
# Install steps (make install*, cmake --install, ninja/meson install) run with
# DESTDIR=<stage>; the staged <stage><opt path> merges into the keg right after
# the step, so the formula's next line sees the installed files.

require "fileutils"
require "find"
require "shellwords"

den_keg_re = /#{Regexp.escape(dest.to_s)}(?=\/|\z|[^\w.+@-])/
den_rewrite = lambda do |a|
  case a
  when Pathname then a.to_s.match?(den_keg_re) ? Pathname.new(a.to_s.gsub(den_keg_re, opt_path.to_s)) : a
  when String then a.match?(den_keg_re) ? a.gsub(den_keg_re, opt_path.to_s) : a
  else a
  end
end

den_install_kind = lambda do |argv|
  words = argv.flat_map { |a| a.to_s.split }
  words = words.drop_while { |w| w.include?("=") && !w.start_with?("-") }
  tool = File.basename(words.first.to_s)
  rest = words.drop(1)
  case tool
  when "make", "gmake"
    :make if rest.any? { |w| w == "install" || w.start_with?("install-", "install_") }
  when "cmake"
    :env if rest.include?("--install") || rest.include?("--target=install") ||
            rest.each_cons(2).any? { |a, b| %w[--target -t].include?(a) && b == "install" }
  when "ninja", "samu"
    :env if rest.include?("install")
  when "meson"
    :env if rest.first == "install"
  end
end

den_merge_tree = lambda do |src, dst, replace|
  dst.mkpath
  src.children.each do |from|
    to = dst / from.basename
    if from.directory? && !from.symlink? && to.directory? && !to.symlink?
      den_merge_tree.call(from, to, replace)
    elsif to.exist? || to.symlink?
      unless replace
        $stderr.puts "==> Keeping existing #{to}"
        next
      end
      FileUtils.rm_rf(to)
      FileUtils.mv(from, to)
    else
      FileUtils.mv(from, to)
    end
  end
end

den_root = Pathname.new("/")
den_staged_keg = stage / opt_path.relative_path_from(den_root)
den_keg_fallback = ENV.delete("DEN_BUILD_KEG_FALLBACK") == "1"

# Same contract as staged_strays in src/build/opt_prefix_build.cpp.
den_stage_strays = lambda do
  next [] unless stage.exist?
  staged_etc = stage / (prefix_root / "etc").relative_path_from(den_root)
  staged_var = stage / (prefix_root / "var").relative_path_from(den_root)
  kept = [den_staged_keg, staged_etc, staged_var].map(&:to_s)
  stray = []
  Find.find(stage.to_s) do |path|
    if kept.any? { |k| path == k || path.start_with?("#{k}/") }
      Find.prune
    elsif !File.lstat(path).directory?
      stray << path.delete_prefix(stage.to_s)
    end
  end
  stray
end

# Same contract as merge_destdir_stage in src/build/opt_prefix_build.cpp.
den_merge_stage = lambda do
  next unless stage.exist?
  staged_etc = stage / (prefix_root / "etc").relative_path_from(den_root)
  staged_var = stage / (prefix_root / "var").relative_path_from(den_root)
  stray = den_stage_strays.call
  unless stray.empty?
    stage.rmtree
    abort "error: source build installed files outside its keg (expected everything under " \
          "#{opt_path}):\n  #{stray.join("\n  ")}"
  end
  den_merge_tree.call(den_staged_keg, dest, true) if den_staged_keg.directory?
  den_merge_tree.call(staged_etc, prefix_root / "etc", false) if staged_etc.directory?
  den_merge_tree.call(staged_var, prefix_root / "var", false) if staged_var.directory?
  stage.rmtree
end

# Same contract as relocate_staged_keg in src/build/opt_prefix_build.cpp.
den_relocate_staged = lambda do
  from = den_staged_keg.to_s
  binary = []
  Find.find(from) do |path|
    st = File.lstat(path)
    if st.symlink?
      target = File.readlink(path)
      next unless target.include?(from)
      File.unlink(path)
      File.symlink(target.gsub(from, opt_path.to_s), path)
    elsif st.file?
      data = File.binread(path)
      next unless data.include?(from)
      if data.include?("\0")
        binary << path.delete_prefix("#{from}/")
        next
      end
      File.binwrite(path, data.gsub(from, opt_path.to_s))
      File.chmod(st.mode & 07777, path)
    end
  end
  unless binary.empty?
    abort "error: source build baked its staging path into binary files, which den can't " \
          "relocate (see docs/source-builds.md):\n  #{binary.join("\n  ")}"
  end
end

# Defined below; the install-step retry checks the live opt path with them.
den_opt_state = nil
opt_before = nil
den_opt_settle = lambda do
  now = den_opt_state.call
  next true if now == opt_before
  next false unless opt_before.nil?
  FileUtils.rm_rf(opt_path)
  true
end

den_opt_re = /#{Regexp.escape(opt_path.to_s)}(?=\/|\z|[^\w.+@-])/
den_retarget = lambda do |a|
  case a
  when Pathname then Pathname.new(a.to_s.gsub(den_opt_re, den_staged_keg.to_s))
  when String then a.gsub(den_opt_re, den_staged_keg.to_s)
  else a
  end
end

# pkgconf's default search path (--with-pkg-config-dir=, baked in at
# configure time): Homebrew's formula lists /usr/local/lib/pkgconfig and its
# library's system .pc set, which for den is the Ruby bundle under DEN_HOME.
# den drops /usr/local and points the system set at the copy it owns under the
# prefix (write_system_pkgconfig in src/env/post_install.h, run when pkgconf
# lands in the prefix).
den_ruby_os_re = %r{\A#{Regexp.escape((den_home / "ruby").to_s)}/(?:.*/)?os/}
den_pc_os = (prefix_root / "Library" / "Homebrew" / "os").to_s
den_pc_dirs = lambda do |a|
  next a unless a.is_a?(String) || a.is_a?(Pathname)
  m = a.to_s.match(/\A((?:-D|--)with-pkg-config-dir=)(.*)\z/m)
  next a unless m
  dirs = m[2].split(File::PATH_SEPARATOR).reject { |d| d == "/usr/local" || d.start_with?("/usr/local/") }
  dirs = dirs.map { |d| d.sub(den_ruby_os_re, "#{den_pc_os}/") }
  "#{m[1]}#{dirs.uniq.join(File::PATH_SEPARATOR)}"
end

f.define_singleton_method(:system) do |cmd, *args|
  cmd = den_pc_dirs.call(den_rewrite.call(cmd))
  args = args.map { |a| den_pc_dirs.call(den_rewrite.call(a)) }
  kind = den_install_kind.call([cmd, *args])
  plain_cmd = cmd
  plain_args = args
  saved_env = {}
  ENV.to_h.each do |k, v|
    next unless v.match?(den_keg_re)
    saved_env[k] = v
    ENV[k] = v.gsub(den_keg_re, opt_path.to_s)
  end
  if kind == :make
    if args.empty? && cmd.to_s.include?(" ")
      cmd = "#{cmd} DESTDIR=#{Shellwords.escape(stage.to_s)}"
    else
      args += ["DESTDIR=#{stage}"]
    end
  elsif kind == :env
    saved_env["DESTDIR"] = ENV.fetch("DESTDIR", nil) unless saved_env.key?("DESTDIR")
    ENV["DESTDIR"] = stage.to_s
  end
  failure = nil
  begin
    super(cmd, *args)
  rescue StandardError => e
    raise unless kind
    failure = e
  ensure
    saved_env.each { |k, v| ENV[k] = v }
  end
  next unless kind

  # An install step that ignored DESTDIR (tree's Makefile uses it as its bin
  # directory): unless the formula bakes a fixed prefix, retry it brew-style
  # into the staging keg <stage><opt>, then relocate text naming it to <opt>.
  # Same rule as run_step in src/build/source_build.cpp.
  ignored = (failure && den_sandbox_profile) ||
            (!failure && (!den_stage_strays.call.empty? || den_opt_state.call != opt_before))
  if ignored && den_keg_fallback && den_opt_settle.call
    stage.rmtree if stage.exist?
    den_staged_keg.mkpath
    $stderr.puts "==> Install step ignored DESTDIR; installing into a staging keg"
    saved_env = {}
    ENV.to_h.each do |k, v|
      next unless v.match?(den_keg_re) || v.match?(den_opt_re)
      saved_env[k] = v
      ENV[k] = den_retarget.call(v.gsub(den_keg_re, opt_path.to_s))
    end
    if kind == :env
      saved_env["DESTDIR"] = ENV.fetch("DESTDIR", nil) unless saved_env.key?("DESTDIR")
      ENV.delete("DESTDIR")
    end
    begin
      super(den_retarget.call(plain_cmd), *plain_args.map { |a| den_retarget.call(a) })
    ensure
      saved_env.each { |k, v| ENV[k] = v }
    end
    den_relocate_staged.call
  elsif failure
    raise failure
  end
  den_merge_stage.call
end

# Formula#system forks, sets up logging, and calls exec_cmd in the child,
# which ends in Kernel.exec(cmd, *args). In that child only, Kernel.exec runs
# the command under sandbox-exec, as Homebrew's own sandbox wraps commands; a
# lone string goes through /bin/sh, as Kernel.exec would run it.
if den_sandbox_profile
  unless f.respond_to?(:exec_cmd, true)
    abort "error: this Homebrew library has no Formula#exec_cmd, so den cannot sandbox the build"
  end
  f.define_singleton_method(:exec_cmd) do |cmd, args, out, log_filename|
    profile = den_sandbox_profile
    Kernel.singleton_class.prepend(Module.new do
      define_method(:exec) do |c, *a|
        c = c.first if c.is_a?(Array)
        if a.empty?
          super("/usr/bin/sandbox-exec", "-p", profile, "/bin/sh", "-c", c.to_s)
        else
          super("/usr/bin/sandbox-exec", "-p", profile, c.to_s, *a.map(&:to_s))
        end
      end
    end)
    super(cmd, args, out, log_filename)
  end
end

f.define_singleton_method(:inreplace) do |paths, before = nil, after = nil, **opts, &block|
  super(paths, before, den_rewrite.call(after), **opts, &block)
end

# The live opt path must come through the build untouched: an install step
# that ignores DESTDIR would otherwise write into the linked (older) keg.
den_opt_state = lambda do
  next nil unless opt_path.exist? || opt_path.symlink?
  files = []
  if opt_path.directory?
    root = "#{opt_path}/"
    Find.find(root) do |path|
      st = File.lstat(path)
      files << [path.delete_prefix(root), st.symlink? ? File.readlink(path) : st.size,
                st.file? ? st.mtime.to_r : nil]
    end
  end
  [opt_path.symlink?, opt_path.symlink? ? opt_path.readlink.to_s : nil, files.sort_by(&:first)]
end
opt_before = den_opt_state.call

at_exit { stage.rmtree if stage.exist? }

# --- Set up build environment ---

# Formulae call Homebrew's ENV helpers (ENV.append_to_cflags, ENV.append,
# ENV.deparallelize, ENV.cxx11, ...). Give ENV the methods of Homebrew's
# standard build environment, as `brew install --env=std` does. den sets the
# build's paths and flags itself, so setup_build_environment is not run.
require "extend/ENV"
unless ENV.respond_to?(:append_to_cflags)
  ENV.extend(Stdenv)
  # Stdenv's sig rejects the plain `ENV.deparallelize` (no block) that
  # formulae use; Superenv's accepts it. Same behaviour, no sig.
  module DenStdenvFixes
    def deparallelize
      old = self["MAKEFLAGS"]
      remove "MAKEFLAGS", /-j\d+/
      if block_given?
        begin
          yield
        ensure
          self["MAKEFLAGS"] = old
        end
      end
      old
    end
  end
  ENV.extend(DenStdenvFixes)
end

# Formulae hand the compiler to make themselves (git: "CC=#{ENV.cc}").
# Homebrew's build environment always names one; without it ENV.cc is nil
# and the command line gets an empty CC=, which overrides make's default
# and breaks every compile. Name the platform compiler unless the caller did.
ENV["CC"] = (RUBY_PLATFORM.include?("darwin") ? "clang" : "cc") if ENV["CC"].to_s.empty?
ENV["CXX"] = (RUBY_PLATFORM.include?("darwin") ? "clang++" : "c++") if ENV["CXX"].to_s.empty?

lib_paths = installed.values.map { |p| (p / "lib").to_s }.select { |p| File.directory?(p) }
inc_paths = installed.values.map { |p| (p / "include").to_s }.select { |p| File.directory?(p) }
pc_paths = installed.values.flat_map { |p|
  [(p / "lib" / "pkgconfig").to_s, (p / "share" / "pkgconfig").to_s]
}.select { |p| File.directory?(p) }

ENV["PKG_CONFIG_PATH"] = (pc_paths + [ENV["PKG_CONFIG_PATH"]].compact).join(":")
ENV["CPATH"] = (inc_paths + [ENV["CPATH"]].compact).join(":")
ENV["LIBRARY_PATH"] = (lib_paths + [ENV["LIBRARY_PATH"]].compact).join(":")
bin_paths = installed.values.map { |p| (p / "bin").to_s }.select { |p| File.directory?(p) }
ENV["PATH"] = (bin_paths + [ENV["PATH"]].compact).join(":")

# --- Download and extract source ---

# Take the URL and its checksum from the same spec so they always match.
stable = (api_formula&.stable&.url ? api_formula.stable : klass.stable) rescue nil
url = stable&.url
abort "error: no source URL for #{name}" unless url
checksum = stable.checksum rescue nil
expected_sha256 = (checksum.respond_to?(:hexdigest) ? checksum.hexdigest : checksum).to_s.downcase
unless expected_sha256.match?(/\A[0-9a-f]{64}\z/)
  abort "error: no sha256 for #{name} source; refusing to build an unverified tarball"
end

require "digest"
sha256_of = ->(path) { Digest::SHA256.file(path.to_s).hexdigest }

# Every resource and external patch must carry a sha256 as well (Homebrew
# only warns when one has none). den fetches and verifies each resource
# itself before the build (below); patches still go through Homebrew's
# downloader, which verifies them. den records them all in the keg's receipt.
den_hex_of = lambda do |checksum|
  (checksum.respond_to?(:hexdigest) ? checksum.hexdigest : checksum).to_s.downcase
end
den_resources = []
(stable.resources.values rescue []).each do |r|
  sha = den_hex_of.call(r.checksum)
  unless sha.match?(/\A[0-9a-f]{64}\z/)
    abort "error: no sha256 for #{name} resource #{r.name}; refusing to build with an unverified resource"
  end
  den_resources << [r.name, r.url.to_s, sha]
end
den_patches = []
(stable.patches rescue []).each do |p|
  next unless p.respond_to?(:external?) && p.external?
  res = p.respond_to?(:resource) ? p.resource : p
  sha = den_hex_of.call(res.checksum)
  unless sha.match?(/\A[0-9a-f]{64}\z/)
    abort "error: no sha256 for #{name} patch #{p.url}; refusing to build with an unverified patch"
  end
  den_patches << [p.url.to_s, sha]
end

# The cache name keeps the URL's archive suffix (see source_archive_suffix in
# src/build/opt_prefix_build.cpp); tar detects the format from content.
den_archive_suffix = lambda do |u|
  base = u.to_s.sub(/[?#].*\z/m, "").split("/").last.to_s.downcase
  %w[.tar.gz .tar.xz .tar.bz2 .tar.zst .tar.lz .tar.lzma .tar.z .tgz .txz .tbz2 .tbz .tar .zip .7z]
    .find { |sfx| base.length > sfx.length && base.end_with?(sfx) } || ".src"
end

# Fetch `u` into `path` unless a cached copy still matches `sha`; a download
# that does not match is discarded.
den_fetch_verified = lambda do |u, sha, path, what|
  path.dirname.mkpath
  # A cached file is only reused if it still matches; otherwise fetch again.
  if path.exist? && sha256_of.call(path) != sha
    $stderr.puts "==> Cached #{what} does not match its sha256; downloading again"
    path.delete
  end
  next if path.exist?

  $stderr.puts "==> Downloading #{u}"
  partial = Pathname.new("#{path}.download")
  partial.delete if partial.exist?
  Kernel.system("curl", "-fsSL", "-o", partial.to_s, u.to_s) or abort "download failed: #{u}"
  actual = sha256_of.call(partial)
  if actual != sha
    partial.delete
    abort "error: SHA256 mismatch for #{what}\n" \
          "  expected: #{sha}\n  actual:   #{actual}\n  url:      #{u}"
  end
  partial.rename(path)
end

# Unpack `archive` into the empty directory `into` (or copy a plain file in,
# named after its URL) and return the directory a build step works in: the
# archive's single top-level directory if it has one, as Homebrew does.
den_unpack = lambda do |archive, into, u|
  into.mkpath
  case den_archive_suffix.call(u)
  when ".zip"
    Kernel.system("unzip", "-q", archive.to_s, "-d", into.to_s) or abort "extraction failed: #{archive}"
  when ".src"
    plain = File.basename(u.to_s.sub(/[?#].*\z/m, ""))
    plain = archive.basename.to_s if plain.empty?
    FileUtils.cp(archive, into / plain)
  else
    Kernel.system("tar", "xf", archive.to_s, "-C", into.to_s) or abort "extraction failed: #{archive}"
  end
  entries = into.children
  entries.length == 1 && entries.first.directory? && !entries.first.symlink? ? entries.first : into
end

archive_suffix = den_archive_suffix.call(url)
tarball = den_home / "cache" / "sources" / "#{name}-#{version}#{archive_suffix}"
den_fetch_verified.call(url, expected_sha256, tarball, "#{name} source tarball")

# --- Resources ---
#
# Every resource is fetched and checksummed into den's source cache before
# the build starts (cache/sources/<name>-<version>--<resource><suffix>), and
# its stage reads that file, so Homebrew's downloader and cache are never
# used. Staging follows Resource#stage: with a block, the block runs inside
# the unpacked resource; with a target (`man.install resource("man")`), its
# contents move into the target.
den_safe_name = ->(s) { s.to_s.gsub(/[^A-Za-z0-9._+@-]+/, "-") }
den_resource_stage_root = den_home / "cache" / "build" / "#{name}-#{version}.resources"
den_stage_context = Struct.new(:resource, :staging)
(stable.resources.values rescue []).each do |r|
  unless (r.patches rescue []).empty?
    abort "error: #{name} resource #{r.name} carries patches, which den source builds do not apply yet"
  end
  r_url = r.url.to_s
  r_sha = den_hex_of.call(r.checksum)
  r_file = den_home / "cache" / "sources" /
           "#{name}-#{version}--#{den_safe_name.call(r.name)}#{den_archive_suffix.call(r_url)}"
  den_fetch_verified.call(r_url, r_sha, r_file, "#{name} resource #{r.name}")

  r.define_singleton_method(:cached_download) { r_file }
  r.define_singleton_method(:downloaded?) { true }
  r.define_singleton_method(:fetch) { |**_opts| r_file }
  r.define_singleton_method(:stage) do |target = nil, **_opts, &block|
    raise ArgumentError, "Target directory or block is required" if !target && !block

    dir = den_resource_stage_root / den_safe_name.call(r.name)
    dir.rmtree if dir.exist?
    begin
      root = den_unpack.call(r_file, dir, r_url)
      Dir.chdir(root) do
        if block
          block.call(den_stage_context.new(r, root))
        else
          Pathname(target).install root.children
        end
      end
    ensure
      dir.rmtree if dir.exist?
      den_resource_stage_root.rmdir if den_resource_stage_root.exist? && den_resource_stage_root.empty?
    end
  end
end
at_exit { den_resource_stage_root.rmtree if den_resource_stage_root.exist? }

# The formula sees the same spec, so resource("x") and resources are the
# instances staged from den's cache, and `build.stable?` answers as it does
# in Homebrew.
f.instance_variable_set(:@active_spec, stable)
f.instance_variable_set(:@active_spec_sym, :stable)
f.instance_variable_set(:@build, (stable.build rescue nil))
f.define_singleton_method(:resource) do |res_name = nil, *rest, &blk|
  next super(res_name, *rest, &blk) if res_name.nil? || blk

  stable.resources.fetch(res_name.to_s) { raise "#{name} has no resource #{res_name}" }
end
f.define_singleton_method(:resources) { stable.resources.values }

build_dir = den_home / "cache" / "build" / "#{name}-#{version}"
build_dir.rmtree if build_dir.exist?
build_dir.mkpath

$stderr.puts "==> Extracting"
if archive_suffix == ".zip"
  system("unzip", "-q", tarball.to_s, "-d", build_dir.to_s) or abort "extraction failed"
else
  system("tar", "xf", tarball.to_s, "-C", build_dir.to_s) or abort "extraction failed"
end

entries = build_dir.children.select(&:directory?)
src_dir = entries.length == 1 ? entries.first : build_dir

# --- Build ---

dest.mkpath

$stderr.puts "==> Building"
Dir.chdir(src_dir) do
  f.define_singleton_method(:buildpath) { src_dir }
  f.instance_variable_set(:@buildpath, src_dir)
  f.install
end

if den_opt_state.call != opt_before
  FileUtils.rm_rf(opt_path) if opt_before.nil? # created by the build: never leave it in place
  dest.rmtree if dest.exist?
  abort "error: source build wrote #{opt_path} directly (an install step ignored DESTDIR; " \
        "see docs/source-builds.md)"
end

# Mach-O ids, load commands and rpaths are fixed up by den (C++) after the
# keg is in place: see src/build/macho_fixup.h.

# Clean up.
build_dir.rmtree rescue nil

$stderr.puts "==> Built #{name} #{version}"

# Output for den's C++ code.
puts "name=#{name}"
puts "version=#{version}"
puts "prefix=#{dest}"
# Verified inputs, for the keg's receipt (parse_ruby_build_inputs).
puts "source=#{url}\t#{expected_sha256}"
den_resources.each { |n, u, s| puts "resource=#{n}\t#{u}\t#{s}" }
den_patches.each { |u, s| puts "patch=#{u}\t#{s}" }
puts "status=ok"
