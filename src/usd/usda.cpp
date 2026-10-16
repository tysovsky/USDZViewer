// Recursive-descent parser for the .usda text format. Produces the same Layer/Spec/field layout that
// the crate reader produces, so composition does not care which format a layer came from.
#include "usda.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <stdexcept>

namespace usd {

namespace {

struct ParseError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

// Untyped parse tree for values. Numeric data (scalars, tuples, matrices and lists of them) is kept
// flattened in `nums` so large arrays do not allocate per element.
struct PV {
    enum Kind { None, Nums, String, Ident, Asset, Path, Tuple, List, Dict, Samples };
    Kind kind = None;
    std::vector<double> nums;
    int tuple = 1;      // Nums: components per list element
    bool list = false;  // Nums: came from a [...] list
    std::string str;
    std::vector<PV> items;           // Tuple/List items, Dict/Samples values
    std::vector<std::string> keys;   // Dict keys
    std::vector<std::string> types;  // Dict declared types
    std::vector<double> times;       // Samples times
};

struct TypeInfo {
    enum Cat { Unknown, Number, String, Token, Asset, Dictionary, Opaque } cat = Unknown;
    uint8_t tuple = 1;
    bool quat = false;
};

TypeInfo LookupType(std::string_view base) {
    TypeInfo t;
    auto num = [&](uint8_t n) {
        t.cat = TypeInfo::Number;
        t.tuple = n;
    };
    static const char* scalars[] = {"bool", "uchar", "int", "uint", "int64", "uint64", "half", "float", "double", "timecode"};
    for (const char* s : scalars)
        if (base == s) { num(1); return t; }
    if (base == "string" || base == "pathExpression") { t.cat = TypeInfo::String; return t; }
    if (base == "token") { t.cat = TypeInfo::Token; return t; }
    if (base == "asset") { t.cat = TypeInfo::Asset; return t; }
    if (base == "dictionary") { t.cat = TypeInfo::Dictionary; return t; }
    if (base == "opaque" || base == "group") { t.cat = TypeInfo::Opaque; return t; }
    if (base == "matrix2d") { num(4); return t; }
    if (base == "matrix3d") { num(9); return t; }
    if (base == "matrix4d" || base == "frame4d") { num(16); return t; }
    if (base == "quath" || base == "quatf" || base == "quatd") { num(4); t.quat = true; return t; }
    // Vector-like roles: int3, float2, point3f, normal3f, color4f, texCoord2f, vector3d, ...
    for (size_t i = 0; i < base.size(); ++i) {
        char c = base[i];
        if (c >= '2' && c <= '4') {
            std::string_view prefix = base.substr(0, i);
            std::string_view suffix = base.substr(i + 1);
            static const char* prefixes[] = {"int", "half", "float", "double", "point", "normal", "vector", "color", "texCoord"};
            bool known = false;
            for (const char* p : prefixes)
                if (prefix == p) known = true;
            if (known && (suffix.empty() || suffix == "h" || suffix == "f" || suffix == "d")) {
                num(uint8_t(c - '0'));
                return t;
            }
        }
    }
    return t;
}

bool IsIdentStart(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; }
bool IsIdentChar(char c) { return IsIdentStart(c) || (c >= '0' && c <= '9'); }

Value ToDict(const PV& pv);

Value Infer(const PV& pv) {
    using K = Value::Kind;
    switch (pv.kind) {
        case PV::None: return Value::Blocked();
        case PV::Nums:
            return Value::Numbers(pv.nums, uint8_t(pv.list ? pv.tuple : int(pv.nums.size())), pv.list);
        case PV::String: return Value::Text(K::String, pv.str);
        case PV::Ident:
            if (pv.str == "true") return Value::Scalar(1.0);
            if (pv.str == "false") return Value::Scalar(0.0);
            return Value::Text(K::Token, pv.str);
        case PV::Asset: return Value::Text(K::Asset, pv.str);
        case PV::Path: return Value::Text(K::Path, pv.str);
        case PV::Dict: return ToDict(pv);
        case PV::List: {
            K kind = K::String;
            std::vector<std::string> strs;
            for (auto& it : pv.items) {
                if (it.kind == PV::Asset) kind = K::Asset;
                else if (it.kind == PV::Path) kind = K::Path;
                else if (it.kind == PV::Ident) kind = K::Token;
                strs.push_back(it.str);
            }
            return Value::Texts(kind, std::move(strs));
        }
        default: return Value();
    }
}

Value ToValue(const PV& pv, std::string_view typeName) {
    using K = Value::Kind;
    bool isArray = typeName.size() > 2 && typeName.substr(typeName.size() - 2) == "[]";
    std::string_view base = isArray ? typeName.substr(0, typeName.size() - 2) : typeName;
    if (pv.kind == PV::None) return Value::Blocked();
    TypeInfo ti = LookupType(base);
    switch (ti.cat) {
        case TypeInfo::Number: {
            std::vector<double> nums;
            if (pv.kind == PV::Nums) nums = pv.nums;
            else if (pv.kind == PV::Ident) nums.push_back(pv.str == "true" ? 1.0 : 0.0);
            else if (pv.kind == PV::List) {
                for (auto& it : pv.items) {
                    if (it.kind == PV::Nums) nums.insert(nums.end(), it.nums.begin(), it.nums.end());
                    else if (it.kind == PV::Ident) nums.push_back(it.str == "true" ? 1.0 : 0.0);
                }
            }
            if (ti.quat) {  // text order is (real, i, j, k); store (i, j, k, real)
                for (size_t i = 0; i + 3 < nums.size(); i += 4) {
                    double w = nums[i];
                    nums[i] = nums[i + 1];
                    nums[i + 1] = nums[i + 2];
                    nums[i + 2] = nums[i + 3];
                    nums[i + 3] = w;
                }
            }
            return Value::Numbers(std::move(nums), ti.tuple, isArray);
        }
        case TypeInfo::String:
        case TypeInfo::Token:
        case TypeInfo::Asset: {
            K kind = ti.cat == TypeInfo::String ? K::String : ti.cat == TypeInfo::Token ? K::Token : K::Asset;
            std::vector<std::string> strs;
            if (pv.kind == PV::List) {
                for (auto& it : pv.items) strs.push_back(it.str);
            } else {
                strs.push_back(pv.str);
            }
            if (isArray) return Value::Texts(kind, std::move(strs));
            return Value::Text(kind, strs.empty() ? std::string() : strs[0]);
        }
        case TypeInfo::Dictionary: return ToDict(pv);
        case TypeInfo::Opaque: return Value();
        default: return Infer(pv);
    }
}

Value ToDict(const PV& pv) {
    auto d = std::make_shared<Dictionary>();
    if (pv.kind == PV::Dict) {
        for (size_t i = 0; i < pv.items.size(); ++i) {
            const std::string& type = pv.types[i];
            (*d)[pv.keys[i]] = type.empty() ? Infer(pv.items[i]) : ToValue(pv.items[i], type);
        }
    }
    Value v;
    v.kind = Value::Kind::Dictionary;
    v.dict = std::move(d);
    return v;
}

class Parser {
public:
    Parser(const char* text, size_t size, Layer& layer, std::vector<std::string>& warnings)
        : p_(text), end_(text + size), layer_(layer), warnings_(warnings) {}

    void ParseLayer() {
        // Header: "#usda 1.0"
        if (end_ - p_ < 5 || std::strncmp(p_, "#usda", 5) != 0) Fail("missing #usda header");
        const char* eol = p_;
        while (eol < end_ && *eol != '\n') ++eol;
        layer_.version = Trim(std::string(p_ + 5, eol));
        layer_.format = "usda";
        p_ = eol;
        Spec& root = layer_.GetOrCreateSpec("/", SpecType::PseudoRoot);
        if (Peek('(')) ParseMetadata(root, "/", true);
        while (true) {
            SkipWS();
            if (p_ >= end_) break;
            std::string word = ReadIdent();
            if (word == "def" || word == "over" || word == "class") {
                ParsePrim("/", word);
            } else if (word == "reorder") {
                ReadIdent();  // rootPrims
                Expect('=');
                ParseValue();
            } else {
                Fail("expected a prim definition, found '" + word + "'");
            }
        }
    }

    [[noreturn]] void Fail(const std::string& msg) const {
        throw ParseError("line " + std::to_string(line_) + ": " + msg);
    }

private:
    const char* p_;
    const char* end_;
    int line_ = 1;
    Layer& layer_;
    std::vector<std::string>& warnings_;

    static std::string Trim(std::string s) {
        size_t a = s.find_first_not_of(" \t\r");
        size_t b = s.find_last_not_of(" \t\r");
        return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
    }

    void SkipWS() {
        while (p_ < end_) {
            char c = *p_;
            if (c == '\n') {
                ++line_;
                ++p_;
            } else if (c == ' ' || c == '\t' || c == '\r') {
                ++p_;
            } else if (c == '#') {
                while (p_ < end_ && *p_ != '\n') ++p_;
            } else {
                break;
            }
        }
    }
    bool Peek(char c) {
        SkipWS();
        return p_ < end_ && *p_ == c;
    }
    bool Accept(char c) {
        if (Peek(c)) {
            ++p_;
            return true;
        }
        return false;
    }
    void Expect(char c) {
        if (!Accept(c)) Fail(std::string("expected '") + c + "'");
    }

    std::string PeekIdent() {
        SkipWS();
        const char* q = p_;
        if (q >= end_ || !IsIdentStart(*q)) return std::string();
        while (q < end_ && IsIdentChar(*q)) ++q;
        return std::string(p_, q);
    }
    std::string ReadIdent() {
        SkipWS();
        if (p_ >= end_ || !IsIdentStart(*p_)) Fail("expected an identifier");
        const char* q = p_;
        while (p_ < end_ && IsIdentChar(*p_)) ++p_;
        return std::string(q, p_);
    }
    // Identifiers joined by ':' (property names such as primvars:st:indices).
    std::string ReadNamespacedIdent() {
        std::string s = ReadIdent();
        while (p_ < end_ && *p_ == ':' && p_ + 1 < end_ && IsIdentStart(p_[1])) {
            ++p_;
            s += ':';
            const char* q = p_;
            while (p_ < end_ && IsIdentChar(*p_)) ++p_;
            s.append(q, p_);
        }
        return s;
    }

    std::string ReadString() {
        SkipWS();
        if (p_ >= end_ || (*p_ != '"' && *p_ != '\'')) Fail("expected a string");
        const char q = *p_;
        const bool triple = end_ - p_ >= 3 && p_[1] == q && p_[2] == q;
        p_ += triple ? 3 : 1;
        std::string s;
        while (true) {
            if (p_ >= end_) Fail("unterminated string");
            char c = *p_;
            if (triple) {
                if (c == q && end_ - p_ >= 3 && p_[1] == q && p_[2] == q) {
                    p_ += 3;
                    break;
                }
            } else if (c == q) {
                ++p_;
                break;
            } else if (c == '\n') {
                Fail("newline in string");
            }
            if (c == '\n') ++line_;
            if (c == '\\' && p_ + 1 < end_) {
                ++p_;
                char e = *p_++;
                switch (e) {
                    case 'n': s += '\n'; break;
                    case 't': s += '\t'; break;
                    case 'r': s += '\r'; break;
                    case 'a': s += '\a'; break;
                    case 'b': s += '\b'; break;
                    case 'f': s += '\f'; break;
                    case 'v': s += '\v'; break;
                    case 'x': {
                        int v = 0, n = 0;
                        while (n < 2 && p_ < end_ && std::isxdigit(static_cast<unsigned char>(*p_))) {
                            char h = *p_++;
                            v = v * 16 + (h <= '9' ? h - '0' : (h | 0x20) - 'a' + 10);
                            ++n;
                        }
                        s += char(v);
                        break;
                    }
                    default:
                        if (e >= '0' && e <= '7') {
                            int v = e - '0', n = 1;
                            while (n < 3 && p_ < end_ && *p_ >= '0' && *p_ <= '7') {
                                v = v * 8 + (*p_++ - '0');
                                ++n;
                            }
                            s += char(v);
                        } else {
                            if (e == '\n') ++line_;
                            s += e;
                        }
                }
                continue;
            }
            s += c;
            ++p_;
        }
        return s;
    }

    std::string ReadAsset() {
        SkipWS();
        if (end_ - p_ >= 3 && std::strncmp(p_, "@@@", 3) == 0) {
            p_ += 3;
            std::string s;
            while (true) {
                if (p_ >= end_) Fail("unterminated asset path");
                if (end_ - p_ >= 4 && std::strncmp(p_, "\\@@@", 4) == 0) {
                    s += "@@@";
                    p_ += 4;
                } else if (end_ - p_ >= 3 && std::strncmp(p_, "@@@", 3) == 0) {
                    p_ += 3;
                    return s;
                } else {
                    s += *p_++;
                }
            }
        }
        Expect('@');
        const char* q = p_;
        while (p_ < end_ && *p_ != '@' && *p_ != '\n') ++p_;
        if (p_ >= end_ || *p_ != '@') Fail("unterminated asset path");
        std::string s(q, p_);
        ++p_;
        return s;
    }

    std::string ReadPathLiteral() {
        Expect('<');
        const char* q = p_;
        while (p_ < end_ && *p_ != '>' && *p_ != '\n') ++p_;
        if (p_ >= end_ || *p_ != '>') Fail("unterminated path");
        std::string s(q, p_);
        ++p_;
        return s;
    }

    bool AtNumber() {
        SkipWS();
        if (p_ >= end_) return false;
        char c = *p_;
        if ((c >= '0' && c <= '9') || c == '.') return true;
        if ((c == '-' || c == '+') && p_ + 1 < end_) {
            char d = p_[1];
            return (d >= '0' && d <= '9') || d == '.' || d == 'i' || d == 'n';
        }
        if (c == 'i' && end_ - p_ >= 3 && std::strncmp(p_, "inf", 3) == 0 && (end_ - p_ == 3 || !IsIdentChar(p_[3])))
            return true;
        if (c == 'n' && end_ - p_ >= 3 && std::strncmp(p_, "nan", 3) == 0 && (end_ - p_ == 3 || !IsIdentChar(p_[3])))
            return true;
        return false;
    }

    double ReadNumber() {
        SkipWS();
        bool neg = false;
        const char* start = p_;
        if (p_ < end_ && (*p_ == '-' || *p_ == '+')) {
            neg = *p_ == '-';
            ++p_;
        }
        if (end_ - p_ >= 3 && std::strncmp(p_, "inf", 3) == 0) {
            p_ += 3;
            return neg ? -std::numeric_limits<double>::infinity() : std::numeric_limits<double>::infinity();
        }
        if (end_ - p_ >= 3 && std::strncmp(p_, "nan", 3) == 0) {
            p_ += 3;
            return std::numeric_limits<double>::quiet_NaN();
        }
        p_ = start;
        char buf[64];
        size_t n = 0;
        while (p_ < end_ && n < sizeof(buf) - 1) {
            char c = *p_;
            if ((c >= '0' && c <= '9') || c == '.' || c == '-' || c == '+' || c == 'e' || c == 'E') {
                buf[n++] = c;
                ++p_;
            } else {
                break;
            }
        }
        buf[n] = '\0';
        char* endp = nullptr;
        double v = std::strtod(buf, &endp);
        if (endp == buf) Fail("invalid number");
        return v;
    }

    // Fast path for (1, 2, 3) and ((1,0),(0,1)) style numeric tuples. Returns false (position
    // unspecified) if a non-numeric element is found.
    bool ReadNumTuple(std::vector<double>& out) {
        if (!Accept('(')) return false;
        while (true) {
            if (Accept(')')) return true;
            if (AtNumber()) {
                out.push_back(ReadNumber());
            } else if (Peek('(')) {
                if (!ReadNumTuple(out)) return false;
            } else {
                return false;
            }
            Accept(',');
        }
    }

    PV ParseList() {
        // Try the flat numeric fast path first; fall back to the generic parse.
        const char* save = p_;
        const int saveLine = line_;
        {
            PV r;
            r.kind = PV::Nums;
            r.list = true;
            int tuple = -1;
            bool ok = true;
            Expect('[');
            while (true) {
                if (Accept(']')) break;
                size_t before = r.nums.size();
                if (AtNumber()) {
                    r.nums.push_back(ReadNumber());
                } else if (Peek('(')) {
                    if (!ReadNumTuple(r.nums)) {
                        ok = false;
                        break;
                    }
                } else {
                    ok = false;
                    break;
                }
                int count = int(r.nums.size() - before);
                if (tuple == -1) tuple = count;
                else if (tuple != count) {
                    ok = false;
                    break;
                }
                Accept(',');
            }
            if (ok && tuple > 0) {
                r.tuple = tuple;
                return r;
            }
            if (ok) {  // empty list
                PV e;
                e.kind = PV::List;
                return e;
            }
        }
        p_ = save;
        line_ = saveLine;
        PV r;
        r.kind = PV::List;
        Expect('[');
        while (true) {
            if (Accept(']')) break;
            r.items.push_back(ParseValue());
            Accept(',');
        }
        return r;
    }

    PV ParseValue() {
        SkipWS();
        if (p_ >= end_) Fail("unexpected end of file");
        PV r;
        char c = *p_;
        if (AtNumber()) {
            r.kind = PV::Nums;
            r.nums.push_back(ReadNumber());
            return r;
        }
        switch (c) {
            case '[': return ParseList();
            case '(': {
                const char* save = p_;
                const int saveLine = line_;
                if (ReadNumTuple(r.nums)) {
                    r.kind = PV::Nums;
                    r.tuple = int(r.nums.size());
                    return r;
                }
                p_ = save;
                line_ = saveLine;
                r.nums.clear();
                r.kind = PV::Tuple;
                Expect('(');
                while (true) {
                    if (Accept(')')) break;
                    r.items.push_back(ParseValue());
                    Accept(',');
                }
                return r;
            }
            case '{': {
                ++p_;
                SkipWS();
                if (AtNumber()) {
                    r.kind = PV::Samples;
                    while (true) {
                        if (Accept('}')) break;
                        r.times.push_back(ReadNumber());
                        Expect(':');
                        r.items.push_back(ParseValue());
                        Accept(',');
                    }
                    return r;
                }
                r.kind = PV::Dict;
                while (true) {
                    if (Accept('}')) break;
                    std::string type, key;
                    if (Peek('"') || Peek('\'')) {
                        key = ReadString();
                    } else {
                        type = ReadIdent();
                        if (Accept('[')) {
                            Expect(']');
                            type += "[]";
                        }
                        if (Peek('"') || Peek('\'')) key = ReadString();
                        else key = ReadNamespacedIdent();
                    }
                    Expect('=');
                    r.keys.push_back(key);
                    r.types.push_back(type);
                    r.items.push_back(ParseValue());
                    Accept(';');
                    Accept(',');
                }
                return r;
            }
            case '"':
            case '\'':
                r.kind = PV::String;
                r.str = ReadString();
                return r;
            case '@':
                r.kind = PV::Asset;
                r.str = ReadAsset();
                return r;
            case '<':
                r.kind = PV::Path;
                r.str = ReadPathLiteral();
                return r;
            default:
                break;
        }
        if (IsIdentStart(c)) {
            std::string id = ReadNamespacedIdent();
            if (id == "None") return r;
            r.kind = PV::Ident;
            r.str = std::move(id);
            if (r.str == "true" || r.str == "false") r.nums.push_back(r.str == "true" ? 1.0 : 0.0);
            return r;
        }
        Fail(std::string("unexpected character '") + c + "'");
    }

    // ---- references / sublayers ----

    Reference ParseReferenceItem(const std::string& anchor) {
        Reference ref;
        SkipWS();
        if (Peek('@')) {
            ref.assetPath = ReadAsset();
            if (Peek('<')) ref.primPath = path::MakeAbsolute(anchor, ReadPathLiteral());
        } else if (Peek('<')) {
            ref.primPath = path::MakeAbsolute(anchor, ReadPathLiteral());
        } else {
            Fail("expected an asset path or prim path");
        }
        if (Peek('(')) {
            ++p_;
            while (true) {
                if (Accept(')')) break;
                std::string key = ReadIdent();
                Expect('=');
                PV v = ParseValue();
                if (key == "offset" && v.kind == PV::Nums && !v.nums.empty()) ref.layerOffset.offset = v.nums[0];
                if (key == "scale" && v.kind == PV::Nums && !v.nums.empty()) ref.layerOffset.scale = v.nums[0];
                Accept(';');
                Accept(',');
            }
        }
        return ref;
    }

    std::vector<Reference> ParseReferenceList(const std::string& anchor) {
        std::vector<Reference> refs;
        if (PeekIdent() == "None") {
            ReadIdent();
            return refs;
        }
        if (Accept('[')) {
            while (true) {
                if (Accept(']')) break;
                refs.push_back(ParseReferenceItem(anchor));
                Accept(',');
            }
        } else {
            refs.push_back(ParseReferenceItem(anchor));
        }
        return refs;
    }

    // Applies a list-op keyword when storing a flattened list.
    template <class T>
    static void MergeList(std::vector<T>& existing, std::vector<T> items, const std::string& op) {
        if (op == "delete" || op == "reorder") return;
        if (op.empty()) {
            existing = std::move(items);
        } else if (op == "prepend") {
            items.insert(items.end(), existing.begin(), existing.end());
            existing = std::move(items);
        } else {
            existing.insert(existing.end(), items.begin(), items.end());
        }
    }

    static void MergeTexts(Spec& spec, const char* field, Value::Kind kind, std::vector<std::string> items,
                           const std::string& op) {
        Value& v = spec.Set(field);
        if (v.kind != kind) {
            v = Value();
            v.kind = kind;
        }
        v.isArray = true;
        MergeList(v.strs, std::move(items), op);
    }

    static std::vector<std::string> CollectPaths(const PV& pv, const std::string& anchor) {
        std::vector<std::string> out;
        if (pv.kind == PV::Path) out.push_back(path::MakeAbsolute(anchor, pv.str));
        else if (pv.kind == PV::List)
            for (auto& it : pv.items)
                if (it.kind == PV::Path) out.push_back(path::MakeAbsolute(anchor, it.str));
        return out;
    }

    static std::vector<std::string> CollectStrings(const PV& pv) {
        std::vector<std::string> out;
        if (pv.kind == PV::String || pv.kind == PV::Ident) out.push_back(pv.str);
        else if (pv.kind == PV::List)
            for (auto& it : pv.items) out.push_back(it.str);
        return out;
    }

    // Parses "( key = value ... )". `anchor` is the prim path relative paths resolve against.
    void ParseMetadata(Spec& spec, const std::string& anchor, bool isLayer) {
        Expect('(');
        while (true) {
            if (Accept(')')) break;
            if (Peek('"') || Peek('\'')) {
                spec.Set("documentation") = Value::Text(Value::Kind::String, ReadString());
                continue;
            }
            std::string op;
            std::string key = ReadNamespacedIdent();
            if (key == "prepend" || key == "append" || key == "add" || key == "delete" || key == "reorder") {
                op = key;
                key = ReadNamespacedIdent();
            }
            Expect('=');
            if (key == "references" || key == "payload") {
                std::vector<Reference> refs = ParseReferenceList(anchor);
                Value& v = spec.Set(key);
                std::vector<Reference> existing;
                if (v.kind == Value::Kind::References && v.refs) existing = *v.refs;
                MergeList(existing, std::move(refs), op);
                v = Value();
                v.kind = Value::Kind::References;
                v.isArray = true;
                v.refs = std::make_shared<std::vector<Reference>>(std::move(existing));
            } else if (isLayer && key == "subLayers") {
                std::vector<Reference> subs = ParseReferenceList(anchor);
                std::vector<std::string> paths;
                std::vector<double> offsets;
                for (auto& s : subs) {
                    paths.push_back(s.assetPath);
                    offsets.push_back(s.layerOffset.offset);
                    offsets.push_back(s.layerOffset.scale);
                }
                spec.Set("subLayers") = Value::Texts(Value::Kind::Asset, std::move(paths));
                spec.Set("subLayerOffsets") = Value::Numbers(std::move(offsets), 2, true);
            } else {
                PV pv = ParseValue();
                if (key == "inherits" || key == "specializes") {
                    // Sdf field names: "inheritPaths" and "specializes".
                    MergeTexts(spec, key == "inherits" ? "inheritPaths" : "specializes", Value::Kind::Path,
                               CollectPaths(pv, anchor), op);
                } else if (key == "variants") {
                    Value& v = spec.Set("variantSelection");
                    Value d = ToDict(pv);
                    if (v.kind == Value::Kind::Dictionary && v.dict) {
                        auto merged = std::make_shared<Dictionary>(*v.dict);
                        for (auto& kv : *d.dict) (*merged)[kv.first] = kv.second;
                        d.dict = merged;
                    }
                    v = std::move(d);
                } else if (key == "variantSets") {
                    MergeTexts(spec, "variantSetNames", Value::Kind::String, CollectStrings(pv), op);
                } else if (key == "apiSchemas") {
                    MergeTexts(spec, "apiSchemas", Value::Kind::Token, CollectStrings(pv), op);
                } else if (op != "delete" && op != "reorder") {
                    spec.Set(key == "doc" ? std::string("documentation") : key) = Infer(pv);
                }
            }
            Accept(';');
        }
    }

    static void AddName(Spec& spec, const char* field, const std::string& name) {
        Value& v = spec.Set(field);
        if (v.kind != Value::Kind::Token) {
            v = Value();
            v.kind = Value::Kind::Token;
            v.isArray = true;
        }
        for (auto& s : v.strs)
            if (s == name) return;
        v.strs.push_back(name);
    }

    void ParsePrim(const std::string& parentPath, const std::string& specifier) {
        SkipWS();
        std::string typeName;
        if (!Peek('"') && !Peek('\'')) {
            typeName = ReadIdent();
            while (p_ < end_ && *p_ == '.' && p_ + 1 < end_ && IsIdentStart(p_[1])) {  // namespaced type
                ++p_;
                typeName += '.';
                typeName += ReadIdent();
            }
        }
        std::string name = ReadString();
        std::string primPath = path::AppendChild(parentPath, name);
        Spec& parent = layer_.GetOrCreateSpec(parentPath, parentPath == "/" ? SpecType::PseudoRoot : SpecType::Prim);
        AddName(parent, "primChildren", name);
        Spec& spec = layer_.GetOrCreateSpec(primPath, SpecType::Prim);
        spec.Set("specifier") = Value::Scalar(specifier == "def" ? 0.0 : specifier == "over" ? 1.0 : 2.0);
        if (!typeName.empty()) spec.Set("typeName") = Value::Text(Value::Kind::Token, typeName);
        const std::string anchor = path::StripVariants(primPath);
        if (Peek('(')) ParseMetadata(spec, anchor, false);
        Expect('{');
        ParsePrimBody(primPath);
        Expect('}');
    }

    void ParsePrimBody(const std::string& primPath) {
        while (true) {
            SkipWS();
            if (p_ >= end_) Fail("unexpected end of file inside prim");
            if (*p_ == '}') return;
            if (*p_ == ';') {
                ++p_;
                continue;
            }
            std::string word = PeekIdent();
            if (word == "def" || word == "over" || word == "class") {
                ReadIdent();
                ParsePrim(primPath, word);
            } else if (word == "variantSet") {
                ReadIdent();
                ParseVariantSet(primPath);
            } else if (word == "reorder") {
                const char* save = p_;
                const int saveLine = line_;
                ReadIdent();
                std::string what = PeekIdent();
                if (what == "nameChildren" || what == "properties") {
                    ReadIdent();
                    Expect('=');
                    ParseValue();
                } else {
                    p_ = save;
                    line_ = saveLine;
                    ParseProperty(primPath);
                }
            } else if (word.empty()) {
                Fail(std::string("unexpected character '") + *p_ + "' in prim body");
            } else {
                ParseProperty(primPath);
            }
        }
    }

    void ParseVariantSet(const std::string& primPath) {
        std::string setName = ReadString();
        Expect('=');
        Expect('{');
        Spec& prim = layer_.GetOrCreateSpec(primPath, SpecType::Prim);
        {
            Value& v = prim.Set("variantSetChildren");
            if (v.kind != Value::Kind::Token) {
                v = Value();
                v.kind = Value::Kind::Token;
                v.isArray = true;
            }
            bool found = false;
            for (auto& s : v.strs) found |= s == setName;
            if (!found) v.strs.push_back(setName);
        }
        Spec& setSpec = layer_.GetOrCreateSpec(path::AppendVariant(primPath, setName, ""), SpecType::VariantSet);
        while (true) {
            if (Accept('}')) break;
            std::string variant = ReadString();
            AddName(setSpec, "variantChildren", variant);
            std::string vpath = path::AppendVariant(primPath, setName, variant);
            Spec& vspec = layer_.GetOrCreateSpec(vpath, SpecType::Variant);
            if (Peek('(')) ParseMetadata(vspec, path::StripVariants(primPath), false);
            Expect('{');
            ParsePrimBody(vpath);
            Expect('}');
        }
    }

    void ParseProperty(const std::string& primPath) {
        std::string op;
        std::string word = PeekIdent();
        if (word == "prepend" || word == "append" || word == "add" || word == "delete" || word == "reorder") {
            op = ReadIdent();
            word = PeekIdent();
        }
        bool custom = false;
        if (word == "custom") {
            ReadIdent();
            custom = true;
            word = PeekIdent();
        }
        bool uniform = false;
        if (word == "uniform" || word == "config") {
            ReadIdent();
            uniform = true;
            word = PeekIdent();
        } else if (word == "varying") {
            ReadIdent();
            word = PeekIdent();
        }
        const std::string anchor = path::StripVariants(primPath);
        Spec& prim = layer_.GetOrCreateSpec(primPath, SpecType::Prim);

        if (word == "rel") {
            ReadIdent();
            std::string name = ReadNamespacedIdent();
            if (p_ < end_ && *p_ == '.') {  // e.g. rel foo.default = </x>
                ++p_;
                ReadIdent();
            }
            AddName(prim, "properties", name);
            Spec& spec = layer_.GetOrCreateSpec(path::AppendProperty(primPath, name), SpecType::Relationship);
            if (custom) spec.Set("custom") = Value::Scalar(1.0);
            if (Accept('=')) {
                PV pv = ParseValue();
                MergeTexts(spec, "targetPaths", Value::Kind::Path, CollectPaths(pv, anchor), op);
            }
            if (Peek('(')) ParseMetadata(spec, anchor, false);
            return;
        }

        std::string typeName = ReadIdent();
        if (Accept('[')) {
            Expect(']');
            typeName += "[]";
        }
        std::string name = ReadNamespacedIdent();
        std::string suffix;
        if (p_ < end_ && *p_ == '.') {
            ++p_;
            suffix = ReadIdent();
        }
        AddName(prim, "properties", name);
        Spec& spec = layer_.GetOrCreateSpec(path::AppendProperty(primPath, name), SpecType::Attribute);
        spec.Set("typeName") = Value::Text(Value::Kind::Token, typeName);
        if (custom) spec.Set("custom") = Value::Scalar(1.0);
        if (uniform) spec.Set("variability") = Value::Scalar(1.0);

        if (suffix == "connect") {
            if (Accept('=')) {
                PV pv = ParseValue();
                MergeTexts(spec, "connectionPaths", Value::Kind::Path, CollectPaths(pv, anchor), op);
            }
        } else if (suffix == "timeSamples") {
            Expect('=');
            PV pv = ParseValue();
            auto samples = std::make_shared<std::vector<TimeSample>>();
            if (pv.kind == PV::Samples) {
                for (size_t i = 0; i < pv.times.size(); ++i) samples->push_back({pv.times[i], ToValue(pv.items[i], typeName)});
                std::stable_sort(samples->begin(), samples->end(),
                                 [](const TimeSample& a, const TimeSample& b) { return a.time < b.time; });
            }
            Value v;
            v.kind = Value::Kind::TimeSamples;
            v.samples = std::move(samples);
            spec.Set("timeSamples") = std::move(v);
        } else if (suffix == "spline") {
            Expect('=');
            ParseValue();
            if (warnings_.size() < 200) warnings_.push_back("spline animation on '" + name + "' is not supported");
        } else if (suffix.empty() || suffix == "default") {
            if (Accept('=')) {
                PV pv = ParseValue();
                if (op != "delete") spec.Set("default") = ToValue(pv, typeName);
            }
        } else {
            Fail("unknown property suffix '." + suffix + "'");
        }
        if (Peek('(')) ParseMetadata(spec, anchor, false);
    }
};

}  // namespace

bool IsUsdaFile(const uint8_t* data, size_t size) {
    return size >= 5 && std::memcmp(data, "#usda", 5) == 0;
}

bool ReadUsda(const char* text, size_t size, Layer& layer, std::string& err, std::vector<std::string>& warnings) {
    try {
        Parser parser(text, size, layer, warnings);
        parser.ParseLayer();
        return true;
    } catch (const std::exception& e) {
        err = std::string("invalid USDA file: ") + e.what();
        return false;
    }
}

}  // namespace usd
