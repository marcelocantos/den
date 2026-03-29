#!/usr/bin/env ruby
# Copyright 2026 Marcelo Cantos
# SPDX-License-Identifier: Apache-2.0
#
# Extract build recipe from a Homebrew formula.
# Usage: ruby extract_formula.rb <formula_name> <homebrew_library_path>
#
# Outputs a line-based key=value format to stdout.
# den's C++ code parses this output.

formula_name = ARGV[0]
homebrew_lib = ARGV[1] || "/opt/homebrew/Library/Homebrew"

# Add Homebrew's library and vendor bundle to the load path.
$LOAD_PATH.unshift(homebrew_lib)

# Add all vendored gems to the load path.
Dir.glob("#{homebrew_lib}/vendor/bundle/ruby/*/gems/*/lib").each { |p| $LOAD_PATH.unshift(p) }

# Use Homebrew's own Sorbet initialization (loads runtime + disables checks).
# This must come first as all Homebrew code uses sig{}.
require "standalone/sorbet"

# Load Homebrew's core extensions (blank?, present?, etc.).
require "extend/blank"

# Set required environment variables for Homebrew's OS module.
ENV["HOMEBREW_OS_VERSION"] ||= `sw_vers -productVersion 2>/dev/null`.strip rescue "26.0"
ENV["HOMEBREW_PREFIX"] ||= "/opt/homebrew"
ENV["HOMEBREW_CELLAR"] ||= "/opt/homebrew/Cellar"
ENV["HOMEBREW_REPOSITORY"] ||= "/opt/homebrew"

# Load OS detection before formula.rb needs it.
require "os"

# Now try to load the formula infrastructure.
begin
  require "formula"
rescue LoadError => e
  # If the full Homebrew library isn't available, use a minimal stub.
  $stderr.puts "warn: couldn't load Homebrew library: #{e.message}"
  $stderr.puts "warn: using minimal formula stub"

  class Formula
    class << self
      attr_reader :_desc, :_homepage, :_license, :_url_str, :_sha256_str,
                  :_deps, :_build_deps, :_uses_from_macos_deps,
                  :_head_str, :_mirror_str, :_keg_only_reason

      def desc(s = nil) = s ? (@_desc = s) : @_desc
      def homepage(s = nil) = s ? (@_homepage = s) : @_homepage
      def license(s = nil) = s ? (@_license = s.to_s) : @_license
      def url(s = nil, **opts) = s ? (@_url_str = s) : @_url_str
      def sha256(s = nil) = s ? (@_sha256_str = s) : @_sha256_str
      def head(s = nil, **opts) = s ? (@_head_str = s) : @_head_str
      def mirror(s = nil) = s ? (@_mirror_str = s) : @_mirror_str
      def revision(n = nil) = n ? (@_revision = n) : @_revision

      def depends_on(dep)
        @_deps ||= []
        if dep.is_a?(Hash)
          dep.each { |k, v| @_deps << k.to_s }
        else
          @_deps << dep.to_s
        end
      end

      def uses_from_macos(dep, **opts)
        @_uses_from_macos_deps ||= []
        name = dep.is_a?(Hash) ? dep.keys.first.to_s : dep.to_s
        @_uses_from_macos_deps << name
      end

      def keg_only(reason, *args)
        @_keg_only_reason = reason.to_s
      end

      def on_macos(&block) = yield if RUBY_PLATFORM.include?("darwin")
      def on_linux(&block) = yield if RUBY_PLATFORM.include?("linux")
      def on_arm(&block) = yield if RUBY_PLATFORM.include?("arm64") || RUBY_PLATFORM.include?("aarch64")
      def on_intel(&block) = yield
      def on_system(_sys, **opts, &block) = nil

      def stable = self
      def deps = @_deps || []
      def build_deps = @_build_deps || []
      def keg_only? = !@_keg_only_reason.nil?
      def bottle(&block) = nil
      def livecheck(&block) = nil
      def resource(name, &block) = nil
      def patch(strip = nil, &block) = nil
      def fails_with(compiler, &block) = nil
      def deprecate!(**opts) = nil
      def disable!(**opts) = nil
      def pour_bottle?(**opts, &block) = nil
      def link_overwrite(*paths) = nil
      def env(*args) = nil
      def option(name, desc = nil) = nil
    end

    attr_reader :name, :prefix, :opt_prefix, :bin, :lib, :include,
                :share, :sbin, :libexec, :pkgshare, :etc, :var

    def initialize(name, prefix = nil)
      @name = name
      @prefix = prefix || "/tmp/den-build/#{name}"
      @opt_prefix = @prefix
      @bin = "#{@prefix}/bin"
      @lib = "#{@prefix}/lib"
      @include = "#{@prefix}/include"
      @share = "#{@prefix}/share"
      @sbin = "#{@prefix}/sbin"
      @libexec = "#{@prefix}/libexec"
      @pkgshare = "#{@prefix}/share/#{name}"
      @etc = "#{@prefix}/etc"
      @var = "#{@prefix}/var"
    end

    def version = self.class._url_str&.match(/[-_](\d+[\d.]*\d+)\./)&.[](1) || "0.0.0"
    def buildpath = Pathname.new(Dir.pwd)
    def cellar = prefix
    def opt_bin = "#{opt_prefix}/bin"
    def opt_lib = "#{opt_prefix}/lib"
    def opt_include = "#{opt_prefix}/include"

    def system(*args)
      @_build_steps ||= []
      @_build_steps << args.map(&:to_s)
    end

    def inreplace(path, old, new_val = nil, &block)
      @_build_steps ||= []
      @_build_steps << ["inreplace", path.to_s, old.to_s, new_val.to_s]
    end

    def build_steps = @_build_steps || []

    def install
      # Subclasses override this.
    end

    # Standard build helpers.
    def std_configure_args
      ["--disable-debug", "--disable-dependency-tracking",
       "--prefix=#{prefix}", "--libdir=#{lib}"]
    end

    def std_cmake_args
      ["-DCMAKE_INSTALL_PREFIX=#{prefix}",
       "-DCMAKE_BUILD_TYPE=Release",
       "-DCMAKE_FIND_FRAMEWORK=LAST",
       "-DCMAKE_VERBOSE_MAKEFILE=ON",
       "-Wno-dev",
       "-DBUILD_TESTING=OFF"]
    end

    def std_meson_args
      ["--prefix=#{prefix}",
       "--libdir=#{lib}",
       "--buildtype=release",
       "--wrap-mode=nofallback"]
    end
  end
end

# Fetch the formula source from the API.
require "json"
require "net/http"
require "uri"

api_url = "https://formulae.brew.sh/api/formula/#{formula_name}.json"
uri = URI(api_url)
response = Net::HTTP.get(uri)
data = JSON.parse(response)

ruby_source_path = data["ruby_source_path"]
unless ruby_source_path
  $stderr.puts "error: no ruby_source_path for #{formula_name}"
  exit 1
end

# Fetch the formula Ruby source from GitHub.
raw_url = "https://raw.githubusercontent.com/Homebrew/homebrew-core/master/#{ruby_source_path}"
formula_source = Net::HTTP.get(URI(raw_url))

# Evaluate the formula.
eval(formula_source, TOPLEVEL_BINDING, ruby_source_path)

# Find the Formula subclass.
klass = ObjectSpace.each_object(Class).select { |c| c < Formula }.last

unless klass
  $stderr.puts "error: no Formula subclass found in #{formula_name}"
  exit 1
end

# Extract metadata.
source_url = klass._url_str || (data.dig("urls", "stable", "url") rescue "")
source_sha256 = klass._sha256_str || (data.dig("urls", "stable", "checksum") rescue "")

puts "name=#{formula_name}"
puts "version=#{data.dig("versions", "stable") || ""}"
puts "description=#{klass._desc || data["desc"] || ""}"
puts "homepage=#{klass._homepage || data["homepage"] || ""}"
puts "license=#{klass._license || ""}"
puts "source_url=#{source_url}"
puts "source_sha256=#{source_sha256}"
puts "keg_only=#{klass.keg_only?}"
puts "dependencies=#{(klass.deps || []).join(",")}"
puts "build_dependencies=#{(klass.build_deps || []).join(",")}"
puts "uses_from_macos=#{(klass._uses_from_macos_deps || []).join(",")}"

# Try to extract build steps by running the install method.
begin
  f = klass.new(formula_name, "/DEN_PREFIX")
  f.install
  f.build_steps.each_with_index do |step, i|
    puts "build_step_#{i}=#{step.join("\t")}"
  end
rescue => e
  puts "build_error=#{e.message}"
end
