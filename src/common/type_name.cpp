#include "zl/common/type_name.hpp"

#include <algorithm>
#include <cctype>
#include <functional>

namespace zl {
namespace {
std::string unionMemberName(const TypeName& spec) {
    const auto name = describeTypeName(spec);
    return spec.name == "func" && !spec.args.empty() ? "(" + name + ")" : name;
}
class TypeNameParser {
public:
    explicit TypeNameParser(const std::string& text) : text_(text) {}

    TypeName parse() {
        auto result = parseUnion();
        skipWhitespace();
        return valid_ && pos_ == text_.size() ? result : invalid();
    }

private:
    TypeName parseUnion() {
        std::vector<TypeName> members;
        members.push_back(parsePrimary());
        skipWhitespace();
        while (consume('|')) {
            members.push_back(parsePrimary());
            skipWhitespace();
        }
        if (members.size() == 1) return members.front();
        std::vector<TypeName> flattened;
        for (auto& member : members) {
            if (member.unionMembers.empty()) flattened.push_back(std::move(member));
            else flattened.insert(flattened.end(), member.unionMembers.begin(), member.unionMembers.end());
        }
        std::sort(flattened.begin(), flattened.end(), [](const auto& a, const auto& b) { return unionMemberName(a) < unionMemberName(b); });
        flattened.erase(std::unique(flattened.begin(), flattened.end(), typeNamesEqual), flattened.end());
        members = std::move(flattened);
        TypeName result;
        result.name = "union";
        result.unionMembers = std::move(members);
        return result;
    }

    TypeName parsePrimary() {
        skipWhitespace();
        if (consume('(')) {
            auto grouped = parseUnion();
            return consume(')') ? grouped : invalid();
        }
        const std::size_t start = pos_;
        while (pos_ < text_.size() && (std::isalnum(static_cast<unsigned char>(text_[pos_])) || text_[pos_] == '_' || text_[pos_] == '.')) ++pos_;
        if (start == pos_) return invalid();
        TypeName result;
        result.name = text_.substr(start, pos_ - start);
        if (result.name == "float") result.name = "double";
        if (result.name == "null") result.name = "nil";
        skipWhitespace();
        if (result.name == "array" && consume('[')) {
            const std::size_t sizeStart = pos_;
            while (pos_ < text_.size() && std::isdigit(static_cast<unsigned char>(text_[pos_]))) ++pos_;
            if (sizeStart == pos_ || !consume(']')) return invalid();
            try {
                result.fixedSize = std::stoull(text_.substr(sizeStart, pos_ - sizeStart - 1));
            } catch (...) {
                return invalid();
            }
            skipWhitespace();
        }
        if (consume('<')) {
            skipWhitespace();
            if (!consume('>')) {
                while (true) {
                    result.args.push_back(parseUnion());
                    skipWhitespace();
                    if (consume('>')) break;
                    if (!consume(',')) return invalid();
                }
            }
        } else if (result.name == "func" && consume('(')) {
            while (true) {
                skipWhitespace();
                if (consume(')')) break;
                result.args.push_back(parseUnion());
                skipWhitespace();
                if (consume(')')) break;
                if (!consume(',')) return invalid();
            }
            skipWhitespace();
            if (!consume(':')) return invalid();
            result.args.push_back(parseUnion());
        }
        return result;
    }

    bool consume(char c) {
        skipWhitespace();
        if (pos_ >= text_.size() || text_[pos_] != c) return false;
        ++pos_;
        return true;
    }

    void skipWhitespace() {
        while (pos_ < text_.size() && std::isspace(static_cast<unsigned char>(text_[pos_]))) ++pos_;
    }

    TypeName invalid() { valid_ = false; return TypeName{"<invalid>", {}, {}}; }
    const std::string& text_;
    std::size_t pos_{0};
    bool valid_{true};
};
} // namespace

TypeName parseTypeName(const std::string& text) { return TypeNameParser(text).parse(); }

std::string describeTypeName(const TypeName& spec) {
    if (!spec.unionMembers.empty()) {
        std::string out;
        for (std::size_t i = 0; i < spec.unionMembers.size(); ++i) {
            if (i) out += "|";
            out += unionMemberName(spec.unionMembers[i]);
        }
        return out;
    }
    std::string out = spec.name;
    if (spec.fixedSize && spec.name == "array") {
        out = "array[" + std::to_string(*spec.fixedSize) + "]";
    }
    if (!spec.args.empty()) {
        // Function types are handled by reflectiveMatchesSpec directly.
        if (spec.name == "func") {
            out += "(";
            for (std::size_t i = 0; i + 1 < spec.args.size(); ++i) {
                if (i) out += ",";
                out += describeTypeName(spec.args[i]);
            }
            out += "):" + describeTypeName(spec.args.back());
            return out;
        }
        out += "<";
        for (std::size_t i = 0; i < spec.args.size(); ++i) {
            if (i) out += ",";
            out += describeTypeName(spec.args[i]);
        }
        out += ">";
    }
    return out;
}
bool typeNamesEqual(const TypeName& a, const TypeName& b) {
    if (!a.unionMembers.empty() || !b.unionMembers.empty()) {
        if (a.unionMembers.size() != b.unionMembers.size()) return false;
        for (std::size_t i = 0; i < a.unionMembers.size(); ++i) {
            if (!typeNamesEqual(a.unionMembers[i], b.unionMembers[i])) return false;
        }
        return true;
    }
    if (a.name != b.name || a.fixedSize != b.fixedSize || a.args.size() != b.args.size()) return false;
    for (std::size_t i = 0; i < a.args.size(); ++i) {
        if (!typeNamesEqual(a.args[i], b.args[i])) return false;
    }
    return true;
}

std::string substituteTypeParams(const std::string& typeName,
                                const std::unordered_map<std::string, std::string>& bindings) {
    if (bindings.empty() || typeName.empty()) return typeName;
    std::function<TypeName(TypeName)> substitute = [&](TypeName type) {
        const auto found = bindings.find(type.name);
        if (found != bindings.end() && type.args.empty() && type.unionMembers.empty())
            return parseTypeName(found->second);
        for (auto& arg : type.args) arg = substitute(std::move(arg));
        for (auto& member : type.unionMembers) member = substitute(std::move(member));
        return type;
    };
    // A substituted argument can itself be a union. Parse the rendered tree
    // once more to flatten/order the resulting union without string guessing.
    return describeTypeName(parseTypeName(describeTypeName(substitute(parseTypeName(typeName)))));
}
} // namespace zl
