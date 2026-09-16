#pragma once

#include <filesystem>

namespace zl {

// The directory used as the Java-style import root for `entryFile`.
//
// Walks from the entry file's directory toward the filesystem root:
//   - a directory literally named `src` is the source root (the
//     `src/io/github/test/App.zl` convention);
//   - a `zlpkg.toml` or `.git` (file or directory) is a project boundary:
//     ancestors above it are not searched, so cloning into a path that
//     happens to contain a directory named `src` (e.g. `/home/src/app`)
//     cannot steal imports from an unrelated tree.
// If neither a `src` directory nor a project boundary is found, the entry
// file's own directory is used, which keeps flat single-file programs
// working without imports.
[[nodiscard]] inline std::filesystem::path resolveProjectSourceRoot(const std::filesystem::path& entryFile) {
    auto dir = std::filesystem::absolute(entryFile).parent_path();
    for (std::filesystem::path p = dir;;) {
        if (p.filename() == "src") return p;
        std::error_code ec;
        if (std::filesystem::exists(p / "zlpkg.toml", ec) ||
            std::filesystem::exists(p / ".git", ec)) {
            return dir;
        }
        std::filesystem::path parent = p.parent_path();
        if (parent == p) break;
        p = parent;
    }
    return dir;
}

} // namespace zl
