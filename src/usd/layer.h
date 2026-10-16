// In-memory form of a single USD layer (a .usdc or .usda file): specs keyed by path.
#pragma once

#include "value.h"

#include <unordered_map>

namespace usd {

class Archive;

enum class SpecType : uint8_t {
    Unknown = 0,
    Attribute = 1,
    Connection = 2,
    Expression = 3,
    Mapper = 4,
    MapperArg = 5,
    Prim = 6,
    PseudoRoot = 7,
    Relationship = 8,
    RelationshipTarget = 9,
    Variant = 10,
    VariantSet = 11,
};

struct Spec {
    SpecType type = SpecType::Unknown;
    std::vector<std::pair<std::string, Value>> fields;

    const Value* Get(std::string_view name) const {
        for (auto& f : fields)
            if (f.first == name) return &f.second;
        return nullptr;
    }
    Value& Set(std::string_view name) {
        for (auto& f : fields)
            if (f.first == name) return f.second;
        fields.emplace_back(std::string(name), Value());
        return fields.back().second;
    }
};

// Where an asset lives: inside a .usdz package, or on disk.
struct AssetLocation {
    std::shared_ptr<Archive> archive;  // set when the asset lives inside a .usdz package
    std::string path;                  // package-relative path, or an absolute file path (UTF-8)

    bool IsValid() const { return !path.empty(); }
    std::string Describe() const;
    bool operator==(const AssetLocation& o) const { return archive == o.archive && path == o.path; }
};

struct Layer {
    AssetLocation location;
    std::string format;   // "usdc" or "usda"
    std::string version;  // file format version
    std::unordered_map<std::string, Spec> specs;

    const Spec* GetSpec(const std::string& path) const {
        auto it = specs.find(path);
        return it == specs.end() ? nullptr : &it->second;
    }
    Spec& GetOrCreateSpec(const std::string& path, SpecType type) {
        Spec& s = specs[path];
        if (s.type == SpecType::Unknown) s.type = type;
        return s;
    }
    const Value* GetField(const std::string& path, std::string_view field) const {
        const Spec* s = GetSpec(path);
        return s ? s->Get(field) : nullptr;
    }
};

// SdfPath-style helpers on plain strings. Prim paths look like "/a/b", properties "/a/b.attr",
// variant selections "/a{set=sel}" (children of a variant: "/a{set=sel}child").
namespace path {
std::string AppendChild(std::string_view parent, std::string_view name);
std::string AppendProperty(std::string_view prim, std::string_view name);
std::string AppendVariant(std::string_view prim, std::string_view set, std::string_view selection);
std::string ParentPrim(std::string_view primPath);  // "/a/b" -> "/a", "/a" -> "/"
std::string Name(std::string_view primPath);        // last element
// Splits "/a/b.prop" into "/a/b" and "prop" (prop empty for prim paths).
void Split(std::string_view p, std::string& prim, std::string& prop);
bool HasPrefix(std::string_view p, std::string_view prefix);
std::string ReplacePrefix(std::string_view p, std::string_view oldPrefix, std::string_view newPrefix);
std::string StripVariants(std::string_view p);
// Resolves a possibly relative path (e.g. "../Mat") against a prim path.
std::string MakeAbsolute(std::string_view anchorPrim, std::string_view p);
int Depth(std::string_view primPath);
}  // namespace path

}  // namespace usd
