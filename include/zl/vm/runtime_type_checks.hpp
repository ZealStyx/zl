#pragma once

#include "value.hpp"
#include "zl/common/type_name.hpp"

namespace zl {
struct Chunk;

// Runtime substitutions are lexical: only bindings from the declaring class
// or captured closure may replace a parameter token. Unknown names are never
// guessed to be type parameters and silently exempted from checking.
RuntimeTypeBindings receiverTypeBindings(const ObjectBox& receiver, const Chunk& chunk,
                                        const std::string& declaringClass);

// A leaf transaction: validate every value, then commit contracts only when
// the boundary/store succeeds. It serializes contract publication with native
// mutations. Never invoke managed callbacks or wait on native resources here.
class RuntimeTypeCheck {
public:
    explicit RuntimeTypeCheck(const Chunk* chunk = nullptr);
    enum class Mode { Assignment, Pattern };
    bool check(const Value& value, const std::string& typeName, Mode mode = Mode::Assignment);
    void require(const Value& value, const std::string& typeName);
    void listWrite(ListRef list, const Value* inserted, std::size_t resultingSize);
    void mapWrite(MapRef map, const Value& key, const Value& value);
    void commit() noexcept;
private:
    struct Pending { NativeContainerTypeRef* slot; NativeContainerTypeRef type; };
    bool matches(const Value& value, const TypeName& type, Mode mode = Mode::Assignment);
    bool container(const Value& value, const TypeName& type);
    NativeContainerTypeRef proposed(NativeContainerTypeRef& slot) const;
    const Chunk* chunk_;
    std::unique_lock<std::recursive_mutex> lock_;
    std::vector<Pending> pending_;
};

// The declaring owner matters for an inherited field such as Base<T>.value.
std::string runtimeFieldType(const Chunk& chunk, const std::string& className,
                             const std::string& fieldName, const ObjectBox* receiver = nullptr);

bool reflectiveTypeMatchesName(const Value& value, const std::string& typeName, const Chunk* chunk = nullptr);
bool runtimeAssignableToType(const Value& value, const std::string& typeName, const Chunk* chunk);
std::string runtimeValueTypeName(const Value& value);
// Attach the compiler-declared identity to a newly allocated object only.
void initializeObjectType(Value& value, const std::string& typeName, const Chunk& chunk);
} // namespace zl
