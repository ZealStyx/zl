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
// checked out, and returns both. When `ref` is already a commit sha whose
// cache directory exists and verifies, the clone is skipped and the cache
// entry is reused - this is what makes repeated resolves of pinned
// dependencies fast. Anything else always re-clones (a branch or tag may
// have moved since the last fetch), with an already-cached sha directory
// simply kept instead of the fresh clone.
//
// Throws GitError (including git's own stderr) if the clone or checkout
// fails - e.g. an unreachable URL, or a ref that doesn't exist.
GitFetchResult fetchGit(const std::string& url, const std::string& ref,
                         const std::filesystem::path& cacheRoot, const std::string& depName);

// True when `dir` is a pristine git checkout of exactly `sha` (hex,
// 4..64 chars): HEAD matches AND the worktree is clean, so neither a
// checked-out-elsewhere cache nor uncommitted edits/dropped-in files pass.
// Purely local - no network - and never throws: anything unexpected
// (missing dir, missing git binary, unparseable output) is simply "not
// verified". Used to revalidate cached git dependencies before trusting
// them.
bool verifyGitCache(const std::filesystem::path& dir, const std::string& sha);

// True when `url` is an allowed git remote for a zlpkg.toml `git = "..."`
// value. The string is attacker-controllable (it may come from a transitive
// dependency's own manifest), so helper transports (`ext::`) and the
// unauthenticated `http://`, `git://`, and `file://` schemes are rejected.
// Only `https://`, `ssh://`, and the scp-like `user@host:path` spelling
// are accepted.
[[nodiscard]] bool isAllowedGitDependencyUrl(const std::string& url);

} // namespace zlpkg
