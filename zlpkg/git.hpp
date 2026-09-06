#pragma once
// Thin wrapper around shelling out to the system `git` binary. zlpkg
// deliberately does not link a git library (libgit2 etc.) - the system
// `git` CLI is assumed to be installed and is invoked the same way a
// person would type it themselves. This keeps zlpkg's own dependency
// footprint at zero.

#include <filesystem>
#include <stdexcept>
#include <string>

namespace zlpkg {

class GitError : public std::runtime_error {
public:
    explicit GitError(const std::string& message) : std::runtime_error(message) {}
};

struct GitFetchResult {
    std::string resolvedSha; // full commit SHA that `ref` (or the default branch) resolved to
    std::filesystem::path dir; // where the checkout ended up: cacheRoot/depName/resolvedSha
};

// Clones `url` (optionally checking out `ref` - a tag, branch, or commit;
// empty means "leave it on the default branch") into a fresh directory
// under cacheRoot/depName/<resolved-sha>/, resolves the SHA actually
// checked out, and returns both. If that exact sha's directory already
// exists from a previous fetch, the existing directory is reused as-is
// (no re-clone) and its sha is returned - this is what makes repeated
// `zlpkg install` calls fast and exact-reproducible.
//
// Throws GitError (including git's own stderr) if the clone or checkout
// fails - e.g. an unreachable URL, or a ref that doesn't exist.
GitFetchResult fetchGit(const std::string& url, const std::string& ref,
                         const std::filesystem::path& cacheRoot, const std::string& depName);

// Re-materializes a previously resolved (name, sha) pin exactly - used by
// `zlpkg install` when a lock file already pins an exact commit but the
// local .zlpkg/ cache for it is missing (e.g. after a clean checkout).
// Equivalent to fetchGit(url, sha, cacheRoot, depName) but phrased for the
// "restore a pin" call site.
GitFetchResult restoreGitPin(const std::string& url, const std::string& sha,
                              const std::filesystem::path& cacheRoot, const std::string& depName);

} // namespace zlpkg
