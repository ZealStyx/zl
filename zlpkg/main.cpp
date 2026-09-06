// zlpkg - the ZL package manager CLI (Phase 6). Keeps zl_language itself
// completely dependency-unaware: this tool's whole job is resolving a
// project's zlpkg.toml into a set of directories and handing them to
// zl_language as `--root` flags. See ROADMAP.md, Phase 6, for
// the full design rationale.
//
// Usage:
//   zlpkg install [--update]
//   zlpkg run <entry.zl> [program args...]
//
// Both subcommands look for zlpkg.toml in the current directory.

#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

#include "commands.hpp"

namespace {

void printUsage() {
    std::cerr << "usage:\n"
                 "  zlpkg install [--update]        resolve dependencies and write zlpkg.lock\n"
                 "  zlpkg run <entry.zl> [args...]  install if needed, then run entry.zl with\n"
                 "                                   dependencies wired in\n";
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        printUsage();
        return 1;
    }

    std::filesystem::path projectDir = std::filesystem::current_path();
    std::string subcommand = argv[1];

    if (subcommand == "install") {
        bool update = false;
        for (int i = 2; i < argc; ++i) {
            std::string arg = argv[i];
            if (arg == "--update" || arg == "-u") {
                update = true;
            } else {
                std::cerr << "zlpkg install: unrecognized argument '" << arg << "'\n";
                return 2;
            }
        }
        return zlpkg::cmdInstall(projectDir, update);
    }

    if (subcommand == "run") {
        if (argc < 3) {
            std::cerr << "zlpkg run: missing entry file\n";
            printUsage();
            return 2;
        }
        std::filesystem::path entryFile = argv[2];
        std::vector<std::string> programArgs;
        for (int i = 3; i < argc; ++i) programArgs.emplace_back(argv[i]);
        return zlpkg::cmdRun(projectDir, entryFile, programArgs, argv[0]);
    }

    if (subcommand == "--help" || subcommand == "-h") {
        printUsage();
        return 0;
    }

    std::cerr << "zlpkg: unknown subcommand '" << subcommand << "'\n";
    printUsage();
    return 2;
}
