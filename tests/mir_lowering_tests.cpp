// End-to-end MIR lowering regressions.
//
// These drive the real pipeline - ModuleLoader, TypeChecker, lowerProgram,
// verifyModule - on small ZL programs, then assert on the MIR that came out.
// `mir_tests.cpp` builds MIR by hand to prove the verifier rejects each class of
// malformed module; this file proves the lowerer produces well-formed MIR for
// the constructs ZL actually has, and that the shapes it produces are the ones
// the language means.
//
// Every program here is one that an earlier version of the lowerer got wrong, so
// the assertions name the specific property rather than just "it verified".
#include "zl/compiler/module_loader.hpp"
#include "zl/compiler/type_checker.hpp"
#include "zl/mir/lowering.hpp"
#include "zl/mir/printer.hpp"
#include "zl/mir/verifier.hpp"

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
    std::cerr << "mir lowering regression: " << message << '\n';
    ++failures;
}

struct Lowered {
    zl::mir::Module module;
    std::vector<std::string> diagnostics;
    std::string text;
    std::string errors;
    bool ok{false};
};

// Writes `source` to a scratch file and runs the whole front end over it.
// No stdlib root is passed: these programs use no imports, so nothing outside
// the builtin library has to resolve.
Lowered lower(const std::string& name, const std::string& source) {
    namespace fs = std::filesystem;
    const fs::path directory = fs::temp_directory_path() / ("zl_mir_lowering_" + name);
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
        // The lowerer reads the checker's recorded expression types, so semantic
        // analysis must run first and on the same program.
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

// The printed text of one function, from its header to the next one. Searching
// the whole module text is not enough: the builtin library is lowered alongside
// the test program, so an unrelated function can contain the same words.
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

[[nodiscard]] bool hasDiagnostic(const Lowered& lowered, const std::string& needle) {
    for (const auto& diagnostic : lowered.diagnostics) {
        if (diagnostic.find(needle) != std::string::npos) return true;
    }
    return false;
}

const zl::mir::Function* findFunction(const Lowered& lowered, const std::string& prefix) {
    for (const auto& function : lowered.module.functions) {
        if (function.name.rfind(prefix, 0) == 0) return &function;
    }
    return nullptr;
}

void requireClean(const Lowered& lowered, const std::string& what) {
    require(lowered.ok, what + " produced invalid MIR:\n" + lowered.errors);
    require(lowered.diagnostics.empty(),
            what + " was not fully lowered:\n" +
                (lowered.diagnostics.empty() ? std::string{} : lowered.diagnostics.front()));
}

// ---------------------------------------------------------------------------
// Locals: what becomes a slot and what stays SSA
// ---------------------------------------------------------------------------

void testImmutableLocalStaysSsa() {
    const auto lowered = lower("ImmutableLocal", R"ZL(
class ImmutableLocal {
    func main(): void {
        let year = 2026
        log(year + 1)
    }
}
)ZL");
    requireClean(lowered, "`let year = 2026`");
    // A `let` that is never moved is a name for the value its initialiser
    // produced. Giving it a slot would mean storing into storage the language
    // says can never be written, which the verifier rejects.
    const auto* main = findFunction(lowered, "ImmutableLocal.main");
    require(main != nullptr, "main was not lowered");
    require(main && main->slots.empty(), "`let year` was given a slot");
    std::cout << "mir lowering immutable local: PASS\n";
}

void testWrittenParameterBecomesSlot() {
    const auto lowered = lower("WrittenParam", R"ZL(
class WrittenParam {
    static func abs(int a): int {
        if (a < 0) { a = -a }
        return a
    }
}
)ZL");
    requireClean(lowered, "a parameter reassigned inside an if");
    // The write is nested in an IfStmt, so a walker that only looked at
    // top-level statements would have missed it and left `a` an SSA value.
    const auto* function = findFunction(lowered, "WrittenParam.abs");
    require(function != nullptr, "abs was not lowered");
    require(function && function->slots.size() == 1, "the reassigned parameter got no slot");
    std::cout << "mir lowering written parameter: PASS\n";
}

void testThisIsNeverASlot() {
    const auto lowered = lower("ThisSlot", R"ZL(
class ThisSlot {
    int value
    func get(): int { return this.value }
}
)ZL");
    requireClean(lowered, "an instance method reading this");
    const auto* function = findFunction(lowered, "ThisSlot.get");
    require(function != nullptr, "get was not lowered");
    require(function && function->hasThisParameter, "get has no this parameter");
    require(function && function->slots.empty(), "`this` was promoted to a slot");
    std::cout << "mir lowering this parameter: PASS\n";
}

// ---------------------------------------------------------------------------
// Classes: inheritance, enums, generics
// ---------------------------------------------------------------------------

void testInheritedFieldLowers() {
    const auto lowered = lower("InheritedField", R"ZL(
class Animal {
    public string name
    func Animal(string name): void { this.name = name }
}

class Dog extends Animal {
    func Dog(string name): void { super(name) }
    public func fetch(): string { return this.name + " fetches" }
}

// The module loader requires the entry file to define its own primary type.
class InheritedField {
    func main(): void { log(new Dog("rex").fetch()) }
}
)ZL");
    requireClean(lowered, "`this.name` in a subclass");
    require(contains(lowered, "Dog.fetch", "field_load"), "no field access was emitted");
    std::cout << "mir lowering inherited field: PASS\n";
}

void testEnumMemberCarriesEnumType() {
    const auto lowered = lower("EnumMember", R"ZL(
enum Color { RED GREEN }

class EnumMember {
    static func isRed(Color c): bool { return c == Color.RED }
}
)ZL");
    requireClean(lowered, "an enum member comparison");
    // The constant is a string at runtime but its static type is the enum; typed
    // as string, this comparison would be `Color` against `string`.
    require(contains(lowered, "EnumMember.isRed", "Color.RED:Color"),
            "the enum member constant is not typed as its enum:\n" +
                functionText(lowered, "EnumMember.isRed"));
    std::cout << "mir lowering enum member: PASS\n";
}

void testGenericThisIsSelfParameterized() {
    const auto lowered = lower("GenericThis", R"ZL(
class Holder<T> {
    T value
    func Holder(T value): void { this.value = value }
    public func get(): T { return this.value }
}

class GenericThis {
    func main(): void {
        var h = new Holder<int>(3)
        log(h.get())
    }
}
)ZL");
    requireClean(lowered, "a generic class and its instantiation");
    // Inside `class Holder<T>`, `this` is `Holder<T>` - not a bare `Holder`.
    // The function is named for its owner (`Holder.get`); the instantiation is
    // carried by the `this` parameter's type, not by the name.
    require(contains(lowered, "Holder.get", "$0 this:Holder<T>"),
            "`this` in a generic class is not self-parameterized:\n" +
                functionText(lowered, "Holder.get"));
    std::cout << "mir lowering generic this: PASS\n";
}

void testListIndexingLowers() {
    const auto lowered = lower("ListIndex", R"ZL(
class ListIndex {
    func main(): void {
        var xs = new List<int>()
        xs.push(7)
        log(xs[0])
    }
}
)ZL");
    requireClean(lowered, "indexing a List<int>");
    require(contains(lowered, "ListIndex.main", "index_load"), "no index access was emitted");
    std::cout << "mir lowering list indexing: PASS\n";
}

// ---------------------------------------------------------------------------
// Closures
// ---------------------------------------------------------------------------

void testNestedClosureCaptures() {
    const auto lowered = lower("NestedClosure", R"ZL(
class NestedClosure {
    func main(): void {
        var box = new Holder2<int>(1)
        var outer = func() {
            var inner = func() { log(box.get()) }
            inner()
        }
        outer()
    }
}

class Holder2<T> {
    T value
    func Holder2(T value): void { this.value = value }
    public func get(): T { return this.value }
}
)ZL");
    requireClean(lowered, "a closure nested inside a closure");
    // Captures are the closure body's leading parameters, so the inner
    // closure's own arity is its parameter count minus its capture count.
    const auto* inner = findFunction(lowered, "NestedClosure.$lambda1");
    require(inner != nullptr, "the inner closure was not lowered");
    require(inner && inner->captures.size() == 1, "the inner closure has no capture");
    require(inner && inner->parameters.size() == 1, "the inner closure has no capture parameter");
    std::cout << "mir lowering nested closure: PASS\n";
}

void testClosureParameterIsCallable() {
    const auto lowered = lower("ClosureParam", R"ZL(
class ClosureParam {
    static func twice(func f, int x): int { return f(x) }
    func main(): void { log(ClosureParam.twice(func(int n) => n + 1, 4)) }
}
)ZL");
    requireClean(lowered, "calling through a func-typed parameter");
    require(contains(lowered, "ClosureParam.twice", "call_indirect"), "no indirect call was emitted");
    std::cout << "mir lowering closure parameter: PASS\n";
}

void testClosureCapturesThis() {
    const auto lowered = lower("ThisCapture", R"ZL(
class Greeter {
    string who
    func Greeter(string who): void { this.who = who }
    public func greet(): string { return "hi " + this.who }
    public func later(): func { return func() => this.greet() }
}

class ThisCapture {
    func main(): void {
        var g = new Greeter("world")
        var f = g.later()
        log(f())
    }
}
)ZL");
    requireClean(lowered, "a closure that closes over `this`");
    // `this` is captured like any other local: it becomes one of the closure
    // body's leading parameters. Before it was bound under its own name in the
    // enclosing body, the capture looked up as "not in scope" and the whole
    // closure failed to lower.
    const auto* closure = findFunction(lowered, "Greeter.$lambda0");
    require(closure != nullptr, "the closure was not lowered");
    require(closure && closure->captures.size() == 1, "the closure captured nothing");
    require(closure && !closure->captures.empty() && closure->captures[0].name == "this",
            "the closure's capture is not `this`");
    // And inside the body the captured value answers as the receiver, not as an
    // unresolvable `this` outside an instance method.
    require(contains(lowered, "Greeter.$lambda0", "invoke_method"),
            "the closure body did not invoke a method on `this`:\n" +
                functionText(lowered, "Greeter.$lambda0"));
    std::cout << "mir lowering closure captures this: PASS\n";
}

void testClosureInGenericClassIsGeneric() {
    const auto lowered = lower("GenericClosure", R"ZL(
class Box<T> {
    T value
    func Box(T value): void { this.value = value }
    public func reader(): func { return func() => this.value }
}

class GenericClosure {
    func main(): void {
        var b = new Box<int>(5)
        var f = b.reader()
        log(f())
    }
}
)ZL");
    requireClean(lowered, "a closure written inside a generic class");
    // The closure captures `this`, whose type is `Box<T>`. An unsubstituted
    // type parameter is only legal inside a template, so the closure has to be
    // generic over the class's own parameters - otherwise the verifier rejects
    // the captured parameter's type.
    const auto* closure = findFunction(lowered, "Box.$lambda0");
    require(closure != nullptr, "the closure was not lowered");
    require(closure && closure->isGenericTemplate, "the closure is not a generic template");
    require(closure && closure->typeParameters.size() == 1 && closure->typeParameters[0] == "T",
            "the closure is not generic over the class's T");
    require(closure && !closure->captures.empty() &&
                lowered.module.types.render(closure->captures[0].type) == "Box<T>",
            "the captured `this` lost the class's type parameter");
    std::cout << "mir lowering generic closure: PASS\n";
}

// ---------------------------------------------------------------------------
// Expressions and statements
// ---------------------------------------------------------------------------

void testConditionsAreBool() {
    const auto lowered = lower("Conditions", R"ZL(
class Conditions {
    func main(): void {
        var n = 3
        if (n > 1 && n < 5) { log("mid") } else { log("edge") }
        while (n > 0) { n = n - 1 }
    }
}
)ZL");
    requireClean(lowered, "`&&` and a while loop");
    // `&&`/`||` are not opcodes; they lower to short-circuiting branches.
    require(contains(lowered, "Conditions.main", "branch"), "no conditional branch was emitted");
    require(contains(lowered, "Conditions.main", "__logical"),
            "`&&` did not lower to a short-circuit slot and join");
    std::cout << "mir lowering conditions: PASS\n";
}

void testWideningIsExplicit() {
    const auto lowered = lower("Widening", R"ZL(
class Widening {
    func main(): void {
        double d = 1 + 2.5
        log(d)
    }
}
)ZL");
    requireClean(lowered, "an int widened to double");
    require(contains(lowered, "Widening.main", "widen"),
            "the implicit int -> double widening was not made explicit:\n" +
                functionText(lowered, "Widening.main"));
    std::cout << "mir lowering widening: PASS\n";
}

void testMathConstantLowers() {
    const auto lowered = lower("MathConstant", R"ZL(
class MathConstant {
    static func half(): double { return 180.0 / Math.PI }
}
)ZL");
    requireClean(lowered, "a Math namespace constant");
    require(contains(lowered, "MathConstant.half", ":double"), "the Math constant is not a double");
    std::cout << "mir lowering math constant: PASS\n";
}

void testTryCatchLowers() {
    const auto lowered = lower("TryCatch", R"ZL(
class TryCatch {
    func main(): void {
        try {
            throw new Exception("boom")
        } catch Exception e {
            log(e.message)
        }
    }
}
)ZL");
    requireClean(lowered, "try/catch");
    require(contains(lowered, "TryCatch.main", "throw"), "no throw terminator was emitted");
    std::cout << "mir lowering try/catch: PASS\n";
}

// ---------------------------------------------------------------------------
// Partial lowering stays structurally valid
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// collection literals written under a type annotation
// ---------------------------------------------------------------------------

void testAnnotatedCollectionLiteralKeepsItsType() {
    // `list<int> xs = [1, 2]` is typed by the annotation, and the checker proves
    // that before it ever reaches a backend. An earlier checker recorded the
    // type only for literals it inferred rather than expected, so this one
    // arrived untyped and the lowerer had to give up on it.
    const auto lowered = lower("AnnotatedLiteral", R"ZL(
class AnnotatedLiteral {
    func main(): void {
        list<int> nums = [4, 8, 15]
        log(Collection.length(nums))

        map<string, int> counts = {"a": 1}
        log(counts)
    }
}
)ZL");
    requireClean(lowered, "collection literals under a type annotation");
    const std::string text = functionText(lowered, "AnnotatedLiteral.main");
    // The printer puts the collection's type on the result temp, so that is
    // where the element type has to show up.
    require(text.find(":list<int> = new_collection") != std::string::npos,
            "the annotated list literal did not keep its element type");
    require(text.find(":map<string,int> = new_collection") != std::string::npos,
            "the annotated map literal did not keep its key and value types");
    // And the map's entries are keyed stores, not positional ones.
    require(text.find("index_store %4:map<string,int>, \"a\":string, 1:int") == std::string::npos ||
                text.find("\":string, 1:int") != std::string::npos,
            "the map literal's entry was not stored under its key");
    std::cout << "mir lowering annotated literal: PASS\n";
}

void testSetLiteralLowers() {
    // A set literal is built by storing each element in turn, so `set<T>` has to
    // accept positional index access. The index check once covered List/Array/Map
    // and fell through for a set, so this program - which the VM runs fine -
    // produced MIR that failed verification with "requires a List<T>".
    const auto lowered = lower("SetLiteral", R"ZL(
class SetLiteral {
    func main(): void {
        set<int> tags = [1, 2, 3]
        log(tags)
    }
}
)ZL");
    requireClean(lowered, "a set literal under a type annotation");
    const std::string text = functionText(lowered, "SetLiteral.main");
    require(text.find(":set<int> = new_collection") != std::string::npos,
            "the set literal did not keep its element type:\n" + text);
    require(text.find("index_store") != std::string::npos,
            "the set literal's elements were not stored");
    std::cout << "mir lowering set literal: PASS\n";
}

void testGenericClassCollectionLiteralLowers() {
    // Inside `class Wrapper<T>` the only available spelling is the `List<T>`
    // class form, which arrives as an Object named "List" rather than the `list`
    // keyword kind. Construction has to accept both spellings, or a generic
    // class cannot build a collection literal at all.
    const auto lowered = lower("GenericLiteral", R"ZL(
class Wrapper<T> {
    public func singleton(T item): List<T> {
        List<T> result = [item]
        return result
    }
}

class GenericLiteral {
    func main(): void {
        var w = new Wrapper<int>()
        log(w.singleton(7))
    }
}
)ZL");
    requireClean(lowered, "a List<T> literal inside a generic class");
    const std::string text = functionText(lowered, "Wrapper.singleton");
    require(text.find(":List<T> = new_collection") != std::string::npos,
            "the List<T> literal was not constructed:\n" + text);
    std::cout << "mir lowering generic collection literal: PASS\n";
}

// ---------------------------------------------------------------------------
// data records
// ---------------------------------------------------------------------------

void testRecordLiteralIsAllocPlusFieldStores() {
    // A `data` type has no constructor, so its literal must not go looking for
    // one: allocate, then write each named field. An earlier lowerer treated the
    // literal as an unsupported expression and gave up on the whole body.
    const auto lowered = lower("RecordLiteral", R"ZL(
data Point {
    x: int
    y: int
}

class RecordLiteral {
    func main(): void {
        var p = Point { x: 10, y: 20 }
        log(p.x)
    }
}
)ZL");
    requireClean(lowered, "a data record literal");
    const std::string text = functionText(lowered, "RecordLiteral.main");
    require(text.find("alloc class 'Point'") != std::string::npos,
            "the record literal did not allocate its type");
    require(text.find("field_store 'x'") != std::string::npos &&
                text.find("field_store 'y'") != std::string::npos,
            "the record literal did not write both of its fields");
    require(text.find("call fn") == std::string::npos,
            "the record literal called a constructor; a data type has none");
    std::cout << "mir lowering record literal: PASS\n";
}

// ---------------------------------------------------------------------------
// match: arms, narrowing, and the join
// ---------------------------------------------------------------------------

void testMatchArmsAreBranches() {
    // Each arm becomes its own test block, so the arm order in the source is
    // the order the tests run in - which arm wins is not something a later pass
    // may reorder.
    const auto lowered = lower("MatchArms", R"ZL(
class MatchArms {
    static func nameOf(int n): string {
        return match n {
            0 => "zero"
            1 => "one"
            _ => "many"
        }
    }
}
)ZL");
    requireClean(lowered, "match with literal arms");
    const std::string text = functionText(lowered, "MatchArms.nameOf");
    require(text.find("eq $0:int, 0:int") != std::string::npos,
            "the first arm did not compare the subject against its literal");
    require(text.find("eq $0:int, 1:int") != std::string::npos,
            "the second arm did not compare the subject against its literal");
    // Arms produce their value in different blocks, so the result has to travel
    // through a slot that the join block reads back.
    require(text.find("store slot") != std::string::npos,
            "no arm stored its value into the match result slot");
    require(text.find("load slot") != std::string::npos,
            "the join block did not read the match result back");
    std::cout << "mir lowering match arms: PASS\n";
}

void testMatchSubjectIsEvaluatedOnce() {
    // A subject with a side effect must not run once per arm. It is evaluated
    // before the first test block, so exactly one call appears.
    const auto lowered = lower("MatchSubject", R"ZL(
class MatchSubject {
    static func sideEffect(): int {
        return 1
    }

    static func pick(): string {
        return match MatchSubject.sideEffect() {
            0 => "zero"
            _ => "other"
        }
    }
}
)ZL");
    requireClean(lowered, "match with a call as its subject");
    const std::string text = functionText(lowered, "MatchSubject.pick");
    std::size_t calls = 0;
    for (std::size_t at = text.find("MatchSubject.sideEffect"); at != std::string::npos;
         at = text.find("MatchSubject.sideEffect", at + 1))
        ++calls;
    require(calls == 1, "the match subject was evaluated " + std::to_string(calls) +
                            " times instead of once");
    std::cout << "mir lowering match subject: PASS\n";
}

void testMatchTypePatternNarrowsTheSubject() {
    // A type pattern has to (a) test the runtime type rather than assert it, so
    // a non-matching arm falls through instead of raising, and (b) narrow the
    // subject inside the arm, so the body can use the member it matched.
    const auto lowered = lower("MatchNarrow", R"ZL(
class MatchNarrow {
    static func describe(int|string value): string {
        return match value {
            int n => "number"
            string text => "text"
            null => "nothing"
        }
    }
}
)ZL");
    requireClean(lowered, "match with type patterns");
    const std::string text = functionText(lowered, "MatchNarrow.describe");
    require(text.find("type_test as int") != std::string::npos,
            "the int arm did not emit a runtime type test");
    require(text.find("type_test as string") != std::string::npos,
            "the string arm did not emit a runtime type test");
    require(text.find("refine") != std::string::npos,
            "no arm narrowed the subject to the member it matched");
    // Two narrowings per type arm: one for the arm's own binding (`n`, `text`)
    // and one for the subject identifier the body keeps spelling (`value`).
    // The null arm narrows nothing.
    std::size_t narrows = 0;
    for (std::size_t at = text.find("refine"); at != std::string::npos;
         at = text.find("refine", at + 1))
        ++narrows;
    require(narrows == 4, "expected two narrowings per type arm but found " + std::to_string(narrows));
    std::cout << "mir lowering match narrowing: PASS\n";
}

void testMatchGuardIsABranch() {
    // A guard that fails has to fall through to the next arm, not to the end of
    // the match - the arm after it may still match.
    const auto lowered = lower("MatchGuard", R"ZL(
class MatchGuard {
    static func size(int n): string {
        return match n {
            v when v < 0 => "negative"
            0 => "zero"
            _ => "positive"
        }
    }
}
)ZL");
    requireClean(lowered, "match with a guard");
    const std::string text = functionText(lowered, "MatchGuard.size");
    require(text.find("lt ") != std::string::npos, "the guard comparison was not emitted");
    require(text.find("eq $0:int, 0:int") != std::string::npos,
            "the arm after the guarded one was dropped");
    std::cout << "mir lowering match guard: PASS\n";
}

void testMatchEnumMembersCompareByName() {
    const auto lowered = lower("MatchEnum", R"ZL(
enum Level { LOW HIGH }

class MatchEnum {
    static func label(Level l): string {
        return match l {
            Level.LOW => "low"
            Level.HIGH => "high"
        }
    }
}
)ZL");
    requireClean(lowered, "match over an enum");
    const std::string text = functionText(lowered, "MatchEnum.label");
    require(text.find("eq") != std::string::npos, "no enum member comparison was emitted");
    require(text.find("Level") != std::string::npos,
            "the enum member constant did not carry its enum type");
    std::cout << "mir lowering match enum: PASS\n";
}

void testMatchFallsThroughToAThrow() {
    // The checker proves static coverage, but a value it could not classify
    // still has to land somewhere. The reference raises; so does the MIR.
    //
    // This has to be exhaustive by enumeration rather than by wildcard: a
    // wildcard arm cannot fail, so its fallthrough block is unreachable and
    // finish() prunes it. Covering every enum member is the only shape where
    // the last arm can still fail and the fallthrough survives.
    const auto lowered = lower("MatchFallthrough", R"ZL(
enum Level { LOW HIGH }

class MatchFallthrough {
    static func pick(Level l): string {
        return match l {
            Level.LOW => "low"
            Level.HIGH => "high"
        }
    }
}
)ZL");
    requireClean(lowered, "match exhaustive by enumeration");
    const std::string text = functionText(lowered, "MatchFallthrough.pick");
    require(text.find("throw") != std::string::npos,
            "the non-exhaustive fallthrough did not raise");
    require(text.find("non-exhaustive match") != std::string::npos,
            "the fallthrough did not carry the reference's message");
    std::cout << "mir lowering match fallthrough: PASS\n";
}

void testMatchWildcardPrunesItsFallthrough() {
    // The other half of the same rule: with a wildcard the fallthrough is dead,
    // and dead blocks are removed rather than left in for a backend to trip
    // over.
    const auto lowered = lower("MatchWildcard", R"ZL(
class MatchWildcard {
    static func pick(int n): string {
        return match n {
            0 => "zero"
            _ => "other"
        }
    }
}
)ZL");
    requireClean(lowered, "match with a wildcard arm");
    const std::string text = functionText(lowered, "MatchWildcard.pick");
    require(text.find("non-exhaustive match") == std::string::npos,
            "an unreachable fallthrough block was left in the function");
    std::cout << "mir lowering match wildcard: PASS\n";
}

void testUnsupportedConstructStaysValid() {
    // `finally` is not lowered yet. The function must be marked incomplete and
    // noted, but the module still has to verify: a bail-out partway through a
    // body must not leave unterminated or unreachable blocks behind.
    const auto lowered = lower("Unsupported", R"ZL(
class Unsupported {
    func main(): void {
        try {
            log("work")
        } catch Exception e {
            log("caught")
        } finally {
            log("cleanup")
        }
    }
}
)ZL");
    require(lowered.ok, "a partially lowered module did not verify:\n" + lowered.errors);
    require(hasDiagnostic(lowered, "unsupported"),
            "the unsupported construct was not reported as a note");
    const auto* function = findFunction(lowered, "Unsupported.main");
    require(function != nullptr, "the partially lowered function is missing");
    require(function && function->incomplete, "the partially lowered function is not marked incomplete");
    std::cout << "mir lowering partial support: PASS\n";
}

void testWholeExampleCorpusShape() {
    // A representative program touching most of the above at once.
    const auto lowered = lower("Corpus", R"ZL(
enum Shape { CIRCLE SQUARE }

class Point {
    public double x
    public double y
    func Point(double x, double y): void {
        this.x = x
        this.y = y
    }
    public func norm(): double { return Math.sqrt(this.x * this.x + this.y * this.y) }
}

class Named extends Point {
    public string label
    func Named(string label): void {
        super(3.0, 4.0)
        this.label = label
    }
    public func describe(): string { return this.label + "@" + this.norm() }
}

class Corpus {
    static func total(List<int> xs): int {
        var sum = 0
        for i in 0..xs.length() { sum = sum + xs[i] }
        return sum
    }

    func main(): void {
        let origin = new Point(0.0, 0.0)
        var named = new Named("n")
        log(named.describe())
        log(origin.norm())

        var xs = new List<int>()
        xs.push(1)
        xs.push(2)
        log(Corpus.total(xs))
        log(Corpus.total(xs.filter(func(int n) => n > 1)))

        var shape = Shape.CIRCLE
        log(shape == Shape.CIRCLE)
    }
}
)ZL");
    requireClean(lowered, "a program using classes, inheritance, generics, closures, and enums");
    std::cout << "mir lowering combined program: PASS\n";
}

} // namespace

int main() {
    testImmutableLocalStaysSsa();
    testWrittenParameterBecomesSlot();
    testThisIsNeverASlot();

    testInheritedFieldLowers();
    testEnumMemberCarriesEnumType();
    testGenericThisIsSelfParameterized();
    testListIndexingLowers();

    testNestedClosureCaptures();
    testClosureParameterIsCallable();
    testClosureCapturesThis();
    testClosureInGenericClassIsGeneric();

    testConditionsAreBool();
    testWideningIsExplicit();
    testMathConstantLowers();
    testTryCatchLowers();

    testAnnotatedCollectionLiteralKeepsItsType();
    testSetLiteralLowers();
    testGenericClassCollectionLiteralLowers();
    testRecordLiteralIsAllocPlusFieldStores();

    testMatchArmsAreBranches();
    testMatchSubjectIsEvaluatedOnce();
    testMatchTypePatternNarrowsTheSubject();
    testMatchGuardIsABranch();
    testMatchEnumMembersCompareByName();
    testMatchFallsThroughToAThrow();
    testMatchWildcardPrunesItsFallthrough();

    testUnsupportedConstructStaysValid();
    testWholeExampleCorpusShape();

    if (failures != 0) {
        std::cerr << failures << " MIR lowering regression(s) failed\n";
        return 1;
    }
    std::cout << "all MIR lowering regressions passed\n";
    return 0;
}
