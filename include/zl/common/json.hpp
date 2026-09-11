#pragma once
#include <string>

namespace zl::common {
// JSON string literal, including quotes. Escape control bytes even in compiler
// messages/source paths, so diagnostic streams remain parseable.
inline std::string jsonString(const std::string& text) {
    constexpr char hex[] = "0123456789abcdef";
    std::string out = "\"";
    for (unsigned char c : text) {
        if (c == '"' || c == '\\') { out += '\\'; out += static_cast<char>(c); }
        else if (c < 0x20) { out += "\\u00"; out += hex[c >> 4]; out += hex[c & 15]; }
        else out += static_cast<char>(c);
    }
    return out + '"';
}
} // namespace zl::common
