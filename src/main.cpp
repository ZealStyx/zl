#include <cstdlib>
#include <fstream>
#include <regex>
#include <set>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>


#include "zl/common/executable_path.hpp"
#include "zl/common/stdlib_version.hpp"
#include "zl/compiler/module_loader.hpp"
#include "zl/compiler/type_checker.hpp"
#include "zl/compiler/compiler.hpp"
#include "zl/parser/parser.hpp"
#include "zl/lexer/lexer.hpp"
#include "zl/vm/vm.hpp"
#include "zl/vm/native.hpp"
#include "zl/compiler/native_compiler.hpp"
#include "zl/compiler/ir_lowering.hpp"
#include "zl/compiler/ir_optimizer.hpp"
#include "zl/compiler/machine_code.hpp"
#include "zl/mir/lowering.hpp"
#include "zl/mir/printer.hpp"
#include "zl/mir/ssa.hpp"
#include "zl/mir/verifier.hpp"
#include "zl/mir/vm_backend.hpp"



namespace {

std::vector<std::filesystem::path> splitPathList(const std::string& value) {
    std::vector<std::filesystem::path> result;
#ifdef _WIN32
    const char sep = ';';
#else
    const char sep = ':';
#endif
    std::stringstream ss(value);
    std::string segment;
    while (std::getline(ss, segment, sep)) {
        if (!segment.empty()) result.emplace_back(segment);
    }
    return result;
}

std::filesystem::path findNearestManifest(const std::filesystem::path& entryFile) {
    std::filesystem::path dir = std::filesystem::absolute(entryFile).parent_path();
    for (std::filesystem::path p = dir;;) {
        const auto candidate = p / "zlpkg.toml";
        if (std::filesystem::exists(candidate) && std::filesystem::is_regular_file(candidate)) return candidate;
        const auto parent = p.parent_path();
        if (parent == p) break;
        p = parent;
    }
    return {};
}

std::vector<std::filesystem::path> discoverLocalPackageRoots(const std::filesystem::path& manifestPath) {
    std::vector<std::filesystem::path> roots;
    std::set<std::filesystem::path> visited;
    std::vector<std::filesystem::path> queue{manifestPath};
    const std::regex pathDep(R"zl(path\s*=\s*"([^"]+)")zl");

    while (!queue.empty()) {
        const auto currentManifest = std::filesystem::weakly_canonical(queue.back());
        queue.pop_back();
        if (!visited.insert(currentManifest).second) continue;

        std::ifstream file(currentManifest);
        if (!file) continue;
        std::string line;
        bool inDependencies = false;
        while (std::getline(file, line)) {
            const auto comment = line.find('#');
            if (comment != std::string::npos) line.erase(comment);
            const auto first = line.find_first_not_of(" \t\r\n");
            if (first == std::string::npos) continue;
            const auto last = line.find_last_not_of(" \t\r\n");
            line = line.substr(first, last - first + 1);

            if (!line.empty() && line.front() == '[' && line.back() == ']') {
                inDependencies = (line == "[dependencies]");
                continue;
            }
            if (!inDependencies) continue;

            std::smatch match;
            if (!std::regex_search(line, match, pathDep)) continue;
            const auto depDir = std::filesystem::weakly_canonical(currentManifest.parent_path() / match[1].str());
            if (!std::filesystem::exists(depDir) || !std::filesystem::is_directory(depDir)) continue;

            const auto src = depDir / "src";
            roots.push_back(std::filesystem::exists(src) && std::filesystem::is_directory(src) ? src : depDir);
            const auto nestedManifest = depDir / "zlpkg.toml";
            if (std::filesystem::exists(nestedManifest)) queue.push_back(nestedManifest);
        }
    }
    return roots;
}

std::filesystem::path resolveStdlibRoot(const char* argv0) {
    if (const char* env = std::getenv("ZL_STDLIB_ROOT")) {
        if (*env != '\0') return std::filesystem::path(env);
    }
    if (const char* env = std::getenv("ZL_HOME")) {
        if (*env != '\0') return std::filesystem::path(env) / "lib" / "zl" / "stdlib";
    }

    const auto exeDir = zl::common::executableDir(argv0);
    const auto adjacent = exeDir / "stdlib";
    if (std::filesystem::exists(adjacent)) return adjacent;

    // Installed layout: <prefix>/bin/zl -> <prefix>/lib/zl/stdlib.
    const auto installed = exeDir.parent_path() / "lib" / "zl" / "stdlib";
    if (std::filesystem::exists(installed)) return installed;

    return adjacent;
}

} // namespace

int main(int argc, char** argv) {
    if (argc >= 2) {
        const std::string command = argv[1];
        if (command == "--emit-machine-code") {
            if (argc != 4) {
                std::cerr << "usage: zl --emit-machine-code <output.zlm> <file.zl>\n";
                return 2;
            }
            try {
                std::vector<std::filesystem::path> roots;
                if (const char* env = std::getenv("ZL_EXTRA_ROOTS"))
                    if (*env != '\0') for (auto& root : splitPathList(env)) roots.push_back(std::move(root));
                const auto stdlibRoot = resolveStdlibRoot(argv[0]);
                const auto stdlibVersion = zl::common::checkStdlibVersion(stdlibRoot, ZL_VERSION_STRING);
                if (!stdlibVersion.compatible) { std::cerr << "error: " << stdlibVersion.error << "\n"; return 3; }
                roots.push_back(stdlibRoot);
                zl::ModuleLoader loader(argv[3], roots);
                auto program = loader.load();
                zl::TypeChecker typeChecker;
                typeChecker.check(*program, /*requireMain=*/false);
                auto lowered = zl::ir::lowerProgram(*program, /*nativeOnly=*/true);
                if (!lowered.complete) {
                    for (const auto& d : lowered.diagnostics) std::cerr << d << "\n";
                    return 4;
                }
                auto optimized = zl::ir::optimize(lowered.module);
                auto machine = zl::machine::emitX64(optimized);
                if (!machine.success) { std::cerr << "machine compile error: " << machine.error << "\n"; return 4; }
                std::ofstream out(argv[2], std::ios::binary);
                if (!out) { std::cerr << "error: cannot open machine output '" << argv[2] << "'\n"; return 5; }
                auto u32 = [&](std::uint32_t v) { out.write(reinterpret_cast<const char*>(&v), 4); };
                auto u64 = [&](std::uint64_t v) { out.write(reinterpret_cast<const char*>(&v), 8); };
                out.write("ZLM1", 4); u32(1); u32(static_cast<std::uint32_t>(machine.functions.size()));
                std::uint64_t offset = 0;
                for (const auto& fn : machine.functions) {
                    u32(static_cast<std::uint32_t>(fn.name.size())); out.write(fn.name.data(), static_cast<std::streamsize>(fn.name.size()));
                    u64(offset); u64(static_cast<std::uint64_t>(fn.bytes.size())); u64(static_cast<std::uint64_t>(fn.parameterCount));
                    offset += fn.bytes.size();
                }
                for (const auto& fn : machine.functions) out.write(reinterpret_cast<const char*>(fn.bytes.data()), static_cast<std::streamsize>(fn.bytes.size()));
                return out.good() ? 0 : 5;
            } catch (const std::exception& e) {
                std::cerr << "machine compile error: " << e.what() << "\n";
                return 1;
            }
        }
        if (command == "--emit-mir") {
            if (argc != 4) {
                std::cerr << "usage: zl --emit-mir <output|-> <file.zl>\n";
                return 2;
            }
            try {
                std::vector<std::filesystem::path> roots;
                if (const char* env = std::getenv("ZL_EXTRA_ROOTS"))
                    if (*env != '\0') for (auto& root : splitPathList(env)) roots.push_back(std::move(root));
                const auto stdlibRoot = resolveStdlibRoot(argv[0]);
                const auto stdlibVersion = zl::common::checkStdlibVersion(stdlibRoot, ZL_VERSION_STRING);
                if (!stdlibVersion.compatible) { std::cerr << "error: " << stdlibVersion.error << "\n"; return 3; }
                roots.push_back(stdlibRoot);
                zl::ModuleLoader loader(argv[3], roots);
                auto program = loader.load();
                // The MIR lowerer reads the checker's recorded expression types,
                // so semantic analysis must run first and on the same program.
                zl::TypeChecker typeChecker;
                typeChecker.check(*program, /*requireMain=*/false);
                const auto lowered = zl::mir::lowerProgram(*program, typeChecker);
                for (const auto& diagnostic : lowered.diagnostics) std::cerr << "note: " << diagnostic << "\n";

                const auto report = zl::mir::verifyModule(lowered.module);
                if (!report.ok()) {
                    std::cerr << report.describe();
                    return 4;
                }
                const std::string text = zl::mir::printModule(lowered.module);
                if (std::string(argv[2]) == "-") {
                    std::cout << text;
                    return std::cout.good() ? 0 : 5;
                }
                std::ofstream out(argv[2], std::ios::binary);
                if (!out) { std::cerr << "error: cannot open MIR output '" << argv[2] << "'\n"; return 5; }
                out << text;
                return out.good() ? 0 : 5;
            } catch (const std::exception& e) {
                std::cerr << "MIR compile error: " << e.what() << "\n";
                return 1;
            }
        }
        if (command == "--emit-ssa") {
            // Like --emit-mir, but with mutable locals promoted to block
            // parameters (the MIR's SSA form) where that is provably safe. This
            // is the seam that makes explicit data flow visible: a value written
            // on two branches and read after the join shows up as a block
            // parameter with one argument per incoming edge, instead of as a
            // store/load pair whose merge has to be inferred.
            if (argc != 4) {
                std::cerr << "usage: zl --emit-ssa <output|-> <file.zl>\n";
                return 2;
            }
            try {
                std::vector<std::filesystem::path> roots;
                if (const char* env = std::getenv("ZL_EXTRA_ROOTS"))
                    if (*env != '\0') for (auto& root : splitPathList(env)) roots.push_back(std::move(root));
                const auto stdlibRoot = resolveStdlibRoot(argv[0]);
                const auto stdlibVersion = zl::common::checkStdlibVersion(stdlibRoot, ZL_VERSION_STRING);
                if (!stdlibVersion.compatible) { std::cerr << "error: " << stdlibVersion.error << "\n"; return 3; }
                roots.push_back(stdlibRoot);
                zl::ModuleLoader loader(argv[3], roots);
                auto program = loader.load();
                zl::TypeChecker typeChecker;
                typeChecker.check(*program, /*requireMain=*/false);
                auto lowered = zl::mir::lowerProgram(*program, typeChecker);
                for (const auto& diagnostic : lowered.diagnostics) std::cerr << "note: " << diagnostic << "\n";
                auto report = zl::mir::verifyModule(lowered.module);
                if (!report.ok()) { std::cerr << report.describe(); return 4; }

                std::size_t promoted = 0, slots = 0, loads = 0, stores = 0;
                bool verboseSsa = std::getenv("ZL_MIR_SSA_VERBOSE") != nullptr;
                for (auto& function : lowered.module.functions) {
                    const auto promotion = zl::mir::promoteSlotsToBlockParameters(function);
                    promoted += promotion.promotedSlots;
                    slots += promotion.parametersAdded;
                    loads += promotion.loadsRemoved;
                    stores += promotion.storesRemoved;
                    if (verboseSsa && !promotion.skipped.empty()) {
                        for (const auto& reason : promotion.skipped) {
                            std::cerr << "  ssa: " << function.name << ": " << reason << "\n";
                        }
                    }
                }
                std::cerr << "ssa: promoted " << promoted << " slot(s) into " << slots
                          << " block parameter(s); removed " << loads << " load(s), "
                          << stores << " store(s)\n";

                // The promotion must leave verifiable MIR behind; re-verify so a
                // bug in the rewrite cannot be mistaken for a bug in the input.
                report = zl::mir::verifyModule(lowered.module);
                if (!report.ok()) { std::cerr << "after ssa promotion:\n" << report.describe(); return 4; }

                const std::string text = zl::mir::printModule(lowered.module);
                if (std::string(argv[2]) == "-") {
                    std::cout << text;
                    return std::cout.good() ? 0 : 5;
                }
                std::ofstream out(argv[2], std::ios::binary);
                if (!out) { std::cerr << "error: cannot open MIR output '" << argv[2] << "'\n"; return 5; }
                out << text;
                return out.good() ? 0 : 5;
            } catch (const std::exception& e) {
                std::cerr << "MIR compile error: " << e.what() << "\n";
                return 1;
            }
        }
        if (command == "--mir-vm") {
            // Run a program through the MIR -> bytecode backend path:
            // source -> type analysis -> MIR -> verify -> bytecode -> VM.
            // This is the differential sibling of the default AST -> bytecode
            // path; both must produce identical observable behaviour.
            if (argc < 3) {
                std::cerr << "usage: zl --mir-vm <file.zl> [program args...]\n";
                return 2;
            }
            try {
                std::vector<std::filesystem::path> roots;
                if (const char* env = std::getenv("ZL_EXTRA_ROOTS"))
                    if (*env != '\0') for (auto& root : splitPathList(env)) roots.push_back(std::move(root));
                const auto stdlibRoot = resolveStdlibRoot(argv[0]);
                const auto stdlibVersion = zl::common::checkStdlibVersion(stdlibRoot, ZL_VERSION_STRING);
                if (!stdlibVersion.compatible) { std::cerr << "error: " << stdlibVersion.error << "\n"; return 3; }
                roots.push_back(stdlibRoot);
                zl::ModuleLoader loader(argv[2], roots);
                auto program = loader.load();
                zl::TypeChecker typeChecker;
                typeChecker.check(*program, /*requireMain=*/true);
                auto lowered = zl::mir::lowerProgram(*program, typeChecker);
                auto report = zl::mir::verifyModule(lowered.module);
                if (!report.ok()) { std::cerr << report.describe(); return 4; }
                // Optional: run MIR in its SSA form (mutable locals promoted to
                // block parameters) before translating. The bytecode backend
                // must behave identically either way - block parameters and
                // slots are interchangeable in meaning - so this doubles as a
                // differential check of the promotion itself.
                if (std::getenv("ZL_MIR_PROMOTE") != nullptr) {
                    for (auto& function : lowered.module.functions)
                        (void)zl::mir::promoteSlotsToBlockParameters(function);
                    report = zl::mir::verifyModule(lowered.module);
                    if (!report.ok()) {
                        std::cerr << "after ssa promotion:\n" << report.describe();
                        return 4;
                    }
                }
                const auto backend = zl::mir::compileModuleToBytecode(lowered.module);
                if (!backend.ok()) {
                    for (const auto& e : backend.errors) std::cerr << "MIR bytecode error: " << e << "\n";
                    return 4;
                }
                if (backend.stubbed != 0) {
                    std::cerr << "MIR bytecode: " << backend.stubbed
                              << " function(s) not translatable (stubbed; reachable ones raise at runtime)\n";
                }
                for (std::size_t i = 0; i < backend.stubbedFunctions.size() && i < 400; ++i) {
                    std::cerr << "  stub: " << backend.stubbedFunctions[i];
                    if (i < backend.stubbedReasons.size() && !backend.stubbedReasons[i].empty())
                        std::cerr << " - " << backend.stubbedReasons[i];
                    std::cerr << "\n";
                }
                std::vector<std::string> programArgs;
                for (int i = 3; i < argc; ++i) programArgs.emplace_back(argv[i]);
                zl::VM vm;
                return vm.run(backend.chunk, programArgs);
            } catch (const zl::SystemExitException& ex) {
                return ex.code;
            } catch (const zl::ModuleError& e) {
                std::cerr << "module error: " << e.what() << "\n";
                return 1;
            } catch (const zl::TypeCheckError& e) {
                std::cerr << "compile error: " << e.what() << "\n";
                return 1;
            } catch (const zl::ZlThrownException& e) {
                if (e.value()) {
                    auto it = e.value()->fields.find("message");
                    std::string msg = e.what();
                    if (it != e.value()->fields.end() && std::holds_alternative<std::string>(it->second))
                        msg = std::get<std::string>(it->second);
                    std::cerr << "runtime error (" << e.value()->className << "): " << msg << "\n";
                    return 1;
                }
                std::cerr << "runtime error: " << e.what() << "\n";
                return 1;
            } catch (const std::exception& e) {
                std::cerr << "MIR bytecode error: " << e.what() << "\n";
                return 1;
            }
        }
        if (command == "--emit-native") {
            if (argc != 4) {
                std::cerr << "usage: zl --emit-native <output.cpp> <file.zl>\n";
                return 2;
            }
            const char* nativeOut = argv[2];
            const char* nativeEntry = argv[3];
            try {
                std::vector<std::filesystem::path> roots;
                if (const char* env = std::getenv("ZL_EXTRA_ROOTS")) {
                    if (*env != '\0') for (auto& root : splitPathList(env)) roots.push_back(std::move(root));
                }
                const auto stdlibRoot = resolveStdlibRoot(argv[0]);
                const auto stdlibVersion = zl::common::checkStdlibVersion(stdlibRoot, ZL_VERSION_STRING);
                if (!stdlibVersion.compatible) { std::cerr << "error: " << stdlibVersion.error << "\n"; return 3; }
                roots.push_back(stdlibRoot);
                zl::ModuleLoader loader(nativeEntry, roots);
                auto program = loader.load();
                zl::TypeChecker typeChecker;
                typeChecker.check(*program, /*requireMain=*/false);
                const auto nativeResult = zl::native::emitMirCpp(*program);
                if (!nativeResult.success) { std::cerr << "native compile error: " << nativeResult.error << "\n"; return 4; }
                std::ofstream out(nativeOut, std::ios::binary);
                if (!out) { std::cerr << "error: cannot open native output '" << nativeOut << "'\n"; return 5; }
                out << nativeResult.source;
                return out.good() ? 0 : 5;
            } catch (const std::exception& e) {
                std::cerr << "native compile error: " << e.what() << "\n";
                return 1;
            }
        }
        if (command == "--parse-only") {
            if (argc != 3) {
                std::cerr << "usage: zl --parse-only <file.zl>\n";
                return 2;
            }
            try {
                std::ifstream in(argv[2], std::ios::binary);
                if (!in) {
                    std::cerr << "error: cannot open source file '" << argv[2] << "\'\n";
                    return 2;
                }
                std::ostringstream source;
                source << in.rdbuf();
                zl::Lexer lexer(source.str());
                auto tokens = lexer.tokenize();
                zl::Parser parser(std::move(tokens));
                (void)parser.parse();
                return 0;
            } catch (const zl::ParseError& e) {
                std::cerr << "syntax error: " << e.what() << "\n";
                return 1;
            } catch (const std::exception& e) {
                std::cerr << "error: " << e.what() << "\n";
                return 1;
            }
        }
        if (command == "--version" || command == "-v") {
            std::cout << "ZL " << ZL_VERSION_STRING << "\n";
            return 0;
        }
        if (command == "--help" || command == "-h") {
            std::cout << "usage:\n"
                         "  zl [--root <path>]... <file.zl> [program args...]\n"
                         "  zl --check [--root <path>]... <file.zl>\n"
                         "  zl --parse-only <file.zl>\n"
                         "  zl --emit-native <output.cpp> <file.zl>\n"
                         "  zl --emit-machine-code <output.zlm> <file.zl>\n"
                         "  zl --mir-vm <file.zl> [program args...]\n"
                         "  zl --emit-mir <output|-> <file.zl>\n"
                         "  zl --emit-ssa <output|-> <file.zl>\n"
                         "  zl --version\n"
                         "  zl --help\n";
            return 0;
        }
    }

    // Extra module search roots, in the precedence order zl_language will
    // try them (after the project's own sourceRoot_, which ModuleLoader
    // always tries first). This is the "dependency-unaware interpreter"
    // half of Phase 6's package manager: zl_language itself has no notion
    // of zlpkg.toml/.zlpkg caches/git - it just takes an ordered list of
    // roots, the same way it always has for the stdlib root, and `zlpkg
    // run` is responsible for resolving dependencies into roots and
    // passing them here (via repeated --root flags).
    //
    //   1. --root <path> flags, one per dependency, in the order given
    //      (this is how `zlpkg run` wires resolved dependencies in)
    //   2. ZL_EXTRA_ROOTS, an OS-path-list env var, for ad-hoc/manual use
    //      without going through zlpkg at all
    //   3. the stdlib root (ZL_STDLIB_ROOT, or <exe dir>/stdlib), always
    //      last, so a project or dependency can shadow a stdlib package
    //      with its own file of the same dotted path
    std::vector<std::filesystem::path> extraRoots;

    bool checkOnly = false;
    if (argc >= 2 && std::string(argv[1]) == "--check") {
        checkOnly = true;
        for (int i = 2; i < argc; ++i) argv[i - 1] = argv[i];
        --argc;
    }

    int argi = 1;
    for (; argi < argc; ++argi) {
        std::string arg = argv[argi];
        if (arg == "--root") {
            if (argi + 1 >= argc) {
                std::cerr << "error: --root requires a path argument\n";
                return 2;
            }
            extraRoots.emplace_back(argv[++argi]);
        } else if (arg.rfind("--root=", 0) == 0) {
            extraRoots.emplace_back(arg.substr(7));
        } else {
            break; // first non-flag argument is the entry file
        }
    }

    if (argi >= argc) {
        std::cerr << "usage: zl [--root <path>]... <file.zl> [program args...]\n";
        return 1;
    }
    const char* entryFile = argv[argi];

    // Direct editor/--check invocations do not pass through zlpkg, so discover
    // local path dependencies from the nearest zlpkg.toml. Git dependencies
    // remain managed by zlpkg and are not fetched by the compiler.
    if (checkOnly) {
        const auto manifest = findNearestManifest(entryFile);
        if (!manifest.empty()) {
            auto discovered = discoverLocalPackageRoots(manifest);
            extraRoots.insert(extraRoots.end(), discovered.begin(), discovered.end());
        }
    }

    if (const char* env = std::getenv("ZL_EXTRA_ROOTS")) {
        if (*env != '\0') {
            for (auto& root : splitPathList(env)) extraRoots.push_back(std::move(root));
        }
    }
    const auto stdlibRoot = resolveStdlibRoot(argv[0]);
    const auto stdlibVersion = zl::common::checkStdlibVersion(stdlibRoot, ZL_VERSION_STRING);
    if (!stdlibVersion.compatible) {
        std::cerr << "error: " << stdlibVersion.error << "\n";
        return 3;
    }
    extraRoots.push_back(stdlibRoot);

    // Anything after the entry file is passed through to main(args: list<string>).
    std::vector<std::string> programArgs;
    for (int i = argi + 1; i < argc; ++i) programArgs.emplace_back(argv[i]);

    try {
        // Parses the entry file plus everything it (transitively) imports -
        // see module_loader.hpp for how `import a.b.C` resolves to a file
        // path - and merges them into one Program. Enforces the "class name
        // matches file name" rule per file (including the entry file
        // itself), so this fully replaces what main() used to do by hand
        // for just the single entry file.
        zl::ModuleLoader loader(entryFile, extraRoots);
        auto program = loader.load();

        // Semantic analysis: enforce static types before generating bytecode.
        zl::TypeChecker typeChecker;
        typeChecker.check(*program, /*requireMain=*/!checkOnly);

        if (checkOnly) {
            // Check mode is parse + type-check only. Never compile or execute.
            return 0;
        }

        zl::Compiler compiler;
        zl::Chunk chunk = compiler.compile(*program);

        zl::VM vm;
        return vm.run(chunk, programArgs);
    } catch (const zl::SystemExitException& ex) {
        // System.exit(code) - deliberately NOT caught by zl's own try/catch
        // (it isn't a std::runtime_error), so it always terminates the program.
        return ex.code;
    } catch (const zl::ModuleError& e) {
        std::cerr << "module error: " << e.what() << "\n";
        return 1;
    } catch (const zl::ParseError& e) {
        std::cerr << "syntax error: " << e.what() << "\n";
        return 1;
    } catch (const zl::TypeCheckError& e) {
        std::cerr << "compile error: " << e.what() << "\n";
        return 1;
    } catch (const zl::ZlThrownException& e) {
        std::string message = e.what();
        if (e.value()) {
            auto it = e.value()->fields.find("message");
            if (it != e.value()->fields.end() && std::holds_alternative<std::string>(it->second)) {
                message = std::get<std::string>(it->second);
            }
            auto traceIt = e.value()->fields.find("stackTrace");
            if (traceIt != e.value()->fields.end() && std::holds_alternative<std::string>(traceIt->second)) {
                const auto& trace = std::get<std::string>(traceIt->second);
                if (!trace.empty()) {
                    std::cerr << "runtime error: " << message << "\n" << trace << "\n";
                    return 1;
                }
            }
        }
        std::cerr << "runtime error: " << message << "\n";
        return 1;
    } catch (const std::exception& e) {
        std::cerr << "runtime error: " << e.what() << "\n";
        return 1;
    }
}
