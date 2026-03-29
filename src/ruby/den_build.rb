#!/usr/bin/env ruby
# Copyright 2026 Marcelo Cantos
# SPDX-License-Identifier: Apache-2.0
#
# Build a Homebrew formula into den's store.
# Run via: brew ruby src/ruby/den_build.rb <formula_file> <store_path> <den_home>
#
# The formula Ruby source is passed as a file (from `brew cat`).
# Homebrew's full library is loaded by `brew ruby`.

require "formula"
require "formulary"

formula_file = ARGV[0] or abort "usage: den_build.rb <formula_file> <store_path> <den_home>"
store_path = Pathname.new(ARGV[1])
den_home = Pathname.new(ARGV[2])

# --- Scan den's store for installed packages ---

installed = {} # name -> Pathname (latest version dir)
if store_path.directory?
  store_path.children.select(&:directory?).each do |pkg_dir|
    name = pkg_dir.basename.to_s
    versions = pkg_dir.children.select(&:directory?).sort
    installed[name] = versions.last if versions.any?
  end
end

# --- Patch Formula[] to resolve deps from den's store ---

original_bracket = Formula.method(:[])
Formula.define_singleton_method(:[]) do |name|
  f = original_bracket.call(name)
  den_path = installed[f.name] || installed[name.to_s]
  if den_path
    f.define_singleton_method(:opt_prefix) { den_path }
    f.define_singleton_method(:prefix) { den_path }
    f.define_singleton_method(:opt_bin) { den_path / "bin" }
    f.define_singleton_method(:opt_lib) { den_path / "lib" }
    f.define_singleton_method(:opt_include) { den_path / "include" }
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
api_formula = Formulary.factory(klass.name.downcase) rescue nil
version = api_formula&.pkg_version&.to_s || "0.0.0"
name = klass.name.downcase

dest = store_path / name / version
$stderr.puts "==> #{name} #{version} -> #{dest}"

# --- Create formula instance via allocate (bypass abstract! check) ---

f = klass.allocate
f.instance_variable_set(:@name, name)

# Override all path methods to point at den's store.
f.define_singleton_method(:prefix) { dest }
f.define_singleton_method(:opt_prefix) { dest }
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
f.define_singleton_method(:etc) { dest / "etc" }
f.define_singleton_method(:var) { dest / "var" }
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

# --- Set up build environment ---

lib_paths = installed.values.map { |p| (p / "lib").to_s }.select { |p| File.directory?(p) }
inc_paths = installed.values.map { |p| (p / "include").to_s }.select { |p| File.directory?(p) }
pc_paths = installed.values.flat_map { |p|
  [(p / "lib" / "pkgconfig").to_s, (p / "share" / "pkgconfig").to_s]
}.select { |p| File.directory?(p) }

ENV["PKG_CONFIG_PATH"] = (pc_paths + [ENV["PKG_CONFIG_PATH"]].compact).join(":")
ENV["CPATH"] = (inc_paths + [ENV["CPATH"]].compact).join(":")
ENV["LIBRARY_PATH"] = (lib_paths + [ENV["LIBRARY_PATH"]].compact).join(":")

# --- Download and extract source ---

url = api_formula&.stable&.url || klass.stable&.url rescue nil
abort "error: no source URL for #{name}" unless url

tarball = den_home / "cache" / "sources" / "#{name}-#{version}.tar.gz"
tarball.dirname.mkpath

unless tarball.exist?
  $stderr.puts "==> Downloading #{url}"
  system("curl", "-fsSL", "-o", tarball.to_s, url) or abort "download failed"
end

build_dir = den_home / "cache" / "build" / "#{name}-#{version}"
build_dir.rmtree if build_dir.exist?
build_dir.mkpath

$stderr.puts "==> Extracting"
system("tar", "xzf", tarball.to_s, "-C", build_dir.to_s) or abort "extraction failed"

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
