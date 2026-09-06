# ZL Release Packaging

The release layout is driven by CMake install rules and CPack.

On Linux, `packaging/build-release.sh` produces TGZ, ZIP, and DEB artifacts from a Release build.

On Windows, run `packaging/build-release.ps1` from PowerShell with CMake and Ninja available. The same
CMake/CPack configuration produces a ZIP containing `bin/zl`, `bin/zlpkg`, and the matching `lib/zl/stdlib`.

On macOS, use the same CMake commands as the Linux script but select the native generator available on the
machine, then run `cpack -G TGZ` or `cpack -G ZIP`. The resulting artifact is native to the machine that built it.

Release artifacts must be built on their target OS. This repository does not cross-compile Windows or macOS
binaries from Linux.

## RPM and Homebrew

On Linux hosts with `rpmbuild` installed, CPack automatically adds an RPM generator. The generated RPM
uses the same install tree and version metadata as the DEB/TGZ packages.

A Homebrew formula template is provided at `packaging/homebrew/zl.rb`. Before publishing it to a tap,
replace the repository URL and release tarball SHA-256 with the actual release repository values. The formula
builds ZL natively with CMake/Ninja and runs the installed `zl --version`/`zl --help` smoke test.

## Native Windows installer

The Windows release workflow continues to publish a native ZIP. An MSI/equivalent installer requires a
Windows-native installer toolchain (for example WiX) and should be produced and tested on the Windows runner,
not cross-generated on Linux. The source tree is intentionally prepared for that runner-side addition rather
than claiming an unverified MSI artifact.
