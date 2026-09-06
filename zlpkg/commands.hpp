#pragma once
// The two zlpkg subcommands. Both operate on the zlpkg.toml found in
// `projectDir`. zl_language itself stays completely unaware any of this
// exists - see main.cpp's --root flag: these commands' whole job is to
// turn a dependency graph into a flat list of --root paths and hand off
// to the zl_language binary sitting next to this one.

#include <filesystem>
#include <string>
#include <vector>

namespace zlpkg {

// Resolves (or reuses a lock's pinned) dependencies and writes zlpkg.lock.
// `update` forces a fresh resolve even if a lock already exists. Returns
// the process exit code (0 on success).
int cmdInstall(const std::filesystem::path& projectDir, bool update);

// Ensures dependencies are installed (running install first if no lock
// exists yet), then execs `zl_language --root <dep1> --root <dep2> ...
// <entryFile> [programArgs...]`. `entryFile` may be relative to
// projectDir or absolute. `argv0` is zlpkg's own argv[0], used as a last
// resort (alongside platform APIs) to locate the zl_language binary
// expected to sit next to it. Returns zl_language's own exit code (or a
// zlpkg-level error code if it couldn't even get that far).
int cmdRun(const std::filesystem::path& projectDir, const std::filesystem::path& entryFile,
           const std::vector<std::string>& programArgs, const char* argv0);

} // namespace zlpkg
