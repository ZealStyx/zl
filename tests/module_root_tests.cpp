// Source-root resolution: a directory named `src` above a project boundary
// (zlpkg.toml or .git) must not become the import root.
#include "zl/compiler/source_root.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

namespace fs = std::filesystem;

int failures = 0;

void require(bool condition, const std::string& message) {
    if (condition) return;
    std::cerr << "module-root: " << message << '\n';
    ++failures;
}

void writeFile(const fs::path& path, const std::string& contents) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    out << contents;
}

int main() {
    const auto tmp = fs::temp_directory_path() / "zl-module-root-tests";
    std::error_code ec;
    fs::remove_all(tmp, ec);
    fs::create_directories(tmp);

    // Clone-into-/.../src/project: the ancestor named src must not win once
    // the project marker is found.
    const auto decoy = tmp / "src" / "project";
    writeFile(decoy / ".git", "gitdir: /dev/null\n");
    writeFile(decoy / "examples" / "Hello.zl", "class Hello {}\n");
    const auto decoyRoot = zl::resolveProjectSourceRoot(decoy / "examples" / "Hello.zl");
    require(decoyRoot == fs::absolute(decoy / "examples"),
            "project .git must stop the walk before an ancestor named src; got " + decoyRoot.string());

    writeFile(decoy / "zlpkg.toml", "[package]\nname = \"p\"\nversion = \"0.1.0\"\n");
    fs::remove(decoy / ".git", ec);
    const auto tomlRoot = zl::resolveProjectSourceRoot(decoy / "examples" / "Hello.zl");
    require(tomlRoot == fs::absolute(decoy / "examples"),
            "zlpkg.toml must stop the walk before an ancestor named src; got " + tomlRoot.string());

    // Conventional Java-style tree: src/pkg/App.zl -> source root is src/.
    const auto pkg = tmp / "app" / "src" / "pkg" / "App.zl";
    writeFile(pkg, "class App {}\n");
    writeFile(tmp / "app" / "zlpkg.toml", "[package]\nname = \"app\"\nversion = \"0.1.0\"\n");
    const auto pkgRoot = zl::resolveProjectSourceRoot(pkg);
    require(pkgRoot == fs::absolute(tmp / "app" / "src"),
            "src/pkg/App.zl should resolve to the src directory; got " + pkgRoot.string());

    // Flat single-file program with no marker and no src ancestor.
    const auto flat = tmp / "flat" / "Hello.zl";
    writeFile(flat, "class Hello {}\n");
    const auto flatRoot = zl::resolveProjectSourceRoot(flat);
    require(flatRoot == fs::absolute(tmp / "flat"),
            "flat program should use its own directory; got " + flatRoot.string());

    fs::remove_all(tmp, ec);
    if (failures != 0) {
        std::cerr << "module-root: " << failures << " failure(s)\n";
        return 1;
    }
    return 0;
}
