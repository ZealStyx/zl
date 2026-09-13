// Regex engine regressions for the dedicated AST backend: leftmost-first
// matching, alternation/quantifier semantics (greedy, lazy, possessive),
// captures (positional, named, unmatched), backreferences, lookahead and
// fixed-width lookbehind, matchAll scans, replacement expansion, and every
// documented parse/compile rejection.
#include "zl/regex/regex.hpp"

#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using namespace zl::regex_engine;

int failures = 0;

void require(bool condition, const std::string& message) {
    if (condition) return;
    std::cerr << "regex regression: " << message << '\n';
    ++failures;
}

void expectNoMatch(const std::string& pattern, const std::string& text, const std::string& label) {
    const auto r = match(parse(pattern), text, true);
    require(!r.matched, label + " should not match");
}

void expectMatch(const std::string& pattern, const std::string& text, std::size_t start, std::size_t end,
                 const std::string& label) {
    const auto r = match(parse(pattern), text, true);
    require(r.matched, label + " should match");
    if (!r.matched) return;
    require(r.start == start && r.end == end,
            label + " should span [" + std::to_string(start) + "," + std::to_string(end) + ") got [" +
                std::to_string(r.start) + "," + std::to_string(r.end) + ")");
    require(!r.groups.empty() && r.groups[0] == text.substr(start, end - start),
            label + " group 0 must be the whole match");
}

void expectParseError(const std::string& pattern, const std::string& needle, const std::string& label) {
    bool threw = false;
    std::string message;
    try {
        parse(pattern);
    } catch (const std::exception& e) {
        threw = true;
        message = e.what();
    }
    require(threw, label + " must be rejected at parse time");
    require(threw && message.find(needle) != std::string::npos,
            label + " rejected with the right diagnostic (got: " + message + ")");
}

// ---------------------------------------------------------------------------
// Core matching
// ---------------------------------------------------------------------------

void testLeftmostMatch() {
    expectMatch("abc", "xabcx", 1, 4, "a literal finds its leftmost position");
    expectMatch("n", "banana", 2, 3, "single characters find their first position");
    const auto anchored = match(parse("abc"), "xabcx", false);
    require(!anchored.matched, "search=false anchors at the start");
    const auto anchored2 = match(parse("ba"), "banana", false);
    require(anchored2.matched && anchored2.start == 0 && anchored2.end == 2,
            "search=false matches at position zero");
}

void testAlternationLeftmostFirst() {
    const auto r = match(parse("cat|ca"), "cat", true);
    require(r.matched && r.groups[0] == "cat", "the first alternation arm wins, not the longest");
    const auto r2 = match(parse("a|ab"), "ab", true);
    require(r2.matched && r2.groups[0] == "a", "leftmost-first: the shorter first arm wins at the same start");
}

void testQuantifiers() {
    expectMatch("a+", "baaa", 1, 4, "greedy + takes everything");
    const auto lazy = match(parse("a+?"), "baaa", true);
    require(lazy.matched && lazy.start == 1 && lazy.end == 2, "lazy +? takes the minimum");
    expectMatch("a{2,3}", "xaaaY", 1, 4, "greedy {2,3} takes the maximum");
    const auto lazyRange = match(parse("a{2,3}?"), "xaaaY", true);
    require(lazyRange.matched && lazyRange.start == 1 && lazyRange.end == 3,
            "lazy {2,3}? takes the minimum");
    expectMatch("a{2}", "xaaxy", 1, 3, "exact {2}");
    expectMatch("a*", "xxb", 0, 0, "star matches the empty string");
    expectNoMatch("a+", "bbb", "a + with no candidate fails");

    // Possessive quantifiers never give back.
    const auto backtracking = match(parse("a+a"), "aa", true);
    require(backtracking.matched, "a+a matches by backtracking");
    const auto possessive = match(parse("a++a"), "aa", true);
    require(!possessive.matched, "a++a cannot give an 'a' back, so it fails");
}

void testCharacterClassesAndAny() {
    expectMatch("[a-c]", "xab", 1, 2, "a class range matches inside it");
    expectNoMatch("[a-c]", "xd", "a class range rejects outside it");
    expectMatch("[^0-9]", "x5q", 0, 1, "a negated class takes the leftmost non-digit");
    expectNoMatch("[^0-9]", "5", "a negated class rejects digits");
    const auto dot = match(parse("."), "z", true);
    require(dot.matched && dot.end - dot.start == 1, "dot matches one code point");
}

void testAnchors() {
    expectMatch("^abc", "abcdef", 0, 3, "^ anchors at the start");
    expectNoMatch("^abc", "xabc", "refuses when the literal is not first");
    expectMatch("cde$", "abcde", 2, 5, "$ anchors at the end");
    expectNoMatch("cde$", "cdef", "refuses when more text follows");
    expectMatch("^abc$", "abc", 0, 3, "a full anchor pair pins the whole string");
}

void testCaptures() {
    const auto r = match(parse("(\\d+)-(\\d+)"), "range 10-200 here", true);
    require(r.matched && r.groups[0] == "10-200", "group 0 is the whole match");
    require(r.groups.size() == 3 && r.groups[1] == "10" && r.groups[2] == "200",
            "positional groups capture in order");

    const auto named = match(parse("(?<year>\\d{4})"), "year 1999 x", true);
    require(named.matched && named.groups[1] == "1999", "a named group captures like a positional one");

    const auto optional = match(parse("(a)?(b)"), "b", true);
    require(optional.matched && optional.groups[0] == "b", "an unmatched optional group still lets the match succeed");
    require(optional.groups.size() == 3, "every declared group has a slot");
    require(optional.groupMatched.size() == 3 && !optional.groupMatched[1] && optional.groupMatched[2],
            "groupMatched flags the group that did not take part");
    require(optional.groups[1].empty(), "an unmatched group reports an empty string");
}

void testBackreferences() {
    const auto r = match(parse("(.)\\1"), "aabbcc", true);
    require(r.matched && r.groups[0] == "aa" && r.start == 0, "\\1 repeats the first capture");
    const auto named = match(parse("(?<w>\\w+) \\k<w>"), "mirror mirror", true);
    require(named.matched && named.groups[0] == "mirror mirror", "\\k<name> repeats the named capture");

    bool unknown = false, outOfRange = false;
    std::string message;
    try {
        match(parse("\\k<nope>"), "x", true);
    } catch (const std::exception& e) {
        unknown = true;
        message = e.what();
    }
    require(unknown && message.find("unknown named backreference") != std::string::npos,
            "a backreference to a capture nobody declared is rejected (got: " + message + ")");
    try {
        match(parse("(a)\\9"), "a", true);
    } catch (const std::exception& e) {
        outOfRange = true;
        message = e.what();
    }
    require(outOfRange && message.find("backreference index out of range") != std::string::npos,
            "a backreference beyond the capture count is rejected (got: " + message + ")");
    expectNoMatch("(.)\\1", "ab", "a backreference that cannot repeat rejects the candidate");
}

void testLookarounds() {
    expectMatch("a(?=b)", "xab", 1, 2, "a positive lookahead succeeds without consuming");
    expectMatch("a(?!b)", "xac", 1, 2, "a negative lookahead succeeds when the lookahead fails");
    expectNoMatch("a(?!b)", "xab", "a negative lookahead blocks when the lookahead holds");
    expectMatch("(?<=ab)c", "xxabc", 4, 5, "a fixed-width lookbehind looks behind without consuming");
    expectNoMatch("(?<!a)b", "ab", "a negative lookbehind blocks when the context is present");
    expectMatch("(?<!a)b", "xb", 1, 2, "a negative lookbehind passes when the context is absent");
    expectNoMatch("(?<=ab)c", "xxbc", "a lookbehind whose context is absent fails");

    expectParseError("(?<=a+)", "lookbehind must be a non-empty fixed-width expression",
                     "a variable-width lookbehind is rejected");
    bool hostRejected = false;
    try {
        compileHost("(?<=ab)c");
    } catch (const std::exception& e) {
        hostRejected = std::string(e.what()).find("requires the ZL runtime regex backend") != std::string::npos;
    }
    require(hostRejected, "the legacy host backend refuses lookbehind and points at the runtime backend");
}

void testAtomicGroups() {
    const auto atomic = match(parse("(?>a+)a"), "aa", true);
    require(!atomic.matched, "an atomic group never backtracks");
    const auto plain = match(parse("(a+)a"), "aa", true);
    require(plain.matched, "the same group without (?>) backtracks and matches");
}

// ---------------------------------------------------------------------------
// matchAll and replace
// ---------------------------------------------------------------------------

void testMatchAll() {
    const auto all = matchAll(parse("a"), "banana");
    require(all.size() == 3, "matchAll finds every 'a'");
    if (all.size() == 3) {
        require(all[0].start == 1 && all[1].start == 3 && all[2].start == 5,
                "matchAll reports the exact positions in order");
    }

    const auto overlapping = matchAll(parse("ab"), "abxab");
    require(overlapping.size() == 2, "matchAll continues after each match");
    if (overlapping.size() == 2) {
        require(overlapping[0].start == 0 && overlapping[1].start == 3,
                "matchAll resumes after the end of the previous match");
    }

    const auto longScan = matchAll(parse("a{2}"), "aaXaaaa");
    require(longScan.size() == 3, "matchAll keeps scanning past rejected starts");
    if (longScan.size() == 3) {
        require(longScan[0].start == 0 && longScan[1].start == 3 && longScan[2].start == 5,
                "matchAll spans are absolute");
    }

    // Empty matches advance one position instead of looping forever.
    const auto empties = matchAll(parse("x*"), "xxx");
    require(!empties.empty(), "empty matches are reported");
    for (std::size_t i = 1; i < empties.size(); ++i) {
        require(empties[i].start > empties[i - 1].start, "empty matches still advance");
    }
}

void testReplace() {
    const auto swapped = replace(parse("(a)(b)"), "abab", "$2$1");
    require(swapped == "baba", "$1/$2 expand every match");
    const auto named = replace(parse("(?<first>[a-z]+) (?<second>[a-z]+)"), "hello world", "${second} ${first}");
    require(named == "world hello", "${name} expands named groups");
    const auto unmatched = replace(parse("a"), "xa", "$1");
    require(unmatched == "x$1", "a $ without a group is left verbatim");
    require(replace(parse("b"), "banana", "-") == "-anana", "replace rewrites every occurrence");
    require(replace(parse("q"), "banana", "z") == "banana", "replace with no match is the identity");
}

// ---------------------------------------------------------------------------
// Parse/compile rejections
// ---------------------------------------------------------------------------

void testParseRejections() {
    expectParseError("(abc", "unterminated group", "an unterminated group");
    expectParseError("a|", "empty alternation arm", "an empty alternation arm");
    expectParseError("a{2,1}", "quantifier maximum is less than minimum", "a backwards range quantifier");
    expectParseError("a{", "expected quantifier minimum", "a quantifier without a minimum");
    expectParseError("[", "unterminated character class", "an unterminated character class");
    // The empty pattern is valid by design (parsePattern special-cases it) and
    // matches the empty string at position zero - pin both directions.
    bool emptyThrew = false;
    try {
        parse("");
    } catch (const std::exception&) {
        emptyThrew = true;
    }
    require(!emptyThrew, "the empty pattern is a valid pattern");
    const auto emptyMatch = match(parse(""), "anything", true);
    require(emptyMatch.matched && emptyMatch.start == 0 && emptyMatch.end == 0,
            "the empty pattern matches the empty string at the start");
    expectParseError("()", "empty sequence", "an empty group");
    expectParseError("(?<a>x)(?<a>y)", "duplicate capture name", "a duplicated capture name");
    expectParseError("a**", "quantifier without a preceding atom", "a quantifier stacked on a quantifier");
    expectParseError("(?>a", "unterminated atomic group", "an unterminated atomic group");
    expectParseError("(?<", "invalid named capture syntax", "a named capture without a closing delimiter");

    // The diagnostic carries the offending offset.
    bool offsetCarried = false;
    try {
        parse("(abc");
    } catch (const std::exception& e) {
        offsetCarried = std::string(e.what()) == "Regex: unterminated group at offset 4";
    }
    require(offsetCarried, "parse errors report the exact offset");
}

// Escapes: \d \w \s and friends, plus literal escaping.
void testEscapes() {
    expectMatch("\\d+", "x123ab4", 1, 4, "\\d+ runs of decimal digits");
    expectNoMatch("\\d+", "abc", "digits only");
    expectMatch("\\w+", "ab1_c", 0, 5, "\\w+ covers letters digits and underscore");
    expectMatch("\\s", "a b", 1, 2, "\\s matches a space");
    expectNoMatch("\\S", " ", "a lone space is not \\S");
    expectMatch("\\.", "a.b", 1, 2, "an escaped dot is literal");
    expectNoMatch("a.b", "ab", "an unescaped dot still consumes one code point");
    expectMatch("a.b", "a-b", 0, 3, "an unescaped dot matches any code point");
}

} // namespace

int main() {
    testLeftmostMatch();
    testAlternationLeftmostFirst();
    testQuantifiers();
    testCharacterClassesAndAny();
    testAnchors();
    testCaptures();
    testBackreferences();
    testLookarounds();
    testAtomicGroups();
    testMatchAll();
    testReplace();
    testParseRejections();
    testEscapes();

    if (failures != 0) {
        std::cerr << failures << " regex regression(s) failed\n";
        return 1;
    }
    std::cout << "all regex regressions passed\n";
    return 0;
}
