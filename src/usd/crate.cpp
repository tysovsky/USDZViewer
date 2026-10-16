// USDC ("crate") reader. Follows the layout written by OpenUSD's crateFile.cpp:
//   bootstrap header -> table of contents -> TOKENS, STRINGS, FIELDS, FIELDSETS, PATHS, SPECS sections.
// Each spec references a field set; each field is a (token, ValueRep) pair. A ValueRep packs a type id,
// array/inline/compressed flags and a 48-bit payload that is either the value itself or a file offset.
#include "crate.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdexcept>
#include <type_traits>

namespace usd {

namespace {

struct CrateError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

// ---- LZ4 block format + TfFastCompression framing ---------------------------------------------

size_t Lz4Decompress(const uint8_t* src, size_t srcLen, uint8_t* dst, size_t dstCap) {
    const uint8_t* ip = src;
    const uint8_t* const iend = src + srcLen;
    uint8_t* op = dst;
    uint8_t* const oend = dst + dstCap;
    while (ip < iend) {
        const unsigned token = *ip++;
        size_t lit = token >> 4;
        if (lit == 15) {
            unsigned b;
            do {
                if (ip >= iend) throw CrateError("lz4: truncated literal length");
                b = *ip++;
                lit += b;
            } while (b == 255);
        }
        if (lit > size_t(iend - ip) || lit > size_t(oend - op)) throw CrateError("lz4: literal overrun");
        std::memcpy(op, ip, lit);
        op += lit;
        ip += lit;
        if (ip >= iend) break;  // the last sequence has literals only
        if (iend - ip < 2) throw CrateError("lz4: truncated match offset");
        const size_t offset = size_t(ip[0]) | (size_t(ip[1]) << 8);
        ip += 2;
        if (offset == 0 || offset > size_t(op - dst)) throw CrateError("lz4: invalid match offset");
        size_t mlen = token & 15;
        if (mlen == 15) {
            unsigned b;
            do {
                if (ip >= iend) throw CrateError("lz4: truncated match length");
                b = *ip++;
                mlen += b;
            } while (b == 255);
        }
        mlen += 4;
        if (mlen > size_t(oend - op)) throw CrateError("lz4: match overrun");
        const uint8_t* match = op - offset;
        for (size_t i = 0; i < mlen; ++i) op[i] = match[i];  // regions may overlap
        op += mlen;
    }
    return size_t(op - dst);
}

size_t FastDecompress(const uint8_t* src, size_t srcLen, uint8_t* dst, size_t dstCap) {
    if (srcLen == 0) return 0;
    const int nChunks = src[0];
    if (nChunks == 0) return Lz4Decompress(src + 1, srcLen - 1, dst, dstCap);
    size_t total = 0, pos = 1;
    for (int i = 0; i < nChunks; ++i) {
        if (pos + 4 > srcLen) throw CrateError("compressed chunk header out of range");
        int32_t chunk;
        std::memcpy(&chunk, src + pos, 4);
        pos += 4;
        if (chunk < 0 || pos + size_t(chunk) > srcLen) throw CrateError("compressed chunk out of range");
        total += Lz4Decompress(src + pos, size_t(chunk), dst + total, dstCap - total);
        pos += size_t(chunk);
    }
    return total;
}

// ---- Usd_IntegerCompression decoding ---------------------------------------------------------
// Encoded form: common delta, 2-bit codes (4 per byte), then variable-width deltas.

template <class Int>
void DecodeInts(const uint8_t* data, size_t size, Int* out, size_t n) {
    using SInt = std::make_signed_t<Int>;
    using UInt = std::make_unsigned_t<Int>;
    using Small = std::conditional_t<sizeof(Int) == 4, int8_t, int16_t>;
    using Medium = std::conditional_t<sizeof(Int) == 4, int16_t, int32_t>;
    if (n == 0) return;
    const size_t codeBytes = (n * 2 + 7) / 8;
    if (size < sizeof(SInt) + codeBytes) throw CrateError("compressed integers truncated");
    SInt common;
    std::memcpy(&common, data, sizeof(SInt));
    const uint8_t* codes = data + sizeof(SInt);
    const uint8_t* p = codes + codeBytes;
    const uint8_t* end = data + size;
    UInt prev = 0;
    auto take = [&](auto tag) {
        using T = decltype(tag);
        if (size_t(end - p) < sizeof(T)) throw CrateError("compressed integers truncated");
        T v;
        std::memcpy(&v, p, sizeof(T));
        p += sizeof(T);
        return UInt(SInt(v));
    };
    for (size_t i = 0; i < n; ++i) {
        switch ((codes[i >> 2] >> ((i & 3) * 2)) & 3) {
            case 0: prev += UInt(common); break;
            case 1: prev += take(Small()); break;
            case 2: prev += take(Medium()); break;
            default: prev += take(SInt()); break;
        }
        out[i] = Int(prev);
    }
}

// ---- crate value types (crateDataTypes.h) -----------------------------------------------------

enum CrateType {
    T_Bool = 1, T_UChar, T_Int, T_UInt, T_Int64, T_UInt64, T_Half, T_Float, T_Double,
    T_String, T_Token, T_AssetPath, T_Matrix2d, T_Matrix3d, T_Matrix4d, T_Quatd, T_Quatf, T_Quath,
    T_Vec2d, T_Vec2f, T_Vec2h, T_Vec2i, T_Vec3d, T_Vec3f, T_Vec3h, T_Vec3i, T_Vec4d, T_Vec4f, T_Vec4h, T_Vec4i,
    T_Dictionary, T_TokenListOp, T_StringListOp, T_PathListOp, T_ReferenceListOp, T_IntListOp,
    T_Int64ListOp, T_UIntListOp, T_UInt64ListOp, T_PathVector, T_TokenVector, T_Specifier,
    T_Permission, T_Variability, T_VariantSelectionMap, T_TimeSamples, T_Payload, T_DoubleVector,
    T_LayerOffsetVector, T_StringVector, T_ValueBlock, T_Value, T_UnregisteredValue,
    T_UnregisteredValueListOp, T_PayloadListOp, T_TimeCode, T_PathExpression, T_Relocates, T_Spline,
    T_AnimationBlock,
};

enum Comp : uint8_t { C_Bool, C_U8, C_I32, C_U32, C_I64, C_U64, C_F16, C_F32, C_F64 };

struct NumInfo {
    uint8_t tuple;
    Comp comp;
};

bool GetNumInfo(int type, NumInfo& ni) {
    switch (type) {
        case T_Bool: ni = {1, C_Bool}; return true;
        case T_UChar: ni = {1, C_U8}; return true;
        case T_Int: ni = {1, C_I32}; return true;
        case T_UInt: ni = {1, C_U32}; return true;
        case T_Int64: ni = {1, C_I64}; return true;
        case T_UInt64: ni = {1, C_U64}; return true;
        case T_Half: ni = {1, C_F16}; return true;
        case T_Float: ni = {1, C_F32}; return true;
        case T_Double: ni = {1, C_F64}; return true;
        case T_TimeCode: ni = {1, C_F64}; return true;
        case T_Matrix2d: ni = {4, C_F64}; return true;
        case T_Matrix3d: ni = {9, C_F64}; return true;
        case T_Matrix4d: ni = {16, C_F64}; return true;
        case T_Quatd: ni = {4, C_F64}; return true;
        case T_Quatf: ni = {4, C_F32}; return true;
        case T_Quath: ni = {4, C_F16}; return true;
        case T_Vec2d: ni = {2, C_F64}; return true;
        case T_Vec2f: ni = {2, C_F32}; return true;
        case T_Vec2h: ni = {2, C_F16}; return true;
        case T_Vec2i: ni = {2, C_I32}; return true;
        case T_Vec3d: ni = {3, C_F64}; return true;
        case T_Vec3f: ni = {3, C_F32}; return true;
        case T_Vec3h: ni = {3, C_F16}; return true;
        case T_Vec3i: ni = {3, C_I32}; return true;
        case T_Vec4d: ni = {4, C_F64}; return true;
        case T_Vec4f: ni = {4, C_F32}; return true;
        case T_Vec4h: ni = {4, C_F16}; return true;
        case T_Vec4i: ni = {4, C_I32}; return true;
        default: return false;
    }
}

size_t CompSize(Comp c) {
    switch (c) {
        case C_Bool: case C_U8: return 1;
        case C_F16: return 2;
        case C_I32: case C_U32: case C_F32: return 4;
        default: return 8;
    }
}

double CompToDouble(const uint8_t* p, Comp c) {
    switch (c) {
        case C_Bool: return *p ? 1.0 : 0.0;
        case C_U8: return double(*p);
        case C_I32: { int32_t v; std::memcpy(&v, p, 4); return double(v); }
        case C_U32: { uint32_t v; std::memcpy(&v, p, 4); return double(v); }
        case C_I64: { int64_t v; std::memcpy(&v, p, 8); return double(v); }
        case C_U64: { uint64_t v; std::memcpy(&v, p, 8); return double(v); }
        case C_F16: { uint16_t v; std::memcpy(&v, p, 2); return double(HalfToFloat(v)); }
        case C_F32: { float v; std::memcpy(&v, p, 4); return double(v); }
        default: { double v; std::memcpy(&v, p, 8); return v; }
    }
}

struct Stream {
    const uint8_t* data;
    size_t size;
    size_t pos = 0;

    void Seek(uint64_t p) {
        if (p > size) throw CrateError("seek out of range");
        pos = size_t(p);
    }
    const uint8_t* Take(uint64_t n) {
        if (n > size - pos) throw CrateError("read out of range");
        const uint8_t* p = data + pos;
        pos += size_t(n);
        return p;
    }
    template <class T>
    T Read() {
        T v;
        std::memcpy(&v, Take(sizeof(T)), sizeof(T));
        return v;
    }
};

constexpr uint64_t kArrayBit = 1ull << 63;
constexpr uint64_t kInlineBit = 1ull << 62;
constexpr uint64_t kCompressedBit = 1ull << 61;
constexpr uint64_t kPayloadMask = (1ull << 48) - 1;
constexpr size_t kMinCompressedArraySize = 16;

class CrateReader {
public:
    CrateReader(const uint8_t* d, size_t n, std::vector<std::string>& warnings) : s_{d, n}, warnings_(warnings) {}

    void Read(Layer& layer) {
        if (s_.size < 88) throw CrateError("file too small");
        const uint8_t* boot = s_.Take(88);
        if (std::memcmp(boot, "PXR-USDC", 8) != 0) throw CrateError("missing PXR-USDC header");
        ver_ = (int(boot[8]) << 16) | (int(boot[9]) << 8) | int(boot[10]);
        layer.version = std::to_string(boot[8]) + "." + std::to_string(boot[9]) + "." + std::to_string(boot[10]);
        if (ver_ < Ver(0, 4, 0))
            throw CrateError("crate version " + layer.version + " is too old (0.4.0 or newer is supported)");
        int64_t tocOffset;
        std::memcpy(&tocOffset, boot + 16, 8);
        s_.Seek(uint64_t(tocOffset));
        const uint64_t nSections = s_.Read<uint64_t>();
        struct Sec {
            uint64_t start = 0, size = 0;
            bool found = false;
        } tokens, strings, fields, fieldSets, paths, specs;
        for (uint64_t i = 0; i < nSections; ++i) {
            const uint8_t* rec = s_.Take(32);
            char name[17] = {};
            std::memcpy(name, rec, 16);
            Sec sec;
            std::memcpy(&sec.start, rec + 16, 8);
            std::memcpy(&sec.size, rec + 24, 8);
            sec.found = true;
            std::string_view n(name);
            if (n == "TOKENS") tokens = sec;
            else if (n == "STRINGS") strings = sec;
            else if (n == "FIELDS") fields = sec;
            else if (n == "FIELDSETS") fieldSets = sec;
            else if (n == "PATHS") paths = sec;
            else if (n == "SPECS") specs = sec;
        }
        if (!tokens.found || !fields.found || !fieldSets.found || !paths.found || !specs.found)
            throw CrateError("missing required sections");

        ReadTokens(tokens.start);
        if (strings.found) ReadStrings(strings.start);
        ReadFields(fields.start);
        ReadFieldSets(fieldSets.start);
        ReadPaths(paths.start);
        ReadSpecs(specs.start);

        layer.format = "usdc";
        for (const SpecRec& rec : specs_) {
            if (rec.path >= paths_.size()) continue;
            const std::string& p = paths_[rec.path];
            Spec& spec = layer.specs[p];
            spec.type = static_cast<SpecType>(rec.type);
            for (size_t i = rec.fieldSet; i < fieldSets_.size() && fieldSets_[i] != ~0u; ++i) {
                const uint32_t fi = fieldSets_[i];
                if (fi >= fieldReps_.size()) continue;
                std::string name;
                try {
                    name = Token(fieldTokens_[fi]);
                    Value v = Unpack(fieldReps_[fi]);
                    spec.fields.emplace_back(std::move(name), std::move(v));
                } catch (const std::exception& e) {
                    if (warnings_.size() < 200)
                        warnings_.push_back("could not read field '" + name + "' of <" + p + ">: " + e.what());
                }
            }
        }
    }

private:
    Stream s_;
    int ver_ = 0;
    std::vector<std::string>& warnings_;
    std::vector<std::string> tokens_;
    std::vector<uint32_t> strings_;
    std::vector<uint32_t> fieldTokens_;
    std::vector<uint64_t> fieldReps_;
    std::vector<uint32_t> fieldSets_;
    std::vector<std::string> paths_;
    struct SpecRec {
        uint32_t path, fieldSet, type;
    };
    std::vector<SpecRec> specs_;

    static int Ver(int a, int b, int c) { return (a << 16) | (b << 8) | c; }

    const std::string& Token(uint32_t i) const {
        if (i >= tokens_.size()) throw CrateError("token index out of range");
        return tokens_[i];
    }
    const std::string& String(uint32_t i) const {
        if (i >= strings_.size()) throw CrateError("string index out of range");
        return Token(strings_[i]);
    }
    const std::string& PathAt(uint32_t i) const {
        if (i >= paths_.size()) throw CrateError("path index out of range");
        return paths_[i];
    }

    template <class Int>
    void ReadCompressedInts(Int* out, size_t n) {
        const uint64_t compSize = s_.Read<uint64_t>();
        const uint8_t* comp = s_.Take(compSize);
        std::vector<uint8_t> enc(sizeof(Int) + (n * 2 + 7) / 8 + n * sizeof(Int));
        const size_t got = FastDecompress(comp, size_t(compSize), enc.data(), enc.size());
        DecodeInts(enc.data(), got, out, n);
    }

    void CheckCount(uint64_t n, size_t elemBytes) const {
        if (elemBytes && n > s_.size / elemBytes + 1) throw CrateError("element count out of range");
    }

    void ReadTokens(uint64_t start) {
        s_.Seek(start);
        const uint64_t numTokens = s_.Read<uint64_t>();
        const uint64_t uncompressedSize = s_.Read<uint64_t>();
        const uint64_t compressedSize = s_.Read<uint64_t>();
        CheckCount(uncompressedSize, 1);  // compression ratios are bounded; guards absurd sizes
        const uint8_t* comp = s_.Take(compressedSize);
        std::vector<char> chars(size_t(uncompressedSize) + 1, '\0');
        const size_t got = FastDecompress(comp, size_t(compressedSize), reinterpret_cast<uint8_t*>(chars.data()),
                                          size_t(uncompressedSize));
        tokens_.clear();
        tokens_.reserve(size_t(numTokens));
        size_t pos = 0;
        while (tokens_.size() < numTokens && pos < got) {
            size_t len = std::strlen(chars.data() + pos);
            tokens_.emplace_back(chars.data() + pos, len);
            pos += len + 1;
        }
        while (tokens_.size() < numTokens) tokens_.emplace_back();
    }

    void ReadStrings(uint64_t start) {
        s_.Seek(start);
        const uint64_t n = s_.Read<uint64_t>();
        CheckCount(n, 4);
        strings_.resize(size_t(n));
        for (auto& s : strings_) s = s_.Read<uint32_t>();
    }

    void ReadFields(uint64_t start) {
        s_.Seek(start);
        const uint64_t n = s_.Read<uint64_t>();
        CheckCount(n, 1);
        fieldTokens_.resize(size_t(n));
        ReadCompressedInts(fieldTokens_.data(), size_t(n));
        const uint64_t repsSize = s_.Read<uint64_t>();
        const uint8_t* comp = s_.Take(repsSize);
        fieldReps_.assign(size_t(n), 0);
        FastDecompress(comp, size_t(repsSize), reinterpret_cast<uint8_t*>(fieldReps_.data()), size_t(n) * 8);
    }

    void ReadFieldSets(uint64_t start) {
        s_.Seek(start);
        const uint64_t n = s_.Read<uint64_t>();
        CheckCount(n, 1);
        fieldSets_.resize(size_t(n));
        ReadCompressedInts(fieldSets_.data(), size_t(n));
    }

    void ReadPaths(uint64_t start) {
        s_.Seek(start);
        const uint64_t numPaths = s_.Read<uint64_t>();
        CheckCount(numPaths, 1);
        paths_.assign(size_t(numPaths), std::string());
        const uint64_t numEncoded = s_.Read<uint64_t>();
        CheckCount(numEncoded, 1);
        const size_t n = size_t(numEncoded);
        std::vector<uint32_t> pathIndexes(n);
        std::vector<int32_t> elementTokens(n), jumps(n);
        ReadCompressedInts(pathIndexes.data(), n);
        ReadCompressedInts(elementTokens.data(), n);
        ReadCompressedInts(jumps.data(), n);

        // Iterative version of CrateFile::_BuildDecompressedPathsImpl.
        struct Job {
            size_t index;
            std::string parent;
            bool hasParent;
        };
        std::vector<Job> stack;
        if (n) stack.push_back({0, std::string(), false});
        while (!stack.empty()) {
            Job job = std::move(stack.back());
            stack.pop_back();
            size_t cur = job.index;
            std::string parent = std::move(job.parent);
            bool hasParent = job.hasParent;
            while (true) {
                if (cur >= n) throw CrateError("path tree out of range");
                const size_t self = cur++;
                const uint32_t pi = pathIndexes[self];
                if (pi >= paths_.size()) throw CrateError("path index out of range");
                if (!hasParent) {
                    parent = "/";
                    hasParent = true;
                    paths_[pi] = "/";
                } else {
                    const int32_t ti = elementTokens[self];
                    const std::string& elem = Token(uint32_t(ti < 0 ? -ti : ti));
                    if (ti < 0) {
                        paths_[pi] = path::AppendProperty(parent, elem);
                    } else if (!elem.empty() && (elem[0] == '{' || elem[0] == '[')) {
                        paths_[pi] = parent + elem;
                    } else {
                        paths_[pi] = path::AppendChild(parent, elem);
                    }
                }
                const int32_t jump = jumps[self];
                const bool hasChild = jump > 0 || jump == -1;
                const bool hasSibling = jump >= 0;
                if (hasChild) {
                    if (hasSibling) stack.push_back({self + size_t(jump), parent, true});
                    parent = paths_[pi];
                }
                if (!hasChild && !hasSibling) break;
            }
        }
    }

    void ReadSpecs(uint64_t start) {
        s_.Seek(start);
        const uint64_t n = s_.Read<uint64_t>();
        CheckCount(n, 1);
        std::vector<uint32_t> pathIdx(static_cast<size_t>(n)), fieldSetIdx(static_cast<size_t>(n)), types(static_cast<size_t>(n));
        ReadCompressedInts(pathIdx.data(), size_t(n));
        ReadCompressedInts(fieldSetIdx.data(), size_t(n));
        ReadCompressedInts(types.data(), size_t(n));
        specs_.resize(size_t(n));
        for (size_t i = 0; i < n; ++i) specs_[i] = {pathIdx[i], fieldSetIdx[i], types[i]};
    }

    // ---- values ----

    Value ReadNumScalar(const NumInfo& ni) {
        const size_t cs = CompSize(ni.comp);
        const uint8_t* p = s_.Take(cs * ni.tuple);
        std::vector<double> v(ni.tuple);
        for (size_t i = 0; i < ni.tuple; ++i) v[i] = CompToDouble(p + i * cs, ni.comp);
        return Value::Numbers(std::move(v), ni.tuple, false);
    }

    Value DecodeInlineNumber(int type, const NumInfo& ni, uint32_t bits) {
        switch (type) {
            case T_Bool: return Value::Scalar((bits & 0xff) ? 1.0 : 0.0);
            case T_UChar: return Value::Scalar(double(bits & 0xff));
            case T_Int: case T_Int64: return Value::Scalar(double(int32_t(bits)));
            case T_UInt: case T_UInt64: return Value::Scalar(double(bits));
            case T_Half: return Value::Scalar(double(HalfToFloat(uint16_t(bits & 0xffff))));
            case T_Float: case T_Double: case T_TimeCode: {
                float f;
                std::memcpy(&f, &bits, 4);
                return Value::Scalar(double(f));
            }
            case T_Matrix2d: case T_Matrix3d: case T_Matrix4d: {
                const int dim = type == T_Matrix2d ? 2 : type == T_Matrix3d ? 3 : 4;
                std::vector<double> m(size_t(dim * dim), 0.0);
                for (int i = 0; i < dim; ++i) m[size_t(i * dim + i)] = double(int8_t((bits >> (8 * i)) & 0xff));
                return Value::Numbers(std::move(m), uint8_t(dim * dim), false);
            }
            case T_Quatd: case T_Quatf: case T_Quath:
                throw CrateError("unexpected inlined quaternion");
            default: {  // vectors: one int8 per component
                std::vector<double> v(ni.tuple);
                for (size_t i = 0; i < ni.tuple; ++i) v[i] = double(int8_t((bits >> (8 * i)) & 0xff));
                return Value::Numbers(std::move(v), ni.tuple, false);
            }
        }
    }

    uint64_t ReadArrayCount() {
        if (ver_ < Ver(0, 5, 0)) s_.Read<uint32_t>();  // pre-0.5 files store a shape rank first
        return ver_ < Ver(0, 7, 0) ? uint64_t(s_.Read<uint32_t>()) : s_.Read<uint64_t>();
    }

    Value ReadNumArray(const NumInfo& ni, uint64_t payload, bool compressed) {
        Value v;
        v.kind = Value::Kind::Number;
        v.isArray = true;
        v.tupleSize = ni.tuple;
        if (payload == 0) return v;
        s_.Seek(payload);
        const uint64_t n = ReadArrayCount();
        CheckCount(n, 1);
        const size_t count = size_t(n);
        const size_t cs = CompSize(ni.comp);
        const bool isInt = ni.comp == C_I32 || ni.comp == C_U32 || ni.comp == C_I64 || ni.comp == C_U64;
        const bool isFloat = ni.comp == C_F16 || ni.comp == C_F32 || ni.comp == C_F64;
        if (compressed && ni.tuple == 1 && count >= kMinCompressedArraySize && isInt) {
            v.nums.resize(count);
            if (ni.comp == C_I32) {
                std::vector<int32_t> t(count);
                ReadCompressedInts(t.data(), count);
                for (size_t i = 0; i < count; ++i) v.nums[i] = double(t[i]);
            } else if (ni.comp == C_U32) {
                std::vector<uint32_t> t(count);
                ReadCompressedInts(t.data(), count);
                for (size_t i = 0; i < count; ++i) v.nums[i] = double(t[i]);
            } else if (ni.comp == C_I64) {
                std::vector<int64_t> t(count);
                ReadCompressedInts(t.data(), count);
                for (size_t i = 0; i < count; ++i) v.nums[i] = double(t[i]);
            } else {
                std::vector<uint64_t> t(count);
                ReadCompressedInts(t.data(), count);
                for (size_t i = 0; i < count; ++i) v.nums[i] = double(t[i]);
            }
            return v;
        }
        if (compressed && ni.tuple == 1 && count >= kMinCompressedArraySize && isFloat) {
            v.nums.resize(count);
            const char code = s_.Read<char>();
            if (code == 'i') {
                std::vector<int32_t> t(count);
                ReadCompressedInts(t.data(), count);
                for (size_t i = 0; i < count; ++i) v.nums[i] = double(t[i]);
            } else if (code == 't') {
                const uint32_t lutSize = s_.Read<uint32_t>();
                const uint8_t* lutData = s_.Take(uint64_t(lutSize) * cs);
                std::vector<double> lut(lutSize);
                for (size_t i = 0; i < lutSize; ++i) lut[i] = CompToDouble(lutData + i * cs, ni.comp);
                std::vector<uint32_t> idx(count);
                ReadCompressedInts(idx.data(), count);
                for (size_t i = 0; i < count; ++i) {
                    if (idx[i] >= lutSize) throw CrateError("float lookup index out of range");
                    v.nums[i] = lut[idx[i]];
                }
            } else {
                throw CrateError("unknown float array compression code");
            }
            return v;
        }
        const size_t total = count * ni.tuple;
        const uint8_t* p = s_.Take(uint64_t(total) * cs);
        v.nums.resize(total);
        for (size_t i = 0; i < total; ++i) v.nums[i] = CompToDouble(p + i * cs, ni.comp);
        return v;
    }

    Value ReadTextArray(Value::Kind kind, uint64_t payload, bool stringIndices) {
        Value v;
        v.kind = kind;
        v.isArray = true;
        if (payload == 0) return v;
        s_.Seek(payload);
        const uint64_t n = ReadArrayCount();
        CheckCount(n, 4);
        v.strs.reserve(size_t(n));
        for (uint64_t i = 0; i < n; ++i) {
            const uint32_t idx = s_.Read<uint32_t>();
            v.strs.push_back(stringIndices ? String(idx) : Token(idx));
        }
        return v;
    }

    // A VtValue nested in a dictionary: int64 jump to a ValueRep, read the rep, then continue after it.
    Value ReadEmbeddedValue() {
        const size_t start = s_.pos;
        const int64_t off = s_.Read<int64_t>();
        s_.Seek(uint64_t(int64_t(start) + off));
        const uint64_t rep = s_.Read<uint64_t>();
        const size_t saved = s_.pos;
        Value v = Unpack(rep);
        s_.Seek(saved);
        return v;
    }

    Value ReadDictionary() {
        const uint64_t n = s_.Read<uint64_t>();
        CheckCount(n, 12);
        auto d = std::make_shared<Dictionary>();
        for (uint64_t i = 0; i < n; ++i) {
            std::string key = String(s_.Read<uint32_t>());
            Value v = ReadEmbeddedValue();
            (*d)[std::move(key)] = std::move(v);
        }
        Value r;
        r.kind = Value::Kind::Dictionary;
        r.dict = std::move(d);
        return r;
    }

    LayerOffset ReadLayerOffset() {
        LayerOffset o;
        o.offset = s_.Read<double>();
        o.scale = s_.Read<double>();
        return o;
    }

    Reference ReadReference() {
        Reference r;
        r.assetPath = String(s_.Read<uint32_t>());
        r.primPath = PathAt(s_.Read<uint32_t>());
        r.layerOffset = ReadLayerOffset();
        ReadDictionary();  // customData (unused)
        return r;
    }

    Reference ReadPayload() {
        Reference r;
        r.assetPath = String(s_.Read<uint32_t>());
        r.primPath = PathAt(s_.Read<uint32_t>());
        if (ver_ >= Ver(0, 8, 0)) r.layerOffset = ReadLayerOffset();
        return r;
    }

    // Reads an SdfListOp and flattens it: explicit items if explicit, else prepended + added + appended.
    template <class T, class F>
    std::vector<T> ReadListOp(F readItem) {
        const uint8_t h = s_.Read<uint8_t>();
        std::vector<T> expl, added, prepended, appended, dummy;
        auto readVec = [&](std::vector<T>& out) {
            const uint64_t n = s_.Read<uint64_t>();
            CheckCount(n, 4);
            for (uint64_t i = 0; i < n; ++i) out.push_back(readItem());
        };
        if (h & 0x02) readVec(expl);
        if (h & 0x04) readVec(added);
        if (h & 0x20) readVec(prepended);
        if (h & 0x40) readVec(appended);
        if (h & 0x08) readVec(dummy);  // deleted
        if (h & 0x10) readVec(dummy);  // ordered
        if (h & 0x01) return expl;
        std::vector<T> r = std::move(prepended);
        r.insert(r.end(), added.begin(), added.end());
        r.insert(r.end(), appended.begin(), appended.end());
        return r;
    }

    Value ReadTimeSamples(uint64_t payload) {
        s_.Seek(payload);
        size_t start = s_.pos;
        int64_t off = s_.Read<int64_t>();
        s_.Seek(uint64_t(int64_t(start) + off));
        const uint64_t timesRep = s_.Read<uint64_t>();
        size_t saved = s_.pos;
        Value times = Unpack(timesRep);
        s_.Seek(saved);

        start = s_.pos;
        off = s_.Read<int64_t>();
        s_.Seek(uint64_t(int64_t(start) + off));
        const uint64_t numValues = s_.Read<uint64_t>();
        CheckCount(numValues, 8);
        std::vector<uint64_t> reps(static_cast<size_t>(numValues));
        for (auto& r : reps) r = s_.Read<uint64_t>();

        auto samples = std::make_shared<std::vector<TimeSample>>();
        const size_t n = std::min(reps.size(), times.nums.size());
        samples->reserve(n);
        for (size_t i = 0; i < n; ++i) samples->push_back({times.nums[i], Unpack(reps[i])});
        Value v;
        v.kind = Value::Kind::TimeSamples;
        v.samples = std::move(samples);
        return v;
    }

    Value Unpack(uint64_t rep) {
        const int type = int((rep >> 48) & 0xff);
        const bool isArray = (rep & kArrayBit) != 0;
        const bool inlined = (rep & kInlineBit) != 0;
        const bool compressed = (rep & kCompressedBit) != 0;
        const uint64_t payload = rep & kPayloadMask;

        NumInfo ni;
        if (GetNumInfo(type, ni)) {
            if (isArray) return ReadNumArray(ni, payload, compressed);
            if (inlined) return DecodeInlineNumber(type, ni, uint32_t(payload & 0xffffffffu));
            s_.Seek(payload);
            return ReadNumScalar(ni);
        }

        using K = Value::Kind;
        switch (type) {
            case T_String:
                if (isArray) return ReadTextArray(K::String, payload, true);
                return Value::Text(K::String, String(uint32_t(payload)));
            case T_Token:
                if (isArray) return ReadTextArray(K::Token, payload, false);
                return Value::Text(K::Token, Token(uint32_t(payload)));
            case T_AssetPath:
                if (isArray) return ReadTextArray(K::Asset, payload, false);
                return Value::Text(K::Asset, Token(uint32_t(payload)));
            case T_Specifier:
            case T_Permission:
            case T_Variability:
                return Value::Scalar(double(int32_t(payload & 0xffffffffu)));
            case T_ValueBlock:
            case T_AnimationBlock:
                return Value::Blocked();
            case T_Dictionary:
                if (inlined || payload == 0) {
                    Value v;
                    v.kind = K::Dictionary;
                    v.dict = std::make_shared<Dictionary>();
                    return v;
                }
                s_.Seek(payload);
                return ReadDictionary();
            case T_TokenListOp:
                s_.Seek(payload);
                return Value::Texts(K::Token, ReadListOp<std::string>([&] { return Token(s_.Read<uint32_t>()); }));
            case T_StringListOp:
                s_.Seek(payload);
                return Value::Texts(K::String, ReadListOp<std::string>([&] { return String(s_.Read<uint32_t>()); }));
            case T_PathListOp:
                s_.Seek(payload);
                return Value::Texts(K::Path, ReadListOp<std::string>([&] { return PathAt(s_.Read<uint32_t>()); }));
            case T_ReferenceListOp: {
                s_.Seek(payload);
                Value v;
                v.kind = K::References;
                v.isArray = true;
                v.refs = std::make_shared<std::vector<Reference>>(ReadListOp<Reference>([&] { return ReadReference(); }));
                return v;
            }
            case T_PayloadListOp: {
                s_.Seek(payload);
                Value v;
                v.kind = K::References;
                v.isArray = true;
                v.refs = std::make_shared<std::vector<Reference>>(ReadListOp<Reference>([&] { return ReadPayload(); }));
                return v;
            }
            case T_Payload: {
                s_.Seek(payload);
                Value v;
                v.kind = K::References;
                v.refs = std::make_shared<std::vector<Reference>>(1, ReadPayload());
                return v;
            }
            case T_IntListOp:
            case T_UIntListOp: {
                s_.Seek(payload);
                std::vector<double> items = ReadListOp<double>([&] { return double(s_.Read<int32_t>()); });
                return Value::Numbers(std::move(items), 1, true);
            }
            case T_Int64ListOp:
            case T_UInt64ListOp: {
                s_.Seek(payload);
                std::vector<double> items = ReadListOp<double>([&] { return double(s_.Read<int64_t>()); });
                return Value::Numbers(std::move(items), 1, true);
            }
            case T_PathVector:
            case T_TokenVector:
            case T_StringVector: {
                s_.Seek(payload);
                const uint64_t n = s_.Read<uint64_t>();
                CheckCount(n, 4);
                std::vector<std::string> items;
                items.reserve(size_t(n));
                for (uint64_t i = 0; i < n; ++i) {
                    const uint32_t idx = s_.Read<uint32_t>();
                    items.push_back(type == T_PathVector ? PathAt(idx) : type == T_TokenVector ? Token(idx) : String(idx));
                }
                return Value::Texts(type == T_PathVector ? K::Path : type == T_TokenVector ? K::Token : K::String,
                                    std::move(items));
            }
            case T_DoubleVector: {
                s_.Seek(payload);
                const uint64_t n = s_.Read<uint64_t>();
                CheckCount(n, 8);
                std::vector<double> items(static_cast<size_t>(n));
                for (auto& d : items) d = s_.Read<double>();
                return Value::Numbers(std::move(items), 1, true);
            }
            case T_LayerOffsetVector: {
                s_.Seek(payload);
                const uint64_t n = s_.Read<uint64_t>();
                CheckCount(n, 16);
                std::vector<double> items;
                for (uint64_t i = 0; i < n; ++i) {
                    LayerOffset o = ReadLayerOffset();
                    items.push_back(o.offset);
                    items.push_back(o.scale);
                }
                return Value::Numbers(std::move(items), 2, true);
            }
            case T_VariantSelectionMap: {
                s_.Seek(payload);
                const uint64_t n = s_.Read<uint64_t>();
                CheckCount(n, 8);
                auto d = std::make_shared<Dictionary>();
                for (uint64_t i = 0; i < n; ++i) {
                    std::string key = String(s_.Read<uint32_t>());
                    std::string val = String(s_.Read<uint32_t>());
                    (*d)[std::move(key)] = Value::Text(K::String, std::move(val));
                }
                Value v;
                v.kind = K::Dictionary;
                v.dict = std::move(d);
                return v;
            }
            case T_TimeSamples:
                return ReadTimeSamples(payload);
            case T_Value:
                s_.Seek(payload);
                return ReadEmbeddedValue();
            case T_UnregisteredValue:
            case T_UnregisteredValueListOp:
            case T_PathExpression:
            case T_Relocates:
            case T_Spline:
                return Value();  // not needed for viewing
            default:
                throw CrateError("unknown value type " + std::to_string(type));
        }
    }
};

}  // namespace

bool IsCrateFile(const uint8_t* data, size_t size) {
    return size >= 8 && std::memcmp(data, "PXR-USDC", 8) == 0;
}

bool ReadCrate(const uint8_t* data, size_t size, Layer& layer, std::string& err, std::vector<std::string>& warnings) {
    try {
        CrateReader reader(data, size, warnings);
        reader.Read(layer);
        return true;
    } catch (const std::exception& e) {
        err = std::string("invalid USDC file: ") + e.what();
        return false;
    }
}

}  // namespace usd
