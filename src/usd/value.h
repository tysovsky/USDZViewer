// Generic USD value representation shared by the USDC and USDA readers.
#pragma once

#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace usd {

struct Value;
struct TimeSample;
using Dictionary = std::map<std::string, Value, std::less<>>;

// Maps a time authored in a layer to stage time: stage = offset + scale * layer.
struct LayerOffset {
    double offset = 0.0;
    double scale = 1.0;

    double Apply(double t) const { return offset + scale * t; }
    double Invert(double t) const { return scale != 0.0 ? (t - offset) / scale : t; }
    // The offset equivalent to applying `inner` first and then this one.
    LayerOffset Compose(const LayerOffset& inner) const {
        return {offset + scale * inner.offset, scale * inner.scale};
    }
};

// A reference or payload arc.
struct Reference {
    std::string assetPath;
    std::string primPath;
    LayerOffset layerOffset;
};

// Every value is stored in one of a few generic shapes:
//  - numbers (scalars, vectors, matrices, quaternions and arrays of them) as doubles,
//    `tupleSize` components per element. Quaternions are stored as (x, y, z, w).
//  - strings, tokens, asset paths and prim/property paths as strings.
//  - dictionaries, reference lists and time samples behind shared pointers.
// List ops are flattened when read (explicit items, or prepended + added + appended items).
struct Value {
    enum class Kind : uint8_t {
        None,        // no value
        Blocked,     // explicitly blocked (None in .usda, SdfValueBlock)
        Number,
        String,
        Token,
        Asset,
        Path,
        Dictionary,
        References,  // references or payloads
        TimeSamples,
    };

    Kind kind = Kind::None;
    bool isArray = false;
    uint8_t tupleSize = 1;
    std::vector<double> nums;
    std::vector<std::string> strs;
    std::shared_ptr<const Dictionary> dict;
    std::shared_ptr<const std::vector<Reference>> refs;
    std::shared_ptr<const std::vector<TimeSample>> samples;

    bool IsEmpty() const { return kind == Kind::None; }
    bool IsBlocked() const { return kind == Kind::Blocked; }
    bool IsNumber() const { return kind == Kind::Number; }
    bool IsText() const {
        return kind == Kind::String || kind == Kind::Token || kind == Kind::Asset || kind == Kind::Path;
    }

    // Number of array elements (tuples for numbers, strings for text values).
    size_t Count() const {
        if (kind == Kind::Number) return tupleSize ? nums.size() / tupleSize : 0;
        if (IsText()) return strs.size();
        if (kind == Kind::References && refs) return refs->size();
        return 0;
    }
    double Num(size_t i = 0, double fallback = 0.0) const { return i < nums.size() ? nums[i] : fallback; }
    bool Bool(bool fallback = false) const { return nums.empty() ? fallback : nums[0] != 0.0; }
    const std::string& Str(size_t i = 0) const;
    const Value* Find(std::string_view key) const;  // dictionary lookup

    static Value Scalar(double v) {
        Value r;
        r.kind = Kind::Number;
        r.nums.push_back(v);
        return r;
    }
    static Value Numbers(std::vector<double> v, uint8_t tuple, bool array) {
        Value r;
        r.kind = Kind::Number;
        r.tupleSize = tuple ? tuple : 1;
        r.isArray = array;
        r.nums = std::move(v);
        return r;
    }
    static Value Text(Kind k, std::string s) {
        Value r;
        r.kind = k;
        r.strs.push_back(std::move(s));
        return r;
    }
    static Value Texts(Kind k, std::vector<std::string> s) {
        Value r;
        r.kind = k;
        r.isArray = true;
        r.strs = std::move(s);
        return r;
    }
    static Value Blocked() {
        Value r;
        r.kind = Kind::Blocked;
        return r;
    }
};

struct TimeSample {
    double time = 0.0;
    Value value;
};

inline const std::string& Value::Str(size_t i) const {
    static const std::string empty;
    return i < strs.size() ? strs[i] : empty;
}

inline const Value* Value::Find(std::string_view key) const {
    if (kind != Kind::Dictionary || !dict) return nullptr;
    auto it = dict->find(key);
    return it == dict->end() ? nullptr : &it->second;
}

// Converts an IEEE half float to float.
inline float HalfToFloat(uint16_t h) {
    uint32_t sign = (uint32_t(h) & 0x8000u) << 16;
    uint32_t exp = (h >> 10) & 0x1f;
    uint32_t mant = h & 0x3ff;
    uint32_t bits;
    if (exp == 0) {
        if (mant == 0) {
            bits = sign;
        } else {  // subnormal: renormalize
            exp = 127 - 15 + 1;
            while ((mant & 0x400) == 0) {
                mant <<= 1;
                --exp;
            }
            mant &= 0x3ff;
            bits = sign | (exp << 23) | (mant << 13);
        }
    } else if (exp == 31) {
        bits = sign | 0x7f800000u | (mant << 13);
    } else {
        bits = sign | ((exp - 15 + 127) << 23) | (mant << 13);
    }
    float f;
    static_assert(sizeof(f) == sizeof(bits));
    std::memcpy(&f, &bits, sizeof(f));
    return f;
}

}  // namespace usd
