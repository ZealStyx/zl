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

// Git URL transport allowlist. A zlpkg.toml `git = "..."` value is
// attacker-controllable (it may come from a transitive dependency's own
// manifest), so it must never reach git as an arbitrary transport string:
// git honors helper transports such as `ext::sh -c ...` that execute
// commands by design. Only the ordinary network/local transports and the
// scp-like ssh spelling are accepted.
bool isAllowedGitUrl(const std::string& url) {
    if (url.empty() || url.front() == '-' || url.front() == '/') return false;
    for (unsigned char c : url) {
        if (c < 0x20 || c == 0x7F) return false; // no control characters
    }
    static const char* kAllowedSchemes[] = {"https://", "http://", "ssh://", "git://", "file://"};
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

// Clones `url` (and optionally checks out `ref`) into a scratch directory,
// resolves HEAD's full sha, and returns {sha, scratchDir}. Caller is
// responsible for moving/discarding scratchDir.
struct RawClone { std::string sha; std::filesystem::path dir; };

RawClone rawClone(const std::string& url, const std::string& ref, const std::filesystem::path& cacheRoot) {
    if (!isAllowedGitUrl(url)) {
        throw GitError("git dependency URL '" + url + "' is not allowed: only https://, http://, "
                       "ssh://, git://, file:// and user@host:path URLs are accepted "
                       "(helper transports such as ext:: are rejected)");
    }
    if (!isAllowedGitRef(ref)) {
        throw GitError("git dependency ref '" + ref + "' is not allowed: refs must not start with '-' "
                       "or contain whitespace/control characters");
    }

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
// that exact sha is already cached from a previous fetch.
std::filesystem::path settle(const std::filesystem::path& scratchDir, const std::filesystem::path& cacheRoot,
                              const std::string& depName, const std::string& sha) {
    // Defense in depth: depName reaches a filesystem path here, so it must
    // be a validated package name even if every caller already checked it.
    if (!isValidPackageName(depName)) {
        throw GitError("dependency name '" + depName + "' is not a valid package name");
    }
    std::filesystem::path finalDir = cacheRoot / depName / sha;
    // The name is validated, so `finalDir` is cacheRoot/<name>/<sha>; assert
    // that rather than trusting the path arithmetic.
    std::error_code canonEc;
    const std::filesystem::path cacheAbs = std::filesystem::weakly_canonical(cacheRoot, canonEc);
    const std::filesystem::path finalAbs = std::filesystem::weakly_canonical(finalDir.parent_path(), canonEc);
    if (!canonEc && !finalAbs.string().empty() &&
        finalAbs.string().rfind(cacheAbs.string(), 0) != 0) {
        throw GitError("dependency cache path '" + finalDir.string() + "' escapes the cache root");
    }
    std::error_code ec;
    if (std::filesystem::exists(finalDir)) {
        std::filesystem::remove_all(scratchDir, ec);
        return finalDir;
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
    RawClone clone = rawClone(url, ref, cacheRoot);
    std::filesystem::path finalDir = settle(clone.dir, cacheRoot, depName, clone.sha);
    return {clone.sha, finalDir};
}

GitFetchResult restoreGitPin(const std::string& url, const std::string& sha, const std::filesystem::path& cacheRoot,
                              const std::string& depName) {
    // The name comes back out of zlpkg.lock here; validate it the same way
    // as at manifest-parse time so a hand-edited lock can't aim the cache
    // path outside .zlpkg/.
    if (!isValidPackageName(depName)) {
        throw GitError("locked dependency name '" + depName + "' is not a valid package name - regenerate zlpkg.lock");
    }
    std::filesystem::path finalDir = cacheRoot / depName / sha;
    if (std::filesystem::exists(finalDir)) return {sha, finalDir};
    // Not cached (or cache was cleared) - re-clone and check out the exact
    // pinned sha, which reproduces the same content deterministically.
    RawClone clone = rawClone(url, sha, cacheRoot);
    if (clone.sha != sha) {
        // Extremely unlikely (would mean the pinned sha no longer exists,
        // and checkout somehow succeeded on something else), but worth a
        // clear error rather than silently drifting.
        throw GitError("expected to restore pinned commit " + sha + " for '" + url + "' but got " + clone.sha);
    }
    std::filesystem::path finalDir2 = settle(clone.dir, cacheRoot, depName, clone.sha);
    return {clone.sha, finalDir2};
}

} // namespace zlpkg
