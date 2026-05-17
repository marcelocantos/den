class HelloTap < Formula
  desc "A tiny fixture formula for tap management tests"
  homepage "https://example.com/hello-tap"
  url "https://example.com/hello-tap-1.0.0.tar.gz"
  sha256 "aabbccdd" * 8
  version "1.0.0"
  license "Apache-2.0"

  def install
    bin.install "hello-tap"
  end
end
