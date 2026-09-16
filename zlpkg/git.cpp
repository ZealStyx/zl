#include "git.hpp"

#include <cstdio>
#include <random>
#include <sstream>

#include "manifest.hpp"
#include "process.hpp"

namespace zlpkg {
namespace {

std::string trim(const std::string& s) {
    size_t start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return "";
    size_t end = s.find_last_not_of(" \t\r\n");
    return s.substr(start, end - start + 1);
}

std::string randomSuffix() {
    static std::random_device rd;
    static std::mt19937_64 gen(rd());
    std::uniform_int_distribution<uint64_t> dist;
    std::ostringstream oss;
    oss << std::hex << dist(gen);
    return oss.str();
}

} // namespace

bool isAllowedGitDependencyUrl(const std::string& url) {
    if (url.empty() || url.front() == '-' || url.front() == '/') return false;
    for (unsigned char c : url) {
        if (c < 0x20 || c == 0x7F) return false; // no control characters
    }
    // Authenticated/encrypted remotes only. `http://` is plaintext,
    // `git://` is unauthenticated, `file://` (and a leading '/') is a
    // local path, and helper transports such as `ext::sh -c ...` execute
    // commands by design. git's `http::` / `git::` helper spellings are
    // rejected because they do not match these prefixes either.
    static const char* kAllowedSchemes[] = {"https://", "ssh://"};
    for (const char* scheme : kAllowedSchemes) {
        if (url.rfind(scheme, 0) == 0) return true;
    }
    // scp-like ssh shorthand: user@host:path (no scheme). Require an '@'
    // before the first ':' so a local relative path is never mistaken for
    // one, and reject the characters git itself forbids in this spelling.
    const size_t colon = url.find(':');
    if (colon != std::string::npos) {
        const size_t at = url.find('@');
        if (at != std::string::npos && at < colon && url.find("://") == std::string::npos) {
            for (char c : url) {
                if (c == ' ' || c == '~' || c == '^' || c == '?' || c == '*' || c == '[' || c == '\\') return false;
            }
            return true;
        }
    }
    return false;
}

namespace {

// A ref (tag/branch/commit) is passed to `git checkout` as one argv element,
// so shell syntax cannot leak through; the remaining risk is the ref being
// parsed as an option, so reject a leading '-' (and empty/whitespace refs,
// which cannot be meaningful).
bool isAllowedGitRef(const std::string& ref) {
    if (ref.empty()) return true; // empty = remote's default branch
    if (ref.front() == '-') return false;
    for (unsigned char c : ref) {
        if (c < 0x20 || c == ' ') return false;
    }
    return true;
}

// A pinned commit sha: lowercase hex, 4..64 chars (git's own loosest shape
// for an unambiguous abbreviation through a full sha-1). Anything else in
// a sha position is a corrupt/tampered pin, never a commit - and since
// shas become cache directory names, this also keeps path metacharacters
// like '/' and ".." out of the cache path by construction.
bool isPinnedSha(const std::string& sha) {
    if (sha.size() < 4 || sha.size() > 64) return false;
    for (unsigned char c : sha) {
        const bool hex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
        if (!hex) return false;
    }
    return true;
}

void validateGitRemote(const std::string& url, const std::string& ref) {
    if (!isAllowedGitDependencyUrl(url)) {
        throw GitError("git dependency URL '" + url + "' is not allowed: only https://, "
                       "ssh:// and user@host:path URLs are accepted "
                       "(http://, git://, file:// and helper transports such as ext:: are rejected)");
    }
    if (!isAllowedGitRef(ref)) {
        throw GitError("git dependency ref '" + ref + "' is not allowed: refs must not start with '-' "
                       "or contain whitespace/control characters");
    }
}

// Clones `url` (and optionally checks out `ref`) into a scratch directory,
// resolves HEAD's full sha, and returns {sha, scratchDir}. Caller is
// responsible for moving/discarding scratchDir.
struct RawClone { std::string sha; std::filesystem::path dir; };

RawClone rawClone(const std::string& url, const std::string& ref, const std::filesystem::path& cacheRoot) {
    validateGitRemote(url, ref);

    std::filesystem::path tmpDir = cacheRoot / ".tmp" / ("clone-" + randomSuffix());
    std::error_code ec;
    std::filesystem::create_directories(tmpDir.parent_path(), ec);

    const std::string tmpDirStr = tmpDir.string();
    const auto clone = runCaptured({"git", "clone", "--quiet", url, tmpDirStr});
    if (clone.exitCode != 0) {
        std::filesystem::remove_all(tmpDir, ec);
        throw GitError("git clone failed for '" + url + "':\n" + trim(clone.output));
    }

    if (!ref.empty()) {
        const auto checkout = runCaptured({"git", "-C", tmpDirStr, "checkout", "--quiet", ref});
        if (checkout.exitCode != 0) {
            std::filesystem::remove_all(tmpDir, ec);
            throw GitError("git checkout of ref '" + ref + "' failed in '" + url + "':\n" + trim(checkout.output));
        }
    }

    const auto rev = runCaptured({"git", "-C", tmpDirStr, "rev-parse", "HEAD"});
    if (rev.exitCode != 0) {
        std::filesystem::remove_all(tmpDir, ec);
        throw GitError("git rev-parse HEAD failed in '" + url + "':\n" + trim(rev.output));
    }
    return {trim(rev.output), tmpDir};
}

// Moves scratchDir into place at cacheRoot/depName/sha, or discards it if
// that exact sha is already cached from a previous fetch - but only when
// the cached entry verifies as a pristine checkout of that sha. A stale,
// half-written, or tampered entry is replaced by the fresh clone.
std::filesystem::path settle(const std::filesystem::path& scratchDir, const std::filesystem::path& cacheRoot,
                              const std::string& depName, const std::string& sha) {
    // Defense in depth: depName reaches a filesystem path here, so it must
    // be a validated package name even if every caller already checked it.
    if (!isValidPackageName(depName)) {
        throw GitError("dependency name '" + depName + "' is not a valid package name");
    }
    std::filesystem::path finalDir = cacheRoot / depName / sha;
    // The name is validated, so `finalDir` is cacheRoot/<name>/<sha>; assert
    // that rather than trusting the path arithmetic. The trailing separator
    // matters: a bare prefix check would accept "/cache-evil" as inside
    // "/cache".
    std::error_code canonEc;
    const std::filesystem::path cacheAbs = std::filesystem::weakly_canonical(cacheRoot, canonEc);
    const std::filesystem::path finalAbs = std::filesystem::weakly_canonical(finalDir.parent_path(), canonEc);
    const std::string prefix = cacheAbs.string() + std::filesystem::path::preferred_separator;
    if (!canonEc && !finalAbs.string().empty() && finalAbs != cacheAbs &&
        finalAbs.string().rfind(prefix, 0) != 0) {
        throw GitError("dependency cache path '" + finalDir.string() + "' escapes the cache root");
    }
    std::error_code ec;
    if (std::filesystem::exists(finalDir)) {
        if (verifyGitCache(finalDir, sha)) {
            std::filesystem::remove_all(scratchDir, ec);
            return finalDir;
        }
        std::filesystem::remove_all(finalDir, ec);
    }
    std::filesystem::create_directories(finalDir.parent_path(), ec);
    std::filesystem::rename(scratchDir, finalDir, ec);
    if (ec) {
        // rename() can fail across filesystem boundaries (e.g. /tmp on a
        // different mount than the project); fall back to copy+remove.
        ec.clear();
        std::filesystem::copy(scratchDir, finalDir, std::filesystem::copy_options::recursive, ec);
        if (ec) throw GitError("could not place fetched files at '" + finalDir.string() + "': " + ec.message());
        std::filesystem::remove_all(scratchDir, ec);
    }
    return finalDir;
}

} // namespace

GitFetchResult fetchGit(const std::string& url, const std::string& ref, const std::filesystem::path& cacheRoot,
                         const std::string& depName) {
    validateGitRemote(url, ref);
    // Pinned sha + verified cache: no network needed. The rev-parse check
    // is what makes this sound - a cache entry is trusted only when HEAD
    // is exactly the requested commit, so a substituted or half-written
    // directory falls through to a fresh clone below.
    if (isPinnedSha(ref) && isValidPackageName(depName)) {
        std::filesystem::path cached = cacheRoot / depName / ref;
        if (verifyGitCache(cached, ref)) return {ref, cached};
    }
    RawClone clone = rawClone(url, ref, cacheRoot);
    std::filesystem::path finalDir = settle(clone.dir, cacheRoot, depName, clone.sha);
    return {clone.sha, finalDir};
}

bool verifyGitCache(const std::filesystem::path& dir, const std::string& sha) {
    if (!isPinnedSha(sha)) return false;
    std::error_code ec;
    if (!std::filesystem::is_directory(dir, ec) || ec) return false;
    const auto rev = runCaptured({"git", "-C", dir.string(), "rev-parse", "HEAD"});
    if (rev.exitCode != 0 || trim(rev.output) != sha) return false;
    // HEAD alone is not enough: the worktree may hold uncommitted edits
    // (or dropped-in files) on top of the right commit. A cache entry is
    // a pristine clone, so any status output at all fails verification.
    const auto status =
        runCaptured({"git", "-C", dir.string(), "status", "--porcelain=v1", "--untracked-files=all"});
    if (status.exitCode != 0) return false;
    return trim(status.output).empty();
}

} // namespace zlpkg
