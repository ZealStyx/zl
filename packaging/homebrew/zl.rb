class Zl < Formula
  desc "ZL programming language toolchain"
  homepage "https://github.com/REPLACE_WITH_OWNER/REPLACE_WITH_REPOSITORY"
  url "https://github.com/REPLACE_WITH_OWNER/REPLACE_WITH_REPOSITORY/archive/refs/tags/v#{version}.tar.gz"
  version "0.1.0"
  sha256 "REPLACE_WITH_RELEASE_TARBALL_SHA256"
  license "Proprietary"

  depends_on "cmake" => :build
  depends_on "ninja" => :build

  def install
    system "cmake", "-S", ".", "-B", "build", "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release"
    system "cmake", "--build", "build", "--parallel"
    system "cmake", "--install", "build", "--prefix", prefix
  end

  test do
    system bin/"zl", "--version"
    system bin/"zl", "--help"
  end
end
