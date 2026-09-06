#pragma once

#include <memory>
#include <string>
#include <vector>

namespace zl::regex_engine {

enum class NodeKind {
    Sequence,
    Alternation,
    Literal,
    Escape,
    CharacterClass,
    Any,
    Group,
    NamedGroup,
    AtomicGroup,
    Backreference,
    AnchorStart,
    AnchorEnd,
    Quantifier,
    Lookahead,
    NegativeLookahead,
    Lookbehind,
    NegativeLookbehind,
};

struct Node {
    NodeKind kind{NodeKind::Sequence};
    std::string text;
    int minimum{1};
    int maximum{1};
    bool greedy{true};
    bool possessive{false};
    int captureIndex{0};
    std::string captureName;
    int backreferenceIndex{0};
    std::vector<std::shared_ptr<Node>> children;
};

struct Pattern {
    std::shared_ptr<Node> root;
    std::string canonical;
};

// Parse and validate a ZL regex pattern once into the common AST representation.
// The initial backend renders that AST to the host std::regex engine, keeping
// parsing/validation independent from execution and giving both literal and
// fluent construction one semantic representation.
Pattern parse(const std::string& source);

// Compile a validated pattern for the host backend. Advanced lookbehind is intentionally
// rejected here because ECMAScript std::regex has no portable lookbehind support.
// The VM-native regex path handles the supported lookbehind slice separately.
struct MatchResult {
    bool matched{false};
    std::size_t start{0};
    std::size_t end{0};
    std::vector<std::string> groups;
    std::vector<bool> groupMatched;
};

// Dedicated AST execution backend. It is leftmost-first and backtracking, with
// explicit support for captures, backreferences, lookahead, and fixed-width
// lookbehind.
MatchResult match(const Pattern& pattern, const std::string& text, bool search = true);
std::vector<MatchResult> matchAll(const Pattern& pattern, const std::string& text);
std::string replace(const Pattern& pattern, const std::string& text, const std::string& replacement);

// Legacy host adapter retained for migration/testing only.
void compileHost(const std::string& source);

} // namespace zl::regex_engine
