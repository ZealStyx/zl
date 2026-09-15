#include "commands.hpp"

#include "zl/common/executable_path.hpp"

#include <array>
#include <iostream>
#include <map>
#include <optional>
#include <set>
#include <vector>

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
    const auto result = runCaptured({zlBinary.string(), "--version"});
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

// Validates a lock file against the current manifest. Returns the usable
// dependency set, or nullopt (with `reason` set) when the lock is stale -
// in which case the caller re-resolves from scratch and rewrites it. The
// manifest is authoritative for every declaration; the lock contributes
// only the resolved git shas. In particular the lock's recorded `dir`
// and (for root deps) `source` are never trusted: both are recomputed,
// so a tampered or cross-machine lock can at worst force a re-resolve,
// never redirect a dependency at an attacker's directory.
//
// Anything that fails validation degrades to "stale" rather than an
// error, EXCEPT a lock whose syntax itself is corrupt (readLockFile
// throws before this runs) - a corrupt lock is a hard error, matching
// ecosystem behavior.
std::optional<std::vector<ResolvedDependency>> tryUseLock(const Manifest& manifest, const LockFile& lock,
        const std::filesystem::path& lockPath, const std::filesystem::path& cacheRoot, std::string& reason) {
    const std::string wantSha = lockManifestFingerprint(manifest);
    if (lock.manifestSha != wantSha) {
        reason = lock.manifestSha.empty()
            ? "zlpkg.lock predates manifest fingerprinting"
            : "zlpkg.lock is stale (the manifest changed since it was written)";
        return std::nullopt;
    }

    std::map<std::string, const ResolvedDependency*> byName;
    for (const auto& d : lock.deps) {
        if (!byName.emplace(d.name, &d).second) {
            reason = "zlpkg.lock lists dependency '" + d.name + "' twice";
            return std::nullopt;
        }
    }

    std::map<std::string, const Dependency*> rootDecls;
    for (const auto& dep : manifest.dependencies) rootDecls[dep.name] = &dep;

    // Every root declaration must be present with identical
    // kind/source/ref - a mismatch means the lock describes dependency
    // declarations that no longer exist.
    for (const auto& dep : manifest.dependencies) {
        auto it = byName.find(dep.name);
        if (it == byName.end()) {
            reason = "zlpkg.lock has no entry for dependency '" + dep.name + "'";
            return std::nullopt;
        }
        const ResolvedDependency* locked = it->second;
        if (locked->kind != dep.kind || locked->source != dep.source || locked->declaredRef != dep.ref) {
            reason = "zlpkg.lock entry for '" + dep.name + "' no longer matches its manifest declaration";
            return std::nullopt;
        }
    }

    std::vector<ResolvedDependency> deps;
    std::map<std::string, Manifest> depManifests; // verified entries' manifests, for the reachability pass
    bool changed = false;

    for (const auto& d : lock.deps) {
        if (!isValidPackageName(d.name)) {
            reason = "zlpkg.lock entry '" + d.name + "' is not a valid package name";
            return std::nullopt;
        }
        auto rootIt = rootDecls.find(d.name);
        const bool isRoot = rootIt != rootDecls.end();
        const std::string pin = isRoot ? rootIt->second->version : "";

        ResolvedDependency rd;
        rd.name = d.name;
        rd.kind = d.kind;
        std::filesystem::path dir;
        if (d.kind == Dependency::Kind::Path) {
            // Root path deps resolve from the manifest's own relative
            // path - never the lock's baked absolute directory - which
            // also keeps a committed lock portable across checkouts.
            // Transitive entries keep the lock's absolute dir (their
            // source is relative to whichever manifest requested them,
            // which the lock does not record) and are verified by the
            // package-manifest checks below instead.
            std::string source;
            if (isRoot) {
                source = rootIt->second->source;
                std::error_code ec;
                dir = std::filesystem::weakly_canonical(manifest.manifestDir / source, ec);
                if (ec) {
                    reason = "locked dependency '" + d.name + "': path '" + source + "' does not resolve";
                    return std::nullopt;
                }
            } else {
                source = d.source;
                dir = d.dir;
            }
            if (!std::filesystem::exists(dir)) {
                reason = "locked dependency '" + d.name + "': directory '" + dir.string() + "' is missing";
                return std::nullopt;
            }
            rd.source = source;
            rd.resolvedRef = "local";
        } else {
            const std::string& url = isRoot ? rootIt->second->source : d.source;
            const std::string& sha = d.resolvedRef;
            // The cache path is recomputed from (name, sha), never taken
            // from the lock - and the entry is used only when HEAD is
            // exactly the pinned commit, so substituted or half-written
            // cache content falls back to a re-resolve instead of running.
            std::filesystem::path expected = cacheRoot / d.name / sha;
            std::error_code cacheEc;
            const bool cached = std::filesystem::exists(expected, cacheEc) && !cacheEc;
            if (!cached || !verifyGitCache(expected, sha)) {
                reason = "locked dependency '" + d.name + "': pinned commit " + sha +
                         (!cached ? " is not in the cache" : " failed cache verification");
                return std::nullopt;
            }
            dir = expected;
            rd.source = url;
            rd.resolvedRef = sha;
            rd.declaredRef = isRoot ? rootIt->second->ref : d.declaredRef;
        }

        Manifest depManifest;
        try {
            depManifest = loadManifest(dir / "zlpkg.toml");
        } catch (const ManifestError&) {
            reason = "locked dependency '" + d.name + "': package manifest at '" +
                     (dir / "zlpkg.toml").string() + "' is missing or invalid";
            return std::nullopt;
        }
        if (depManifest.name != d.name) {
            reason = "locked dependency '" + d.name + "': directory holds package '" + depManifest.name + "' instead";
            return std::nullopt;
        }
        if (!pin.empty() && depManifest.version != pin) {
            reason = "locked dependency '" + d.name + "': pinned version '" + pin + "' but package is '" +
                     depManifest.version + "'";
            return std::nullopt;
        }
        // The dependency's own manifest must still declare the same edges
        // it did at resolve time - otherwise the transitive graph changed
        // under the lock (an edge added, removed, or retargeted) and the
        // whole graph must be re-resolved.
        if (d.depsFingerprint.empty() || lockDepsFingerprint(depManifest) != d.depsFingerprint) {
            reason = "locked dependency '" + d.name + "': its manifest changed since the lock was written";
            return std::nullopt;
        }
        rd.packageVersion = depManifest.version;
        rd.depsFingerprint = d.depsFingerprint;
        if (rd.packageVersion != d.packageVersion) changed = true; // live tree moved under a fresh lock; rewrite
        rd.dir = dir;
        depManifests[d.name] = std::move(depManifest);
        deps.push_back(std::move(rd));
    }

    // Reachability: every dependency named by any verified manifest must
    // be in the lock, and every locked entry must be reachable from the
    // root - otherwise a transitive manifest changed its edges (or an
    // entry was added/removed by hand) and the graph must be re-resolved.
    std::set<std::string> reached;
    std::vector<std::string> stack;
    for (const auto& dep : manifest.dependencies) stack.push_back(dep.name);
    while (!stack.empty()) {
        std::string name = std::move(stack.back());
        stack.pop_back();
        if (!reached.insert(name).second) continue;
        if (!byName.count(name)) {
            reason = "zlpkg.lock has no entry for transitive dependency '" + name + "'";
            return std::nullopt;
        }
        for (const auto& sub : depManifests[name].dependencies) stack.push_back(sub.name);
    }
    for (const auto& d : lock.deps) {
        if (!reached.count(d.name)) {
            reason = "zlpkg.lock entry '" + d.name + "' is no longer referenced by any manifest";
            return std::nullopt;
        }
    }

    if (changed) writeLockFile(lockPath, deps, wantSha);
    return deps;
}

// Central "make sure the dependency graph is fetched and the lock reflects
// it" routine, shared by both `install` and `run`. When a lock already
// exists and `update` isn't requested, it is validated against the current
// manifest and reused when fresh - this is what makes repeated
// `zlpkg install` calls both fast and exactly reproducible. A stale lock
// (or no lock yet, or `update`) re-runs the full resolver and overwrites
// the lock.
std::vector<ResolvedDependency> ensureInstalled(const Manifest& manifest, bool update) {
    std::filesystem::path lockPath = manifest.manifestDir / "zlpkg.lock";
    std::filesystem::path cacheRoot = manifest.manifestDir / ".zlpkg";

    if (!update && std::filesystem::exists(lockPath)) {
        LockFile lock = readLockFile(lockPath);
        std::string reason;
        if (auto deps = tryUseLock(manifest, lock, lockPath, cacheRoot, reason)) return *deps;
        // Flush: `run` forks+execs below, and a buffered notice would
        // otherwise appear after the child's own output.
        std::cout << "zlpkg: " << reason << "; re-resolving dependencies..." << std::endl;
    }

    auto deps = resolveAll(manifest);
    writeLockFile(lockPath, deps, lockManifestFingerprint(manifest));
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

    std::vector<std::string> argv;
    argv.push_back(zlBinary.string());
    for (const auto& d : deps) {
        argv.push_back("--root");
        argv.push_back(d.moduleRoot().string());
    }
    argv.push_back(entry.string());
    for (const auto& a : programArgs) argv.push_back(a);

    return run(argv);
}

} // namespace zlpkg
