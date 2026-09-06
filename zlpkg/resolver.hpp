#pragma once
// Recursively resolves a project's zlpkg.toml dependencies (and each
// dependency's own zlpkg.toml, transitively) into a flat, order-preserving
// list, fetching git dependencies and locating path dependencies as it
// goes.
//
// Resolution policy (see ROADMAP.md, Phase 6): if the same
// dependency NAME is reached twice through different paths in the
// dependency graph (a "diamond") and they resolve to different actual
// content (different git commit, or a different local path), that is a
// hard error - the resolver never silently picks one version over
// another. If both paths resolve to the *same* content, it's resolved
// once and simply shared.

#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>

#include "manifest.hpp"

namespace zlpkg {

class ResolveError : public std::runtime_error {
public:
    explicit ResolveError(const std::string& message) : std::runtime_error(message) {}
};

struct ResolvedDependency {
    std::string name;
    Dependency::Kind kind{};
    std::string source;      // git URL, or path, exactly as declared
    std::string declaredRef; // git tag/branch/rev as written in the manifest (empty = default branch); unused for Path
    std::string resolvedRef; // git commit sha, or "local" for a Path dependency
    std::string packageVersion; // version declared by the dependency package manifest
    std::filesystem::path dir; // fetched/local content root (repo root, or the path dependency's own root)

    // Root to pass to `zl_language --root`: dir/src if that exists,
    // otherwise dir itself (mirrors how a project's own sourceRoot_ is
    // picked in module_loader.cpp).
    [[nodiscard]] std::filesystem::path moduleRoot() const;
};

// Resolves every (transitive) dependency of `rootManifest`, fetching git
// dependencies into rootManifest.manifestDir/.zlpkg/<name>/<sha>/ and
// resolving path dependencies relative to whichever manifest declared
// them. Returns the resolved set in first-encountered (breadth-first)
// order. Throws ResolveError on a version conflict, a missing/unreachable
// dependency, or a malformed transitive zlpkg.toml.
std::vector<ResolvedDependency> resolveAll(const Manifest& rootManifest);

} // namespace zlpkg
