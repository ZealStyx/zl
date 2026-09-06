#pragma once

#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "zl/parser/ast.hpp"
#include "zl/compiler/module_graph.hpp"

namespace zl {

// Thrown for anything import/package related: a file that can't be found for
// an import path, a circular import chain, or two loaded files defining a
// class with the same name. Reported by main.cpp as its own "module error"
// category, alongside the existing syntax/compile/runtime ones.
class ModuleError : public std::runtime_error {
public:
    explicit ModuleError(const std::string& message) : std::runtime_error(message) {}
};

// Resolves `import a.b.C` statements into a single, merged Program by
// recursively loading, parsing, and combining every transitively-imported
// file, Java-style: a dotted import path maps directly to a file path, one
// segment per directory, with the last segment naming the `.zl` file, e.g.
//
//   import io.github.test.Helpers
//
// resolves to `<sourceRoot>/io/github/test/Helpers.zl`. Every loaded file
// must still follow the existing "class name matches file name" rule
// (checked against that file's own basename, not the dotted import path).
//
// The source root is: the nearest ancestor directory literally named `src`,
// walking up from the entry file; if none is found (e.g. today's flat
// `examples/*.zl` files with no imports), the entry file's own directory is
// used, which keeps existing single-file/no-import programs working exactly
// as before.
class ModuleLoader {
public:
    // `extraRoots`, if non-empty, are searched IN ORDER after the project's
    // own sourceRoot_ comes up empty - this is what lets `import
    // zl.util.Queue` resolve to a bundled stdlib file (or, with Phase 6's
    // package manager, a resolved dependency's src/ tree) without the
    // caller's own project needing a matching file of its own. Precedence
    // is always: project root, then extraRoots in the order given. Pass an
    // empty vector to disable extra-root resolution entirely (e.g. tests
    // that only care about project-local imports).
    //
    // Conventionally the caller puts dependency roots before the stdlib
    // root (so a project can shadow a stdlib package with its own
    // dependency of the same dotted path) - see main.cpp for how the roots
    // vector is actually assembled from --root flags / ZL_EXTRA_ROOTS /
    // ZL_STDLIB_ROOT.
    explicit ModuleLoader(std::filesystem::path entryFile,
                           std::vector<std::filesystem::path> extraRoots = {});

    // Convenience overload for the common single-extra-root case (mainly
    // the stdlib root) - equivalent to passing a one-element vector, or an
    // empty vector if `stdlibRoot` itself is empty.
    ModuleLoader(std::filesystem::path entryFile, std::filesystem::path stdlibRoot);

    // Loads the entry file and everything it (transitively) imports, and
    // returns one merged Program: the entry file's own declarations first,
    // followed by every imported file's declarations in load order. Callers
    // can then run the existing TypeChecker/Compiler/VM over it completely
    // unchanged, since neither of those care which file a declaration came
    // from.
    [[nodiscard]] std::unique_ptr<Program> load();

    // The resolved source root, exposed mainly for diagnostics/tests.
    [[nodiscard]] const std::filesystem::path& sourceRoot() const { return sourceRoot_; }

    // The extra roots, in search order, exposed mainly for diagnostics/tests.
    [[nodiscard]] const std::vector<std::filesystem::path>& extraRoots() const { return extraRoots_; }

private:
    std::filesystem::path entryFile_;
    std::filesystem::path sourceRoot_;
    std::vector<std::filesystem::path> extraRoots_;

    ModuleGraph graph_;

    [[nodiscard]] std::filesystem::path resolveSourceRoot() const;
    // Tries sourceRoot_ first, then each of extraRoots_ in order. Throws
    // ModuleError - naming every root that was tried - if none has the file.
    [[nodiscard]] std::filesystem::path resolveImportPath(const ImportDecl& imp, const std::filesystem::path& importingFile) const;
    // The entry file's own dotted name as if it had been reached via an
    // import path (e.g. "io.github.test.AppTest"), so a cycle that loops
    // back around to the entry file is recognized as a cycle - not merely
    // as "some other file happens to redefine the same class name" - even
    // though the entry file was reached directly (from argv), not via an
    // `import`. Falls back to the file's own basename if it doesn't live
    // under sourceRoot_ at all (e.g. a flat, package-less script).
    [[nodiscard]] std::string entryDottedName() const;

    // Parses `filePath`, enforcing that its primary class matches
    // `expectedClassName` (that file's own basename) while allowing helper
    // classes. Throws ParseError/ModuleError on failure.
    [[nodiscard]] std::unique_ptr<Program> parseFile(const std::filesystem::path& filePath,
                                                       const std::string& expectedClassName) const;

    // Recursively loads `filePath` (identified by `dottedName` for
    // diagnostics/cycle-detection, or "<entry>" for the entry file itself)
    // and appends its declarations - and everything it imports - into
    // `merged`, after registering its classes in the module graph and
    // detecting duplicates.
    void loadInto(Program& merged, const std::filesystem::path& filePath, const std::string& dottedName);
};

} // namespace zl
