// MIR type-system integration regressions.
//
// MIR is the layer later optimisation and backend stages reason from, so its
// types must *be* the language's types: primitives, classes, generics,
// instantiated generics, function types, unions, the Option/Result sums,
// collections, and nullability - with the canonical resolved shape preserved
// through lowering and with no place where a dynamic value quietly becomes a
// typed one. Each test here names a way the layer used to lose or invent type
// information, and pins the behaviour that must not regress.
//
// The file has two halves:
//   * pure type-table tests (TypeArena, sum predicates) - no front end;
//   * pipeline tests that drive ModuleLoader + TypeChecker + lowerProgram +
//     verifyModule (and the bytecode backend) over real ZL source, mirroring
//     the harness in mir_lowering_tests.cpp.
#include "zl/compiler/module_loader.hpp"
#include "zl/compiler/type_checker.hpp"
#include "zl/mir/lowering.hpp"
#include "zl/mir/printer.hpp"
#include "zl/mir/ssa.hpp"
#include "zl/mir/type.hpp"
#include "zl/mir/verifier.hpp"
#include "zl/mir/vm_backend.hpp"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace {

int failures = 0;

void require(bool condition, const std::string& message) {
    if (condition) return;
    std::cerr << "mir type regression: " << message << '\n';
    ++failures;
}

// ---------------------------------------------------------------------------
// Pure type-table tests
// ---------------------------------------------------------------------------

void typeTableTests() {
    zl::mir::TypeArena arena;

    // Primitives and the composites keep their canonical ids.
    require(arena.intType() != arena.doubleType(), "int and double must be distinct types");
    require(arena.stringType() != arena.objectType("string"), "string is not an object named string");

    // Interning: same shape, same id; different shape, different id.
    const std::uint32_t listInt = arena.listType(arena.intType());
    require(listInt == arena.listType(arena.intType()), "list<int> must intern to one id");
    require(listInt != arena.listType(arena.stringType()), "list<int> and list<string> are different types");

    // The five shapes from the brief, nested end to end. A nested argument is
    // a full type, not a rendered string, so depth survives.
    const std::uint32_t listString = arena.listType(arena.stringType());
    const std::uint32_t mapStringInt = arena.mapType(arena.stringType(), arena.intType());
    const std::uint32_t mapStringListInt = arena.mapType(arena.stringType(), listInt);
    const std::uint32_t optionListInt = arena.optionType(listInt);
    require(mapStringListInt != mapStringInt, "Map<string,List<int>> is not Map<string,int>");
    require(optionListInt != listInt, "Option<List<int>> is not List<int>");
    require(arena.render(mapStringListInt) == "map<string,list<int>>",
            "nested map renders with its nested argument intact");
    require(arena.render(optionListInt) == "Option<list<int>>", "Option<List<int>> renders as the source spells it");
    require(arena.find(optionListInt) != nullptr && arena.find(optionListInt)->arguments.size() == 1 &&
                arena.find(optionListInt)->arguments.front() == listInt,
            "the Option payload is the List<int> type id, not a string");

    // Instantiated generic classes keep the full argument list.
    const std::uint32_t boxListInt = arena.objectType("Box", {listInt});
    require(arena.render(boxListInt) == "Box<list<int>>", "generic class instantiation renders its arguments");

    // Function types.
    zl::mir::FunctionSignature signature;
    signature.parameterTypes = {arena.intType(), arena.stringType()};
    signature.returnType = arena.boolType();
    const std::uint32_t fn = arena.functionType(signature);
    const zl::mir::Type* fnType = arena.find(fn);
    require(fnType != nullptr && fnType->kind == zl::mir::TypeKind::Function &&
                fnType->signature.parameterTypes.size() == 2 &&
                fnType->signature.parameterTypes[0] == arena.intType() &&
                fnType->signature.returnType == arena.boolType(),
            "function type keeps its parameter and result types");

    // Unions: order-independent identity, nullability from members.
    const std::uint32_t intString = arena.unionType({arena.intType(), arena.stringType()});
    const std::uint32_t stringInt = arena.unionType({arena.stringType(), arena.intType()});
    require(intString == stringInt, "int|string and string|int are the same type");
    require(arena.isNullable(intString), "int|string admits null because string does");
    require(!arena.isNullable(arena.unionType({arena.intType(), arena.doubleType()})),
            "int|double does not admit null");

    // Option / Result are first-class kinds with payload identities.
    const std::uint32_t optionInt = arena.optionType(arena.intType());
    const std::uint32_t optionString = arena.optionType(arena.stringType());
    require(optionInt != optionString, "Option<int> and Option<string> are different types");
    require(arena.render(optionInt) == "Option<int>", "Option<int> renders in source spelling");
    const std::uint32_t resultIntString = arena.resultType(arena.intType(), arena.stringType());
    require(arena.render(resultIntString) == "Result<int,string>", "Result<T,E> renders in source spelling");
    require(arena.isNullable(optionInt), "Option is a reference type and is nullable");
    require(arena.isNullable(resultIntString), "Result is a reference type and is nullable");

    // Sum predicates accept both spellings; ordinary classes do not qualify.
    const std::uint32_t someInt = arena.objectType("Some", {arena.intType()});
    const std::uint32_t noneInt = arena.objectType("None", {arena.intType()});
    const std::uint32_t okIntString = arena.objectType("Ok", {arena.intType(), arena.stringType()});
    const std::uint32_t plainUserClass = arena.objectType("Widget", {arena.intType()});
    require(zl::mir::isOptionType(*arena.find(optionInt)), "the Option kind is an option");
    require(zl::mir::isOptionType(*arena.find(someInt)), "Some<int> reads as an option");
    require(zl::mir::isOptionType(*arena.find(noneInt)), "None<int> reads as an option");
    require(zl::mir::isResultType(*arena.find(resultIntString)), "the Result kind is a result");
    require(zl::mir::isResultType(*arena.find(okIntString)), "Ok<int,string> reads as a result");
    require(!zl::mir::isSumType(*arena.find(plainUserClass)), "a user generic class is not a sum");

    // Payload extraction works across spellings.
    require(zl::mir::optionPayloadFor(*arena.find(someInt)) == arena.intType(),
            "Some<int>'s payload is int");
    require(zl::mir::optionPayloadFor(*arena.find(optionListInt)) == listInt,
            "Option<List<int>>'s payload is the List<int> id");
    const zl::mir::ResultParts parts = zl::mir::resultPartsFor(*arena.find(okIntString));
    require(parts.ok == arena.intType() && parts.error == arena.stringType(),
            "Ok<int,string> carries [int, string]");

    // The sum relation, composed with an assignability lambda the way the
    // verifier composes it: same family, assignable payloads, in either
    // spelling - and nothing across families or mismatched payloads.
    const auto exact = [](std::uint32_t a, std::uint32_t b) { return a == b; };
    require(zl::mir::sumAssignable(*arena.find(someInt), *arena.find(optionInt), exact),
            "Some<int> is assignable to Option<int>");
    require(zl::mir::sumAssignable(*arena.find(noneInt), *arena.find(optionInt), exact),
            "None<int> is assignable to Option<int>");
    require(zl::mir::sumAssignable(*arena.find(okIntString), *arena.find(resultIntString), exact),
            "Ok<int,string> is assignable to Result<int,string>");
    require(!zl::mir::sumAssignable(*arena.find(arena.objectType("Some", {arena.stringType()})),
                                    *arena.find(optionInt), exact),
            "Some<string> is not assignable to Option<int>");
    require(!zl::mir::sumAssignable(*arena.find(optionInt), *arena.find(resultIntString), exact),
            "Option is not assignable to Result");
}

// ---------------------------------------------------------------------------
// Pipeline harness
// ---------------------------------------------------------------------------

struct Lowered {
    zl::mir::Module module;
    std::vector<std::string> diagnostics;
    std::string text;
    std::string errors;
    bool ok{false};
};

Lowered lower(const std::string& name, const std::string& source) {
    namespace fs = std::filesystem;
    const fs::path directory = fs::temp_directory_path() / ("zl_mir_type_" + name);
    fs::create_directories(directory);
    const fs::path file = directory / (name + ".zl");
    {
        std::ofstream out(file);
        out << source;
    }

    Lowered result;
    try {
        zl::ModuleLoader loader(file, std::vector<fs::path>{});
        auto program = loader.load();
        zl::TypeChecker checker;
        checker.check(*program, /*requireMain=*/false);
        auto lowered = zl::mir::lowerProgram(*program, checker);
        result.diagnostics = lowered.diagnostics;
        const auto report = zl::mir::verifyModule(lowered.module);
        result.errors = report.ok() ? std::string{} : report.describe();
        result.ok = report.ok();
        result.text = zl::mir::printModule(lowered.module);
        result.module = std::move(lowered.module);
    } catch (const std::exception& e) {
        result.errors = std::string("exception: ") + e.what();
    }
    return result;
}

[[nodiscard]] std::string functionText(const Lowered& lowered, const std::string& prefix) {
    const std::size_t start = lowered.text.find("func " + prefix);
    if (start == std::string::npos) return {};
    const std::size_t end = lowered.text.find("\nfunc ", start + 1);
    return lowered.text.substr(start, end == std::string::npos ? std::string::npos : end - start);
}

[[nodiscard]] bool contains(const Lowered& lowered, const std::string& functionPrefix,
                            const std::string& needle) {
    return functionText(lowered, functionPrefix).find(needle) != std::string::npos;
}

const zl::mir::Function* findFunction(const Lowered& lowered, const std::string& name) {
    for (const auto& function : lowered.module.functions) {
        if (function.name == name) return &function;
    }
    return nullptr;
}

// The id of the interned type whose render is `spelling`, or 0.
std::uint32_t findType(const Lowered& lowered, const std::string& spelling) {
    for (std::uint32_t id = 1; id < lowered.module.types.size(); ++id) {
        if (lowered.module.types.render(id) == spelling) return id;
    }
    return 0;
}

// ---------------------------------------------------------------------------
// Pipeline tests: canonical types through lowering
// ---------------------------------------------------------------------------

void nestedGenericPreservationTest() {
    const Lowered lowered = lower("Nested",
        "class Nested {\n"
        "    func main(): void {\n"
        "        var a = new List<int>()\n"
        "        var b = new List<string>()\n"
        "        var c = new Map<string, int>()\n"
        "        var d = new Map<string, List<int>>()\n"
        "        var e = new Option<List<int>>()\n"
        "        log(a)\n"
        "        log(b)\n"
        "        log(c)\n"
        "        log(d)\n"
        "        log(e)\n"
        "    }\n"
        "}\n");
    require(lowered.ok, "nested generics lowered to invalid MIR:\n" + lowered.errors);

    const std::uint32_t listInt = findType(lowered, "List<int>");
    const std::uint32_t listString = findType(lowered, "List<string>");
    const std::uint32_t mapStringInt = findType(lowered, "Map<string,int>");
    const std::uint32_t mapStringListInt = findType(lowered, "Map<string,List<int>>");
    const std::uint32_t optionListInt = findType(lowered, "Option<List<int>>");
    require(listInt != 0 && listString != 0, "List<int> and List<string> must both appear as types");
    require(listInt != listString, "List<int> and List<string> must not collapse to one type");
    require(mapStringInt != 0, "Map<string,int> must appear as a type");
    require(mapStringListInt != 0, "Map<string,List<int>> must appear as a type");
    require(mapStringListInt != mapStringInt, "Map<string,List<int>> must not collapse to Map<string,int>");
    require(optionListInt != 0, "Option<List<int>> must appear as a type");

    // Nested arguments are type ids, not strings: the map's value argument is
    // the same id the list variable's slot carries.
    const zl::mir::Type* mapType = lowered.module.types.find(mapStringListInt);
    require(mapType != nullptr && mapType->arguments.size() == 2 && mapType->arguments[1] == listInt,
            "Map<string,List<int>>'s value argument is the List<int> type id");
    const zl::mir::Type* optionType = lowered.module.types.find(optionListInt);
    require(optionType != nullptr && optionType->arguments.size() == 1 &&
                optionType->arguments.front() == listInt,
            "Option<List<int>>'s payload is the List<int> type id");

    // The local slots carry those same canonical types.
    const zl::mir::Function* main = findFunction(lowered, "Nested.main()");
    require(main != nullptr, "Nested.main() must be lowered");
    bool sawListIntSlot = false;
    bool sawNestedSlot = false;
    for (const auto& slot : main->slots) {
        if (slot.type == listInt) sawListIntSlot = true;
        if (slot.type == mapStringListInt || slot.type == optionListInt) sawNestedSlot = true;
    }
    require(sawListIntSlot, "a local declared List<int> keeps that type on its slot");
    require(sawNestedSlot, "nested-generic locals keep their canonical slot types");
}

void optionResultKindTest() {
    const Lowered lowered = lower("Sums",
        "class Sums {\n"
        "    static func find(int n): Option<int> {\n"
        "        if (n > 0) { return new Some<int>(n) }\n"
        "        return new None<int>()\n"
        "    }\n"
        "    static func parse(int n): Result<int, string> {\n"
        "        if (n > 0) { return new Ok<int, string>(n) }\n"
        "        return new Err<int, string>(\"bad\")\n"
        "    }\n"
        "    func main(): void {\n"
        "        var o = Sums.find(3)\n"
        "        var r = Sums.parse(-1)\n"
        "        log(o)\n"
        "        log(r)\n"
        "    }\n"
        "}\n");
    require(lowered.ok, "Option/Result program lowered to invalid MIR:\n" + lowered.errors);

    const std::uint32_t optionInt = findType(lowered, "Option<int>");
    const std::uint32_t resultIntString = findType(lowered, "Result<int,string>");
    require(optionInt != 0 && resultIntString != 0, "declared Option/Result types must appear");
    const zl::mir::Type* option = lowered.module.types.find(optionInt);
    const zl::mir::Type* result = lowered.module.types.find(resultIntString);
    require(option != nullptr && option->kind == zl::mir::TypeKind::Option,
            "a declared Option<int> parameter/variable is the Option kind, not a plain object");
    require(result != nullptr && result->kind == zl::mir::TypeKind::Result,
            "a declared Result<int,string> is the Result kind");
    require(option != nullptr && option->arguments.size() == 1 && option->arguments.front() != 0 &&
                lowered.module.types.render(option->arguments.front()) == "int",
            "the Option kind carries its payload type");
    require(result != nullptr && result->arguments.size() == 2 &&
                lowered.module.types.render(result->arguments[0]) == "int" &&
                lowered.module.types.render(result->arguments[1]) == "string",
            "the Result kind carries [ok, error]");

    // Construction keeps the class spelling, and the return is accepted as a
    // member of the declared sum.
    const std::uint32_t someInt = findType(lowered, "Some<int>");
    require(someInt != 0, "new Some<int>(..) keeps the Some<int> class type");
    const zl::mir::Function* find = findFunction(lowered, "Sums.find(int)");
    require(find != nullptr, "Sums.find(int) must be lowered");
    bool returnsSome = false;
    for (const auto& block : find->blocks) {
        if (block.terminator.kind == zl::mir::TerminatorKind::Return &&
            block.terminator.value.type == someInt) {
            returnsSome = true;
        }
    }
    require(returnsSome, "find returns the Some<int> value it constructed");
}

void unionReturnTypeTest() {
    const Lowered lowered = lower("Uni",
        "class Uni {\n"
        "    static func pick(int flag): int|string {\n"
        "        if (flag > 0) { return 42 }\n"
        "        return \"none\"\n"
        "    }\n"
        "    func main(): void {\n"
        "        log(pick(1))\n"
        "    }\n"
        "}\n");
    require(lowered.ok, "union return type lowered to invalid MIR:\n" + lowered.errors);
    const zl::mir::Function* pick = findFunction(lowered, "Uni.pick(int)");
    require(pick != nullptr, "Uni.pick(int) must be lowered");
    const zl::mir::Type* returnType = lowered.module.types.find(pick->returnType);
    require(returnType != nullptr && returnType->kind == zl::mir::TypeKind::Union &&
                returnType->arguments.size() == 2,
            "the union return type survives as a union with both members");
    for (const auto& block : pick->blocks) {
        if (block.terminator.kind != zl::mir::TerminatorKind::Return) continue;
        if (block.terminator.value.isNone()) continue;
        const std::uint32_t returned = block.terminator.value.type;
        bool isMember = false;
        for (std::uint32_t member : returnType->arguments) {
            if (member == returned) isMember = true;
        }
        require(isMember, "every returned value is one of the union's members");
    }
}

void functionTypeTest() {
    const Lowered lowered = lower("Fns",
        "class Fns {\n"
        "    static func apply(func(int): int f, int x): int {\n"
        "        return f(x)\n"
        "    }\n"
        "    func main(): void {\n"
        "        var r = apply(func(n) => n + 1, 5)\n"
        "        log(r)\n"
        "    }\n"
        "}\n");
    require(lowered.ok, "function-typed parameter lowered to invalid MIR:\n" + lowered.errors);
    const zl::mir::Function* apply = findFunction(lowered, "Fns.apply(func(int):int)");
    if (apply == nullptr) {
        // The dispatch spelling may differ; fall back to scanning by name.
        for (const auto& function : lowered.module.functions) {
            if (function.name.rfind("Fns.apply(", 0) == 0) apply = &function;
        }
    }
    require(apply != nullptr, "Fns.apply must be lowered");
    require(apply->parameters.size() == 2, "apply takes the callable and the int");
    const zl::mir::Type* parameter = lowered.module.types.find(apply->parameters[0].type);
    require(parameter != nullptr && parameter->kind == zl::mir::TypeKind::Function &&
                parameter->signature.hasSignature &&
                parameter->signature.parameterTypes.size() == 1 &&
                parameter->signature.parameterTypes.front() == lowered.module.types.intType() &&
                parameter->signature.returnType == lowered.module.types.intType(),
            "the declared func(int): int parameter keeps its full signature");
}

// ---------------------------------------------------------------------------
// Pipeline tests: dynamic-to-static boundaries
// ---------------------------------------------------------------------------

void dynamicBoundaryTest() {
    const Lowered lowered = lower("Dyn",
        "class Dyn {\n"
        "    static func take(int x): int {\n"
        "        return x + 1\n"
        "    }\n"
        "    func main(): void {\n"
        "        unknown alias = 5\n"
        "        var r = take(alias)\n"
        "        int m = alias\n"
        "        log(r)\n"
        "        log(m)\n"
        "    }\n"
        "}\n");
    require(lowered.ok, "dynamic boundary program lowered to invalid MIR:\n" + lowered.errors);

    const zl::mir::Function* main = findFunction(lowered, "Dyn.main()");
    require(main != nullptr, "Dyn.main() must be lowered");
    // Both boundaries are explicit refine instructions: the argument into
    // take's int parameter, and the typed declaration of m.
    std::size_t refines = 0;
    for (const auto& block : main->blocks) {
        for (const auto& instruction : block.instructions) {
            if (instruction.opcode == zl::mir::Opcode::Refine &&
                instruction.resultType == lowered.module.types.intType()) {
                ++refines;
            }
        }
    }
    require(refines >= 2, "each dynamic-to-static boundary is an explicit refine, saw " +
                              std::to_string(refines));
    require(contains(lowered, "Dyn.main()", "refine"), "the printed MIR shows the boundary");

    // And the module the backend produces asserts the same fact at runtime:
    // the boundary is not merely recorded, it is executed.
    const auto bytecode = zl::mir::compileModuleToBytecode(lowered.module);
    require(bytecode.ok(), "the MIR module translates to bytecode:\n" +
                               (bytecode.errors.empty() ? std::string{} : bytecode.errors.front()));
    bool sawAssertInt = false;
    for (const auto& instruction : bytecode.chunk.code) {
        if (instruction.op != zl::OpCode::AssertType) continue;
        if (instruction.operand < bytecode.chunk.names.size() &&
            bytecode.chunk.names[instruction.operand] == "int") {
            sawAssertInt = true;
        }
    }
    require(sawAssertInt, "the backend emits a runtime type assertion for the boundary");
}

void unionNarrowingTest() {
    const Lowered lowered = lower("Narrow",
        "class Narrow {\n"
        "    static func describe(int|string value): string {\n"
        "        return match value {\n"
        "            int n => \"number\"\n"
        "            string text => \"text\"\n"
        "            null => \"missing\"\n"
        "        }\n"
        "    }\n"
        "    func main(): void {\n"
        "        log(describe(5))\n"
        "        log(describe(\"x\"))\n"
        "    }\n"
        "}\n");
    require(lowered.ok, "union narrowing lowered to invalid MIR:\n" + lowered.errors);
    const zl::mir::Function* describe = findFunction(lowered, "Narrow.describe(int|string)");
    require(describe != nullptr, "Narrow.describe must be lowered");
    // The int arm both asks (type_test) and asserts (refine) the member, and
    // the arm's body uses the narrowed value.
    bool sawTypeTest = false;
    bool sawRefine = false;
    for (const auto& block : describe->blocks) {
        for (const auto& instruction : block.instructions) {
            if (instruction.opcode == zl::mir::Opcode::TypeTest &&
                lowered.module.types.render(instruction.testedType) == "int") {
                sawTypeTest = true;
            }
            if (instruction.opcode == zl::mir::Opcode::Refine &&
                instruction.resultType == lowered.module.types.intType()) {
                sawRefine = true;
            }
        }
    }
    require(sawTypeTest, "the match arm tests the union member with type_test");
    require(sawRefine, "the match arm narrows the subject with refine");
}

// ---------------------------------------------------------------------------
// Verifier tests: invalid type assumptions must be detectable
// ---------------------------------------------------------------------------

// A hand-built module that stores an unknown-typed value into an int slot
// without a refine. The lowerer never produces this shape any more; the
// verifier rejects it so the boundary cannot erode quietly.
void verifierRejectsUnrefinedStoreTest() {
    zl::mir::Module module; module.name = "t";
    zl::mir::TypeArena& types = module.types;

    zl::mir::Function function;
    function.id = 1;
    function.name = "f";
    function.returnType = types.voidType();
    zl::mir::Parameter dynamic;
    dynamic.name = "v";
    dynamic.type = types.unknownType();
    function.parameters.push_back(dynamic);

    zl::mir::BasicBlock entry;
    entry.id = 1;
    function.entryBlock = entry.id;

    zl::mir::Slot slot;
    slot.name = "x";
    slot.type = types.intType();
    slot.isMutable = true;
    function.slots.push_back(slot);

    zl::mir::Instruction store;
    store.opcode = zl::mir::Opcode::Store;
    store.slot = 1; // slots are 1-based
    store.operands.push_back(zl::mir::Operand::param(0, types.unknownType()));
    entry.instructions.push_back(store);
    entry.terminator.kind = zl::mir::TerminatorKind::Return;
    function.blocks.push_back(std::move(entry));
    function.rebuildEdges();
    module.functions.push_back(std::move(function));
    module.entryPoint = 1;

    const auto report = zl::mir::verifyModule(module);
    require(!report.ok(), "an unknown-typed store into an int slot without refine must be rejected");
    require(report.describe().find("refine") != std::string::npos,
            "the rejection names the missing runtime type assertion");
}

// The same module with the boundary made explicit verifies.
void verifierAcceptsRefinedStoreTest() {
    zl::mir::Module module; module.name = "t";
    zl::mir::TypeArena& types = module.types;

    zl::mir::Function function;
    function.id = 1;
    function.name = "f";
    function.returnType = types.voidType();
    zl::mir::Parameter dynamic;
    dynamic.name = "v";
    dynamic.type = types.unknownType();
    function.parameters.push_back(dynamic);

    zl::mir::BasicBlock entry;
    entry.id = 1;
    function.entryBlock = entry.id;

    zl::mir::Slot slot;
    slot.name = "x";
    slot.type = types.intType();
    slot.isMutable = true;
    function.slots.push_back(slot);

    const zl::mir::TempId refinedTemp = 1;
    zl::mir::Instruction refine;
    refine.opcode = zl::mir::Opcode::Refine;
    refine.result = refinedTemp;
    refine.resultType = types.intType();
    refine.operands.push_back(zl::mir::Operand::param(0, types.unknownType()));
    entry.instructions.push_back(refine);
    zl::mir::Instruction store;
    store.opcode = zl::mir::Opcode::Store;
    store.slot = 1; // slots are 1-based
    store.operands.push_back(zl::mir::Operand::temp(refinedTemp, types.intType()));
    entry.instructions.push_back(store);
    entry.terminator.kind = zl::mir::TerminatorKind::Return;
    function.blocks.push_back(std::move(entry));
    function.rebuildEdges();
    module.functions.push_back(std::move(function));
    module.entryPoint = 1;

    const auto report = zl::mir::verifyModule(module);
    require(report.ok(), "the refined boundary verifies:\n" + report.describe());
}

// A union return accepted with member returns, and Option assignability in
// the verifier's own relation.
void verifierSumAndUnionTest() {
    zl::mir::Module module; module.name = "t";
    zl::mir::TypeArena& types = module.types;

    const std::uint32_t optionInt = types.optionType(types.intType());
    const std::uint32_t someInt = types.objectType("Some", {types.intType()});

    zl::mir::Function function;
    function.id = 1;
    function.name = "pick";
    function.returnType = optionInt;
    zl::mir::BasicBlock entry;
    entry.id = 1;
    function.entryBlock = entry.id;
    const zl::mir::TempId temp = 1;
    zl::mir::Instruction def;
    def.opcode = zl::mir::Opcode::Alloc;
    def.result = temp;
    def.resultType = someInt;
    def.className = "Some";
    def.typeArguments = {types.intType()};
    entry.instructions.push_back(def);
    entry.terminator.kind = zl::mir::TerminatorKind::Return;
    entry.terminator.value = zl::mir::Operand::temp(temp, someInt);
    function.blocks.push_back(std::move(entry));
    function.rebuildEdges();
    module.functions.push_back(std::move(function));
    module.entryPoint = 1;

    const auto report = zl::mir::verifyModule(module);
    require(report.ok(), "Some<int> may be returned from a function declared Option<int>:\n" +
                             report.describe());

    // The negative case: Some<string> is not an Option<int>. Build the same
    // shape with the wrong payload and expect the verifier to refuse it.
    zl::mir::Module module2; module2.name = "t2";
    zl::mir::TypeArena& types2 = module2.types;
    const std::uint32_t optionInt2 = types2.optionType(types2.intType());
    const std::uint32_t someString2 = types2.objectType("Some", {types2.stringType()});
    zl::mir::Function function2;
    function2.id = 1;
    function2.name = "bad";
    function2.returnType = optionInt2;
    zl::mir::BasicBlock entry2;
    entry2.id = 1;
    function2.entryBlock = entry2.id;
    const zl::mir::TempId temp2 = 1;
    zl::mir::Instruction def2;
    def2.opcode = zl::mir::Opcode::Alloc;
    def2.result = temp2;
    def2.resultType = someString2;
    def2.className = "Some";
    def2.typeArguments = {types2.stringType()};
    entry2.instructions.push_back(def2);
    entry2.terminator.kind = zl::mir::TerminatorKind::Return;
    entry2.terminator.value = zl::mir::Operand::temp(temp2, someString2);
    function2.blocks.push_back(std::move(entry2));
    function2.rebuildEdges();
    module2.functions.push_back(std::move(function2));
    module2.entryPoint = 1;

    const auto report2 = zl::mir::verifyModule(module2);
    require(!report2.ok(), "Some<string> must not satisfy a declared Option<int>");
}

// ---------------------------------------------------------------------------
// Pipeline test: overload dispatch keeps generic identities distinct
// ---------------------------------------------------------------------------

void overloadDispatchTest() {
    const Lowered lowered = lower("Over",
        "class Over {\n"
        "    static func label(Option<int> o): string {\n"
        "        if (o.isSome()) { return \"int-box\" }\n"
        "        return \"none\"\n"
        "    }\n"
        "    static func label(List<int> l): string {\n"
        "        return \"list\"\n"
        "    }\n"
        "    func main(): void {\n"
        "        log(label(new Some<int>(1)))\n"
        "        log(label(new List<int>()))\n"
        "    }\n"
        "}\n");
    require(lowered.ok, "overloaded program lowered to invalid MIR:\n" + lowered.errors);

    // The two declarations must survive as two distinct MIR functions. Under
    // the old erasure-of-everything dispatch names both were "Over.label
    // (object)" and the second silently replaced the first in the function
    // table, so the first call dispatched to the wrong body.
    const zl::mir::Function* optionOverload = nullptr;
    const zl::mir::Function* listOverload = nullptr;
    for (const auto& function : lowered.module.functions) {
        if (function.name.rfind("Over.label(", 0) != 0) continue;
        if (function.name.find("Option") != std::string::npos) optionOverload = &function;
        if (function.name.find("List") != std::string::npos) listOverload = &function;
    }
    require(optionOverload != nullptr && listOverload != nullptr,
            "the two label overloads keep distinct names (Option vs List)");
    if (optionOverload != nullptr && listOverload != nullptr) {
        require(optionOverload->name != listOverload->name,
                "the overloads must not share one name");
        // And each call site must target its own overload. A resolved static
        // call lowers to a direct Call naming the callee's FunctionId (1-based,
        // matching its position in the module's function list).
        const zl::mir::Function* main = findFunction(lowered, "Over.main()");
        require(main != nullptr, "Over.main() must be lowered");
        const std::uint32_t optionId = static_cast<std::uint32_t>(optionOverload - lowered.module.functions.data()) + 1;
        const std::uint32_t listId = static_cast<std::uint32_t>(listOverload - lowered.module.functions.data()) + 1;
        std::size_t optionCalls = 0;
        std::size_t listCalls = 0;
        for (const auto& block : main->blocks) {
            for (const auto& instruction : block.instructions) {
                if (instruction.opcode != zl::mir::Opcode::Call &&
                    instruction.opcode != zl::mir::Opcode::InvokeStatic) continue;
                if (instruction.target.function == optionId) ++optionCalls;
                if (instruction.target.function == listId) ++listCalls;
            }
        }
        require(optionCalls == 1 && listCalls == 1,
                "each call site names the overload it resolved to (option=" +
                    std::to_string(optionCalls) + ", list=" + std::to_string(listCalls) + ")");
    }
}

// ---------------------------------------------------------------------------
// Pipeline test: SSA promotion must not retype a dynamic slot
// ---------------------------------------------------------------------------

void promotionKeepsDeclaredTypesTest() {
    const Lowered lowered = lower("Prom",
        "class Prom {\n"
        "    func main(): void {\n"
        "        list<int> numbers = [1, 2]\n"
        "        unknown alias = numbers\n"
        "        var n = 0\n"
        "        match alias {\n"
        "            int v => n = v + 1\n"
        "            _ => n = -1\n"
        "        }\n"
        "        log(n)\n"
        "        log(alias)\n"
        "    }\n"
        "}\n");
    require(lowered.ok, "promotion program lowered to invalid MIR:\n" + lowered.errors);
    Lowered& mutableModule = const_cast<Lowered&>(lowered);
    for (auto& function : mutableModule.module.functions) {
        if (function.name != "Prom.main()") continue;
        (void)zl::mir::promoteSlotsToBlockParameters(function);
    }
    const auto report = zl::mir::verifyModule(lowered.module);
    require(report.ok(), "SSA promotion must not retype a dynamic slot:\n" + report.describe());
}

} // namespace

int main() {
    typeTableTests();
    nestedGenericPreservationTest();
    optionResultKindTest();
    unionReturnTypeTest();
    functionTypeTest();
    dynamicBoundaryTest();
    unionNarrowingTest();
    verifierRejectsUnrefinedStoreTest();
    verifierAcceptsRefinedStoreTest();
    verifierSumAndUnionTest();
    overloadDispatchTest();
    promotionKeepsDeclaredTypesTest();

    if (failures != 0) {
        std::cerr << failures << " MIR type regression(s) failed\n";
        return 1;
    }
    std::cout << "all MIR type regressions passed\n";
    return 0;
}
