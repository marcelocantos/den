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
  # Homebrew's layout: HOMEBREW_LIBRARY/Homebrew is the library itself (den
  # links ruby/Library/Homebrew to ruby/homebrew).
  ENV["HOMEBREW_LIBRARY"] = File.expand_path("Library", bundle_dir)
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
