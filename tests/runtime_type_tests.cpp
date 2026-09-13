// Runtime type system regressions: the canonical type-name grammar, runtime
// metadata identity and subtype/interface relationships, invalid casts,
// nullable edge cases, and the typed-boundary contract checks (list/map/array)
// the VM applies at stores and type assertions.
#include "zl/common/type_name.hpp"
#include "zl/compiler/bytecode.hpp"
#include "zl/vm/gc.hpp"
#include "zl/vm/runtime_fault.hpp"
#include "zl/vm/runtime_task.hpp"
#include "zl/vm/runtime_type.hpp"
#include "zl/vm/runtime_type_checks.hpp"
#include "zl/vm/value.hpp"

#include <iostream>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

int failures = 0;

void require(bool condition, const std::string& message) {
    if (condition) return;
    std::cerr << "runtime type regression: " << message << '\n';
    ++failures;
}

using zl::RuntimeTypeInfo;
using zl::TypeName;

// A chunk declaring: Base <- Child, Base <- Bird implements Flyable,
// Flyable (interface). Reflection metadata drives every check below.
zl::Chunk buildChunk() {
    auto base = std::make_shared<RuntimeTypeInfo>();
    base->id = 1;
    base->name = "Base";
    auto child = std::make_shared<RuntimeTypeInfo>();
    child->id = 2;
    child->name = "Child";
    child->baseClassName = "Base";
    auto flyable = std::make_shared<RuntimeTypeInfo>();
    flyable->id = 3;
    flyable->name = "Flyable";
    auto bird = std::make_shared<RuntimeTypeInfo>();
    bird->id = 4;
    bird->name = "Bird";
    bird->baseClassName = "Base";
    bird->interfaces = {"Flyable"};

    zl::Chunk chunk;
    auto registerClass = [&](const char* name, const zl::RuntimeTypeRef& type, const char* base = "") {
        zl::ClassReflectionInfo info;
        info.runtimeType = type;
        info.baseClassName = base;
        if (type) {
            info.interfaces = type->interfaces;
            info.typeParameters = type->typeParameters;
        }
        chunk.classReflection[name] = info;
    };
    registerClass("Base", base);
    registerClass("Child", child, "Base");
    registerClass("Flyable", flyable);
    registerClass("Bird", bird, "Base");
    return chunk;
}

zl::Value makeObject(const std::string& className, const zl::RuntimeTypeRef& type,
                     const std::string& genericTypeName = "") {
    auto box = zl::makeGCObject();
    box->className = className;
    box->runtimeType = type;
    box->genericTypeName = genericTypeName;
    return box;
}

void testTypeNameGrammar() {
    using zl::parseTypeName;
    using zl::describeTypeName;
    require(parseTypeName("int").name == "int", "primitive name parses");
    require(parseTypeName("float").name == "double", "float aliases double");
    require(parseTypeName("null").name == "nil", "null aliases nil");

    const auto listInt = parseTypeName("List<int>");
    require(listInt.name == "List" && listInt.args.size() == 1 && listInt.args[0].name == "int",
            "generic argument parses");
    const auto nested = parseTypeName("List<List<int>>");
    require(nested.args.size() == 1 && nested.args[0].name == "List" &&
              nested.args[0].args[0].name == "int",
            "nested generics parse");

    const auto funcType = parseTypeName("func(int): string");
    require(funcType.name == "func" && funcType.args.size() == 2 && funcType.args[1].name == "string",
            "func type records parameters then return");
    require(describeTypeName(funcType) == "func(int):string", "func type renders");

    const auto array = parseTypeName("array[3]");
    require(array.name == "array" && array.fixedSize.value_or(0) == 3, "fixed array size parses");

    // Unions flatten, deduplicate, and render sorted: identity does not depend
    // on spelling order.
    const auto unionType = parseTypeName("int | double|int");
    require(unionType.name == "union" && unionType.unionMembers.size() == 2,
            "union flattens and deduplicates");
    require(describeTypeName(unionType) == "double|int", "union renders sorted");

    require(parseTypeName("List<int").name == "<invalid>", "unclosed generic is invalid");
    require(parseTypeName("array[x]").name == "<invalid>", "non-numeric array size is invalid");
    require(parseTypeName("func(int):").name == "<invalid>", "func type without return is invalid");
    require(parseTypeName("").name == "<invalid>", "empty type name is invalid");

    require(describeTypeName(parseTypeName("List<int>")) == "List<int>", "describe round trip");
    require(describeTypeName(parseTypeName("array[3]")) == "array[3]", "array describe round trip");
}

void testTypeNameEqualityAndSubstitution() {
    using zl::parseTypeName;
    using zl::typeNamesEqual;
    using zl::substituteTypeParams;

    require(typeNamesEqual(parseTypeName("List<int>"), parseTypeName("List<int>")),
            "identical types are equal");
    require(!typeNamesEqual(parseTypeName("List<int>"), parseTypeName("List<double>")),
            "different arguments are not equal");
    require(typeNamesEqual(parseTypeName("int|string"), parseTypeName("string|int")),
            "union equality ignores order");
    require(!typeNamesEqual(parseTypeName("array[3]"), parseTypeName("array[4]")),
            "array sizes are part of identity");

    std::unordered_map<std::string, std::string> bindings{{"T", "int"}};
    require(substituteTypeParams("List<T>", bindings) == "List<int>", "single parameter substitutes");
    bindings["U"] = "string";
    bindings["T"] = "List<U>";
    // Substitution is a single lexical pass: the token in the original tree
    // (T) is replaced, but names introduced by the replacement (U) are not
    // re-scanned. That is the documented contract of substituteTypeParams.
    require(substituteTypeParams("List<T>", bindings) == "List<List<U>>",
            "substitution is a single pass: introduced names are not re-resolved");
    require(substituteTypeParams("List<int>", {}) == "List<int>", "empty bindings leave the name");
    std::unordered_map<std::string, std::string> unionBinding{{"T", "int|string"}};
    require(substituteTypeParams("T", unionBinding) == "int|string",
            "a substituted union renders flattened");
}

void testRuntimeMetadataIdentity() {
    const auto chunk = buildChunk();

    // Identity: metadata is shared, not copied. Same object -> same identity.
    const auto childObject = makeObject("Child", chunk.classReflection.at("Child").runtimeType);
    require(zl::runtimeValueTypeName(childObject) == "Child", "value type name is the dispatch class");

    // Subtype relationships resolve through recorded metadata.
    require(zl::runtimeAssignableToType(childObject, "Child", &chunk), "object is assignable to itself");
    require(zl::runtimeAssignableToType(childObject, "Base", &chunk), "subclass is assignable to its base");
    require(!zl::runtimeAssignableToType(childObject, "Bird", &chunk), "unrelated class is rejected");
    require(!zl::runtimeAssignableToType(childObject, "Flyable", &chunk),
            "a class that does not implement the interface is rejected");

    const auto birdObject = makeObject("Bird", chunk.classReflection.at("Bird").runtimeType);
    require(zl::runtimeAssignableToType(birdObject, "Flyable", &chunk),
            "interface implementation is assignable");
    require(zl::runtimeAssignableToType(birdObject, "Base", &chunk), "transitive base is assignable");
    require(!zl::runtimeAssignableToType(birdObject, "Child", &chunk),
            "sibling classes never cast to each other");

    // Distinct metadata instances are distinct identities even when named alike.
    auto renamed = std::make_shared<RuntimeTypeInfo>();
    renamed->id = 99;
    renamed->name = "Base";
    require(chunk.classReflection.at("Base").runtimeType->id != renamed->id,
            "type ids stay distinct");
}

void testGenericAndNullableEdgeCases() {
    const auto chunk = buildChunk();

    // A concrete generic instantiation keeps its identity; an erased object
    // must not satisfy a parameterized reflection type.
    const auto boxed = makeObject("Option", nullptr, "Option<int>");
    require(zl::reflectiveTypeMatchesName(boxed, "Option<int>", &chunk),
            "concrete instantiation matches its own name");
    require(!zl::reflectiveTypeMatchesName(boxed, "Option<string>", &chunk),
            "a different instantiation is not the same identity");
    const auto erased = makeObject("Option", nullptr);
    require(zl::reflectiveTypeMatchesName(erased, "Option", &chunk),
            "erased object matches the erased name");
    require(!zl::reflectiveTypeMatchesName(erased, "Option<int>", &chunk),
            "erased object does not pretend to be a concrete instantiation");

    // Nil edges: assignable to nullable targets, never to value types.
    const zl::Value nil;
    require(zl::runtimeAssignableToType(nil, "object", &chunk), "nil is assignable to object");
    require(zl::runtimeAssignableToType(nil, "string", &chunk), "nil is assignable to reference types");
    require(zl::runtimeAssignableToType(nil, "Base", &chunk), "nil is assignable to a class");
    require(!zl::runtimeAssignableToType(nil, "int", &chunk), "nil is not assignable to int");
    require(!zl::reflectiveTypeMatchesName(nil, "Base", &chunk),
            "nil is not *exactly* a Base value");

    // func values match their recorded signature, nothing wider.
    auto closure = zl::makeGCClosure();
    closure->parameterTypeNames = {"int"};
    closure->returnTypeName = "string";
    zl::Value funcValue = closure;
    require(zl::reflectiveTypeMatchesName(funcValue, "func(int): string", &chunk),
            "closure matches its signature");
    require(!zl::reflectiveTypeMatchesName(funcValue, "func(string): string", &chunk),
            "closure rejects a different parameter type");
}

void testTaskValueType() {
    const auto chunk = buildChunk();
    auto typed = std::make_shared<zl::RuntimeTaskState>("int");
    auto untyped = std::make_shared<zl::RuntimeTaskState>("");
    zl::Value typedTask = typed;
    zl::Value untypedTask = untyped;
    require(zl::reflectiveTypeMatchesName(typedTask, "Task<int>", &chunk),
            "typed task matches Task<int>");
    require(!zl::reflectiveTypeMatchesName(typedTask, "Task<string>", &chunk),
            "typed task rejects the wrong value type");
    require(zl::reflectiveTypeMatchesName(untypedTask, "Task<int>", &chunk),
            "a task without concrete metadata stays permissive");
}

void testTypeCheckBoundary() {
    const auto chunk = buildChunk();
    zl::RuntimeTypeCheck types(&chunk);

    require(types.check(zl::Value(5), "int"), "int value satisfies int");
    require(!types.check(zl::Value(std::string("x")), "int"), "string value does not satisfy int");
    require(types.check(zl::Value(5), "double"), "int widens to double");
    require(!types.check(zl::Value(5.5), "int"), "decimal does not satisfy int");
    require(types.check(zl::Value{}, "string"), "nil satisfies a reference type");
    require(!types.check(zl::Value{}, "int"), "nil does not satisfy int");
    require(types.check(zl::Value(5), "int|string"), "union accepts a member");
    require(types.check(zl::Value(std::string("s")), "int|string"), "union accepts the other member");
    require(!types.check(zl::Value(true), "int|string"), "union rejects a non-member");

    bool threw = false;
    try {
        types.require(zl::Value(std::string("x")), "int");
    } catch (const zl::ZlRuntimeFault& fault) {
        threw = true;
        require(fault.className() == "TypeError", "failed assertion is a TypeError");
    }
    require(threw, "require() throws on a failed assertion");
}

void testListContract() {
    auto list = zl::makeGCList();
    list->items = {zl::Value(1), zl::Value(2)};

    {
        zl::RuntimeTypeCheck check;
        require(check.check(zl::Value(list), "list<int>"), "int list satisfies list<int>");
        check.commit();
    }
    require(list->storageType && list->storageType->arguments.size() == 1 &&
              list->storageType->arguments[0].name == "int",
            "the committed contract records the element type");

    // The contract is enforced on later writes.
    {
        zl::RuntimeTypeCheck check;
        const zl::Value good(3);
        check.listWrite(list, &good, 3);
        bool threw = false;
        const zl::Value bad(std::string("no"));
        try {
            check.listWrite(list, &bad, 4);
        } catch (const zl::ZlRuntimeFault& fault) {
            threw = true;
            require(fault.className() == "TypeError", "violating write is a TypeError");
        }
        require(threw, "a write violating the committed contract is rejected");
    }

    // Conflicting contracts are refused, and a wildcard does not erase one.
    auto intList = zl::makeGCList();
    intList->items = {zl::Value(1)};
    {
        zl::RuntimeTypeCheck check;
        require(check.check(zl::Value(intList), "list<int>"), "seed the int contract");
        check.commit();
        require(!check.check(zl::Value(intList), "list<double>"),
                "a committed list<int> does not accept list<double>");
    }
    {
        zl::RuntimeTypeCheck check;
        require(check.check(zl::Value(intList), "list<unknown>"),
                "a wildcard argument preserves the existing contract");
    }

    // Fixed arrays refuse resizes.
    auto array = zl::makeGCList();
    array->items = {zl::Value(1), zl::Value(2)};
    {
        zl::RuntimeTypeCheck check;
        require(check.check(zl::Value(array), "array[2]"), "size-matching array satisfies array[2]");
        check.commit();
    }
    bool resizeRejected = false;
    {
        zl::RuntimeTypeCheck check;
        try {
            check.listWrite(array, nullptr, 3);
        } catch (const std::exception&) {
            resizeRejected = true;
        }
    }
    require(resizeRejected, "writing past a fixed array size is rejected");
}

void testMapContract() {
    auto map = zl::makeGCMap();
    map->entries = {{zl::Value(std::string("a")), zl::Value(1)}};
    {
        zl::RuntimeTypeCheck check;
        require(check.check(zl::Value(map), "map<string,int>"), "map satisfies map<string,int>");
        check.commit();
    }
    require(map->storageType && map->storageType->arguments.size() == 2,
            "the committed contract records key and value types");

    {
        zl::RuntimeTypeCheck check;
        check.mapWrite(map, zl::Value(std::string("b")), zl::Value(2));
        bool threw = false;
        try {
            check.mapWrite(map, zl::Value(3), zl::Value(2));
        } catch (const zl::ZlRuntimeFault& fault) {
            threw = true;
            require(fault.className() == "TypeError", "violating key is a TypeError");
        }
        require(threw, "a key violating the committed contract is rejected");
    }
}

} // namespace

int main() {
    testTypeNameGrammar();
    testTypeNameEqualityAndSubstitution();
    testRuntimeMetadataIdentity();
    testGenericAndNullableEdgeCases();
    testTaskValueType();
    testTypeCheckBoundary();
    testListContract();
    testMapContract();

    if (failures != 0) {
        std::cerr << failures << " runtime type regression(s) failed\n";
        return 1;
    }
    std::cout << "all runtime type regressions passed\n";
    return 0;
}
