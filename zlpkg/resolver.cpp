#include "resolver.hpp"

#include <deque>
#include <map>
#include <set>
#include <utility>

#include "git.hpp"
#include "lockfile.hpp"

namespace zlpkg {

std::filesystem::path ResolvedDependency::moduleRoot() const {
    std::filesystem::path srcSub = dir / "src";
    if (std::filesystem::exists(srcSub) && std::filesystem::is_directory(srcSub)) return srcSub;
    return dir;
}

namespace {

std::string shortSha(const std::string& sha) {
    return sha.size() > 10 ? sha.substr(0, 10) : sha;
}

std::string describe(const ResolvedDependency& rd) {
    if (rd.kind == Dependency::Kind::Path) {
        return "local path '" + rd.dir.string() + "'";
    }
    std::string s = "git '" + rd.source + "' @ " + shortSha(rd.resolvedRef);
    if (!rd.declaredRef.empty()) s += " (ref '" + rd.declaredRef + "')";
    return s;
}

struct QueueItem {
    Dependency dep;
    std::filesystem::path requesterDir;
    std::string requesterLabel; // human-readable "who asked for this", for error messages
};

// Bookkeeping kept alongside `results`, parallel by index - not part of the
// public ResolvedDependency shape.
struct Track {
    std::string identity;       // "path:<abs dir>" or "git:<url>@<sha>" - used for conflict detection
    std::string requesterLabel; // label captured the first time this name was resolved
};

} // namespace

std::vector<ResolvedDependency> resolveAll(const Manifest& rootManifest) {
    std::filesystem::path cacheRoot = rootManifest.manifestDir / ".zlpkg";

    std::vector<ResolvedDependency> results;
    std::vector<Track> track;
    std::map<std::string, size_t> indexByName;
    std::set<std::string> descended; // identity keys already checked for their own transitive zlpkg.toml
    // One clone per unique (url, ref) per run: without this, a diamond (or
    // a hostile fan-out) pays a full `git clone` for EVERY occurrence of
    // the same dependency, since each queue item used to fetch.
    std::map<std::pair<std::string, std::string>, GitFetchResult> fetchMemo;
    // Manifest version per resolved identity, so a repeat occurrence can
    // still enforce its own version pin without reloading the manifest.
    std::map<std::string, std::string> versionByIdentity;

    std::deque<QueueItem> queue;
    for (const auto& dep : rootManifest.dependencies) {
        queue.push_back({dep, rootManifest.manifestDir, "<project root>"});
    }

    while (!queue.empty()) {
        QueueItem item = std::move(queue.front());
        queue.pop_front();
        const Dependency& dep = item.dep;

        ResolvedDependency rd;
        rd.name = dep.name;
        rd.kind = dep.kind;
        rd.source = dep.source;
        rd.declaredRef = dep.ref;
        std::string identity;

        if (dep.kind == Dependency::Kind::Path) {
            std::error_code ec;
            std::filesystem::path candidate = item.requesterDir / dep.source;
            std::filesystem::path absDir = std::filesystem::weakly_canonical(candidate, ec);
            if (ec || !std::filesystem::exists(absDir)) {
                throw ResolveError("dependency '" + dep.name + "' (via " + item.requesterLabel + "): path '" +
                                    dep.source + "' resolved to '" + candidate.string() + "', which does not exist");
            }
            rd.dir = absDir;
            rd.resolvedRef = "local";
            identity = "path:" + absDir.string();
        } else {
            try {
                const auto memoKey = std::make_pair(dep.source, dep.ref);
                auto memoIt = fetchMemo.find(memoKey);
                if (memoIt == fetchMemo.end()) {
                    memoIt = fetchMemo.emplace(memoKey, fetchGit(dep.source, dep.ref, cacheRoot, dep.name)).first;
                }
                rd.dir = memoIt->second.dir;
                rd.resolvedRef = memoIt->second.resolvedSha;
            } catch (const GitError& e) {
                throw ResolveError("dependency '" + dep.name + "' (via " + item.requesterLabel + "): " +
                                    std::string(e.what()));
            }
            identity = "git:" + dep.source + "@" + rd.resolvedRef;
        }

        // Dedup BEFORE the manifest work below: the identity is already
        // known, so a repeat occurrence needs no reloading - only its own
        // version pin re-checked (a diamond's two edges may pin different
        // versions of the same content, which must still fail).
        {
            auto existingIt = indexByName.find(dep.name);
            if (existingIt != indexByName.end()) {
                size_t idx = existingIt->second;
                if (track[idx].identity != identity) {
                    throw ResolveError("version conflict for dependency '" + dep.name + "': via " +
                                        track[idx].requesterLabel + " it resolved to " + describe(results[idx]) +
                                        ", but via " + item.requesterLabel + " it resolves to " + describe(rd) +
                                        " - zlpkg does not pick one silently, so this needs to be reconciled " +
                                        "(pin both requesters to the same ref/path)");
                }
                const std::string& actualVersion = versionByIdentity[track[idx].identity];
                if (!dep.version.empty() && actualVersion != dep.version) {
                    throw ResolveError("dependency '" + dep.name + "': requested version '" + dep.version +
                                       "' but package declares version '" + actualVersion + "'");
                }
                continue; // identical content already resolved (and already queued for descent, if any)
            }
        }

        // Every resolvable package has an explicit manifest. Its declared name
        // and version are part of the package identity/API contract. A dependency
        // may optionally pin an exact package version; we never silently accept
        // a different package version at the same path/git ref.
        std::filesystem::path depManifestPath = rd.dir / "zlpkg.toml";
        if (!std::filesystem::exists(depManifestPath)) {
            throw ResolveError("dependency '" + dep.name + "': package is missing zlpkg.toml at '" +
                               depManifestPath.string() + "'");
        }
        Manifest depManifest;
        try {
            depManifest = loadManifest(depManifestPath);
        } catch (const ManifestError& e) {
            throw ResolveError("dependency '" + dep.name + "': " + std::string(e.what()));
        }
        if (depManifest.name != dep.name) {
            throw ResolveError("dependency '" + dep.name + "': manifest declares package name '" +
                               depManifest.name + "'");
        }
        rd.packageVersion = depManifest.version;
        rd.depsFingerprint = lockDepsFingerprint(depManifest);
        if (!dep.version.empty() && depManifest.version != dep.version) {
            throw ResolveError("dependency '" + dep.name + "': requested version '" + dep.version +
                               "' but package declares version '" + depManifest.version + "'");
        }

        indexByName[dep.name] = results.size();
        track.push_back({identity, item.requesterLabel});
        versionByIdentity[identity] = rd.packageVersion;
        results.push_back(rd);

        if (descended.insert(identity).second) {
            std::filesystem::path subManifestPath = rd.dir / "zlpkg.toml";
            if (std::filesystem::exists(subManifestPath)) {
                Manifest subManifest;
                try {
                    subManifest = loadManifest(subManifestPath);
                } catch (const ManifestError& e) {
                    throw ResolveError("transitive dependency '" + dep.name + "': " + std::string(e.what()));
                }
                for (const auto& subDep : subManifest.dependencies) {
                    queue.push_back({subDep, subManifest.manifestDir, dep.name});
                }
            }
        }
    }

    return results;
}

} // namespace zlpkg
