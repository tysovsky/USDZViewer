#include "stage.h"

#include "archive.h"
#include "crate.h"
#include "fileio.h"
#include "usda.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>

namespace usd {

namespace {

enum ArcType { ArcRoot = 0, ArcInherit = 1, ArcVariant = 2, ArcReference = 3, ArcPayload = 4, ArcSpecialize = 5 };

std::string MapPath(const PathMap* map, const std::string& p) {
    if (!map) return p;
    std::string r = p;
    for (auto& [from, to] : *map)
        if (path::HasPrefix(r, from)) r = path::ReplacePrefix(r, from, to);
    return r;
}

bool IsZip(const std::vector<uint8_t>& b) {
    return b.size() >= 4 && b[0] == 'P' && b[1] == 'K' && b[2] == 3 && b[3] == 4;
}

bool IsUsdExtension(const std::string& name) {
    std::string ext = FileExtensionLower(name);
    return ext == "usd" || ext == "usda" || ext == "usdc";
}

std::string LowerAscii(std::string s) {
    for (char& c : s)
        if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
    return s;
}

bool IsInterpolatableType(std::string_view type) {
    if (type.empty()) return true;
    static const char* prefixes[] = {"half", "float", "double", "point", "normal", "vector", "color",
                                     "texCoord", "matrix", "quat", "frame", "timecode"};
    for (const char* p : prefixes)
        if (type.substr(0, std::strlen(p)) == p) return true;
    return false;
}

}  // namespace

struct Stage::Node {
    const LayerStack* stack = nullptr;
    std::string path;
    LayerOffset offset;
    std::shared_ptr<const PathMap> map;
    int arc = ArcRoot;
    int depth = 0;
    int order = 0;
    std::vector<Node> children;
    std::vector<std::string> processedSets;
};

// ---- Attribute -------------------------------------------------------------------------------

Value InterpolateValues(const Value& a, const Value& b, double u, std::string_view typeName) {
    if (a.kind != Value::Kind::Number || b.kind != Value::Kind::Number || a.nums.size() != b.nums.size() ||
        a.tupleSize != b.tupleSize)
        return a;
    std::string_view base = typeName;
    if (base.size() > 2 && base.substr(base.size() - 2) == "[]") base = base.substr(0, base.size() - 2);
    if (!IsInterpolatableType(base)) return a;
    Value r = a;
    if (base.substr(0, 4) == "quat" && a.tupleSize == 4) {
        for (size_t i = 0; i + 3 < a.nums.size(); i += 4) {
            double q0[4] = {a.nums[i], a.nums[i + 1], a.nums[i + 2], a.nums[i + 3]};
            double q1[4] = {b.nums[i], b.nums[i + 1], b.nums[i + 2], b.nums[i + 3]};
            double d = q0[0] * q1[0] + q0[1] * q1[1] + q0[2] * q1[2] + q0[3] * q1[3];
            if (d < 0) {
                d = -d;
                for (double& c : q1) c = -c;
            }
            double s0, s1;
            if (d > 0.9995) {
                s0 = 1.0 - u;
                s1 = u;
            } else {
                double theta = std::acos(std::min(1.0, d));
                double st = std::sin(theta);
                s0 = std::sin((1.0 - u) * theta) / st;
                s1 = std::sin(u * theta) / st;
            }
            double len = 0;
            for (int k = 0; k < 4; ++k) {
                r.nums[i + k] = s0 * q0[k] + s1 * q1[k];
                len += r.nums[i + k] * r.nums[i + k];
            }
            len = std::sqrt(len);
            if (len > 0)
                for (int k = 0; k < 4; ++k) r.nums[i + k] /= len;
        }
        return r;
    }
    for (size_t i = 0; i < r.nums.size(); ++i) r.nums[i] = a.nums[i] + (b.nums[i] - a.nums[i]) * u;
    return r;
}

bool Attribute::HasValue() const {
    return (samples && !samples->empty()) || (defaultValue && !defaultValue->IsEmpty());
}

bool Attribute::Get(double time, Value& out) const {
    if (samples && !samples->empty()) {
        const auto& s = *samples;
        const double t = offset.Invert(time);
        const TimeSample* pick = nullptr;
        if (s.size() == 1 || t <= s.front().time) pick = &s.front();
        else if (t >= s.back().time) pick = &s.back();
        if (!pick) {
            auto it = std::upper_bound(s.begin(), s.end(), t, [](double v, const TimeSample& x) { return v < x.time; });
            const TimeSample& b = *it;
            const TimeSample& a = *(it - 1);
            if (t == a.time || b.value.IsBlocked() || b.value.IsEmpty() || b.time <= a.time) {
                pick = &a;
            } else {
                if (a.value.IsBlocked() || a.value.IsEmpty()) return false;
                out = InterpolateValues(a.value, b.value, (t - a.time) / (b.time - a.time), typeName);
                return true;
            }
        }
        if (pick->value.IsBlocked() || pick->value.IsEmpty()) return false;
        out = pick->value;
        return true;
    }
    if (defaultValue && !defaultValue->IsEmpty() && !defaultValue->IsBlocked()) {
        out = *defaultValue;
        return true;
    }
    return false;
}

bool Attribute::GetDefault(Value& out) const {
    if (defaultValue && !defaultValue->IsEmpty() && !defaultValue->IsBlocked()) {
        out = *defaultValue;
        return true;
    }
    if (samples && !samples->empty()) {
        for (auto& s : *samples) {
            if (!s.value.IsBlocked() && !s.value.IsEmpty()) {
                out = s.value;
                return true;
            }
        }
    }
    return false;
}

void Attribute::GetSampleTimes(std::vector<double>& out) const {
    if (!samples) return;
    for (auto& s : *samples) out.push_back(offset.Apply(s.time));
}

// ---- Prim ------------------------------------------------------------------------------------

bool Prim::HasAPI(std::string_view schema) const {
    for (auto& s : apiSchemas_) {
        if (s == schema) return true;
        if (s.size() > schema.size() && s.compare(0, schema.size(), schema) == 0 && s[schema.size()] == ':') return true;
    }
    return false;
}

const Value* Prim::GetMetadata(std::string_view key) const {
    for (auto& op : opinions_)
        if (const Value* v = op.spec->Get(key)) return v;
    return nullptr;
}

bool Prim::HasProperty(std::string_view name) const {
    for (auto& n : propertyNames_)
        if (n == name) return true;
    return false;
}

Attribute Prim::GetAttribute(std::string_view name) const {
    Attribute a;
    a.name = std::string(name);
    bool haveValue = false, haveConn = false, haveElementSize = false;
    for (auto& op : opinions_) {
        const Spec* spec = op.layer->GetSpec(path::AppendProperty(op.path, name));
        if (!spec) continue;
        a.declared = true;
        if (a.typeName.empty())
            if (const Value* v = spec->Get("typeName")) a.typeName = v->Str();
        if (a.interpolation.empty())
            if (const Value* v = spec->Get("interpolation")) a.interpolation = v->Str();
        if (!haveElementSize)
            if (const Value* v = spec->Get("elementSize"); v && v->IsNumber()) {
                a.elementSize = std::max(1, int(v->Num()));
                haveElementSize = true;
            }
        if (!haveConn)
            if (const Value* v = spec->Get("connectionPaths")) {
                for (auto& p : v->strs) a.connections.push_back(MapPath(op.map.get(), p));
                haveConn = true;
            }
        if (!haveValue) {
            const Value* ts = spec->Get("timeSamples");
            if (ts && ts->samples && !ts->samples->empty()) {
                a.samples = ts->samples.get();
                a.offset = op.offset;
                a.valueLayer = op.layer;
                haveValue = true;
            } else if (const Value* d = spec->Get("default"); d && !d->IsEmpty()) {
                a.defaultValue = d;
                a.valueLayer = op.layer;
                haveValue = true;
            }
        }
    }
    return a;
}

std::vector<std::string> Prim::GetTargets(std::string_view relName, bool* authored) const {
    if (authored) *authored = false;
    for (auto& op : opinions_) {
        const Spec* spec = op.layer->GetSpec(path::AppendProperty(op.path, relName));
        if (!spec) continue;
        if (authored) *authored = true;
        const Value* v = spec->Get("targetPaths");
        if (!v) continue;
        std::vector<std::string> out;
        for (auto& p : v->strs) out.push_back(MapPath(op.map.get(), p));
        return out;
    }
    return {};
}

const Value* Prim::GetPropertyMetadata(std::string_view prop, std::string_view key) const {
    for (auto& op : opinions_) {
        const Spec* spec = op.layer->GetSpec(path::AppendProperty(op.path, prop));
        if (!spec) continue;
        if (const Value* v = spec->Get(key)) return v;
    }
    return nullptr;
}

// ---- Stage -----------------------------------------------------------------------------------

Stage::Stage() = default;
Stage::~Stage() = default;

void Stage::Warn(std::string msg) {
    if (warnings.size() >= 500) return;
    for (auto& w : warnings)
        if (w == msg) return;
    warnings.push_back(std::move(msg));
}

Prim* Stage::GetPrim(const std::string& p) const {
    if (p == "/") return root_.get();
    auto it = primMap_.find(p);
    return it == primMap_.end() ? nullptr : it->second;
}

const Value* Stage::RootMetadata(std::string_view key) const {
    return rootLayer_ ? rootLayer_->GetField("/", key) : nullptr;
}

std::shared_ptr<Archive> Stage::OpenPackage(const std::string& fsPath, std::string& err) const {
    std::string key = LowerAscii(fsPath);
    auto it = packageCache_.find(key);
    if (it != packageCache_.end()) return it->second;
    std::vector<uint8_t> bytes;
    if (!ReadFileBytes(Utf8ToWide(fsPath), bytes, err)) return nullptr;
    auto a = Archive::Load(std::move(bytes), fsPath, err);
    if (a) packageCache_[key] = a;
    return a;
}

AssetLocation Stage::ResolveAsset(const Layer* anchor, const std::string& assetPath) const {
    if (assetPath.empty() || !anchor) return {};
    // Package-relative syntax: "package.usdz[inner/file.png]".
    if (assetPath.back() == ']') {
        size_t open = assetPath.find('[');
        if (open != std::string::npos && open > 0) {
            AssetLocation outer = ResolveAsset(anchor, assetPath.substr(0, open));
            std::string inner = assetPath.substr(open + 1, assetPath.size() - open - 2);
            if (!outer.IsValid()) return {};
            std::string err;
            std::shared_ptr<Archive> pkg;
            if (outer.archive) {
                std::string key = outer.Describe();
                auto it = packageCache_.find(key);
                if (it != packageCache_.end()) {
                    pkg = it->second;
                } else {
                    std::vector<uint8_t> bytes;
                    if (ReadAsset(outer, bytes, err)) pkg = Archive::Load(std::move(bytes), key, err);
                    if (pkg) packageCache_[key] = pkg;
                }
            } else {
                pkg = OpenPackage(outer.path, err);
            }
            if (pkg)
                if (const Archive::Entry* e = pkg->Find(inner)) return {pkg, e->name};
            return {};
        }
    }
    const AssetLocation& a = anchor->location;
    if (a.archive) {
        size_t slash = a.path.rfind('/');
        std::string dir = slash == std::string::npos ? std::string() : a.path.substr(0, slash);
        if (const Archive::Entry* e = a.archive->Find(dir.empty() ? assetPath : dir + "/" + assetPath))
            return {a.archive, e->name};
        if (const Archive::Entry* e = a.archive->Find(assetPath)) return {a.archive, e->name};
        std::string fs = JoinFsPath(FsDirName(a.archive->DisplayPath()), assetPath);
        if (FileExists(Utf8ToWide(fs))) return {nullptr, fs};
        return {};
    }
    std::string fs = JoinFsPath(FsDirName(a.path), assetPath);
    if (FileExists(Utf8ToWide(fs))) return {nullptr, fs};
    return {};
}

bool Stage::ReadAsset(const AssetLocation& loc, std::vector<uint8_t>& out, std::string& err) const {
    if (loc.archive) {
        const Archive::Entry* e = loc.archive->Find(loc.path);
        if (!e) {
            err = "'" + loc.path + "' is not in the package";
            return false;
        }
        return loc.archive->Extract(*e, out, err);
    }
    return ReadFileBytes(Utf8ToWide(loc.path), out, err);
}

std::shared_ptr<Layer> Stage::LoadLayer(const AssetLocation& loc, std::string& err) {
    const std::string key = loc.Describe();
    auto it = layerCache_.find(key);
    if (it != layerCache_.end()) return it->second;

    std::vector<uint8_t> bytes;
    if (!ReadAsset(loc, bytes, err)) return nullptr;
    if (IsZip(bytes)) {  // a .usdz used as a layer: its first USD file is the layer
        auto archive = Archive::Load(std::move(bytes), key, err);
        if (!archive) return nullptr;
        packageCache_[key] = archive;
        for (auto& e : archive->Entries()) {
            if (IsUsdExtension(e.name)) {
                auto layer = LoadLayer({archive, e.name}, err);
                if (layer) layerCache_[key] = layer;
                return layer;
            }
        }
        err = key + ": the package contains no USD layer";
        return nullptr;
    }

    auto layer = std::make_shared<Layer>();
    layer->location = loc;
    std::vector<std::string> warns;
    bool ok;
    size_t skip = (bytes.size() >= 3 && bytes[0] == 0xEF && bytes[1] == 0xBB && bytes[2] == 0xBF) ? 3 : 0;
    if (IsCrateFile(bytes.data(), bytes.size())) {
        ok = ReadCrate(bytes.data(), bytes.size(), *layer, err, warns);
    } else if (IsUsdaFile(bytes.data() + skip, bytes.size() - skip)) {
        ok = ReadUsda(reinterpret_cast<const char*>(bytes.data() + skip), bytes.size() - skip, *layer, err, warns);
    } else {
        err = key + ": not a USD file";
        return nullptr;
    }
    if (!ok) {
        err = key + ": " + err;
        return nullptr;
    }
    for (auto& w : warns) Warn(key + ": " + w);
    layerCache_[key] = layer;
    layerList_.push_back(layer);
    return layer;
}

void Stage::BuildLayerStack(const std::shared_ptr<Layer>& layer, const LayerOffset& offset, LayerStack& stack,
                            int depth) {
    for (auto& e : stack.layers)
        if (e.layer == layer) return;  // cycle
    stack.layers.push_back({layer, offset});
    if (depth > 16) return;
    const Spec* root = layer->GetSpec("/");
    const Value* subs = root ? root->Get("subLayers") : nullptr;
    if (!subs) return;
    const Value* offs = root->Get("subLayerOffsets");
    for (size_t i = 0; i < subs->strs.size(); ++i) {
        LayerOffset lo;
        if (offs && offs->nums.size() >= 2 * i + 2) lo = {offs->nums[2 * i], offs->nums[2 * i + 1]};
        AssetLocation loc = ResolveAsset(layer.get(), subs->strs[i]);
        if (!loc.IsValid()) {
            Warn("could not resolve sublayer @" + subs->strs[i] + "@ of " + layer->location.Describe());
            continue;
        }
        std::string err;
        auto sub = LoadLayer(loc, err);
        if (!sub) {
            Warn(err);
            continue;
        }
        BuildLayerStack(sub, offset.Compose(lo), stack, depth + 1);
    }
}

const LayerStack* Stage::GetLayerStack(const AssetLocation& loc, std::string& err) {
    const std::string key = loc.Describe();
    auto it = stackCache_.find(key);
    if (it != stackCache_.end()) return it->second.get();
    auto layer = LoadLayer(loc, err);
    if (!layer) return nullptr;
    auto stack = std::make_unique<LayerStack>();
    BuildLayerStack(layer, LayerOffset{}, *stack, 0);
    if (const Value* dp = layer->GetField("/", "defaultPrim")) stack->defaultPrim = dp->Str();
    if (stack->defaultPrim.empty()) {  // fall back to the first root prim
        if (const Value* kids = layer->GetField("/", "primChildren"); kids && !kids->strs.empty())
            stack->defaultPrim = kids->strs[0];
    }
    const LayerStack* ptr = stack.get();
    stackCache_[key] = std::move(stack);
    return ptr;
}

void Stage::ExpandArcs(Node& node, int depth, int ttl) {
    if (ttl > 24) {
        Warn("composition is nested too deeply at <" + node.path + "> (cyclic references?)");
        return;
    }
    struct Pending {
        int arc;
        const LayerStack* stack;
        std::string path;
        LayerOffset offset;
        std::shared_ptr<const PathMap> map;
    };
    std::vector<Pending> arcs;
    const std::string here = path::StripVariants(node.path);
    auto makeMap = [&](const std::string& target) {
        auto m = std::make_shared<PathMap>();
        m->push_back({path::StripVariants(target), here});
        if (node.map) m->insert(m->end(), node.map->begin(), node.map->end());
        return std::shared_ptr<const PathMap>(std::move(m));
    };

    for (auto& e : node.stack->layers) {
        const Spec* spec = e.layer->GetSpec(node.path);
        if (!spec) continue;
        auto addPathArcs = [&](const char* field, int arc) {
            if (const Value* v = spec->Get(field))
                for (auto& p : v->strs) arcs.push_back({arc, node.stack, p, node.offset, makeMap(p)});
        };
        auto addRefArcs = [&](const char* field, int arc) {
            const Value* v = spec->Get(field);
            if (!v || !v->refs) return;
            for (const Reference& r : *v->refs) {
                const LayerStack* target = node.stack;
                if (!r.assetPath.empty()) {
                    AssetLocation loc = ResolveAsset(e.layer.get(), r.assetPath);
                    if (!loc.IsValid()) {
                        Warn("could not resolve @" + r.assetPath + "@ referenced from <" + node.path + "> in " +
                             e.layer->location.Describe());
                        continue;
                    }
                    std::string err;
                    target = GetLayerStack(loc, err);
                    if (!target) {
                        Warn(err);
                        continue;
                    }
                }
                std::string tpath = r.primPath;
                if (tpath.empty()) {
                    if (target->defaultPrim.empty()) {
                        Warn("reference @" + r.assetPath + "@ has no target prim and the layer has no defaultPrim");
                        continue;
                    }
                    tpath = "/" + target->defaultPrim;
                }
                LayerOffset off = node.offset.Compose(e.offset).Compose(r.layerOffset);
                arcs.push_back({arc, target, tpath, off, makeMap(tpath)});
            }
        };
        addPathArcs("inheritPaths", ArcInherit);
        addRefArcs("references", ArcReference);
        addRefArcs("payload", ArcPayload);
        addPathArcs("specializes", ArcSpecialize);
    }

    int order = int(node.children.size());
    for (auto& a : arcs) {
        if (a.stack == node.stack && a.path == node.path) continue;
        Node child;
        child.stack = a.stack;
        child.path = a.path;
        child.offset = a.offset;
        child.map = a.map;
        child.arc = a.arc;
        child.depth = depth;
        child.order = order++;
        ExpandArcs(child, depth, ttl + 1);
        node.children.push_back(std::move(child));
    }
    std::stable_sort(node.children.begin(), node.children.end(), [](const Node& x, const Node& y) {
        if (x.arc != y.arc) return x.arc < y.arc;
        if (x.depth != y.depth) return x.depth > y.depth;
        return x.order < y.order;
    });
}

void Stage::ExpandTree(Node& root, int depth) {
    // Post-order so that growing a node's child list never invalidates nodes still to be visited.
    std::vector<Node*> order;
    std::function<void(Node&)> collect = [&](Node& n) {
        for (auto& c : n.children) collect(c);
        order.push_back(&n);
    };
    collect(root);
    for (Node* n : order) ExpandArcs(*n, depth, 0);
}

bool Stage::FindVariantSelection(const Node& node, const std::string& set, std::string& sel) const {
    for (auto& e : node.stack->layers) {
        const Spec* spec = e.layer->GetSpec(node.path);
        if (!spec) continue;
        if (const Value* v = spec->Get("variantSelection"))
            if (const Value* s = v->Find(set)) {
                sel = s->Str();
                return true;
            }
    }
    for (auto& c : node.children)
        if (FindVariantSelection(c, set, sel)) return true;
    return false;
}

void Stage::ResolveVariants(Node& root, int depth) {
    std::function<bool(Node&)> visit = [&](Node& n) -> bool {
        for (auto& e : n.stack->layers) {
            const Spec* spec = e.layer->GetSpec(n.path);
            if (!spec) continue;
            for (const char* field : {"variantSetNames", "variantSetChildren"}) {
                const Value* v = spec->Get(field);
                if (!v) continue;
                for (auto& set : v->strs) {
                    if (std::find(n.processedSets.begin(), n.processedSets.end(), set) != n.processedSets.end()) continue;
                    n.processedSets.push_back(set);
                    std::string sel;
                    if (FindVariantSelection(root, set, sel) && !sel.empty()) {
                        std::string vpath = path::AppendVariant(n.path, set, sel);
                        bool exists = false;
                        for (auto& l : n.stack->layers) exists |= l.layer->GetSpec(vpath) != nullptr;
                        if (exists) {
                            Node child;
                            child.stack = n.stack;
                            child.path = vpath;
                            child.offset = n.offset;
                            child.map = n.map;
                            child.arc = ArcVariant;
                            child.depth = depth;
                            child.order = int(n.children.size());
                            ExpandArcs(child, depth, 1);
                            n.children.push_back(std::move(child));
                            std::stable_sort(n.children.begin(), n.children.end(), [](const Node& x, const Node& y) {
                                if (x.arc != y.arc) return x.arc < y.arc;
                                if (x.depth != y.depth) return x.depth > y.depth;
                                return x.order < y.order;
                            });
                        }
                    }
                    return true;  // the tree may have changed: restart the walk
                }
            }
        }
        for (auto& c : n.children)
            if (visit(c)) return true;
        return false;
    };
    for (int i = 0; i < 1024 && visit(root); ++i) {
    }
}

void Stage::Flatten(const Node& node, std::vector<Opinion>& out) const {
    for (auto& e : node.stack->layers) {
        const Spec* s = e.layer->GetSpec(node.path);
        if (s && (s->type == SpecType::Prim || s->type == SpecType::Variant || s->type == SpecType::PseudoRoot))
            out.push_back({e.layer.get(), s, node.path, node.offset.Compose(e.offset), node.map});
    }
    for (auto& c : node.children) Flatten(c, out);
}

void Stage::ComposeChildren(Prim* prim, const Node& index, int depth) {
    std::vector<std::string> names;
    for (auto& op : prim->opinions_) {
        if (const Value* v = op.spec->Get("primChildren"))
            for (auto& n : v->strs)
                if (std::find(names.begin(), names.end(), n) == names.end()) names.push_back(n);
    }
    for (const std::string& name : names) {
        if (primCount_ > 2000000) {
            Warn("scene has too many prims; the rest were skipped");
            return;
        }
        // Re-root the parent's prim index at the child path.
        std::function<void(const Node&, Node&)> copy = [&](const Node& s, Node& d) {
            d.stack = s.stack;
            d.path = path::AppendChild(s.path, name);
            d.offset = s.offset;
            d.map = s.map;
            d.arc = s.arc;
            d.depth = s.depth;
            d.order = s.order;
            d.children.resize(s.children.size());
            for (size_t i = 0; i < s.children.size(); ++i) copy(s.children[i], d.children[i]);
        };
        Node childIndex;
        copy(index, childIndex);
        ExpandTree(childIndex, depth);
        ResolveVariants(childIndex, depth);
        std::vector<Opinion> ops;
        Flatten(childIndex, ops);
        if (ops.empty()) continue;

        auto child = std::make_unique<Prim>();
        child->name_ = name;
        child->path_ = path::AppendChild(prim->path_, name);
        child->parent_ = prim;
        child->stage_ = this;
        child->opinions_ = std::move(ops);
        bool hasDef = false, hasClass = false, haveActive = false;
        for (auto& op : child->opinions_) {
            if (child->typeName_.empty())
                if (const Value* v = op.spec->Get("typeName")) child->typeName_ = v->Str();
            if (const Value* v = op.spec->Get("specifier")) {
                int s = int(v->Num());
                hasDef |= s == 0;
                hasClass |= s == 2;
            }
            if (!haveActive)
                if (const Value* v = op.spec->Get("active"); v && v->IsNumber()) {
                    child->active_ = v->Bool(true);
                    haveActive = true;
                }
            if (const Value* v = op.spec->Get("apiSchemas"))
                for (auto& s : v->strs)
                    if (std::find(child->apiSchemas_.begin(), child->apiSchemas_.end(), s) == child->apiSchemas_.end())
                        child->apiSchemas_.push_back(s);
            if (const Value* v = op.spec->Get("properties"))
                for (auto& s : v->strs)
                    if (std::find(child->propertyNames_.begin(), child->propertyNames_.end(), s) ==
                        child->propertyNames_.end())
                        child->propertyNames_.push_back(s);
        }
        child->specifier_ = hasDef ? 0 : hasClass ? 2 : 1;
        primMap_[child->path_] = child.get();
        ++primCount_;
        if (child->active_) ComposeChildren(child.get(), childIndex, depth + 1);
        prim->children_.push_back(std::move(child));
    }
}

std::unique_ptr<Stage> Stage::Open(const std::wstring& filePath, std::string& err) {
    std::unique_ptr<Stage> stage(new Stage());
    stage->filePath = filePath;
    const std::string fsPath = WideToUtf8(filePath);

    AssetLocation rootLoc;
    if (FileExtensionLower(fsPath) == "usdz") {
        auto archive = stage->OpenPackage(fsPath, err);
        if (!archive) return nullptr;
        const Archive::Entry* first = nullptr;
        for (auto& e : archive->Entries()) {
            if (IsUsdExtension(e.name)) {
                first = &e;
                break;
            }
        }
        if (!first) {
            err = "the package contains no USD layer";
            return nullptr;
        }
        rootLoc = {archive, first->name};
    } else {
        rootLoc = {nullptr, fsPath};
    }

    const LayerStack* stack = stage->GetLayerStack(rootLoc, err);
    if (!stack) return nullptr;
    stage->rootStack_ = stack;
    stage->rootLayer_ = stack->layers[0].layer;

    auto num = [&](const char* key, double& out) {
        const Value* v = stage->RootMetadata(key);
        if (v && v->IsNumber() && !v->nums.empty()) {
            out = v->nums[0];
            return true;
        }
        return false;
    };
    bool haveTcps = num("timeCodesPerSecond", stage->timeCodesPerSecond);
    bool haveFps = num("framesPerSecond", stage->framesPerSecond);
    if (!haveTcps && haveFps) stage->timeCodesPerSecond = stage->framesPerSecond;
    if (!haveFps) stage->framesPerSecond = stage->timeCodesPerSecond;
    if (stage->timeCodesPerSecond <= 0) stage->timeCodesPerSecond = 24;
    if (stage->framesPerSecond <= 0) stage->framesPerSecond = stage->timeCodesPerSecond;
    bool haveStart = num("startTimeCode", stage->startTimeCode);
    bool haveEnd = num("endTimeCode", stage->endTimeCode);
    stage->hasTimeRange = haveStart && haveEnd && stage->endTimeCode > stage->startTimeCode;
    num("metersPerUnit", stage->metersPerUnit);
    if (const Value* v = stage->RootMetadata("upAxis")) stage->upAxis = v->Str();

    Node rootIndex;
    rootIndex.stack = stack;
    rootIndex.path = "/";
    stage->root_ = std::make_unique<Prim>();
    stage->root_->path_ = "/";
    stage->root_->specifier_ = 0;
    stage->root_->stage_ = stage.get();
    stage->Flatten(rootIndex, stage->root_->opinions_);
    stage->ComposeChildren(stage->root_.get(), rootIndex, 1);
    return stage;
}

}  // namespace usd
