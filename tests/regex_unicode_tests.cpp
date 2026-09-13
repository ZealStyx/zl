// Unicode regressions for the regex engine: the engine is byte-oriented on
// purpose, so these pin the code-point contract - literals, classes, \d \w
// and \p{...} must accept real multibyte characters, spans must be byte
// offsets into the original text, and group text must round-trip exactly.
//
// Every code point asserted below was checked against include/zl/regex/unicode.hpp:
//   é U+00E9 (2B) in kL 0xD8-0xF6,   ï U+00EF in kL,
//   Ж U+0416 (2B) in kL 0x3F7-0x481,  中 U+4E2D (3B) in kL 0x4E00-0xA48C,
//   ٣ U+0663 (2B) in kNd 0x660-0x669, 。 U+3002 (3B) in kP 0x3001-0x3003.
#include "zl/regex/regex.hpp"

#include <iostream>
#include <string>

namespace {

using namespace zl::regex_engine;

int failures = 0;

void require(bool condition, const std::string& message) {
    if (condition) return;
    std::cerr << "regex unicode regression: " << message << '\n';
    ++failures;
}

void expectMatch(const std::string& pattern, const std::string& text, std::size_t start, std::size_t end,
                 const std::string& label) {
    const auto r = match(parse(pattern), text, true);
    require(r.matched, label + " should match");
    if (!r.matched) return;
    require(r.start == start && r.end == end,
            label + " byte span [" + std::to_string(start) + "," + std::to_string(end) + ") got [" +
                std::to_string(r.start) + "," + std::to_string(r.end) + ")");
    require(r.groups[0] == text.substr(start, end - start), label + " group text must round-trip");
}

void expectNoMatch(const std::string& pattern, const std::string& text, const std::string& label) {
    require(!match(parse(pattern), text, true).matched, label + " should not match");
}

void testMultibyteLiterals() {
    // "x café au lait": x=0, space=1, c=2, a=3, f=4, é=5-6, space=7
    expectMatch("café", "x café au lait", 2, 7, "a 2-byte literal finds its leftmost position");
    // "a中b": a=0, 中=1-3, b=4
    expectMatch("中", "a中b", 1, 4, "a 3-byte literal spans three bytes");
    expectMatch("café|кофе", "кофе", 0, 8, "an alternation of two scripts");
    expectNoMatch("café", "cafe", "a multibyte literal is not its ASCII shadow");
}

void testUnicodeClasses() {
    expectMatch("\\d", "x٣y", 1, 3, "\\d accepts an Arabic-Indic digit");
    expectMatch("\\d+", "12٣4", 0, 5, "\\d+ mixes ASCII and extended digits");
    expectMatch("\\w+", "naïve", 0, 6, "\\w+ covers accented Latin");
    expectMatch("\\w", "Ж", 0, 2, "\\w accepts Cyrillic");
    expectNoMatch("\\d", "Ж", "a letter is not a decimal digit");
    expectNoMatch("\\w", " ", "a space is not a word character");

    expectMatch("[à-ö]", "xéy", 1, 3, "a class range over 2-byte boundaries");
    expectNoMatch("[à-ö]", "xay", "the range rejects outside letters");
    expectMatch("[^a-z]", "Ж", 0, 2, "a negated ASCII class accepts Cyrillic");
    expectMatch("[éа]", "а", 0, 2, "a class of two 2-byte literals");
}

void testUnicodeProperties() {
    expectMatch("\\p{L}", "Ж", 0, 2, "\\p{L} matches Cyrillic");
    expectMatch("\\p{L}", "中", 0, 3, "\\p{L} matches CJK");
    expectNoMatch("\\p{L}", "5", "\\p{L} rejects digits");
    expectMatch("\\p{N}", "٣", 0, 2, "\\p{N} accepts an Arabic-Indic digit");
    expectMatch("\\p{P}", "。", 0, 3, "\\p{P} accepts the CJK full stop");
    expectNoMatch("\\p{P}", "a", "\\p{P} rejects letters");
    expectMatch("\\P{L}", "!", 0, 1, "\\P{L} rejects letters, accepts punctuation");
    expectNoMatch("\\P{L}", "é", "\\P{L} rejects a real letter");
}

void testCodePointAdvancement() {
    const auto dot = match(parse("."), "a中b", true);
    require(dot.matched && dot.start == 0 && dot.end == 1, "dot consumes exactly one code point of ASCII width");
    const auto dot2 = match(parse("."), "中", true);
    require(dot2.matched && dot2.start == 0 && dot2.end == 3, "dot consumes a 3-byte code point");

    const auto pairs = match(parse("(.)\\1"), "éé", true);
    require(pairs.matched && pairs.start == 0 && pairs.end == 4, "a backreference repeats a 2-byte character");
    require(pairs.groups[1] == "é", "the captured 2-byte character round-trips");
    expectNoMatch("(.)\\1", "éè", "a backreference distinguishes 2-byte characters");
}

void testMatchAllMultibyte() {
    // x=0, é=1-2, y=3, é=4-5, z=6
    const auto all = matchAll(parse("é"), "xéyéz");
    require(all.size() == 2, "matchAll finds both é's");
    if (all.size() == 2) {
        require(all[0].start == 1 && all[0].end == 3 && all[1].start == 4 && all[1].end == 6,
                "matchAll spans are byte offsets into the original text");
        require(all[0].groups[0] == "é" && all[1].groups[0] == "é", "every group round-trips");
    }
}

void testInvalidUtf8() {
    bool threw = false;
    std::string message;
    try {
        parse(std::string("a") + std::string(1, static_cast<char>(0xff)));
    } catch (const std::exception& e) {
        threw = true;
        message = e.what();
    }
    require(threw && message.find("invalid UTF-8 in literal") != std::string::npos,
            "a pattern with a lone 0xFF byte is rejected at parse time");
}

} // namespace

int main() {
    testMultibyteLiterals();
    testUnicodeClasses();
    testUnicodeProperties();
    testCodePointAdvancement();
    testMatchAllMultibyte();
    testInvalidUtf8();

    if (failures != 0) {
        std::cerr << failures << " regex unicode regression(s) failed\n";
        return 1;
    }
    std::cout << "all regex unicode regressions passed\n";
    return 0;
}
