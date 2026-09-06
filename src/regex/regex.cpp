#include "zl/regex/regex.hpp"
#include "zl/regex/unicode.hpp"

#include <cctype>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <optional>
#include <algorithm>
#include <functional>
#include <regex>

namespace zl::regex_engine {
namespace {

static std::size_t utf8Width(unsigned char c) {
    if ((c & 0x80) == 0) return 1;
    if ((c & 0xE0) == 0xC0) return 2;
    if ((c & 0xF0) == 0xE0) return 3;
    if ((c & 0xF8) == 0xF0) return 4;
    return 0;
}

static bool decodeUtf8(const std::string& s, std::size_t pos, std::uint32_t& cp, std::size_t& width) {
    if (pos >= s.size()) return false;
    const unsigned char c0 = static_cast<unsigned char>(s[pos]);
    width = utf8Width(c0);
    if (width == 0 || pos + width > s.size()) return false;
    cp = 0;
    if (width == 1) cp = c0;
    else {
        cp = c0 & ((1u << (7 - width)) - 1u);
        for (std::size_t i = 1; i < width; ++i) {
            const unsigned char c = static_cast<unsigned char>(s[pos + i]);
            if ((c & 0xC0) != 0x80) return false;
            cp = (cp << 6) | (c & 0x3F);
        }
        if ((width == 2 && cp < 0x80) || (width == 3 && cp < 0x800) || (width == 4 && cp < 0x10000) || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return false;
    }
    return true;
}

template <class Ranges>
static bool inRanges(std::uint32_t cp, const Ranges& ranges) {
    std::size_t lo=0, hi=ranges.size();
    while (lo < hi) {
        const std::size_t mid=(lo+hi)/2;
        if (cp < ranges[mid].first) hi=mid;
        else if (cp > ranges[mid].second) lo=mid+1;
        else return true;
    }
    return false;
}

static bool unicodeWord(std::uint32_t cp);

static bool unicodeProperty(std::uint32_t cp, char prop) {
    switch (prop) {
        case 'L': return inRanges(cp, unicode::kL);
        case 'N': return inRanges(cp, unicode::kN);
        case 'M': return inRanges(cp, unicode::kM);
        case 'P': return inRanges(cp, unicode::kP);
        case 'S': return inRanges(cp, unicode::kS);
        case 'Z': return inRanges(cp, unicode::kZ);
        case 'C': return inRanges(cp, unicode::kC);
        default: return false;
    }
}

static bool unicodeDecimalDigit(std::uint32_t cp) { return inRanges(cp, unicode::kNd); }
static bool unicodeSpace(std::uint32_t cp) { return unicodeProperty(cp,'Z') || cp=='\t' || cp=='\n' || cp=='\r' || cp=='\f' || cp=='\v'; }

class Parser {
public:
    explicit Parser(const std::string& source) : source_(source) {}

    Pattern parsePattern() {
        Pattern out;
        if (source_.empty()) {
            out.root = make(NodeKind::Sequence);
            out.canonical.clear();
            return out;
        }
        out.root = parseAlternation();
        if (!atEnd()) fail("unexpected character '" + std::string(1, peek()) + "'");
        if (!captureNames_.empty()) {
            // Validate capture names are unique and deterministic.
            std::unordered_set<std::string> seen;
            for (const auto& name : captureNames_) {
                if (name.empty()) continue;
                if (!seen.insert(name).second) fail("duplicate capture name '" + name + "'");
            }
        }
        out.canonical = render(out.root);
        return out;
    }

private:
    const std::string& source_;
    std::size_t pos_{0};
    int captureCount_{0};
    std::vector<std::string> captureNames_;

    bool atEnd() const { return pos_ >= source_.size(); }
    char peek() const { return atEnd() ? '\0' : source_[pos_]; }
    char advance() { return atEnd() ? '\0' : source_[pos_++]; }

    [[noreturn]] void fail(const std::string& message) const {
        throw std::runtime_error("Regex: " + message + " at offset " + std::to_string(pos_));
    }

    std::shared_ptr<Node> make(NodeKind kind) {
        auto n = std::make_shared<Node>();
        n->kind = kind;
        return n;
    }

    std::shared_ptr<Node> parseAlternation() {
        auto left = parseSequence();
        if (peek() != '|') return left;
        auto n = make(NodeKind::Alternation);
        n->children.push_back(left);
        while (peek() == '|') {
            advance();
            if (peek() == '|' || peek() == ')' || atEnd()) fail("empty alternation arm");
            n->children.push_back(parseSequence());
        }
        return n;
    }

    std::shared_ptr<Node> parseSequence() {
        auto n = make(NodeKind::Sequence);
        while (!atEnd() && peek() != ')' && peek() != '|') {
            n->children.push_back(parseAtom());
        }
        if (n->children.empty()) fail("empty sequence");
        if (n->children.size() == 1) return n->children.front();
        return n;
    }

    std::shared_ptr<Node> parseAtom() {
        const char c = peek();
        std::shared_ptr<Node> n;
        if (c == '^') {
            advance(); n = make(NodeKind::AnchorStart); n->text = "^";
        } else if (c == '$') {
            advance(); n = make(NodeKind::AnchorEnd); n->text = "$";
        } else if (c == '.') {
            advance(); n = make(NodeKind::Any); n->text = ".";
        } else if (c == '(') {
            advance();
            if (peek() == '?') {
                advance();
                if (peek() == ':') {
                    advance();
                    n = make(NodeKind::Group);
                } else if (peek() == '>') {
                    advance();
                    auto inner = parseAlternation();
                    if (peek() != ')') fail("unterminated atomic group");
                    advance();
                    n = make(NodeKind::AtomicGroup);
                    n->children.push_back(inner);
                    return maybeQuantify(n);
                } else if (peek() == '=') {
                    advance();
                    auto inner = parseAlternation();
                    if (peek() != ')') fail("unterminated positive lookahead");
                    advance();
                    n = make(NodeKind::Lookahead);
                    n->children.push_back(inner);
                    return n;
                } else if (peek() == '!') {
                    advance();
                    auto inner = parseAlternation();
                    if (peek() != ')') fail("unterminated negative lookahead");
                    advance();
                    n = make(NodeKind::NegativeLookahead);
                    n->children.push_back(inner);
                    return n;
                } else if (peek() == '<') {
                    advance();
                    if (peek() == '=') {
                        advance();
                        auto inner = parseAlternation();
                        if (peek() != ')') fail("unterminated positive lookbehind");
                        advance();
                        n = make(NodeKind::Lookbehind);
                        n->children.push_back(inner);
                        return n;
                    }
                    if (peek() == '!') {
                        advance();
                        auto inner = parseAlternation();
                        if (peek() != ')') fail("unterminated negative lookbehind");
                        advance();
                        n = make(NodeKind::NegativeLookbehind);
                        n->children.push_back(inner);
                        return n;
                    }
                    std::string name;
                    while (!atEnd() && peek() != '>') name += advance();
                    if (peek() != '>' || name.empty()) fail("invalid named capture syntax");
                    advance();
                    validateName(name);
                    const int index = ++captureCount_;
                    captureNames_.push_back(name);
                    n = make(NodeKind::NamedGroup);
                    n->captureIndex = index;
                    n->captureName = name;
                    auto inner = parseAlternation();
                    if (peek() != ')') fail("unterminated named capture");
                    advance();
                    n->children.push_back(inner);
                    return maybeQuantify(n);
                } else {
                    fail("unsupported group modifier");
                }
            } else {
                const int index = ++captureCount_;
                captureNames_.push_back({});
                n = make(NodeKind::Group);
                n->captureIndex = index;
            }
            auto inner = parseAlternation();
            if (peek() != ')') fail("unterminated group");
            advance();
            n->children.push_back(inner);
        } else if (c == '[') {
            n = parseCharacterClass();
        } else if (c == '\\') {
            advance();
            if (atEnd()) fail("trailing escape");
            const char e = advance();
            if ((e == 'p' || e == 'P') && peek() == '{') {
                advance();
                std::string body;
                while (!atEnd() && peek() != '}') body += advance();
                if (peek() != '}' || body.size() != 1) fail("invalid Unicode property escape");
                advance();
                if (std::string("LNMPSZC").find(body[0]) == std::string::npos) fail("unsupported Unicode property");
                n = make(NodeKind::Escape);
                n->text = std::string("\\") + e + "{" + body + "}";
            } else if (std::isdigit(static_cast<unsigned char>(e))) {
                n = make(NodeKind::Backreference);
                n->backreferenceIndex = e - '0';
                if (n->backreferenceIndex <= 0 || n->backreferenceIndex > 9) fail("numeric backreference out of range");
            } else if (e == 'k' && peek() == '<') {
                advance();
                std::string name;
                while (!atEnd() && peek() != '>') name += advance();
                if (peek() != '>' || name.empty()) fail("invalid named backreference syntax");
                advance();
                n = make(NodeKind::Backreference);
                n->captureName = name;
            } else {
                n = make(NodeKind::Escape);
                n->text = "\\" + std::string(1, e);
            }
        } else if (c == '*' || c == '+' || c == '?' || c == '{') {
            fail("quantifier without a preceding atom");
        } else {
            std::uint32_t cp = 0; std::size_t width = 0;
            if (!decodeUtf8(source_, pos_, cp, width)) fail("invalid UTF-8 in literal");
            n = make(NodeKind::Literal);
            n->text = source_.substr(pos_, width);
            pos_ += width;
        }
        return maybeQuantify(n);
    }

    std::shared_ptr<Node> maybeQuantify(std::shared_ptr<Node> atom) {
        if (peek() == '*' || peek() == '+' || peek() == '?' || peek() == '{') return parseQuantifier(std::move(atom));
        return atom;
    }

    void validateName(const std::string& name) const {
        if (!(std::isalpha(static_cast<unsigned char>(name[0])) || name[0] == '_')) fail("invalid capture name '" + name + "'");
        for (char c : name) if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_')) fail("invalid capture name '" + name + "'");
    }

    std::shared_ptr<Node> parseQuantifier(std::shared_ptr<Node> atom) {
        auto q = make(NodeKind::Quantifier);
        q->children.push_back(std::move(atom));
        const char c = peek();
        if (c == '*') { advance(); q->minimum = 0; q->maximum = -1; }
        else if (c == '+') { advance(); q->minimum = 1; q->maximum = -1; }
        else if (c == '?') { advance(); q->minimum = 0; q->maximum = 1; }
        else {
            advance();
            std::string lo;
            while (std::isdigit(static_cast<unsigned char>(peek()))) lo += advance();
            if (lo.empty()) fail("expected quantifier minimum");
            q->minimum = std::stoi(lo);
            if (peek() == '}') { advance(); q->maximum = q->minimum; }
            else if (peek() == ',') {
                advance();
                std::string hi;
                while (std::isdigit(static_cast<unsigned char>(peek()))) hi += advance();
                if (peek() != '}') fail("unterminated range quantifier");
                advance();
                q->maximum = hi.empty() ? -1 : std::stoi(hi);
                if (q->maximum >= 0 && q->maximum < q->minimum) fail("quantifier maximum is less than minimum");
            } else fail("expected '}' in quantifier");
        }
        if (peek() == '?') { advance(); q->greedy = false; }
        else if (peek() == '+') { advance(); q->possessive = true; }
        return q;
    }

    std::shared_ptr<Node> parseCharacterClass() {
        auto n = make(NodeKind::CharacterClass);
        std::string text;
        text += advance();
        if (peek() == '^') text += advance();
        bool closed = false;
        while (!atEnd()) {
            const char c = advance();
            if (c == ']') { text += c; closed = true; break; }
            if (c == '\\') {
                if (atEnd()) fail("unterminated character-class escape");
                text += c; text += advance();
            } else text += c;
        }
        if (!closed) fail("unterminated character class");
        n->text = std::move(text);
        return n;
    }

    static std::string render(const std::shared_ptr<Node>& n) {
        switch (n->kind) {
            case NodeKind::Literal: case NodeKind::Escape: case NodeKind::CharacterClass:
            case NodeKind::Any: case NodeKind::AnchorStart: case NodeKind::AnchorEnd:
                return n->text;
            case NodeKind::Group:
                return "(" + render(n->children.front()) + ")";
            case NodeKind::AtomicGroup:
                return "(?>" + render(n->children.front()) + ")";
            case NodeKind::NamedGroup:
                return "(?<" + n->captureName + ">" + render(n->children.front()) + ")";
            case NodeKind::Backreference:
                return n->captureName.empty() ? "\\" + std::to_string(n->backreferenceIndex) : "\\k<" + n->captureName + ">";
            case NodeKind::Lookahead: return "(?=" + render(n->children.front()) + ")";
            case NodeKind::NegativeLookahead: return "(?!" + render(n->children.front()) + ")";
            case NodeKind::Lookbehind: return "(?<=" + render(n->children.front()) + ")";
            case NodeKind::NegativeLookbehind: return "(?<!" + render(n->children.front()) + ")";
            case NodeKind::Sequence: { std::string out; for (const auto& child : n->children) out += render(child); return out; }
            case NodeKind::Alternation: { std::string out; for (std::size_t i=0;i<n->children.size();++i){ if(i) out+='|'; out+=render(n->children[i]); } return out; }
            case NodeKind::Quantifier: {
                std::string out = render(n->children.front());
                if (n->minimum == 0 && n->maximum == -1) out += '*';
                else if (n->minimum == 1 && n->maximum == -1) out += '+';
                else if (n->minimum == 0 && n->maximum == 1) out += '?';
                else if (n->maximum == n->minimum) out += '{' + std::to_string(n->minimum) + '}';
                else if (n->maximum == -1) out += '{' + std::to_string(n->minimum) + ",}";
                else out += '{' + std::to_string(n->minimum) + ',' + std::to_string(n->maximum) + '}';
                if (!n->greedy) out += '?';
                return out;
            }
        }
        throw std::logic_error("Regex: unknown AST node");
    }
};

void validateReferences(const std::shared_ptr<Node>& n, int captureCount, const std::unordered_map<std::string, int>& names) {
    if (n->kind == NodeKind::Backreference) {
        if (!n->captureName.empty()) {
            auto it = names.find(n->captureName);
            if (it == names.end()) {
                throw std::runtime_error("Regex: unknown named backreference '" + n->captureName + "'");
            }
            n->backreferenceIndex = it->second;
        } else if (n->backreferenceIndex <= 0 || n->backreferenceIndex > captureCount) {
            throw std::runtime_error("Regex: backreference index out of range at capture " + std::to_string(n->backreferenceIndex));
        }
    }
    for (const auto& child : n->children) validateReferences(child, captureCount, names);
}

void collectCaptureNames(const std::shared_ptr<Node>& n, std::unordered_map<std::string, int>& names, int& nextIndex) {
    if (n->kind == NodeKind::Group && n->captureIndex > 0) {
        ++nextIndex;
    } else if (n->kind == NodeKind::NamedGroup && n->captureIndex > 0) {
        ++nextIndex;
        names[n->captureName] = n->captureIndex;
    }
    for (const auto& child : n->children) collectCaptureNames(child, names, nextIndex);
}


int validationLookbehindWidth(const std::shared_ptr<Node>& n) {
    switch (n->kind) {
        case NodeKind::Literal:
        case NodeKind::Escape:
        case NodeKind::CharacterClass:
        case NodeKind::Any:
            return 1;
        case NodeKind::Sequence: {
            int w = 0;
            for (const auto& c : n->children) { int x = validationLookbehindWidth(c); if (x < 0) return -1; w += x; }
            return w;
        }
        case NodeKind::Group:
        case NodeKind::NamedGroup:
            return validationLookbehindWidth(n->children.front());
        case NodeKind::Quantifier:
            if (n->minimum != n->maximum) return -1;
            { int x = validationLookbehindWidth(n->children.front()); return x < 0 ? -1 : x * n->minimum; }
        default:
            return -1;
    }
}

void validateLookbehinds(const std::shared_ptr<Node>& n) {
    if (n->kind == NodeKind::Lookbehind || n->kind == NodeKind::NegativeLookbehind) {
        const int width = validationLookbehindWidth(n->children.front());
        if (width <= 0 || width > 1024) throw std::runtime_error("Regex: lookbehind must be a non-empty fixed-width expression (maximum width 1024)");
    }
    for (const auto& c : n->children) validateLookbehinds(c);
}

} // namespace

Pattern parse(const std::string& source) {
    auto out = Parser(source).parsePattern();
    std::unordered_map<std::string, int> names;
    int count = 0;
    collectCaptureNames(out.root, names, count);
    validateLookbehinds(out.root);
    validateReferences(out.root, count, names);
    return out;
}

namespace {
bool containsUnsupportedHostFeature(const std::shared_ptr<Node>& n) {
    if (n->kind == NodeKind::Lookbehind || n->kind == NodeKind::NegativeLookbehind) return true;
    for (const auto& child : n->children) if (containsUnsupportedHostFeature(child)) return true;
    return false;
}
void collectHostNames(const std::shared_ptr<Node>& n, std::unordered_map<std::string, int>& names) {
    if (n->kind == NodeKind::NamedGroup && !n->captureName.empty()) names[n->captureName] = n->captureIndex;
    for (const auto& child : n->children) collectHostNames(child, names);
}
std::string renderHostRegex(const std::shared_ptr<Node>& n, const std::unordered_map<std::string, int>& names) {
    switch (n->kind) {
        case NodeKind::NamedGroup: return "(" + renderHostRegex(n->children.front(), names) + ")";
        case NodeKind::Backreference: {
            int index = n->backreferenceIndex;
            if (!n->captureName.empty()) index = names.at(n->captureName);
            return "\\" + std::to_string(index);
        }
        case NodeKind::Sequence: { std::string out; for (const auto& c : n->children) out += renderHostRegex(c, names); return out; }
        case NodeKind::Alternation: { std::string out; for(size_t i=0;i<n->children.size();++i){if(i)out+='|';out+=renderHostRegex(n->children[i], names);}return out; }
        case NodeKind::Group: return "("+renderHostRegex(n->children.front(), names)+")";
        case NodeKind::Lookahead: return "(?="+renderHostRegex(n->children.front(), names)+")";
        case NodeKind::NegativeLookahead: return "(?!"+renderHostRegex(n->children.front(), names)+")";
        case NodeKind::Quantifier: {
            std::string out=renderHostRegex(n->children.front(), names);
            if(n->minimum==0&&n->maximum==-1)out+='*'; else if(n->minimum==1&&n->maximum==-1)out+='+'; else if(n->minimum==0&&n->maximum==1)out+='?'; else if(n->maximum==n->minimum)out+='{'+std::to_string(n->minimum)+'}'; else if(n->maximum==-1)out+='{'+std::to_string(n->minimum)+",}"; else out+='{'+std::to_string(n->minimum)+','+std::to_string(n->maximum)+'}'; if(!n->greedy)out+='?'; return out;
        }
        default: return n->text;
    }
}
}

void compileHost(const std::string& source) {
    thread_local std::unordered_map<std::string, std::unique_ptr<std::regex>> cache;
    auto it = cache.find(source);
    if (it != cache.end()) return;
    auto ast = parse(source);
    if (containsUnsupportedHostFeature(ast.root)) throw std::runtime_error("Regex: pattern requires the ZL runtime regex backend");
    try {
        std::unordered_map<std::string, int> names;
        collectHostNames(ast.root, names);
        auto compiled = std::make_unique<std::regex>(renderHostRegex(ast.root, names), std::regex::ECMAScript);
        const auto* ptr = compiled.get();
        cache.emplace(source, std::move(compiled));
        (void)ptr;
        return;
    } catch (const std::regex_error& e) {
        throw std::runtime_error(std::string("Regex: invalid pattern: ") + e.what());
    }
}


namespace {
struct CapState { std::vector<int> start; std::vector<int> end; std::vector<bool> set; };
struct State { std::size_t pos{0}; CapState caps; };

static CapState makeCaps(int n) { return CapState{std::vector<int>(n + 1, -1), std::vector<int>(n + 1, -1), std::vector<bool>(n + 1, false)}; }
static int maxCapture(const std::shared_ptr<Node>& n) {
    int m = 0; if (n->captureIndex > m) m=n->captureIndex; for (auto& c:n->children) m=std::max(m,maxCapture(c)); return m;
}
static bool classAtom(const std::string& cls, std::uint32_t cp) {
    if (cls.size()<2 || cls.front()!='[' || cls.back()!=']') return false;
    bool neg = cls.size()>2 && cls[1]=='^';
    std::size_t i=neg?2:1; bool ok=false;
    while (i<cls.size()-1) {
        if (cls[i]=='\\') {
            ++i; if (i>=cls.size()-1) break; char e=cls[i++];
            if ((e=='p' || e=='P') && i<cls.size()-1 && cls[i]=='{') {
                ++i; if (i>=cls.size()-1) break; char prop=cls[i++]; if (i>=cls.size()-1 || cls[i++]!='}') continue; bool hit=unicodeProperty(cp,prop); ok = ok || (e=='p'?hit:!hit); continue;
            }
            switch(e){ case 'd': ok|=unicodeDecimalDigit(cp); break; case 'D': ok|=!unicodeDecimalDigit(cp); break; case 'w': ok|=unicodeWord(cp); break; case 'W': ok|=!unicodeWord(cp); break; case 's': ok|=unicodeSpace(cp); break; case 'S': ok|=!unicodeSpace(cp); break; case 't': ok|=cp=='\t'; break; case 'n': ok|=cp=='\n'; break; case 'r': ok|=cp=='\r'; break; default: ok|=cp==(unsigned char)e; break; }
            continue;
        }
        std::uint32_t a=0; std::size_t aw=0;
        if(!decodeUtf8(cls,i,a,aw) || i+aw>cls.size()-1) break; i+=aw;
        if(i<cls.size()-1 && cls[i]=='-') {
            ++i; std::uint32_t b=0; std::size_t bw=0; if(!decodeUtf8(cls,i,b,bw) || i+bw>cls.size()-1) break; i+=bw; if(a<=cp && cp<=b) ok=true;
        } else if(a==cp) ok=true;
    }
    return neg?!ok:ok;
}

static bool unicodeWord(std::uint32_t cp) { return unicodeProperty(cp,'L') || unicodeProperty(cp,'N') || unicodeProperty(cp,'M') || cp=='_'; }

static bool escapeAtom(const std::string& e,std::uint32_t cp){
    if(e.size()==5 && e[0]=='\\' && (e[1]=='p'||e[1]=='P') && e[2]=='{' && e[4]=='}') { bool hit=unicodeProperty(cp,e[3]); return e[1]=='p'?hit:!hit; }
    if(e.size()!=2||e[0]!='\\') return false;
    switch(e[1]){case 'd':return unicodeDecimalDigit(cp);case 'D':return !unicodeDecimalDigit(cp);case 'w':return unicodeWord(cp);case 'W':return !unicodeWord(cp);case 's':return unicodeSpace(cp);case 'S':return !unicodeSpace(cp);case 't':return cp=='\t';case 'n':return cp=='\n';case 'r':return cp=='\r';case 'f':return cp=='\f';case 'v':return cp=='\v';default:return cp==(unsigned char)e[1];}
}

static std::vector<State> matchNode(const std::shared_ptr<Node>& n, const std::string& text, State st);
static std::vector<State> matchSequence(const std::vector<std::shared_ptr<Node>>& nodes,size_t i,const std::string& text,State st){
    if(i>=nodes.size()) return {st};
    std::vector<State> out;
    for(auto &s:matchNode(nodes[i],text,st)) { auto tail=matchSequence(nodes,i+1,text,s); out.insert(out.end(),tail.begin(),tail.end()); }
    return out;
}
static std::vector<State> repeatNode(const std::shared_ptr<Node>& child,int min,int max,bool greedy,bool possessive,const std::string& text,const State& base){
    std::vector<State> out;
    std::function<void(const State&,int)> rec=[&](const State& s,int count){
        if (possessive) {
            State cur=s;
            int c=count;
            while (max < 0 || c < max) {
                auto next=matchNode(child,text,cur);
                if(next.empty()) break;
                auto chosen=next.front();
                if(chosen.pos==cur.pos) break;
                cur=chosen;
                ++c;
            }
            if(c>=min) out.push_back(cur);
            return;
        }

        if(!greedy && count>=min) out.push_back(s);
        if(max>=0 && count>=max) {
            if(greedy && count>=min) out.push_back(s);
            return;
        }

        auto next=matchNode(child,text,s);
        for(auto &ns:next){
            if(ns.pos==s.pos) continue;
            rec(ns,count+1);
        }

        if(greedy && count>=min) out.push_back(s);
    };
    rec(base,0);
    return out;
}
static int fixedWidth(const std::shared_ptr<Node>& n){
    switch(n->kind){
      case NodeKind::Literal: case NodeKind::Escape: case NodeKind::Any: case NodeKind::CharacterClass: return 1;
      case NodeKind::Sequence:{int w=0;for(auto&c:n->children){int x=fixedWidth(c);if(x<0)return -1;w+=x;}return w;}
      case NodeKind::Group: case NodeKind::NamedGroup: case NodeKind::AtomicGroup:return fixedWidth(n->children.front());
      case NodeKind::Quantifier:return n->minimum==n->maximum ? fixedWidth(n->children.front())*n->minimum : -1;
      default:return -1;
    }
}
static bool matchLookbehind(const std::shared_ptr<Node>& n,const std::string& text,size_t pos,const State& st){ int w=fixedWidth(n); if(w<0||pos<(size_t)w)return false; auto cp=st; cp.pos=pos-w; auto r=matchNode(n,text,cp); for(auto&s:r)if(s.pos==pos)return true; return false; }
static std::vector<State> matchNode(const std::shared_ptr<Node>& n,const std::string& text,State st){
    switch(n->kind){
      case NodeKind::Sequence:return matchSequence(n->children,0,text,st);
      case NodeKind::Alternation:{std::vector<State> o;for(auto&c:n->children){auto r=matchNode(c,text,st);o.insert(o.end(),r.begin(),r.end());}return o;}
      case NodeKind::Literal:{if(st.pos+n->text.size()>text.size()||text.compare(st.pos,n->text.size(),n->text)!=0)return {};std::uint32_t cp=0;std::size_t w=0;if(!decodeUtf8(text,st.pos,cp,w))return {};st.pos+=w;return {st};}
      case NodeKind::Escape:{std::uint32_t cp=0;std::size_t w=0;if(!decodeUtf8(text,st.pos,cp,w)||!escapeAtom(n->text,cp))return {};st.pos+=w;return {st};}
      case NodeKind::CharacterClass:{std::uint32_t cp=0;std::size_t w=0;if(!decodeUtf8(text,st.pos,cp,w)||!classAtom(n->text,cp))return {};st.pos+=w;return {st};}
      case NodeKind::Any:{std::uint32_t cp=0;std::size_t w=0;if(!decodeUtf8(text,st.pos,cp,w))return {};st.pos+=w;return {st};}
      case NodeKind::AnchorStart:{return st.pos==0?std::vector<State>{st}:std::vector<State>{};}
      case NodeKind::AnchorEnd:{return st.pos==text.size()?std::vector<State>{st}:std::vector<State>{};}
      case NodeKind::Group: case NodeKind::NamedGroup:{auto base=st;int i=n->captureIndex; if(i>0){base.caps.start[i]=(int)st.pos;base.caps.set[i]=false;}auto r=matchNode(n->children.front(),text,base);for(auto&s:r)if(i>0){s.caps.end[i]=(int)s.pos;s.caps.set[i]=true;}return r;}
      case NodeKind::AtomicGroup:{auto r=matchNode(n->children.front(),text,st); if(r.empty()) return {}; return {r.front()};}
      case NodeKind::Backreference:{int i=n->backreferenceIndex; if(i==0&&!n->captureName.empty()) return {}; if(i<=0||i>=(int)st.caps.set.size()||!st.caps.set[i])return {};size_t a=st.caps.start[i],b=st.caps.end[i];if(b<a||b>text.size()||st.pos+b-a>text.size())return {};if(text.compare(st.pos,b-a,text,a,b-a)!=0)return {};st.pos+=b-a;return {st};}
      case NodeKind::Quantifier:return repeatNode(n->children.front(),n->minimum,n->maximum,n->greedy,n->possessive,text,st);
      case NodeKind::Lookahead:{for(auto&s:matchNode(n->children.front(),text,st)) { (void)s; return {st}; } return {};}
      case NodeKind::NegativeLookahead:{return matchNode(n->children.front(),text,st).empty()?std::vector<State>{st}:std::vector<State>{};}
      case NodeKind::Lookbehind:{return matchLookbehind(n->children.front(),text,st.pos,st)?std::vector<State>{st}:std::vector<State>{};}
      case NodeKind::NegativeLookbehind:{return matchLookbehind(n->children.front(),text,st.pos,st)?std::vector<State>{}:std::vector<State>{st};}
    } return {};
}

static std::unordered_map<std::string,int> captureNameMap(const std::shared_ptr<Node>& n){std::unordered_map<std::string,int> m;if(n->kind==NodeKind::NamedGroup&&!n->captureName.empty())m[n->captureName]=n->captureIndex;for(auto&c:n->children){auto x=captureNameMap(c);m.insert(x.begin(),x.end());}return m;}
static MatchResult materialize(const std::string& text,const State& s){MatchResult out;out.matched=true;out.start=s.caps.start.empty()?s.pos:(s.caps.set[0]?(size_t)s.caps.start[0]:s.pos);out.end=s.pos;for(size_t i=0;i<s.caps.set.size();++i){out.groupMatched.push_back(s.caps.set[i]);out.groups.push_back(s.caps.set[i]?text.substr(s.caps.start[i],s.caps.end[i]-s.caps.start[i]):std::string{});}if(out.groups.empty()){out.groups.push_back(text.substr(out.start,out.end-out.start));out.groupMatched.push_back(true);}return out;}
}

MatchResult match(const Pattern& pattern, const std::string& text, bool search){
    const int n=maxCapture(pattern.root); auto caps=makeCaps(n); caps.start[0]=0; caps.set[0]=true;
    const size_t first=search?0:0, last=search?text.size():0;
    for(size_t pos=first;pos<=last;++pos){State st{pos,caps};auto rs=matchNode(pattern.root,text,st);for(auto&s:rs){s.caps.start[0]=(int)pos;s.caps.end[0]=(int)s.pos;s.caps.set[0]=true;return materialize(text,s);} if(!search)break;}
    return {};
}
std::vector<MatchResult> matchAll(const Pattern& pattern,const std::string& text){
    std::vector<MatchResult> out; size_t pos=0; while(pos<=text.size()){auto r=match(pattern,text,true); // limit search manually by slicing to preserve absolute spans
        if(!r.matched) break; if(!out.empty() && r.start<pos) { pos++; continue; }
        out.push_back(r); pos=r.end>r.start?r.end:r.start+1; if(r.end==text.size()&&r.start==text.size()) break;
        if(pos>text.size()) break; std::string suffix=text.substr(pos); auto rr=match(pattern,suffix,true); if(!rr.matched) break; rr.start+=pos;rr.end+=pos; for(size_t i=1;i<rr.groups.size();++i){} out.push_back(rr); pos=rr.end>rr.start?rr.end:rr.start+1;
    }
    // Above loop is intentionally replaced by a simpler deterministic search pass.
    out.clear();
    for(size_t start=0;start<=text.size();){auto caps=makeCaps(maxCapture(pattern.root)); caps.start[0]=(int)start; caps.set[0]=true; State st{start,caps};auto rs=matchNode(pattern.root,text,st);if(rs.empty()){++start;continue;}auto winner=rs.front(); winner.caps.start[0]=(int)start; winner.caps.end[0]=(int)winner.pos; winner.caps.set[0]=true; auto r=materialize(text,winner);out.push_back(r);start=r.end>r.start?r.end:r.start+1;}
    return out;
}
std::string replace(const Pattern& pattern,const std::string& text,const std::string& replacement){
    auto ms=matchAll(pattern,text);std::string out;size_t cur=0;auto names=captureNameMap(pattern.root);
    auto expand=[&](const MatchResult&m){std::string x;for(size_t i=0;i<replacement.size();){if(replacement[i]=='$'&&i+1<replacement.size()){if(replacement[i+1]=='{'){auto c=replacement.find('}',i+2);if(c!=std::string::npos){auto it=names.find(replacement.substr(i+2,c-i-2));if(it!=names.end()&&it->second<(int)m.groups.size())x+=m.groups[it->second];else x+=replacement.substr(i,c-i+1);i=c+1;continue;}}if(std::isdigit((unsigned char)replacement[i+1])){size_t j=i+1;int num=0;while(j<replacement.size()&&std::isdigit((unsigned char)replacement[j])){num=num*10+replacement[j]-'0';++j;}if(num<(int)m.groups.size())x+=m.groups[num];else x+=replacement.substr(i,j-i);i=j;continue;}}x+=replacement[i++];}return x;};
    for(auto&m:ms){if(m.start<cur)continue;out.append(text,cur,m.start-cur);out+=expand(m);cur=m.end;}out.append(text,cur,std::string::npos);return out;
}

} // namespace zl::regex_engine
