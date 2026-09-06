#include "commands.hpp"

#include "zl/common/executable_path.hpp"

#include <array>
#include <iostream>

#include "git.hpp"
#include "process.hpp"
#include "lockfile.hpp"
#include "manifest.hpp"
#include "resolver.hpp"


namespace zlpkg {
namespace {

std::filesystem::path findZlLanguageBinary(const char* argv0) {
    const auto dir = zl::common::executableDir(argv0);
#ifdef _WIN32
    const std::array<std::filesystem::path, 2> candidates = {
        dir / "zl.exe", dir / "zl_language.exe"
    };
#else
    const std::array<std::filesystem::path, 2> candidates = {
        dir / "zl", dir / "zl_language"
    };
#endif
    for (const auto& candidate : candidates) {
        if (std::filesystem::exists(candidate)) return candidate;
    }
    return {};
}

bool verifyRuntimeCompatibility(const std::filesystem::path& zlBinary) {
    const std::string command = shellQuote(zlBinary.string()) + " --version";
    const auto result = runCaptured(command);
    if (result.exitCode != 0) {
        std::cerr << "zlpkg: could not query runtime version: " << result.output;
        return false;
    }
    const std::string expected = std::string("ZL ") + ZL_VERSION_STRING;
    std::string actual = result.output;
    while (!actual.empty() && (actual.back() == '\n' || actual.back() == '\r')) actual.pop_back();
    if (actual != expected) {
        std::cerr << "zlpkg: runtime version mismatch: expected " << expected
                  << ", found " << actual << "\n";
        return false;
    }
    return true;
}

// Central "make sure the dependency graph is fetched and the lock reflects
// it" routine, shared by both `install` and `run`. When a lock already
// exists and `update` isn't requested, it's trusted as-is (only
// re-fetching a dependency whose cached directory has gone missing, e.g.
// after a clean checkout) - this is what makes repeated `zlpkg install`
// calls both fast and exactly reproducible. `update` (or no lock yet)
// re-runs the full resolver and overwrites the lock.
std::vector<ResolvedDependency> ensureInstalled(const Manifest& manifest, bool update) {
    std::filesystem::path lockPath = manifest.manifestDir / "zlpkg.lock";
    std::filesystem::path cacheRoot = manifest.manifestDir / ".zlpkg";

    if (!update && std::filesystem::exists(lockPath)) {
        auto deps = readLockFile(lockPath);
        bool changed = false;
        for (auto& d : deps) {
            if (d.kind == Dependency::Kind::Path) {
                // Path deps are cheap to re-resolve fresh every time - no
                // fetch involved - so trust the *manifest's own* relative
                // path (d.source) over whatever absolute directory
                // happened to get baked into the lock. This is what keeps
                // a committed zlpkg.lock portable across machines/checkout
                // locations even though the resolved `dir` it records is
                // necessarily absolute.
                std::error_code ec;
                std::filesystem::path resolved = std::filesystem::weakly_canonical(manifest.manifestDir / d.source, ec);
                if (ec || !std::filesystem::exists(resolved)) {
                    throw ResolveError("locked dependency '" + d.name + "': path '" + d.source +
                                        "' does not exist - run `zlpkg install --update`");
                }
                if (resolved != d.dir) {
                    d.dir = resolved;
                    changed = true;
                }
                continue;
            }
            // Git deps: the sha is the source of truth, not the absolute
            // cache path (which is likewise machine/checkout-specific) -
            // only re-fetch if that exact pinned commit isn't present.
            if (!std::filesystem::exists(d.dir)) {
                GitFetchResult restored = restoreGitPin(d.source, d.resolvedRef, cacheRoot, d.name);
                d.dir = restored.dir;
                changed = true;
            }
        }
        if (changed) writeLockFile(lockPath, deps);
        return deps;
    }

    auto deps = resolveAll(manifest);
    writeLockFile(lockPath, deps);
    return deps;
}

} // namespace

int cmdInstall(const std::filesystem::path& projectDir, bool update) {
    std::filesystem::path manifestPath = projectDir / "zlpkg.toml";
    try {
        Manifest manifest = loadManifest(manifestPath);
        auto deps = ensureInstalled(manifest, update);

        if (deps.empty()) {
            std::cout << "no dependencies declared in " << manifestPath.string() << "\n";
        } else {
            std::cout << "resolved " << deps.size() << (deps.size() == 1 ? " dependency" : " dependencies")
                       << ":\n";
            for (const auto& d : deps) {
                std::cout << "  " << d.name << " -> ";
                if (d.kind == Dependency::Kind::Git) {
                    std::cout << d.source << " @ "
                              << d.resolvedRef.substr(0, std::min<size_t>(10, d.resolvedRef.size()));
                } else {
                    std::cout << d.dir.string();
                }
                std::cout << "\n";
            }
        }
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "zlpkg install: " << e.what() << "\n";
        return 1;
    }
}

int cmdRun(const std::filesystem::path& projectDir, const std::filesystem::path& entryFile,
           const std::vector<std::string>& programArgs, const char* argv0) {
    std::vector<ResolvedDependency> deps;
    try {
        Manifest manifest = loadManifest(projectDir / "zlpkg.toml");
        deps = ensureInstalled(manifest, false);
    } catch (const std::exception& e) {
        std::cerr << "zlpkg run: " << e.what() << "\n";
        return 1;
    }

    std::filesystem::path zlBinary = findZlLanguageBinary(argv0);
    if (zlBinary.empty()) {
        std::cerr << "zlpkg run: could not find a zl_language executable next to zlpkg\n";
        return 1;
    }

    if (!verifyRuntimeCompatibility(zlBinary)) return 1;

    std::filesystem::path entry = entryFile.is_absolute() ? entryFile : (projectDir / entryFile);
    if (!std::filesystem::exists(entry)) {
        std::cerr << "zlpkg run: entry file '" << entry.string() << "' does not exist\n";
        return 1;
    }

    std::string cmd = shellQuote(zlBinary.string());
    for (const auto& d : deps) {
        cmd += " --root " + shellQuote(d.moduleRoot().string());
    }
    cmd += " " + shellQuote(entry.string());
    for (const auto& a : programArgs) cmd += " " + shellQuote(a);

    return run(cmd);
}

} // namespace zlpkg
