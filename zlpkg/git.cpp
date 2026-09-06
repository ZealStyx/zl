#include "git.hpp"

#include <cstdio>
#include <random>
#include <sstream>

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

// Clones `url` (and optionally checks out `ref`) into a scratch directory,
// resolves HEAD's full sha, and returns {sha, scratchDir}. Caller is
// responsible for moving/discarding scratchDir.
struct RawClone { std::string sha; std::filesystem::path dir; };

RawClone rawClone(const std::string& url, const std::string& ref, const std::filesystem::path& cacheRoot) {
    std::filesystem::path tmpDir = cacheRoot / ".tmp" / ("clone-" + randomSuffix());
    std::error_code ec;
    std::filesystem::create_directories(tmpDir.parent_path(), ec);

    auto clone = runCaptured("git clone --quiet " + shellQuote(url) + " " + shellQuote(tmpDir.string()));
    if (clone.exitCode != 0) {
        std::filesystem::remove_all(tmpDir, ec);
        throw GitError("git clone failed for '" + url + "':\n" + trim(clone.output));
    }

    if (!ref.empty()) {
        auto checkout = runCaptured("git -C " + shellQuote(tmpDir.string()) + " checkout --quiet " + shellQuote(ref));
        if (checkout.exitCode != 0) {
            std::filesystem::remove_all(tmpDir, ec);
            throw GitError("git checkout of ref '" + ref + "' failed in '" + url + "':\n" + trim(checkout.output));
        }
    }

    auto rev = runCaptured("git -C " + shellQuote(tmpDir.string()) + " rev-parse HEAD");
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
    std::filesystem::path finalDir = cacheRoot / depName / sha;
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
