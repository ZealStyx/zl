#include "zl/compiler/overload_resolver.hpp"
#include "zl/compiler/type_checker.hpp"

namespace zl {

const ClassMethodInfo* OverloadResolver::resolve(
        const std::vector<std::pair<std::string, ClassMethodInfo>>& candidates,
        const std::vector<ZlType>& argTypes, const std::vector<std::string>& argClassNames,
        const std::string& methodName, std::size_t line, std::string* outOwner) const {
    // Only candidates with the right arity are even in the running -
    // arity mismatches aren't "close matches", they're not matches at all.
    std::vector<const std::pair<std::string, ClassMethodInfo>*> arityMatches;
    for (const auto& c : candidates) {
        if (c.second.paramTypes.size() == argTypes.size()) arityMatches.push_back(&c);
    }

    auto describeCandidates = [&]() {
        std::string s;
        for (const auto& c : candidates) {
            s += "\n  - " + c.first + "." + methodName + TypeChecker::methodParamSignature(c.second);
        }
        return s;
    };

    if (arityMatches.empty()) {
        TypeChecker::typeError("no overload of '" + methodName + "' takes " + std::to_string(argTypes.size()) +
                   " argument(s); candidates are:" + describeCandidates(), line);
    }

    // Tier 1: exact type match on every parameter.
    std::vector<const std::pair<std::string, ClassMethodInfo>*> exact;
    for (const auto* c : arityMatches) {
        bool allExact = true;
        for (std::size_t i = 0; i < argTypes.size(); ++i) {
            if (argTypes[i] != c->second.paramTypes[i]) { allExact = false; break; }
            {
                const std::string argClass = i < argClassNames.size() ? argClassNames[i] : std::string();
                const std::string paramClass = i < c->second.paramClassNames.size() ? c->second.paramClassNames[i] : std::string();
                const bool needsIdentity = argTypes[i] == ZlType::OBJECT || argTypes[i] == ZlType::LIST ||
                                           argTypes[i] == ZlType::MAP || argTypes[i] == ZlType::SET ||
                                           argTypes[i] == ZlType::ARRAY || argTypes[i] == ZlType::UNION ||
                                           argTypes[i] == ZlType::TASK;
                if (!argClass.empty() && !paramClass.empty() && needsIdentity && argClass != paramClass) {
                    allExact = false;
                    break;
                }
            }
        }
        if (allExact) exact.push_back(c);
    }
    if (exact.size() == 1) {
        if (outOwner) *outOwner = exact[0]->first;
        return &exact[0]->second;
    }
    if (exact.size() > 1) {
        TypeChecker::typeError("ambiguous call to '" + methodName + "' - multiple overloads match exactly:" +
                   describeCandidates(), line);
    }

    // Tier 2: widening match (e.g. int -> double), same isAssignable rule
    // used for ordinary assignment. The generic dispatch marker selects a
    // shared bytecode slot; it must never exempt an instantiated signature
    // from semantic compatibility checking.
    std::vector<const std::pair<std::string, ClassMethodInfo>*> widening;
    for (const auto* c : arityMatches) {
        bool allAssignable = true;
        for (std::size_t i = 0; i < argTypes.size(); ++i) {
            const std::string argClass = i < argClassNames.size() ? argClassNames[i] : std::string();
            const std::string paramClass = i < c->second.paramClassNames.size() ? c->second.paramClassNames[i] : std::string();
            if (!checker_.isAssignable(argTypes[i], c->second.paramTypes[i], argClass, paramClass)) {
                allAssignable = false;
                break;
            }
        }
        if (allAssignable) widening.push_back(c);
    }
    if (widening.size() == 1) {
        if (outOwner) *outOwner = widening[0]->first;
        return &widening[0]->second;
    }
    if (widening.size() > 1) {
        // Prefer the most-specific overload when several reference-type
        // candidates are applicable. For example, if Child derives from Base
        // and implements Greeter, overloads taking Base and Greeter are both
        // applicable to Child, but Base is the more specific match. Keep the
        // old ambiguity error when there is no unique maximal candidate.
        std::vector<const std::pair<std::string, ClassMethodInfo>*> maximal;
        for (const auto* candidate : widening) {
            bool dominated = false;
            for (const auto* other : widening) {
                if (candidate == other) continue;
                bool otherAtLeastAsSpecific = true;
                bool otherStrictlyMoreSpecific = false;
                if (candidate->second.paramTypes.size() != other->second.paramTypes.size()) continue;
                for (std::size_t i = 0; i < candidate->second.paramTypes.size(); ++i) {
                    const auto candidateType = candidate->second.paramTypes[i];
                    const auto otherType = other->second.paramTypes[i];
                    const std::string candidateClass = i < candidate->second.paramClassNames.size() ?
                        candidate->second.paramClassNames[i] : std::string();
                    const std::string otherClass = i < other->second.paramClassNames.size() ?
                        other->second.paramClassNames[i] : std::string();
                    if (candidateType == ZlType::OBJECT && otherType == ZlType::OBJECT &&
                        !candidateClass.empty() && !otherClass.empty()) {
                        if (!checker_.isAssignable(otherType, candidateType, otherClass, candidateClass)) {
                            otherAtLeastAsSpecific = false;
                            break;
                        }
                        if (checker_.isAssignable(candidateType, otherType, candidateClass, otherClass) &&
                            !checker_.isAssignable(otherType, candidateType, otherClass, candidateClass)) {
                            // handled by the strictness check below
                        }
                        if (candidateClass != otherClass &&
                            checker_.isAssignable(otherType, candidateType, otherClass, candidateClass)) {
                            otherStrictlyMoreSpecific = true;
                        }
                    } else if (candidateType != otherType) {
                        // Primitive widening has already been handled by the
                        // exact tier. Different primitive parameter types here
                        // do not establish a specificity relationship.
                        otherAtLeastAsSpecific = false;
                        break;
                    }
                }
                if (otherAtLeastAsSpecific && otherStrictlyMoreSpecific) {
                    dominated = true;
                    break;
                }
            }
            if (!dominated) maximal.push_back(candidate);
        }
        if (maximal.size() == 1) {
            if (outOwner) *outOwner = maximal[0]->first;
            return &maximal[0]->second;
        }
        TypeChecker::typeError("ambiguous call to '" + methodName + "' - multiple overloads match after "
                   "widening conversions:" + describeCandidates(), line);
    }

    TypeChecker::typeError("no overload of '" + methodName + "' matches the given argument types; candidates are:" +
               describeCandidates(), line);
    return nullptr; // unreachable - typeError throws
}

} // namespace zl
