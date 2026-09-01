# Homebrew formula for ush - see docs/PACKAGING.md for how to test this
# locally and how to publish it (either via a personal tap or by pointing
# people directly at this file in the repo).
#
# No tagged release exists yet, so there is deliberately no stable `url`/
# `sha256` block below - only `head`, which builds from the tip of `main`
# and is usable right now via `brew install --HEAD --formula Formula/ush.rb`
# (or `brew install --HEAD jacobstuff/ush/ush` once tapped). Once a
# version is tagged and pushed (see the Release GitHub Actions workflow),
# add a stable block pointing at that tag's source tarball with its real
# sha256 - see docs/PACKAGING.md for the exact steps.
class Ush < Formula
  desc "POSIX-compatible shell, built from scratch against POSIX.1-2017"
  homepage "https://github.com/jacobStuff/unnamed-shell"
  license "Unlicense"
  head "https://github.com/jacobStuff/unnamed-shell.git", branch: "main"

  depends_on "cmake" => :build

  def install
    system "cmake", "-S", ".", "-B", "build", "-DUSH_BUILD_TESTS=OFF", *std_cmake_args
    system "cmake", "--build", "build"
    system "cmake", "--install", "build"
  end

  test do
    assert_equal "hello", shell_output("#{bin}/ush -c 'echo hello'").strip
    assert_equal "4", shell_output("#{bin}/ush -c 'echo $((2+2))'").strip
  end
end
