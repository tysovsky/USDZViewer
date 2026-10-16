// Composed view of a USD scene: layers + composition arcs flattened into a prim tree.
//
// Supported composition: sublayers (with time offsets), references and payloads (internal, external,
// inside the same .usdz or on disk), inherits, specializes and variant sets. Strength ordering follows
// USD's LIVRPS rules per prim; list ops are approximated by their flattened item lists.
#pragma once

#include "layer.h"

#include <map>
#include <memory>
#include <unordered_map>

namespace usd {

class Archive;
class Prim;
class Stage;

using PathMap = std::vector<std::pair<std::string, std::string>>;  // prefix -> prefix, applied in order

struct LayerStack {
    struct Entry {
        std::shared_ptr<Layer> layer;
        LayerOffset offset;
    };
    std::vector<Entry> layers;  // strongest first
    std::string defaultPrim;
};

// One prim spec contributing to a composed prim.
struct Opinion {
    const Layer* layer = nullptr;
    const Spec* spec = nullptr;
    std::string path;    // path of the spec inside `layer`
    LayerOffset offset;  // layer time -> stage time
    std::shared_ptr<const PathMap> map;  // spec namespace -> stage namespace
};

struct Attribute {
    std::string name;
    std::string typeName;
    const Value* defaultValue = nullptr;
    const std::vector<TimeSample>* samples = nullptr;
    LayerOffset offset;                  // sample time -> stage time
    const Layer* valueLayer = nullptr;   // layer providing the value (anchor for asset paths)
    std::string interpolation;
    int elementSize = 1;
    std::vector<std::string> connections;  // stage namespace
    bool declared = false;

    bool IsValid() const { return declared; }
    bool HasValue() const;
    bool IsAnimated() const { return samples && samples->size() > 1; }
    // Value at a stage time (held or interpolated). Returns false when there is no value or it is blocked.
    bool Get(double time, Value& out) const;
    // Value without time dependence: the default, or the first time sample.
    bool GetDefault(Value& out) const;
    void GetSampleTimes(std::vector<double>& out) const;
};

class Prim {
public:
    const std::string& Name() const { return name_; }
    const std::string& Path() const { return path_; }
    const std::string& TypeName() const { return typeName_; }
    Prim* Parent() const { return parent_; }
    Stage* GetStage() const { return stage_; }
    const std::vector<std::unique_ptr<Prim>>& Children() const { return children_; }

    bool IsActive() const { return active_; }
    bool IsDefined() const { return specifier_ == 0; }
    bool IsAbstract() const { return specifier_ == 2; }
    bool HasAPI(std::string_view schema) const;
    const std::vector<std::string>& APISchemas() const { return apiSchemas_; }

    const Value* GetMetadata(std::string_view key) const;
    const std::vector<std::string>& PropertyNames() const { return propertyNames_; }
    bool HasProperty(std::string_view name) const;
    Attribute GetAttribute(std::string_view name) const;
    // Relationship targets mapped to stage namespace. `authored` reports whether any opinion exists.
    std::vector<std::string> GetTargets(std::string_view relName, bool* authored = nullptr) const;
    // Strongest opinion of a metadata field on a property (e.g. "bindMaterialAs" on a binding).
    const Value* GetPropertyMetadata(std::string_view prop, std::string_view key) const;
    const std::vector<Opinion>& Opinions() const { return opinions_; }

private:
    friend class Stage;
    std::string name_, path_, typeName_;
    int specifier_ = 1;
    bool active_ = true;
    Prim* parent_ = nullptr;
    Stage* stage_ = nullptr;
    std::vector<std::unique_ptr<Prim>> children_;
    std::vector<Opinion> opinions_;
    std::vector<std::string> propertyNames_;
    std::vector<std::string> apiSchemas_;
};

class Stage {
public:
    static std::unique_ptr<Stage> Open(const std::wstring& filePath, std::string& err);
    ~Stage();

    Prim* PseudoRoot() const { return root_.get(); }
    Prim* GetPrim(const std::string& path) const;

    // Resolves an asset path authored in `anchor` (a layer of this stage).
    AssetLocation ResolveAsset(const Layer* anchor, const std::string& assetPath) const;
    bool ReadAsset(const AssetLocation& loc, std::vector<uint8_t>& out, std::string& err) const;

    const Layer& RootLayer() const { return *rootLayer_; }
    const std::vector<std::shared_ptr<Layer>>& Layers() const { return layerList_; }
    const Value* RootMetadata(std::string_view key) const;

    std::wstring filePath;
    double timeCodesPerSecond = 24.0;
    double framesPerSecond = 24.0;
    double startTimeCode = 0.0, endTimeCode = 0.0;
    bool hasTimeRange = false;
    std::string upAxis = "Y";
    double metersPerUnit = 0.01;
    std::vector<std::string> warnings;

private:
    struct Node;
    Stage();
    std::shared_ptr<Layer> LoadLayer(const AssetLocation& loc, std::string& err);
    const LayerStack* GetLayerStack(const AssetLocation& loc, std::string& err);
    void BuildLayerStack(const std::shared_ptr<Layer>& layer, const LayerOffset& offset, LayerStack& stack, int depth);
    std::shared_ptr<Archive> OpenPackage(const std::string& fsPath, std::string& err) const;

    void ExpandArcs(Node& node, int depth, int ttl);
    void ExpandTree(Node& node, int depth);
    void ResolveVariants(Node& root, int depth);
    bool FindVariantSelection(const Node& root, const std::string& set, std::string& sel) const;
    void Flatten(const Node& node, std::vector<Opinion>& out) const;
    void ComposeChildren(Prim* prim, const Node& index, int depth);
    void Warn(std::string msg);

    std::unique_ptr<Prim> root_;
    std::shared_ptr<Layer> rootLayer_;
    std::vector<std::shared_ptr<Layer>> layerList_;
    std::unordered_map<std::string, std::shared_ptr<Layer>> layerCache_;
    std::unordered_map<std::string, std::unique_ptr<LayerStack>> stackCache_;
    mutable std::unordered_map<std::string, std::shared_ptr<Archive>> packageCache_;
    std::unordered_map<std::string, Prim*> primMap_;
    const LayerStack* rootStack_ = nullptr;
    size_t primCount_ = 0;
};

// Interpolates two values of the given USD type name (lerp/slerp; held for non-interpolatable types).
Value InterpolateValues(const Value& a, const Value& b, double u, std::string_view typeName);

}  // namespace usd
