#pragma once

#include <string>
#include <vector>

namespace zl_bind {

struct Param { std::string type; std::string name; };
struct Fn {
    std::string ret, name;
    std::vector<Param> params;
    std::string ownership = "borrowed";
    std::string errors = "exception";
};
struct Method {
    std::string ret, name;
    std::vector<Param> params;
    bool isConst{false};
    bool isDestructor{false};
};
struct NativeClass {
    std::string name;
    std::string kind;
    std::vector<Param> constructorParams;
    bool hasConstructor{false};
    bool hasDestructor{false};
    std::string ownership = "unique";
    std::string errors = "exception";
    std::vector<Method> methods;
};

struct ParsedHeader {
    std::vector<Fn> functions;
    std::vector<NativeClass> classes;
};

ParsedHeader parseHeaderFile(const std::string& path);
std::string mapNativeType(const std::string& type);

} // namespace zl_bind
