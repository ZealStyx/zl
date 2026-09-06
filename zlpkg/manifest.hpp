#pragma once
// zlpkg.toml manifest model + parser.
//
// Only a small, hand-picked subset of TOML is supported - exactly what a
// zlpkg.toml needs and nothing more (see the design notes in
// ROADMAP.md, Phase 6). No third-party dependency is pulled in
// for this on purpose: writing a tiny scoped parser is far less code and
// far fewer moving parts than vendoring a general TOML library for a
// handful of fields.
//
// Supported shape:
//
//   [package]
//   name = "myproject"
//   version = "0.1.0"
//
//   [dependencies]
//   geom = { path = "../geom", version = "0.1.0" }
//   ecs  = { git = "https://example.com/zl-ecs.git", ref = "v1.0.0", version = "1.0.0" }
//
// Notes:
//   - Comments start with '#' and run to end of line (outside of quotes).
//   - Every dependency value must be an inline table with EITHER `path`
//     (a local directory, resolved relative to the manifest's own
//     directory) OR `git` (+ optional `ref`, a tag/branch/commit; omitted
//     means "whatever the remote's default branch currently points at").
//   - `version` inside a dependency table is an optional exact package-version
//     requirement. If present, it must exactly match the dependency's [package]
//     version. It is a compatibility/API contract, not a registry lookup.
//   - A bare version string (`geom = "1.2.0"`) is intentionally rejected:
//     there is no package registry yet to look a name up by version alone
//     (see ROADMAP.md's "later upgrade" note), so every
//     dependency has to say exactly where it comes from.

#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>

namespace zlpkg {

class ManifestError : public std::runtime_error {
public:
    explicit ManifestError(const std::string& message) : std::runtime_error(message) {}
};

struct Dependency {
    enum class Kind { Git, Path };

    std::string name;
    Kind kind{};
    std::string source; // git URL, or path exactly as written in the manifest
    std::string ref;    // git tag/branch/commit; empty = remote's default branch. Unused for Path.
    std::string version; // optional exact package version requirement (e.g. "0.1.0")
};

struct Manifest {
    std::string name;
    std::string version;
    std::vector<Dependency> dependencies;

    // Directory containing zlpkg.toml - every relative `path` dependency
    // (and the .zlpkg/ cache) is resolved relative to this.
    std::filesystem::path manifestDir;
};

// Reads and parses a zlpkg.toml file. Throws ManifestError with a message
// naming the offending line/key on any problem (missing [package] fields,
// a dependency missing both `path` and `git`, malformed syntax, ...).
Manifest loadManifest(const std::filesystem::path& manifestPath);

} // namespace zlpkg
