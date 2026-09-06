#include "zl/vm/value.hpp"
#include "zl/vm/gc.hpp"

#include <stdexcept>
#include <cmath>
#include <iomanip>
#include <limits>
#include <sstream>
#include <unordered_set>

namespace zl {

Value makeEmptyList() {
    return makeGCList();
}

Value makeEmptyMap() {
    return makeGCMap();
}

Value makeEmptyObject(const std::string& className) {
    auto box = makeGCObject();
    box->className = className;
    return box;
}

namespace {

std::string escapeRecordString(const std::string& input) {
    std::string out;
    out.reserve(input.size() + 2);
    for (unsigned char c : input) {
        switch (c) {
            case '\\': out += "\\\\"; break;
            case '"': out += "\\\""; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    std::ostringstream hex;
                    hex << "\\u" << std::hex << std::setw(4) << std::setfill('0') << static_cast<unsigned>(c);
                    out += hex.str();
                } else {
                    out.push_back(static_cast<char>(c));
                }
        }
    }
    return out;
}

// Shared recursive formatter. Top-level values print strings raw (so
// log("hi") prints hi, not "hi"), but elements NESTED inside a list/map print
// strings quoted (so log({"a", "b"}) prints ["a", "b"], not [a, b] - otherwise
// you can't tell a list of strings from a list of identifiers-gone-wrong).
std::string formatValue(const Value& v, bool quoteStrings) {
    return std::visit([quoteStrings](const auto& held) -> std::string {
        using T = std::decay_t<decltype(held)>;
        if constexpr (std::is_same_v<T, std::monostate>) {
            return "nil";
        } else if constexpr (std::is_same_v<T, bool>) {
            return held ? "true" : "false";
        } else if constexpr (std::is_same_v<T, std::int64_t>) {
            return std::to_string(held);
        } else if constexpr (std::is_same_v<T, double>) {
            if (!std::isfinite(held)) {
                if (std::isnan(held)) return "nan";
                return held < 0 ? "-inf" : "inf";
            }
            std::ostringstream oss;
            oss << std::setprecision(std::numeric_limits<double>::max_digits10) << held;
            return oss.str();
        } else if constexpr (std::is_same_v<T, std::string>) {
            return quoteStrings ? ("\"" + held + "\"") : held;
        } else if constexpr (std::is_same_v<T, ListRef>) {
            std::string s = "[";
            if (held) {
                for (std::size_t i = 0; i < held->items.size(); ++i) {
                    if (i) s += ", ";
                    s += formatValue(held->items[i], true);
                }
            }
            return s + "]";
        } else if constexpr (std::is_same_v<T, MapRef>) {
            std::string s = "{";
            if (held) {
                for (std::size_t i = 0; i < held->entries.size(); ++i) {
                    if (i) s += ", ";
                    s += formatValue(held->entries[i].first, true) + ": " + formatValue(held->entries[i].second, true);
                }
            }
            return s + "}";
        } else if constexpr (std::is_same_v<T, ObjectRef>) {
            if (!held) return "nil";
            std::string s = held->className + " { ";
            bool first = true;
            if (held->runtimeType && held->runtimeType->isDataType) {
                for (const auto& field : held->runtimeType->fields) {
                    auto it = held->fields.find(field.name);
                    if (it == held->fields.end()) continue;
                    if (!first) s += ", ";
                    s += field.name + ": ";
                    if (std::holds_alternative<std::string>(it->second)) {
                        s += "\"" + escapeRecordString(std::get<std::string>(it->second)) + "\"";
                    } else {
                        s += formatValue(it->second, true);
                    }
                    first = false;
                }
            } else {
                s = held->className + "{";
                for (const auto& [k, fieldValue] : held->fields) {
                    if (!first) s += ", ";
                    s += k + ": " + formatValue(fieldValue, true);
                    first = false;
                }
            }
            if (held->runtimeType && held->runtimeType->isDataType) return s + "}";
            return s + "}";
        } else if constexpr (std::is_same_v<T, ClosureRef>) {
            return "<func>";
        } else if constexpr (std::is_same_v<T, TaskRef>) {
            return held ? "<task>" : "nil";
        } else if constexpr (std::is_same_v<T, ThreadRef>) {
            return held ? "<thread>" : "nil";
        } else if constexpr (std::is_same_v<T, NativeHandleRef>) {
            return held.valid() ? "<native-handle>" : "nil";
        } else if constexpr (std::is_same_v<T, NativeBufferView>) {
            return held.valid() ? "<native-buffer>" : "nil";
        } else {
            return held.valid() ? "<native-callback>" : "nil";
        }
    }, v);
}

} // namespace

std::string valueToString(const Value& v) {
    return formatValue(v, false);
}

bool isTruthy(const Value& v) {
    return std::visit([](const auto& held) -> bool {
        using T = std::decay_t<decltype(held)>;
        if constexpr (std::is_same_v<T, std::monostate>) {
            return false;
        } else if constexpr (std::is_same_v<T, bool>) {
            return held;
        } else if constexpr (std::is_same_v<T, std::int64_t>) {
            return held != 0;
        } else if constexpr (std::is_same_v<T, double>) {
            return held != 0.0;
        } else if constexpr (std::is_same_v<T, std::string>) {
            return !held.empty();
        } else if constexpr (std::is_same_v<T, ListRef>) {
            return held && !held->items.empty();
        } else if constexpr (std::is_same_v<T, MapRef>) {
            return held && !held->entries.empty();
        } else if constexpr (std::is_same_v<T, ObjectRef>) {
            return held != nullptr; // non-nil object is truthy
        } else if constexpr (std::is_same_v<T, ClosureRef>) {
            return held != nullptr; // non-nil func value is truthy
        } else if constexpr (std::is_same_v<T, TaskRef>) {
            return held != nullptr;
        } else if constexpr (std::is_same_v<T, ThreadRef>) {
            return held != nullptr;
        } else if constexpr (std::is_same_v<T, NativeHandleRef>) {
            return held.valid();
        } else if constexpr (std::is_same_v<T, NativeBufferView>) {
            return held.valid();
        } else {
            return held.valid();
        }
    }, v);
}

bool isNumericValue(const Value& v) {
    return std::holds_alternative<std::int64_t>(v) || std::holds_alternative<double>(v);
}

double toDouble(const Value& v) {
    if (auto p = std::get_if<std::int64_t>(&v)) return static_cast<double>(*p);
    if (auto p = std::get_if<double>(&v)) return *p;
    throw std::runtime_error("expected a number");
}

std::int64_t toInt64Strict(const Value& v) {
    if (auto p = std::get_if<std::int64_t>(&v)) return *p;
    throw std::runtime_error("expected a whole number, got a decimal or non-number");
}

namespace {
int compareIntAndDouble(std::int64_t i, double d) {
    if (std::isnan(d)) return 2;
    if (d > 0.0) {
        constexpr double int64MaxExclusive = 9223372036854775808.0; // 2^63
        if (d >= int64MaxExclusive) return -1;

        const double lower = std::floor(d);
        const auto lowerInt = static_cast<std::int64_t>(lower);
        if (i < lowerInt) return -1;
        if (i > lowerInt) return 1;
        return lower == d ? 0 : -1;
    }

    if (d < 0.0) {
        constexpr double int64Min = -9223372036854775808.0; // -2^63
        if (d < int64Min) return 1;

        const double upper = std::ceil(d);
        const auto upperInt = static_cast<std::int64_t>(upper);
        if (i < upperInt) return -1;
        if (i > upperInt) return 1;
        return upper == d ? 0 : 1;
    }

    return i == 0 ? 0 : (i < 0 ? -1 : 1);
}

int compareNumbers(const Value& a, const Value& b) {
    if (auto ai = std::get_if<std::int64_t>(&a)) {
        if (auto bi = std::get_if<std::int64_t>(&b)) return (*ai > *bi) - (*ai < *bi);
        return compareIntAndDouble(*ai, std::get<double>(b));
    }
    if (auto ad = std::get_if<double>(&a)) {
        if (auto bi = std::get_if<std::int64_t>(&b)) return -compareIntAndDouble(*bi, *ad);
        const double bd = std::get<double>(b);
        return (*ad > bd) - (*ad < bd);
    }
    throw std::runtime_error("expected numeric values");
}
} // namespace

int compareNumericValues(const Value& a, const Value& b) {
    if (!isNumericValue(a) || !isNumericValue(b))
        throw std::runtime_error("expected numeric values");
    return compareNumbers(a, b);
}

namespace {
struct ObjectPairHash {
    std::size_t operator()(const std::pair<const ObjectBox*, const ObjectBox*>& p) const noexcept {
        auto h1 = std::hash<const ObjectBox*>{}(p.first);
        auto h2 = std::hash<const ObjectBox*>{}(p.second);
        return h1 ^ (h2 + static_cast<std::size_t>(0x9e3779b9) + (h1 << 6) + (h1 >> 2));
    }
};

bool recordValuesEqual(const ObjectRef& a, const ObjectRef& b,
                       std::unordered_set<std::pair<const ObjectBox*, const ObjectBox*>, ObjectPairHash>& seen) {
    if (a == b) return true;
    if (!a || !b) return false;
    if (!a->runtimeType || !b->runtimeType || !a->runtimeType->isDataType || !b->runtimeType->isDataType || a->className != b->className) return false;

    const auto key = std::make_pair(a.get(), b.get());
    if (!seen.insert(key).second) return true;

    if (a->fields.size() != b->fields.size()) return false;
    for (const auto& [name, av] : a->fields) {
        auto it = b->fields.find(name);
        if (it == b->fields.end()) return false;
        if (isNumericValue(av) && isNumericValue(it->second)) {
            // Use the public numeric equality rule here too, so nested record
            // fields preserve special-value semantics such as NaN != NaN.
            if (!valuesEqual(av, it->second)) return false;
        } else if (std::holds_alternative<ObjectRef>(av) && std::holds_alternative<ObjectRef>(it->second)) {
            if (!recordValuesEqual(std::get<ObjectRef>(av), std::get<ObjectRef>(it->second), seen)) return false;
        } else if (!valuesEqual(av, it->second)) {
            return false;
        }
    }
    return true;
}
} // namespace

namespace {
std::size_t hashCombineValue(std::size_t seed, std::size_t value) noexcept {
    return seed ^ (value + static_cast<std::size_t>(0x9e3779b97f4a7c15ULL) + (seed << 6) + (seed >> 2));
}

std::size_t valueHashImpl(const Value& v, std::unordered_set<const ObjectBox*>& seen);

std::size_t recordHash(const ObjectRef& obj, std::unordered_set<const ObjectBox*>& seen) {
    if (!obj) return 0;
    if (!seen.insert(obj.get()).second) return std::hash<const ObjectBox*>{}(obj.get());
    std::size_t seed = std::hash<std::string>{}(obj->className);
    if (obj->runtimeType && obj->runtimeType->isDataType) {
        for (const auto& field : obj->runtimeType->fields) {
            auto it = obj->fields.find(field.name);
            if (it == obj->fields.end()) continue;
            seed = hashCombineValue(seed, std::hash<std::string>{}(field.name));
            seed = hashCombineValue(seed, valueHashImpl(it->second, seen));
        }
    }
    return seed;
}

std::size_t valueHashImpl(const Value& v, std::unordered_set<const ObjectBox*>& seen) {
    return std::visit([&](const auto& held) -> std::size_t {
        using T = std::decay_t<decltype(held)>;
        if constexpr (std::is_same_v<T, std::monostate>) return 0x11;
        else if constexpr (std::is_same_v<T, bool>) return hashCombineValue(0x12, std::hash<bool>{}(held));
        else if constexpr (std::is_same_v<T, std::int64_t>) return hashCombineValue(0x13, std::hash<std::int64_t>{}(held));
        else if constexpr (std::is_same_v<T, double>) {
            if (std::isnan(held)) return 0x14;
            if (held == 0.0) return hashCombineValue(0x13, std::hash<std::int64_t>{}(0));
            constexpr double minExactInt = -9223372036854775808.0;
            constexpr double maxExactIntExclusive = 9223372036854775808.0;
            if (held >= minExactInt && held < maxExactIntExclusive && std::floor(held) == held) {
                return hashCombineValue(0x13, std::hash<std::int64_t>{}(static_cast<std::int64_t>(held)));
            }
            return hashCombineValue(0x14, std::hash<double>{}(held));
        } else if constexpr (std::is_same_v<T, std::string>) return hashCombineValue(0x15, std::hash<std::string>{}(held));
        else if constexpr (std::is_same_v<T, ObjectRef>) {
            if (held && held->runtimeType && held->runtimeType->isDataType) return hashCombineValue(0x18, recordHash(held, seen));
            return hashCombineValue(0x18, std::hash<const ObjectBox*>{}(held.get()));
        } else if constexpr (std::is_same_v<T, ListRef>) return hashCombineValue(0x16, std::hash<const ListBox*>{}(held.get()));
        else if constexpr (std::is_same_v<T, MapRef>) return hashCombineValue(0x17, std::hash<const MapBox*>{}(held.get()));
        else if constexpr (std::is_same_v<T, ClosureRef>) return hashCombineValue(0x19, std::hash<const ClosureBox*>{}(held.get()));
        else if constexpr (std::is_same_v<T, TaskRef>) return hashCombineValue(0x1a, std::hash<const void*>{}(held.get()));
        else if constexpr (std::is_same_v<T, ThreadRef>) return hashCombineValue(0x1b, std::hash<const void*>{}(held.get()));
        else if constexpr (std::is_same_v<T, NativeHandleRef>) return hashCombineValue(0x1c, std::hash<std::uint64_t>{}(held.id));
        else if constexpr (std::is_same_v<T, NativeBufferView>) return hashCombineValue(0x1d, hashCombineValue(std::hash<const void*>{}(held.data()), std::hash<std::size_t>{}(held.size())));
        else if constexpr (std::is_same_v<T, NativeStructView>) return hashCombineValue(0x1e, hashCombineValue(std::hash<const void*>{}(held.data()), hashCombineValue(std::hash<std::size_t>{}(held.size()), std::hash<std::size_t>{}(held.alignment()))));
        else return hashCombineValue(0x1f, std::hash<std::uint64_t>{}(held.id));
    }, v);
}
} // namespace

std::int64_t valueHashCode(const Value& v) {
    std::unordered_set<const ObjectBox*> seen;
    return static_cast<std::int64_t>(valueHashImpl(v, seen) & 0x7fffffffffffffffULL);
}

bool valuesEqual(const Value& a, const Value& b) {
    if (isNumericValue(a) && isNumericValue(b)) {
        if (const auto* ad = std::get_if<double>(&a); ad && std::isnan(*ad)) return false;
        if (const auto* bd = std::get_if<double>(&b); bd && std::isnan(*bd)) return false;
        return compareNumbers(a, b) == 0;
    }
    if (std::holds_alternative<ThreadRef>(a) && std::holds_alternative<ThreadRef>(b)) {
        return std::get<ThreadRef>(a) == std::get<ThreadRef>(b);
    }
    if (std::holds_alternative<NativeHandleRef>(a) && std::holds_alternative<NativeHandleRef>(b)) return std::get<NativeHandleRef>(a) == std::get<NativeHandleRef>(b);
    if (std::holds_alternative<NativeBufferView>(a) && std::holds_alternative<NativeBufferView>(b)) return std::get<NativeBufferView>(a) == std::get<NativeBufferView>(b);
    if (std::holds_alternative<NativeStructView>(a) && std::holds_alternative<NativeStructView>(b)) return std::get<NativeStructView>(a).data() == std::get<NativeStructView>(b).data() && std::get<NativeStructView>(a).size() == std::get<NativeStructView>(b).size() && std::get<NativeStructView>(a).alignment() == std::get<NativeStructView>(b).alignment();
    if (std::holds_alternative<NativeCallbackRef>(a) && std::holds_alternative<NativeCallbackRef>(b)) return std::get<NativeCallbackRef>(a) == std::get<NativeCallbackRef>(b);
    if (std::holds_alternative<ObjectRef>(a) && std::holds_alternative<ObjectRef>(b)) {
        const auto& ao = std::get<ObjectRef>(a);
        const auto& bo = std::get<ObjectRef>(b);
        if (ao && bo && ao->runtimeType && bo->runtimeType && ao->runtimeType->isDataType && bo->runtimeType->isDataType) {
            std::unordered_set<std::pair<const ObjectBox*, const ObjectBox*>, ObjectPairHash> seen;
            return recordValuesEqual(ao, bo, seen);
        }
        return ao == bo;
    }
    if (a.index() == b.index()) return a == b; // string==string, bool==bool, ListRef/MapRef compare by identity
    return false;
}

} // namespace zl
