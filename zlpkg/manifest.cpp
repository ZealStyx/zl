#include "manifest.hpp"

#include <fstream>
#include <map>
#include <sstream>

namespace zlpkg {
namespace {

std::string trim(const std::string& s) {
    size_t start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return "";
    size_t end = s.find_last_not_of(" \t\r\n");
    return s.substr(start, end - start + 1);
}

// Strips a trailing '#'-comment, but only outside of a quoted string, so a
// URL fragment or similar occurring after a '#' inside quotes is untouched
// (not that either field is likely to contain one, but better to be precise).
std::string stripComment(const std::string& line) {
    bool inQuotes = false;
    for (size_t i = 0; i < line.size(); ++i) {
        if (line[i] == '"') inQuotes = !inQuotes;
        else if (line[i] == '#' && !inQuotes) return line.substr(0, i);
    }
    return line;
}

// Strips a leading/trailing pair of double quotes, if present.
std::string unquote(const std::string& s, const std::string& context) {
    std::string t = trim(s);
    if (t.size() < 2 || t.front() != '"' || t.back() != '"') {
        throw ManifestError(context + ": expected a quoted string, got '" + t + "'");
    }
    return t.substr(1, t.size() - 2);
}

// Splits "k1 = \"v1\", k2 = \"v2\"" (the inside of an inline table) into a
// map. No nested braces/tables are supported - not needed for this shape.
std::map<std::string, std::string> parseInlineTable(const std::string& body, const std::string& context) {
    std::map<std::string, std::string> fields;
    std::stringstream ss(body);
    std::string piece;
    while (std::getline(ss, piece, ',')) {
        piece = trim(piece);
        if (piece.empty()) continue;
        size_t eq = piece.find('=');
        if (eq == std::string::npos) {
            throw ManifestError(context + ": malformed inline table entry '" + piece + "'");
        }
        std::string key = trim(piece.substr(0, eq));
        std::string value = unquote(piece.substr(eq + 1), context + "." + key);
        fields[key] = value;
    }
    return fields;
}

} // namespace

Manifest loadManifest(const std::filesystem::path& manifestPath) {
    std::ifstream file(manifestPath);
    if (!file) {
        throw ManifestError("cannot open manifest '" + manifestPath.string() + "'");
    }

    Manifest manifest;
    manifest.manifestDir = std::filesystem::absolute(manifestPath).parent_path();

    std::string section;
    std::string rawLine;
    int lineNo = 0;
    bool haveName = false, haveVersion = false;

    while (std::getline(file, rawLine)) {
        ++lineNo;
        std::string line = trim(stripComment(rawLine));
        if (line.empty()) continue;

        auto err = [&](const std::string& msg) -> ManifestError {
            return ManifestError(manifestPath.string() + ":" + std::to_string(lineNo) + ": " + msg);
        };

        if (line.front() == '[' && line.back() == ']') {
            section = trim(line.substr(1, line.size() - 2));
            if (section != "package" && section != "dependencies") {
                throw err("unknown section '[" + section + "]' (only [package] and [dependencies] are supported)");
            }
            continue;
        }

        size_t eq = line.find('=');
        if (eq == std::string::npos) {
            throw err("expected 'key = value', got '" + line + "'");
        }
        std::string key = trim(line.substr(0, eq));
        std::string value = trim(line.substr(eq + 1));
        if (key.empty()) throw err("empty key before '='");

        if (section == "package") {
            std::string v = unquote(value, manifestPath.string() + ":" + std::to_string(lineNo));
            if (key == "name") { manifest.name = v; haveName = true; }
            else if (key == "version") { manifest.version = v; haveVersion = true; }
            // Unknown [package] keys are ignored rather than rejected, so
            // the format can grow (e.g. an "authors" field later) without
            // breaking older manifests being read by a newer zlpkg.
        } else if (section == "dependencies") {
            if (value.empty() || value.front() != '{' || value.back() != '}') {
                throw err("dependency '" + key +
                          "' must be an inline table - either { path = \"...\" } or "
                          "{ git = \"...\", ref = \"...\" } (a bare version string isn't supported: "
                          "there's no package registry yet to resolve a name/version pair against)");
            }
            auto fields = parseInlineTable(value.substr(1, value.size() - 2), manifestPath.string() + ":" + std::to_string(lineNo));

            Dependency dep;
            dep.name = key;
            if (auto v = fields.find("version"); v != fields.end()) dep.version = v->second;
            if (auto it = fields.find("path"); it != fields.end()) {
                dep.kind = Dependency::Kind::Path;
                dep.source = it->second;
                if (fields.count("git")) throw err("dependency '" + key + "' cannot have both 'path' and 'git'");
            } else if (auto it2 = fields.find("git"); it2 != fields.end()) {
                dep.kind = Dependency::Kind::Git;
                dep.source = it2->second;
                if (auto r = fields.find("ref"); r != fields.end()) dep.ref = r->second;
            } else {
                throw err("dependency '" + key + "' needs either 'path' or 'git'");
            }
            manifest.dependencies.push_back(std::move(dep));
        } else {
            throw err("'" + key + " = " + value + "' appears before any [package]/[dependencies] section");
        }
    }

    if (!haveName) throw ManifestError(manifestPath.string() + ": missing [package] name");
    if (!haveVersion) throw ManifestError(manifestPath.string() + ": missing [package] version");

    return manifest;
}

} // namespace zlpkg
