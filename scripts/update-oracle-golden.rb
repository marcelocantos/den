#!/usr/bin/env ruby
# Copyright 2026 Marcelo Cantos
# SPDX-License-Identifier: Apache-2.0
#
# Regenerate oracle golden files for the 🎯T18 soundness oracle.
#
# Reads tests/corpus/simple_formulae.txt, evaluates each named formula from
# the homebrew-core submodule with a minimal Formula stub, and writes
# tests/corpus/golden/<name>.json with the extracted metadata the native
# parser is expected to match.
#
# Rerun this script when:
#   * bumping the tests/corpus/homebrew-core submodule
#   * adding a formula to tests/corpus/simple_formulae.txt
#   * changing the shape of the native parser's ParsedFormula output
#
# Usage: ruby scripts/update-oracle-golden.rb

require "json"
require "fileutils"
require "pathname"

ROOT = Pathname.new(File.expand_path("..", __dir__))
CORPUS = ROOT / "tests" / "corpus"
BREW = CORPUS / "homebrew-core"
GOLDEN = CORPUS / "golden"
BASELINE = CORPUS / "simple_formulae.txt"

unless BREW.directory? && (BREW / "Formula").directory?
  abort "error: homebrew-core submodule not initialised at #{BREW}"
end
abort "error: baseline missing at #{BASELINE}" unless BASELINE.file?

FileUtils.mkdir_p(GOLDEN)

# Sentinel prefix — the native parser receives the same value, so both
# sides substitute identically and produce byte-identical build commands.
# Keep it short and path-legal so tokens like `--prefix=` compare cleanly.
def sentinel_prefix(name)
  "/ORACLE_PREFIX/#{name}"
end

# Minimal Formula stub. Records the sequence of `system` calls and
# ENV mutations performed by install. Ignores everything else — this is
# intentionally lossy: we only care about the fields the native parser
# extracts.
class OracleFormula
  class << self
    attr_reader :_url, :_sha256

    # Track the most recently defined subclass so evaluate() can find
    # the right Formula subclass even when earlier evals left stale ones
    # in ObjectSpace. Reset between runs.
    attr_accessor :last_inherited
    def inherited(subclass)
      super
      OracleFormula.last_inherited = subclass
    end

    def desc(_ = nil); end
    def homepage(_ = nil); end
    def license(*_); end
    def revision(_); end
    def head(_ = nil, **_opts); end
    def mirror(_); end
    def version_scheme(_); end
    def compatibility_version(_); end
    def deny_network_access!(*_); end
    def disable!(**_); end
    def deprecate!(**_); end
    def depends_on(*_, **_opts); end
    def uses_from_macos(*_, **_opts); end
    def keg_only(*_); end
    def bottle(&_block); end
    def livecheck(&_block); end
    def resource(*_, &_block); end
    def patch(*_, &_block); end
    def fails_with(*_, &_block); end
    def pour_bottle?(**_opts, &_block); end
    def link_overwrite(*_); end
    def env(*_); end
    def option(*_); end
    def conflicts_with(*_, **_opts); end
    def on_macos(&_block); end
    def on_linux(&_block); end
    def on_arm(&_block); end
    def on_intel(&_block); end
    def on_system(*_, **_opts, &_block); end
    # `test do ... end` blocks. Shadow Kernel#test which would otherwise
    # try to interpret the block as a file-test operation.
    def test(*_, &_block); end
    def service(*_, &_block); end
    def plist_options(*_, **_opts); end
    def cxxstdlib_check(*_); end
    def needs(*_); end
    def allow_network_access!(*_); end
    def go_resource(*_, &_block); end

    def url(s = nil, **_opts)
      @_url = s if s
    end

    def sha256(s = nil)
      # Inside `bottle do ... end` sha256 is called with a hash; that path
      # routes through our bottle stub and doesn't reach here. This call
      # only fires at the top level, for the source checksum.
      @_sha256 = s if s.is_a?(String)
    end
  end

  attr_reader :name, :prefix

  def initialize(name, prefix)
    @name = name
    @prefix = prefix
    @lib = "#{prefix}/lib"
    @bin = "#{prefix}/bin"
    @include = "#{prefix}/include"
    @share = "#{prefix}/share"
    @sbin = "#{prefix}/sbin"
    @libexec = "#{prefix}/libexec"
    @pkgshare = "#{prefix}/share/#{name}"
    @etc = "#{prefix}/etc"
    @var = "#{prefix}/var"
    @man = "#{prefix}/share/man"
    @build_commands = []
    @env_settings = []
  end

  attr_reader :build_commands, :env_settings
  attr_reader :lib, :bin, :include, :share, :sbin, :libexec, :pkgshare, :etc, :var, :man

  def opt_prefix = @prefix
  def buildpath = "/ORACLE_BUILDPATH"

  def system(*args)
    @build_commands << args.flatten.map(&:to_s).join(" ")
  end

  def mkdir_p(path)
    @build_commands << "mkdir -p #{path}"
  end

  # Match the native parser's std_configure_args output exactly.
  def std_configure_args
    ["--disable-debug", "--disable-dependency-tracking",
     "--prefix=#{@prefix}", "--libdir=#{@lib}"]
  end

  def std_cmake_args
    ["-DCMAKE_INSTALL_PREFIX=#{@prefix}",
     "-DCMAKE_BUILD_TYPE=Release",
     "-DCMAKE_FIND_FRAMEWORK=LAST",
     "-DCMAKE_VERBOSE_MAKEFILE=ON",
     "-Wno-dev",
     "-DBUILD_TESTING=OFF"]
  end

  def std_meson_args
    ["--prefix=#{@prefix}", "--libdir=#{@lib}",
     "--buildtype=release", "--wrap-mode=nofallback"]
  end

  def install
    # Subclasses override.
  end
end

def evaluate(name, source_path)
  # Rebind `Formula` to our stub for the duration of eval. eval'd code
  # will see `class X < Formula` and subclass our OracleFormula.
  Object.send(:remove_const, :Formula) if Object.const_defined?(:Formula)
  Object.const_set(:Formula, OracleFormula)

  # Reset the inherited-tracker; the next eval's `class X < Formula`
  # will populate it.
  OracleFormula.last_inherited = nil

  source = File.read(source_path)
  eval(source, TOPLEVEL_BINDING, source_path.to_s)

  # Pick up the subclass defined by this eval — not whatever stale
  # subclasses are still in ObjectSpace from earlier evals.
  klass = OracleFormula.last_inherited
  raise "no Formula subclass found in #{source_path}" unless klass

  instance = klass.new(name, sentinel_prefix(name))
  instance.install

  {
    "url" => klass._url || "",
    "sha256" => klass._sha256 || "",
    "build_commands" => instance.build_commands,
    "env_settings" => instance.env_settings,
  }
end

# Iterate the SIMPLE baseline.
names = File.readlines(BASELINE).map do |line|
  line = line.sub(/#.*/, "").strip
  line.empty? ? nil : line
end.compact

names.each do |name|
  shard = name[0].downcase
  source_path = BREW / "Formula" / shard / "#{name}.rb"
  unless source_path.file?
    warn "skip #{name}: source not found at #{source_path}"
    next
  end

  data = evaluate(name, source_path)
  out_path = GOLDEN / "#{name}.json"
  out_path.write(JSON.pretty_generate(data) + "\n")
  puts "wrote #{out_path.relative_path_from(ROOT)}"
end
