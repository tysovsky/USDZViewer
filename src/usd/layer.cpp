#include "layer.h"

#include "archive.h"

namespace usd {

std::string AssetLocation::Describe() const {
    if (archive) return archive->DisplayPath() + "[" + path + "]";
    return path;
}

namespace path {

std::string AppendChild(std::string_view parent, std::string_view name) {
    std::string r;
    if (parent.empty() || parent == "/") {
        r.reserve(name.size() + 1);
        r += '/';
    } else {
        r.reserve(parent.size() + name.size() + 1);
        r += parent;
        if (parent.back() != '}') r += '/';
    }
    r += name;
    return r;
}

std::string AppendProperty(std::string_view prim, std::string_view name) {
    std::string r;
    r.reserve(prim.size() + name.size() + 1);
    r += prim;
    r += '.';
    r += name;
    return r;
}

std::string AppendVariant(std::string_view prim, std::string_view set, std::string_view selection) {
    std::string r(prim);
    r += '{';
    r += set;
    r += '=';
    r += selection;
    r += '}';
    return r;
}

std::string ParentPrim(std::string_view p) {
    if (p.size() <= 1) return "/";
    if (p.back() == '}') {
        size_t open = p.rfind('{');
        if (open != std::string_view::npos) return std::string(p.substr(0, open));
    }
    size_t slash = p.rfind('/');
    size_t brace = p.rfind('}');
    if (brace != std::string_view::npos && (slash == std::string_view::npos || brace > slash))
        return std::string(p.substr(0, brace + 1));
    if (slash == 0 || slash == std::string_view::npos) return "/";
    return std::string(p.substr(0, slash));
}

std::string Name(std::string_view p) {
    size_t slash = p.rfind('/');
    size_t brace = p.rfind('}');
    size_t cut = std::string_view::npos;
    if (slash != std::string_view::npos) cut = slash;
    if (brace != std::string_view::npos && (cut == std::string_view::npos || brace > cut)) cut = brace;
    return std::string(cut == std::string_view::npos ? p : p.substr(cut + 1));
}

void Split(std::string_view p, std::string& prim, std::string& prop) {
    size_t start = 0;
    size_t slash = p.rfind('/');
    if (slash != std::string_view::npos) start = slash;
    size_t brace = p.rfind('}');
    if (brace != std::string_view::npos && brace > start) start = brace;
    size_t dot = p.find('.', start);
    if (dot == std::string_view::npos) {
        prim = std::string(p);
        prop.clear();
    } else {
        prim = std::string(p.substr(0, dot));
        prop = std::string(p.substr(dot + 1));
    }
}

bool HasPrefix(std::string_view p, std::string_view prefix) {
    if (prefix == "/") return !p.empty() && p[0] == '/';
    if (p.size() < prefix.size() || p.compare(0, prefix.size(), prefix) != 0) return false;
    if (p.size() == prefix.size()) return true;
    char c = p[prefix.size()];
    return c == '/' || c == '.' || c == '{' || c == '[';
}

std::string ReplacePrefix(std::string_view p, std::string_view oldPrefix, std::string_view newPrefix) {
    if (!HasPrefix(p, oldPrefix)) return std::string(p);
    std::string_view rest = p.substr(oldPrefix == "/" ? 1 : oldPrefix.size());
    if (oldPrefix == "/") {
        if (rest.empty()) return std::string(newPrefix);
        return AppendChild(newPrefix, rest);
    }
    if (newPrefix == "/" && !rest.empty() && rest[0] == '/') return std::string(rest);
    std::string r(newPrefix);
    r += rest;
    return r;
}

std::string StripVariants(std::string_view p) {
    std::string r;
    r.reserve(p.size());
    for (size_t i = 0; i < p.size(); ++i) {
        if (p[i] == '{') {
            size_t close = p.find('}', i);
            if (close == std::string_view::npos) break;
            i = close;
            if (i + 1 < p.size() && p[i + 1] != '.' && p[i + 1] != '/' && p[i + 1] != '{') r += '/';
            continue;
        }
        r += p[i];
    }
    return r;
}

std::string MakeAbsolute(std::string_view anchor, std::string_view p) {
    if (!p.empty() && p[0] == '/') return std::string(p);
    std::string base(anchor.empty() ? "/" : anchor);
    while (true) {
        if (p.substr(0, 3) == "../") {
            base = ParentPrim(base);
            p.remove_prefix(3);
        } else if (p == "..") {
            base = ParentPrim(base);
            p = {};
        } else if (p.substr(0, 2) == "./") {
            p.remove_prefix(2);
        } else {
            break;
        }
    }
    if (p.empty()) return base;
    if (p[0] == '.') return base + std::string(p);
    return AppendChild(base, p);
}

int Depth(std::string_view p) {
    if (p.size() <= 1) return 0;
    int d = 0;
    for (char c : p)
        if (c == '/' || c == '}') ++d;
    return d;
}

}  // namespace path
}  // namespace usd
