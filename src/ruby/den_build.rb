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
end

formula_file = ARGV[0] or abort "usage: den_build.rb <formula_file> <store_path> <den_home> [<version> [<name>]]"
store_path = Pathname.new(ARGV[1])
den_home = Pathname.new(ARGV[2])
want_version = ARGV[3].to_s.empty? ? nil : ARGV[3]
want_name = ARGV[4].to_s.empty? ? nil : ARGV[4]

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
f.define_singleton_method(:version) { PkgVersion.parse(version) rescue version }
f.define_singleton_method(:pkg_version) { PkgVersion.parse(version) rescue version }

# rpath for macOS dylibs.
f.define_singleton_method(:rpath) { |**opts|
  origin = opts[:source] || "bin"
  target = opts[:target] || "lib"
  "@loader_path/#{Pathname.new(origin).relative_path_from(Pathname.new(target))}/../lib" rescue "@loader_path/../lib"
}

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

# Same contract as merge_destdir_stage in src/build/opt_prefix_build.cpp.
den_merge_stage = lambda do
  next unless stage.exist?
  staged_keg = stage / opt_path.relative_path_from(Pathname.new("/"))
  staged_etc = stage / (prefix_root / "etc").relative_path_from(Pathname.new("/"))
  staged_var = stage / (prefix_root / "var").relative_path_from(Pathname.new("/"))
  kept = [staged_keg, staged_etc, staged_var].map(&:to_s)
  stray = []
  Find.find(stage.to_s) do |path|
    if kept.any? { |k| path == k || path.start_with?("#{k}/") }
      Find.prune
    elsif !File.lstat(path).directory?
      stray << path.delete_prefix(stage.to_s)
    end
  end
  unless stray.empty?
    stage.rmtree
    abort "error: source build installed files outside its keg (expected everything under " \
          "#{opt_path}):\n  #{stray.join("\n  ")}"
  end
  den_merge_tree.call(staged_keg, dest, true) if staged_keg.directory?
  den_merge_tree.call(staged_etc, prefix_root / "etc", false) if staged_etc.directory?
  den_merge_tree.call(staged_var, prefix_root / "var", false) if staged_var.directory?
  stage.rmtree
end

f.define_singleton_method(:system) do |cmd, *args|
  cmd = den_rewrite.call(cmd)
  args = args.map { |a| den_rewrite.call(a) }
  kind = den_install_kind.call([cmd, *args])
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
  begin
    super(cmd, *args)
  ensure
    saved_env.each { |k, v| ENV[k] = v }
  end
  den_merge_stage.call if kind
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

# The cache name keeps the URL's archive suffix (see source_archive_suffix in
# src/build/opt_prefix_build.cpp); tar detects the format from content.
archive_suffix = begin
  base = url.to_s.sub(/[?#].*\z/m, "").split("/").last.to_s.downcase
  %w[.tar.gz .tar.xz .tar.bz2 .tar.zst .tar.lz .tar.lzma .tar.z .tgz .txz .tbz2 .tbz .tar .zip .7z]
    .find { |sfx| base.length > sfx.length && base.end_with?(sfx) } || ".src"
end
tarball = den_home / "cache" / "sources" / "#{name}-#{version}#{archive_suffix}"
tarball.dirname.mkpath

# A cached tarball is only reused if it still matches; otherwise fetch again.
if tarball.exist? && sha256_of.call(tarball) != expected_sha256
  $stderr.puts "==> Cached source for #{name} does not match its sha256; downloading again"
  tarball.delete
end

unless tarball.exist?
  $stderr.puts "==> Downloading #{url}"
  partial = Pathname.new("#{tarball}.download")
  partial.delete if partial.exist?
  system("curl", "-fsSL", "-o", partial.to_s, url) or abort "download failed"
  actual = sha256_of.call(partial)
  if actual != expected_sha256
    partial.delete
    abort "error: SHA256 mismatch for #{name} source tarball\n" \
          "  expected: #{expected_sha256}\n  actual:   #{actual}\n  url:      #{url}"
  end
  partial.rename(tarball)
end

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

# --- Post-install: fix dylib paths ---

if OS.mac?
  $stderr.puts "==> Fixing dylib paths"
  Dir.glob("#{dest}/lib/**/*.dylib").each do |dylib|
    basename = File.basename(dylib)
    system("install_name_tool", "-id", "@rpath/#{basename}", dylib)
    system("install_name_tool", "-add_rpath", "#{dest}/lib", dylib, err: "/dev/null")
  end

  Dir.glob("#{dest}/bin/*").each do |bin_file|
    next unless File.executable?(bin_file) && !File.symlink?(bin_file)
    system("install_name_tool", "-add_rpath", "#{dest}/lib", bin_file, err: "/dev/null")
    lib_paths.each { |lp| system("install_name_tool", "-add_rpath", lp, bin_file, err: "/dev/null") }
  end
end

# Clean up.
build_dir.rmtree rescue nil

$stderr.puts "==> Built #{name} #{version}"

# Output for den's C++ code.
puts "name=#{name}"
puts "version=#{version}"
puts "prefix=#{dest}"
puts "status=ok"
