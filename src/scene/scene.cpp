#include "scene/scene.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <functional>
#include <unordered_map>
#include <unordered_set>

namespace scene {

using usd::Attribute;
using usd::Prim;
using usd::Value;

namespace {

constexpr double kPi = 3.14159265358979323846;

bool StartsWith(const std::string& s, const char* prefix) { return s.rfind(prefix, 0) == 0; }

std::vector<int> ToInts(const Value& v) {
    std::vector<int> r(v.nums.size());
    for (size_t i = 0; i < v.nums.size(); ++i) r[i] = int(v.nums[i]);
    return r;
}

std::vector<float> ToFloats(const Value& v) {
    std::vector<float> r(v.nums.size());
    for (size_t i = 0; i < v.nums.size(); ++i) r[i] = float(v.nums[i]);
    return r;
}

struct Primvar {
    Value values;
    std::vector<int> indices;
    std::string interpolation;
    int elementSize = 1;
    int tuple = 1;
};

bool ReadPrimvar(const Prim* prim, const std::string& name, double time, Primvar& pv, const char* defaultInterp) {
    Attribute a = prim->GetAttribute(name);
    if (!a.IsValid()) return false;
    Value v;
    if (!a.Get(time, v) || !v.IsNumber() || v.nums.empty()) return false;
    pv.values = std::move(v);
    pv.tuple = pv.values.tupleSize ? pv.values.tupleSize : 1;
    pv.interpolation = a.interpolation.empty() ? defaultInterp : a.interpolation;
    pv.elementSize = a.elementSize;
    Attribute ia = prim->GetAttribute(name + ":indices");
    Value iv;
    if (ia.IsValid() && ia.Get(time, iv) && iv.IsNumber()) pv.indices = ToInts(iv);
    return true;
}

// Element of a primvar for one face corner; nullptr if out of range.
const double* PrimvarAt(const Primvar& pv, size_t face, size_t point, size_t corner) {
    size_t idx = 0;
    const std::string& in = pv.interpolation;
    if (in == "uniform") idx = face;
    else if (in == "vertex" || in == "varying") idx = point;
    else if (in == "faceVarying") idx = corner;
    if (!pv.indices.empty()) {
        if (idx >= pv.indices.size() || pv.indices[idx] < 0) return nullptr;
        idx = size_t(pv.indices[idx]);
    }
    size_t off = idx * size_t(pv.tuple);
    if (off + size_t(pv.tuple) > pv.values.nums.size()) return nullptr;
    return pv.values.nums.data() + off;
}

Mat4 MatrixFromValue(const Value& v, size_t index = 0) {
    if (v.nums.size() >= (index + 1) * 16) return Mat4::FromArray(v.nums.data() + index * 16);
    return Mat4::Identity();
}

Mat4 OpMatrix(const XformOp& op, double time) {
    Value v;
    if (!op.attr.Get(time, v) || !v.IsNumber() || v.nums.empty()) return Mat4::Identity();
    const auto& n = v.nums;
    auto at = [&](size_t i) { return i < n.size() ? n[i] : 0.0; };
    Mat4 m = Mat4::Identity();
    switch (op.type) {
        case XformOp::Translate: m = Mat4::Translate(at(0), at(1), at(2)); break;
        case XformOp::Scale:
            m = n.size() >= 3 ? Mat4::Scale(n[0], n[1], n[2]) : Mat4::Scale(n[0], n[0], n[0]);
            break;
        case XformOp::RotateX: m = Mat4::RotateAxis(0, at(0)); break;
        case XformOp::RotateY: m = Mat4::RotateAxis(1, at(0)); break;
        case XformOp::RotateZ: m = Mat4::RotateAxis(2, at(0)); break;
        case XformOp::Orient: m = Mat4::FromQuat(at(0), at(1), at(2), n.size() >= 4 ? n[3] : 1.0); break;
        case XformOp::Transform: m = MatrixFromValue(v); break;
        default: {
            // Three-axis rotations: the first named axis is applied first.
            static const int orders[6][3] = {{0, 1, 2}, {0, 2, 1}, {1, 0, 2}, {1, 2, 0}, {2, 0, 1}, {2, 1, 0}};
            const int* o = orders[op.type - XformOp::RotateXYZ];
            double angles[3] = {at(0), at(1), at(2)};
            m = Mat4::RotateAxis(o[0], angles[o[0]]) * Mat4::RotateAxis(o[1], angles[o[1]]) *
                Mat4::RotateAxis(o[2], angles[o[2]]);
            break;
        }
    }
    return op.invert ? m.Inverse() : m;
}

// Simple procedural geometry for UsdGeom implicit shapes.
struct ShapeBuilder {
    std::vector<float> points, normals, uvs;
    void Vertex(const Vec3& p, const Vec3& n, double u, double v) {
        points.insert(points.end(), {float(p.x), float(p.y), float(p.z)});
        normals.insert(normals.end(), {float(n.x), float(n.y), float(n.z)});
        uvs.insert(uvs.end(), {float(u), float(v)});
    }
    void Quad(const Vec3 p[4], const Vec3 n[4], const double uv[4][2]) {
        const int tri[6] = {0, 1, 2, 0, 2, 3};
        for (int i : tri) Vertex(p[i], n[i], uv[i][0], uv[i][1]);
    }
};

// Surface of revolution around +Z: profile points (radius, z) with normals (nr, nz).
void Revolve(ShapeBuilder& b, const std::vector<std::array<double, 4>>& profile, int segments) {
    for (size_t i = 0; i + 1 < profile.size(); ++i) {
        for (int s = 0; s < segments; ++s) {
            double a0 = 2 * kPi * s / segments, a1 = 2 * kPi * (s + 1) / segments;
            const auto& r0 = profile[i];
            const auto& r1 = profile[i + 1];
            Vec3 p[4] = {{r0[0] * std::cos(a0), r0[0] * std::sin(a0), r0[1]},
                         {r0[0] * std::cos(a1), r0[0] * std::sin(a1), r0[1]},
                         {r1[0] * std::cos(a1), r1[0] * std::sin(a1), r1[1]},
                         {r1[0] * std::cos(a0), r1[0] * std::sin(a0), r1[1]}};
            Vec3 n[4] = {Vec3{r0[2] * std::cos(a0), r0[2] * std::sin(a0), r0[3]}.Normalized(),
                         Vec3{r0[2] * std::cos(a1), r0[2] * std::sin(a1), r0[3]}.Normalized(),
                         Vec3{r1[2] * std::cos(a1), r1[2] * std::sin(a1), r1[3]}.Normalized(),
                         Vec3{r1[2] * std::cos(a0), r1[2] * std::sin(a0), r1[3]}.Normalized()};
            double v0 = double(i) / (profile.size() - 1), v1 = double(i + 1) / (profile.size() - 1);
            double uv[4][2] = {{double(s) / segments, v0}, {double(s + 1) / segments, v0},
                               {double(s + 1) / segments, v1}, {double(s) / segments, v1}};
            b.Quad(p, n, uv);
        }
    }
}

}  // namespace

// ---- normals ---------------------------------------------------------------------------------

void ComputeNormals(const std::vector<float>& points, const std::vector<uint32_t>& cornerPoint, bool smooth,
                    std::vector<float>& out) {
    const size_t nc = cornerPoint.size();
    out.assign(nc * 3, 0.0f);
    const size_t np = points.size() / 3;
    auto P = [&](uint32_t i) { return Vec3{points[i * 3], points[i * 3 + 1], points[i * 3 + 2]}; };
    if (smooth) {
        std::vector<double> acc(np * 3, 0.0);
        for (size_t c = 0; c + 2 < nc; c += 3) {
            uint32_t i0 = cornerPoint[c], i1 = cornerPoint[c + 1], i2 = cornerPoint[c + 2];
            if (i0 >= np || i1 >= np || i2 >= np) continue;
            Vec3 n = (P(i1) - P(i0)).Cross(P(i2) - P(i0));
            for (uint32_t i : {i0, i1, i2}) {
                acc[i * 3] += n.x;
                acc[i * 3 + 1] += n.y;
                acc[i * 3 + 2] += n.z;
            }
        }
        for (size_t c = 0; c < nc; ++c) {
            uint32_t i = cornerPoint[c];
            if (i >= np) continue;
            Vec3 n = Vec3{acc[i * 3], acc[i * 3 + 1], acc[i * 3 + 2]}.Normalized();
            out[c * 3] = float(n.x);
            out[c * 3 + 1] = float(n.y);
            out[c * 3 + 2] = float(n.z);
        }
    } else {
        for (size_t c = 0; c + 2 < nc; c += 3) {
            uint32_t i0 = cornerPoint[c], i1 = cornerPoint[c + 1], i2 = cornerPoint[c + 2];
            if (i0 >= np || i1 >= np || i2 >= np) continue;
            Vec3 n = (P(i1) - P(i0)).Cross(P(i2) - P(i0)).Normalized();
            for (size_t k = 0; k < 3; ++k) {
                out[(c + k) * 3] = float(n.x);
                out[(c + k) * 3 + 1] = float(n.y);
                out[(c + k) * 3 + 2] = float(n.z);
            }
        }
    }
}

// ---- builder ---------------------------------------------------------------------------------

struct Scene::Builder {
    Scene& s;
    const usd::Stage& stage;
    double t0 = 0;
    double minTime = 1e300, maxTime = -1e300;
    std::unordered_map<std::string, int> materialIndex, imageIndex, skeletonIndex;
    std::unordered_map<const Prim*, int> nodeIndex;

    Builder(Scene& scene, const usd::Stage& st) : s(scene), stage(st) {}

    void Warn(const std::string& msg) {
        if (s.warnings.size() < 300 && std::find(s.warnings.begin(), s.warnings.end(), msg) == s.warnings.end())
            s.warnings.push_back(msg);
    }

    void NoteTimes(const Attribute& a) {
        if (!a.samples || a.samples->size() < 2) return;
        minTime = std::min(minTime, a.offset.Apply(a.samples->front().time));
        maxTime = std::max(maxTime, a.offset.Apply(a.samples->back().time));
    }

    std::string Token(const Prim* prim, const char* name, const char* fallback = "") {
        Value v;
        Attribute a = prim->GetAttribute(name);
        if (a.IsValid() && a.Get(t0, v) && v.IsText()) return v.Str();
        return fallback;
    }
    double Number(const Prim* prim, const char* name, double fallback) {
        Value v;
        Attribute a = prim->GetAttribute(name);
        if (a.IsValid() && a.Get(t0, v) && v.IsNumber() && !v.nums.empty()) return v.nums[0];
        return fallback;
    }

    // Relationship authored on the prim or its nearest ancestor (UsdSkel binding properties inherit).
    const Prim* InheritedTarget(const Prim* prim, const char* rel) {
        for (const Prim* p = prim; p && p->Path() != "/"; p = p->Parent()) {
            auto targets = p->GetTargets(rel);
            if (!targets.empty()) return stage.GetPrim(targets[0]);
        }
        return nullptr;
    }

    // ---- traversal ----

    struct ProtoCtx {
        int instancer = -1;
        int protoIndex = -1;
        int protoParentNode = -1;
    };

    int AddNode(const Prim* prim, int parent) {
        Node n;
        n.prim = prim;
        n.parent = parent;
        Value order;
        Attribute orderAttr = prim->GetAttribute("xformOpOrder");
        if (orderAttr.IsValid() && orderAttr.GetDefault(order)) {
            for (const std::string& full : order.strs) {
                if (full == "!resetXformStack!") {
                    n.resetXformStack = true;
                    n.ops.clear();
                    continue;
                }
                XformOp op;
                std::string name = full;
                if (StartsWith(name, "!invert!")) {
                    op.invert = true;
                    name = name.substr(8);
                }
                size_t c1 = name.find(':');
                size_t c2 = name.find(':', c1 + 1);
                std::string kind = c1 == std::string::npos ? std::string() : name.substr(c1 + 1, c2 == std::string::npos ? std::string::npos : c2 - c1 - 1);
                static const std::pair<const char*, XformOp::Type> kinds[] = {
                    {"translate", XformOp::Translate}, {"scale", XformOp::Scale}, {"rotateX", XformOp::RotateX},
                    {"rotateY", XformOp::RotateY}, {"rotateZ", XformOp::RotateZ}, {"rotateXYZ", XformOp::RotateXYZ},
                    {"rotateXZY", XformOp::RotateXZY}, {"rotateYXZ", XformOp::RotateYXZ}, {"rotateYZX", XformOp::RotateYZX},
                    {"rotateZXY", XformOp::RotateZXY}, {"rotateZYX", XformOp::RotateZYX}, {"orient", XformOp::Orient},
                    {"transform", XformOp::Transform}};
                bool known = false;
                for (auto& [k, t] : kinds)
                    if (kind == k) {
                        op.type = t;
                        known = true;
                    }
                if (!known) {
                    Warn("unsupported xform op '" + full + "' on <" + prim->Path() + ">");
                    continue;
                }
                op.attr = prim->GetAttribute(name);
                if (!op.attr.IsValid()) {
                    Warn("missing xform op attribute '" + name + "' on <" + prim->Path() + ">");
                    continue;
                }
                if (op.attr.IsAnimated()) {
                    n.animated = true;
                    NoteTimes(op.attr);
                }
                n.ops.push_back(std::move(op));
            }
        }
        n.visibility = prim->GetAttribute("visibility");
        if (n.visibility.IsAnimated()) {
            n.animated = true;
            NoteTimes(n.visibility);
        }
        s.nodes.push_back(std::move(n));
        int idx = int(s.nodes.size() - 1);
        nodeIndex[prim] = idx;
        return idx;
    }

    void Traverse(const Prim* prim, int parentNode, const ProtoCtx& ctx) {
        for (auto& c : prim->Children()) {
            const Prim* child = c.get();
            if (!child->IsActive() || !child->IsDefined()) continue;
            const std::string& type = child->TypeName();
            {
                std::string purpose = Token(child, "purpose", "default");
                if (purpose == "guide" || purpose == "proxy") continue;
            }
            int node = AddNode(child, parentNode);
            ProtoCtx childCtx = ctx;
            if (ctx.instancer >= 0 && ctx.protoIndex < 0) {
                const auto& protos = s.instancers[size_t(ctx.instancer)].prototypes;
                auto it = std::find(protos.begin(), protos.end(), child->Path());
                if (it != protos.end()) {
                    childCtx.protoIndex = int(it - protos.begin());
                    childCtx.protoParentNode = parentNode;
                }
            }
            if (type == "Mesh") {
                BuildMesh(child, node, childCtx);
            } else if (type == "Cube" || type == "Sphere" || type == "Cylinder" || type == "Cone" || type == "Capsule" ||
                       type == "Plane") {
                BuildShape(child, node, childCtx, type);
            } else if (type == "PointInstancer" && ctx.protoIndex < 0) {
                Instancer inst;
                inst.prim = child;
                inst.node = node;
                inst.prototypes = child->GetTargets("prototypes");
                for (const char* a : {"positions", "orientations", "orientationsf", "scales", "protoIndices"})
                    NoteTimes(child->GetAttribute(a));
                s.instancers.push_back(std::move(inst));
                ProtoCtx ic;
                ic.instancer = int(s.instancers.size() - 1);
                Traverse(child, node, ic);
                continue;
            } else if (type == "Points" || type == "BasisCurves" || type == "NurbsCurves" || type == "NurbsPatch") {
                Warn("'" + type + "' prims are not displayed (" + child->Path() + ")");
            }
            Traverse(child, node, childCtx);
        }
    }

    // ---- materials ----

    struct Source {
        bool hasValue = false;
        Value value;
        const usd::Layer* layer = nullptr;
        const Prim* node = nullptr;
        std::string output;
    };

    Source ResolveAttr(const Attribute& a, int depth) {
        Source src;
        if (!a.IsValid() || depth > 16) return src;
        if (!a.connections.empty()) {
            std::string tprim, prop;
            usd::path::Split(a.connections[0], tprim, prop);
            if (const Prim* t = stage.GetPrim(tprim)) {
                if (StartsWith(prop, "outputs:")) {
                    if (t->TypeName() == "Shader") {
                        src.node = t;
                        src.output = prop.substr(8);
                        return src;
                    }
                    Source s2 = ResolveAttr(t->GetAttribute(prop), depth + 1);  // NodeGraph/Material output
                    if (s2.node || s2.hasValue) return s2;
                } else if (StartsWith(prop, "inputs:")) {
                    Source s2 = ResolveAttr(t->GetAttribute(prop), depth + 1);  // interface input
                    if (s2.node || s2.hasValue) return s2;
                }
            }
        }
        Value v;
        if (a.Get(t0, v)) {
            src.hasValue = true;
            src.value = std::move(v);
            src.layer = a.valueLayer;
        }
        return src;
    }
    Source ResolveInput(const Prim* prim, const std::string& input) {
        return ResolveAttr(prim->GetAttribute("inputs:" + input), 0);
    }
    std::string ShaderId(const Prim* shader) { return Token(shader, "info:id"); }

    std::string ResolveVarname(const Prim* reader) {
        Source v = ResolveInput(reader, "varname");
        return v.hasValue ? v.value.Str() : std::string();
    }

    void SetupTexture(const Prim* tex, const std::string& output, TextureSlot& slot, bool colorInput, Material& m) {
        Source file = ResolveInput(tex, "file");
        if (!file.hasValue || file.value.kind != Value::Kind::Asset || file.value.Str().empty()) return;
        usd::AssetLocation loc = stage.ResolveAsset(file.layer, file.value.Str());
        if (!loc.IsValid()) {
            Warn("texture @" + file.value.Str() + "@ was not found");
            return;
        }
        const std::string key = loc.Describe();
        auto it = imageIndex.find(key);
        if (it == imageIndex.end()) {
            s.images.push_back({loc, file.value.Str()});
            it = imageIndex.emplace(key, int(s.images.size() - 1)).first;
        }
        slot.image = it->second;
        if (output == "r") slot.channel = 0;
        else if (output == "g") slot.channel = 1;
        else if (output == "b") slot.channel = 2;
        else if (output == "a") slot.channel = 3;
        else slot.channel = -1;
        auto wrap = [&](const char* name) {
            Source w = ResolveInput(tex, name);
            std::string v = w.hasValue ? w.value.Str() : "useMetadata";
            if (v == "mirror") return Wrap::Mirror;
            if (v == "clamp") return Wrap::Clamp;
            if (v == "black") return Wrap::Black;
            return Wrap::Repeat;
        };
        slot.wrapS = wrap("wrapS");
        slot.wrapT = wrap("wrapT");
        for (const char* name : {"scale", "bias"}) {
            Source v = ResolveInput(tex, name);
            if (v.hasValue && v.value.nums.size() >= 4)
                for (int i = 0; i < 4; ++i) (name[0] == 's' ? slot.scale : slot.bias)[i] = float(v.value.nums[size_t(i)]);
        }
        Source cs = ResolveInput(tex, "sourceColorSpace");
        std::string space = cs.hasValue ? cs.value.Str() : "auto";
        slot.srgb = space == "sRGB" || (space != "raw" && colorInput);

        // Texture coordinates: UsdPrimvarReader_float2, optionally through UsdTransform2d.
        Source st = ResolveInput(tex, "st");
        for (int guard = 0; st.node && guard < 4; ++guard) {
            std::string id = ShaderId(st.node);
            if (id == "UsdTransform2d") {
                Source rot = ResolveInput(st.node, "rotation"), sc = ResolveInput(st.node, "scale"),
                       tr = ResolveInput(st.node, "translation");
                double r = rot.hasValue ? rot.value.Num(0) * kPi / 180.0 : 0.0;
                double sx = sc.hasValue ? sc.value.Num(0, 1) : 1.0, sy = sc.hasValue ? sc.value.Num(1, 1) : 1.0;
                double tx = tr.hasValue ? tr.value.Num(0) : 0.0, ty = tr.hasValue ? tr.value.Num(1) : 0.0;
                double c = std::cos(r), sn = std::sin(r);
                float uv[6] = {float(sx * c), float(-sy * sn), float(tx), float(sx * sn), float(sy * c), float(ty)};
                std::copy(uv, uv + 6, slot.uv);
                st = ResolveInput(st.node, "in");
                continue;
            }
            if (StartsWith(id, "UsdPrimvarReader")) {
                std::string var = ResolveVarname(st.node);
                if (!var.empty() && m.uvSet.empty()) m.uvSet = var;
            }
            break;
        }
    }

    void ReadInput(const Prim* shader, const char* name, float* out, int count, TextureSlot* slot, bool colorInput,
                   Material& m) {
        Source src = ResolveInput(shader, name);
        if (src.node) {
            std::string id = ShaderId(src.node);
            if (id == "UsdUVTexture" && slot) {
                SetupTexture(src.node, src.output, *slot, colorInput, m);
            } else if (StartsWith(id, "UsdPrimvarReader") && std::string(name) == "diffuseColor") {
                m.usesVertexColor = true;
                for (int i = 0; i < count; ++i) out[i] = 1.0f;
            }
            return;
        }
        if (src.hasValue && src.value.IsNumber() && int(src.value.nums.size()) >= count)
            for (int i = 0; i < count; ++i) out[i] = float(src.value.nums[size_t(i)]);
    }

    const Prim* FindPreviewSurface(const Prim* prim) {
        for (auto& c : prim->Children()) {
            if (c->TypeName() == "Shader" && ShaderId(c.get()) == "UsdPreviewSurface") return c.get();
            if (const Prim* p = FindPreviewSurface(c.get())) return p;
        }
        return nullptr;
    }

    int GetMaterial(const Prim* matPrim) {
        auto it = materialIndex.find(matPrim->Path());
        if (it != materialIndex.end()) return it->second;
        Material m;
        m.path = matPrim->Path();
        m.name = matPrim->Name();
        const Prim* shader = nullptr;
        for (const char* out : {"outputs:surface", "outputs:usd:surface"}) {
            Source src = ResolveAttr(matPrim->GetAttribute(out), 0);
            if (src.node && ShaderId(src.node) == "UsdPreviewSurface") {
                shader = src.node;
                break;
            }
        }
        if (!shader) shader = FindPreviewSurface(matPrim);
        if (!shader) {
            Warn("material <" + m.path + "> has no UsdPreviewSurface shader; using display colors");
            m.usesVertexColor = true;
        } else {
            ReadInput(shader, "diffuseColor", m.diffuse, 3, &m.diffuseTex, true, m);
            ReadInput(shader, "emissiveColor", m.emissive, 3, &m.emissiveTex, true, m);
            ReadInput(shader, "specularColor", m.specularColor, 3, &m.specularTex, true, m);
            ReadInput(shader, "metallic", &m.metallic, 1, &m.metallicTex, false, m);
            ReadInput(shader, "roughness", &m.roughness, 1, &m.roughnessTex, false, m);
            ReadInput(shader, "opacity", &m.opacity, 1, &m.opacityTex, false, m);
            ReadInput(shader, "opacityThreshold", &m.opacityThreshold, 1, nullptr, false, m);
            ReadInput(shader, "occlusion", &m.occlusion, 1, &m.occlusionTex, false, m);
            ReadInput(shader, "clearcoat", &m.clearcoat, 1, nullptr, false, m);
            ReadInput(shader, "clearcoatRoughness", &m.clearcoatRoughness, 1, nullptr, false, m);
            ReadInput(shader, "ior", &m.ior, 1, nullptr, false, m);
            float normalDummy[3] = {0, 0, 1};
            ReadInput(shader, "normal", normalDummy, 3, &m.normalTex, false, m);
            float useSpec = 0;
            ReadInput(shader, "useSpecularWorkflow", &useSpec, 1, nullptr, false, m);
            m.useSpecularWorkflow = useSpec != 0;
            // Normal maps must be remapped to [-1, 1]; some exporters forget the scale/bias.
            TextureSlot& n = m.normalTex;
            if (n.image >= 0 && n.scale[0] == 1 && n.scale[1] == 1 && n.scale[2] == 1 && n.bias[0] == 0 &&
                n.bias[1] == 0 && n.bias[2] == 0) {
                for (int i = 0; i < 3; ++i) {
                    n.scale[i] = 2;
                    n.bias[i] = -1;
                }
            }
        }
        if (m.uvSet.empty()) m.uvSet = "st";
        s.materials.push_back(std::move(m));
        int idx = int(s.materials.size() - 1);
        materialIndex[matPrim->Path()] = idx;
        return idx;
    }

    // UsdShadeMaterialBindingAPI: closest binding wins unless an ancestor binds strongerThanDescendants.
    int ResolveBinding(const Prim* prim, bool inherit) {
        const Prim* winner = nullptr;
        for (const Prim* p = prim; p && p->Path() != "/"; p = inherit ? p->Parent() : nullptr) {
            for (const char* rel : {"material:binding:preview", "material:binding"}) {
                auto targets = p->GetTargets(rel);
                if (targets.empty()) continue;
                const Prim* m = stage.GetPrim(targets[0]);
                if (m) {
                    const Value* strength = p->GetPropertyMetadata(rel, "bindMaterialAs");
                    bool stronger = strength && strength->Str() == "strongerThanDescendants";
                    if (!winner || stronger) winner = m;
                } else {
                    Warn("material binding target <" + targets[0] + "> of <" + p->Path() + "> does not exist");
                }
                break;
            }
            if (!inherit) break;
        }
        return winner ? GetMaterial(winner) : -1;
    }

    // ---- skeletons ----

    int GetSkeleton(const Prim* skelPrim) {
        auto it = skeletonIndex.find(skelPrim->Path());
        if (it != skeletonIndex.end()) return it->second;
        Skeleton k;
        k.prim = skelPrim;
        k.path = skelPrim->Path();
        Value v;
        if (skelPrim->GetAttribute("joints").GetDefault(v)) k.joints = v.strs;
        const size_t n = k.joints.size();
        std::unordered_map<std::string, int> byName;
        for (size_t i = 0; i < n; ++i) byName[k.joints[i]] = int(i);
        k.parents.assign(n, -1);
        for (size_t i = 0; i < n; ++i) {
            const std::string& j = k.joints[i];
            size_t slash = j.rfind('/');
            if (slash != std::string::npos) {
                auto p = byName.find(j.substr(0, slash));
                if (p != byName.end()) k.parents[i] = p->second;
            }
        }
        Value bindV, restV;
        bool haveBind = skelPrim->GetAttribute("bindTransforms").GetDefault(bindV) && bindV.nums.size() == n * 16;
        k.hasRest = skelPrim->GetAttribute("restTransforms").GetDefault(restV) && restV.nums.size() == n * 16;
        if (k.hasRest)
            for (size_t i = 0; i < n; ++i) k.rest.push_back(MatrixFromValue(restV, i));
        k.bind.resize(n, Mat4::Identity());
        if (haveBind) {
            for (size_t i = 0; i < n; ++i) k.bind[i] = MatrixFromValue(bindV, i);
        } else if (k.hasRest) {
            Warn("skeleton <" + k.path + "> has no bindTransforms; using its rest pose");
            for (size_t i = 0; i < n; ++i)
                k.bind[i] = k.parents[i] >= 0 && size_t(k.parents[i]) < i ? k.rest[i] * k.bind[size_t(k.parents[i])] : k.rest[i];
        }
        for (auto& b : k.bind) k.bindInverse.push_back(b.Inverse());

        if (const Prim* anim = InheritedTarget(skelPrim, "skel:animationSource")) {
            if (anim->TypeName() == "SkelAnimation") {
                k.anim = anim;
                Value aj;
                if (anim->GetAttribute("joints").GetDefault(aj))
                    for (auto& name : aj.strs) {
                        auto p = byName.find(name);
                        k.animToSkel.push_back(p == byName.end() ? -1 : p->second);
                    }
                k.translations = anim->GetAttribute("translations");
                k.rotations = anim->GetAttribute("rotations");
                k.scales = anim->GetAttribute("scales");
                k.blendShapeWeights = anim->GetAttribute("blendShapeWeights");
                Value bs;
                if (anim->GetAttribute("blendShapes").GetDefault(bs)) k.animBlendShapes = bs.strs;
                for (auto* a : {&k.translations, &k.rotations, &k.scales, &k.blendShapeWeights}) NoteTimes(*a);
            } else {
                Warn("animation source <" + anim->Path() + "> is not a SkelAnimation");
            }
        }
        k.restTRS.resize(n);
        for (size_t i = 0; i < n; ++i) {
            const int p = k.parents[i];
            const Mat4 local = k.hasRest ? k.rest[i] : (p >= 0 ? k.bind[i] * k.bindInverse[size_t(p)] : k.bind[i]);
            k.restTRS[i] = TRS::FromMatrix(local);
        }
        k.valid = n > 0;
        k.skelSpace = k.bind;
        s.skeletons.push_back(std::move(k));
        int idx = int(s.skeletons.size() - 1);
        skeletonIndex[skelPrim->Path()] = idx;
        return idx;
    }

    void SetupSkinning(const Prim* prim, Mesh& m) {
        if (!prim->HasProperty("primvars:skel:jointIndices") && !prim->HasAPI("SkelBindingAPI")) return;
        const Prim* skelPrim = InheritedTarget(prim, "skel:skeleton");
        if (!skelPrim) return;
        if (skelPrim->TypeName() != "Skeleton") {
            Warn("<" + prim->Path() + "> is bound to <" + skelPrim->Path() + ">, which is not a Skeleton");
            return;
        }
        int si = GetSkeleton(skelPrim);
        const Skeleton& k = s.skeletons[size_t(si)];
        if (!k.valid) return;

        Primvar ji, jw;
        if (!ReadPrimvar(prim, "primvars:skel:jointIndices", t0, ji, "vertex") ||
            !ReadPrimvar(prim, "primvars:skel:jointWeights", t0, jw, "vertex")) {
            // Blend shapes without joint influences still need the skeleton's animation.
            if (prim->HasProperty("skel:blendShapes")) m.skeleton = si;
            return;
        }
        // Joint order used by the indices: the mesh's skel:joints, or the skeleton's order.
        Value jointNames;
        bool haveMeshJoints = false;
        for (const Prim* p = prim; p && p->Path() != "/" && !haveMeshJoints; p = p->Parent()) {
            Attribute a = p->GetAttribute("skel:joints");
            haveMeshJoints = a.IsValid() && a.GetDefault(jointNames) && !jointNames.strs.empty();
        }
        if (haveMeshJoints) {
            for (auto& name : jointNames.strs) {
                auto it = std::find(k.joints.begin(), k.joints.end(), name);
                m.paletteJoints.push_back(it == k.joints.end() ? -1 : int(it - k.joints.begin()));
            }
        } else {
            for (size_t i = 0; i < k.joints.size(); ++i) m.paletteJoints.push_back(int(i));
        }
        const int unskinned = int(m.paletteJoints.size());  // extra entry: geomBind * skeleton world
        m.paletteJoints.push_back(-1);

        const int N = std::max(1, ji.elementSize);
        const bool constant = ji.interpolation == "constant";
        if (!constant && ji.values.nums.size() < size_t(m.pointCount) * N) {
            Warn("<" + prim->Path() + ">: joint influence count does not match its points");
            m.paletteJoints.clear();
            return;
        }
        m.jointIndices.assign(size_t(m.pointCount) * 4, 0);
        m.jointWeights.assign(size_t(m.pointCount) * 4, 0.0f);
        std::vector<std::pair<float, int>> infl(static_cast<size_t>(N));
        for (uint32_t p = 0; p < m.pointCount; ++p) {
            size_t base = constant ? 0 : size_t(p) * N;
            for (int i = 0; i < N; ++i) {
                double idx = base + i < ji.values.nums.size() ? ji.values.nums[base + i] : -1;
                double w = base + i < jw.values.nums.size() ? jw.values.nums[base + i] : 0;
                if (idx < 0 || idx >= unskinned) w = 0;
                infl[size_t(i)] = {float(w), int(idx)};
            }
            std::partial_sort(infl.begin(), infl.begin() + std::min(4, N), infl.end(),
                              [](auto& a, auto& b) { return a.first > b.first; });
            float sum = 0;
            for (int i = 0; i < std::min(4, N); ++i) sum += std::max(0.0f, infl[size_t(i)].first);
            if (sum <= 0) {
                m.jointIndices[p * 4] = uint16_t(unskinned);
                m.jointWeights[p * 4] = 1.0f;
                continue;
            }
            for (int i = 0; i < std::min(4, N); ++i) {
                float w = std::max(0.0f, infl[size_t(i)].first) / sum;
                m.jointIndices[p * 4 + i] = uint16_t(w > 0 ? infl[size_t(i)].second : 0);
                m.jointWeights[p * 4 + i] = w;
            }
        }
        Value gb;
        Attribute gba = prim->GetAttribute("primvars:skel:geomBindTransform");
        if (gba.IsValid() && gba.Get(t0, gb) && gb.nums.size() >= 16) m.geomBind = MatrixFromValue(gb);
        m.skeleton = si;
    }

    void SetupBlendShapes(const Prim* prim, Mesh& m) {
        if (m.skeleton < 0) return;
        Value names;
        if (!prim->GetAttribute("skel:blendShapes").GetDefault(names) || names.strs.empty()) return;
        auto targets = prim->GetTargets("skel:blendShapeTargets");
        const Skeleton& k = s.skeletons[size_t(m.skeleton)];
        for (size_t i = 0; i < names.strs.size() && i < targets.size(); ++i) {
            const Prim* bsPrim = stage.GetPrim(targets[i]);
            if (!bsPrim) continue;
            BlendShape b;
            b.name = names.strs[i];
            Value offsets, indices;
            if (!bsPrim->GetAttribute("offsets").GetDefault(offsets)) continue;
            b.offsets = ToFloats(offsets);
            if (bsPrim->GetAttribute("pointIndices").GetDefault(indices))
                for (double d : indices.nums) b.pointIndices.push_back(uint32_t(d));
            size_t count = b.pointIndices.empty() ? m.pointCount : b.pointIndices.size();
            if (b.offsets.size() < count * 3) {
                Warn("blend shape <" + bsPrim->Path() + "> has too few offsets");
                continue;
            }
            auto it = std::find(k.animBlendShapes.begin(), k.animBlendShapes.end(), b.name);
            b.channel = it == k.animBlendShapes.end() ? -1 : int(it - k.animBlendShapes.begin());
            m.blendShapes.push_back(std::move(b));
        }
    }

    // ---- meshes ----

    void Finish(Mesh& m, const ProtoCtx& ctx) {
        if (ctx.protoIndex >= 0) {
            m.instancer = ctx.instancer;
            m.protoIndex = ctx.protoIndex;
            m.protoParentNode = ctx.protoParentNode;
            if (m.skeleton >= 0) {
                Warn("skinned meshes inside PointInstancer prototypes are drawn unskinned (" + m.path + ")");
                m.skeleton = -1;
                m.jointIndices.clear();
                m.jointWeights.clear();
                m.paletteJoints.clear();
                m.blendShapes.clear();
            }
        }
        s.triangleCount += m.cornerPoint.size() / 3;
        s.meshes.push_back(std::move(m));
    }

    void BuildMesh(const Prim* prim, int node, const ProtoCtx& ctx) {
        Mesh m;
        m.prim = prim;
        m.path = prim->Path();
        m.node = node;
        Value v;
        Attribute pointsAttr = prim->GetAttribute("points");
        if (!pointsAttr.Get(t0, v) || v.nums.size() < 9) return;
        m.restPoints = ToFloats(v);
        m.pointCount = uint32_t(m.restPoints.size() / 3);
        if (pointsAttr.IsAnimated()) {
            m.pointsAttr = pointsAttr;
            NoteTimes(pointsAttr);
        }
        Value countsV, indicesV;
        if (!prim->GetAttribute("faceVertexCounts").Get(t0, countsV) ||
            !prim->GetAttribute("faceVertexIndices").Get(t0, indicesV))
            return;
        std::vector<int> counts = ToInts(countsV), fvi = ToInts(indicesV);
        const bool leftHanded = Token(prim, "orientation", "rightHanded") == "leftHanded";
        m.smoothNormals = Token(prim, "subdivisionScheme", "catmullClark") != "none";
        m.doubleSided = Number(prim, "doubleSided", 0) != 0;
        std::unordered_set<int> holes;
        {
            Value h;
            if (prim->GetAttribute("holeIndices").Get(t0, h))
                for (double d : h.nums) holes.insert(int(d));
        }

        // Materials: the mesh binding plus GeomSubset face bindings.
        const size_t faceCount = counts.size();
        int meshMaterial = ResolveBinding(prim, true);
        std::vector<int> faceMaterial(faceCount, meshMaterial);
        for (auto& c : prim->Children()) {
            if (c->TypeName() != "GeomSubset" || !c->IsActive()) continue;
            if (Token(c.get(), "elementType", "face") != "face") continue;
            int mat = ResolveBinding(c.get(), false);
            if (mat < 0) continue;
            Value idx;
            if (!c->GetAttribute("indices").Get(t0, idx)) continue;
            for (double d : idx.nums)
                if (d >= 0 && size_t(d) < faceCount) faceMaterial[size_t(d)] = mat;
        }
        std::string uvSet;
        for (int mi : faceMaterial)
            if (mi >= 0 && uvSet.empty()) uvSet = s.materials[size_t(mi)].uvSet;
        if (meshMaterial >= 0 && uvSet.empty()) uvSet = s.materials[size_t(meshMaterial)].uvSet;

        // Primvars.
        Primvar normals, uvs, color, opacity;
        bool haveNormals = ReadPrimvar(prim, "primvars:normals", t0, normals, "vertex") ||
                           ReadPrimvar(prim, "normals", t0, normals, "vertex");
        if (haveNormals && normals.tuple != 3) haveNormals = false;
        if (m.pointsAttr.IsAnimated()) haveNormals = false;  // recomputed every frame
        bool haveUV = false;
        for (const std::string& name : {uvSet.empty() ? std::string("st") : uvSet, std::string("st")}) {
            if (ReadPrimvar(prim, "primvars:" + name, t0, uvs, "faceVarying") && uvs.tuple >= 2) {
                haveUV = true;
                break;
            }
        }
        if (!haveUV) {  // any 2D texture coordinate primvar
            for (const std::string& pname : prim->PropertyNames()) {
                if (!StartsWith(pname, "primvars:") || pname.find(":indices") != std::string::npos) continue;
                Attribute a = prim->GetAttribute(pname);
                if (StartsWith(a.typeName, "texCoord2") && ReadPrimvar(prim, pname, t0, uvs, "faceVarying")) {
                    haveUV = true;
                    break;
                }
            }
        }
        bool haveColor = ReadPrimvar(prim, "primvars:displayColor", t0, color, "constant") && color.tuple >= 3;
        bool haveOpacity = ReadPrimvar(prim, "primvars:displayOpacity", t0, opacity, "constant");

        // Triangulate (fan) and group triangles by material.
        struct Tri {
            uint32_t face;
            uint32_t c[3];
        };
        std::vector<Tri> tris;
        tris.reserve(fvi.size());
        uint32_t corner = 0;
        for (size_t f = 0; f < faceCount; ++f) {
            const int n = counts[f];
            if (n < 0 || size_t(corner) + size_t(n) > fvi.size()) {
                Warn("<" + m.path + "> has invalid faceVertexCounts");
                break;
            }
            if (n >= 3 && !holes.count(int(f))) {
                for (int k = 1; k + 1 < n; ++k) {
                    Tri t{uint32_t(f), {corner, corner + uint32_t(k), corner + uint32_t(k) + 1}};
                    if (leftHanded) std::swap(t.c[1], t.c[2]);
                    tris.push_back(t);
                }
            }
            corner += uint32_t(n);
        }
        std::vector<int> mats;
        for (const Tri& t : tris) {
            int mat = faceMaterial[t.face];
            if (std::find(mats.begin(), mats.end(), mat) == mats.end()) mats.push_back(mat);
        }
        std::stable_sort(tris.begin(), tris.end(),
                         [&](const Tri& a, const Tri& b) {
                             return std::find(mats.begin(), mats.end(), faceMaterial[a.face]) <
                                    std::find(mats.begin(), mats.end(), faceMaterial[b.face]);
                         });

        bool badNormals = false;
        const size_t nc = tris.size() * 3;
        m.cornerPoint.reserve(nc);
        if (haveNormals) m.normals.reserve(nc * 3);
        if (haveUV) m.uvs.reserve(nc * 2);
        bool needColor = haveColor || haveOpacity;
        if (needColor) m.colors.reserve(nc * 4);
        for (size_t ti = 0; ti < tris.size(); ++ti) {
            const Tri& t = tris[ti];
            int mat = faceMaterial[t.face];
            if (m.subsets.empty() || m.subsets.back().material != (mat < 0 ? 0 : mat)) {
                SubMesh sm;
                sm.first = uint32_t(m.cornerPoint.size());
                sm.material = mat < 0 ? 0 : mat;
                m.subsets.push_back(sm);
            }
            bool valid = true;
            for (int k = 0; k < 3; ++k)
                if (uint32_t(fvi[t.c[k]]) >= m.pointCount || fvi[t.c[k]] < 0) valid = false;
            if (!valid) continue;
            for (int k = 0; k < 3; ++k) {
                const uint32_t c = t.c[k];
                const uint32_t p = uint32_t(fvi[c]);
                m.cornerPoint.push_back(p);
                if (haveNormals) {
                    const double* n = PrimvarAt(normals, t.face, p, c);
                    if (!n) badNormals = true;
                    m.normals.insert(m.normals.end(), {n ? float(n[0]) : 0.f, n ? float(n[1]) : 0.f, n ? float(n[2]) : 1.f});
                }
                if (haveUV) {
                    const double* uv = PrimvarAt(uvs, t.face, p, c);
                    m.uvs.insert(m.uvs.end(), {uv ? float(uv[0]) : 0.f, uv ? float(uv[1]) : 0.f});
                }
                if (needColor) {
                    const double* col = haveColor ? PrimvarAt(color, t.face, p, c) : nullptr;
                    const double* op = haveOpacity ? PrimvarAt(opacity, t.face, p, c) : nullptr;
                    m.colors.insert(m.colors.end(), {col ? float(col[0]) : 0.6f, col ? float(col[1]) : 0.6f,
                                                     col ? float(col[2]) : 0.6f, op ? float(op[0]) : 1.f});
                }
            }
            m.subsets.back().count += 3;
        }
        if (badNormals) {
            Warn("<" + m.path + "> has normals that do not match its topology; recomputing them");
            m.normals.clear();
        }
        if (m.cornerPoint.empty()) return;

        SetupSkinning(prim, m);
        SetupBlendShapes(prim, m);
        Finish(m, ctx);
    }

    void BuildShape(const Prim* prim, int node, const ProtoCtx& ctx, const std::string& type) {
        ShapeBuilder b;
        std::string axis = Token(prim, "axis", "Z");
        if (type == "Cube") {
            double h = Number(prim, "size", 2.0) / 2;
            for (int f = 0; f < 6; ++f) {
                int ax = f / 2;
                double sgn = (f % 2) ? -1.0 : 1.0;
                Vec3 n{0, 0, 0}, u{0, 0, 0}, v{0, 0, 0};
                (&n.x)[ax] = sgn;
                (&u.x)[(ax + 1) % 3] = 1;
                (&v.x)[(ax + 2) % 3] = 1;
                if (sgn < 0) std::swap(u, v);
                Vec3 c = n * h;
                Vec3 p[4] = {c - u * h - v * h, c + u * h - v * h, c + u * h + v * h, c - u * h + v * h};
                Vec3 ns[4] = {n, n, n, n};
                double uv[4][2] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
                b.Quad(p, ns, uv);
            }
            axis = "Z";
        } else if (type == "Sphere") {
            double r = Number(prim, "radius", 1.0);
            std::vector<std::array<double, 4>> prof;
            for (int i = 0; i <= 24; ++i) {
                double a = -kPi / 2 + kPi * i / 24;
                prof.push_back({r * std::cos(a), r * std::sin(a), std::cos(a), std::sin(a)});
            }
            Revolve(b, prof, 48);
        } else if (type == "Plane") {
            double w = Number(prim, "width", 2.0) / 2, l = Number(prim, "length", 2.0) / 2;
            Vec3 p[4] = {{-w, -l, 0}, {w, -l, 0}, {w, l, 0}, {-w, l, 0}};
            Vec3 n[4] = {{0, 0, 1}, {0, 0, 1}, {0, 0, 1}, {0, 0, 1}};
            double uv[4][2] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
            b.Quad(p, n, uv);
        } else {
            double r = Number(prim, "radius", type == "Capsule" ? 0.5 : 1.0);
            double h = Number(prim, "height", type == "Capsule" ? 1.0 : 2.0) / 2;
            std::vector<std::array<double, 4>> prof;
            if (type == "Cylinder") {
                prof = {{0, -h, 0, -1}, {r, -h, 0, -1}, {r, -h, 1, 0}, {r, h, 1, 0}, {r, h, 0, 1}, {0, h, 0, 1}};
            } else if (type == "Cone") {
                double len = std::sqrt(r * r + 4 * h * h);
                double nr = 2 * h / len, nz = r / len;
                prof = {{0, -h, 0, -1}, {r, -h, 0, -1}, {r, -h, nr, nz}, {0, h, nr, nz}};
            } else {  // Capsule: hemispheres on both ends
                for (int i = 0; i <= 12; ++i) {
                    double a = -kPi / 2 + (kPi / 2) * i / 12;
                    prof.push_back({r * std::cos(a), -h + r * std::sin(a), std::cos(a), std::sin(a)});
                }
                for (int i = 0; i <= 12; ++i) {
                    double a = (kPi / 2) * i / 12;
                    prof.push_back({r * std::cos(a), h + r * std::sin(a), std::cos(a), std::sin(a)});
                }
            }
            Revolve(b, prof, 48);
        }
        // Shapes are built around +Z; rotate to the requested axis.
        auto toAxis = [&](float* v) {
            float x = v[0], y = v[1], z = v[2];
            if (axis == "X") { v[0] = z; v[1] = x; v[2] = y; }
            else if (axis == "Y") { v[0] = y; v[1] = z; v[2] = x; }
        };
        for (size_t i = 0; i < b.points.size(); i += 3) {
            toAxis(&b.points[i]);
            toAxis(&b.normals[i]);
        }

        Mesh m;
        m.prim = prim;
        m.path = prim->Path();
        m.node = node;
        m.restPoints = std::move(b.points);
        m.pointCount = uint32_t(m.restPoints.size() / 3);
        m.normals = std::move(b.normals);
        m.uvs = std::move(b.uvs);
        m.doubleSided = Number(prim, "doubleSided", 0) != 0;
        m.cornerPoint.resize(m.pointCount);
        for (uint32_t i = 0; i < m.pointCount; ++i) m.cornerPoint[i] = i;
        Primvar color;
        if (ReadPrimvar(prim, "primvars:displayColor", t0, color, "constant") && color.tuple >= 3) {
            const double* c = color.values.nums.data();
            for (uint32_t i = 0; i < m.pointCount; ++i)
                m.colors.insert(m.colors.end(), {float(c[0]), float(c[1]), float(c[2]), 1.0f});
        }
        int mat = ResolveBinding(prim, true);
        m.subsets.push_back({0, m.pointCount, mat < 0 ? 0 : mat});
        Finish(m, ctx);
    }
};

// ---- Scene -----------------------------------------------------------------------------------

bool Scene::Build(const usd::Stage& st) {
    stage = &st;
    timeCodesPerSecond = st.timeCodesPerSecond;
    framesPerSecond = st.framesPerSecond;
    Builder b(*this, st);
    b.t0 = st.hasTimeRange ? st.startTimeCode : 0.0;

    Material fallback;
    fallback.name = "(display color)";
    fallback.usesVertexColor = true;
    fallback.diffuse[0] = fallback.diffuse[1] = fallback.diffuse[2] = 1.0f;
    fallback.roughness = 0.6f;
    materials.push_back(fallback);

    if (st.upAxis == "Z") upAxisFix = Mat4::RotateAxis(0, -90.0);

    b.Traverse(st.PseudoRoot(), -1, Builder::ProtoCtx());

    for (auto& k : skeletons) {
        auto it = b.nodeIndex.find(k.prim);
        k.node = it == b.nodeIndex.end() ? -1 : it->second;
    }

    if (st.hasTimeRange) {
        startTime = st.startTimeCode;
        endTime = st.endTimeCode;
    } else if (b.maxTime > b.minTime) {
        startTime = b.minTime;
        endTime = b.maxTime;
    } else {
        startTime = endTime = b.t0;
    }
    animated = endTime > startTime && b.maxTime > b.minTime;

    // Clip table written by the Yu-Gi-Oh! converter: customLayerData.ygoClips = {name: {start, end}} (seconds).
    if (const Value* cld = st.RootMetadata("customLayerData")) {
        if (const Value* clipsV = cld->Find("ygoClips"); clipsV && clipsV->dict) {
            for (auto& [name, c] : *clipsV->dict) {
                const Value* a = c.Find("start");
                const Value* e = c.Find("end");
                if (!a || !e || !a->IsNumber() || !e->IsNumber()) continue;
                clips.push_back({name, a->Num() * timeCodesPerSecond, e->Num() * timeCodesPerSecond});
            }
            std::sort(clips.begin(), clips.end(), [](const Clip& x, const Clip& y) { return x.start < y.start; });
        }
    }
    for (auto& w : st.warnings) warnings.push_back(w);
    firstEvaluation_ = true;
    Evaluate(startTime);
    return true;
}

// Evaluation samples the animated values into a Pose (optionally blending two of them for a crossfade), then
// derives world transforms, skeleton space, skinning palettes and deformed points from it.

void Scene::SamplePose(double time, Pose& p) const {
    const size_t nn = nodes.size();
    p.nodeLocal.resize(nn);
    p.nodeVisible.resize(nn);
    for (size_t i = 0; i < nn; ++i) {
        const Node& n = nodes[i];
        if (n.animated || firstEvaluation_) {
            Mat4 m = Mat4::Identity();
            for (size_t k = n.ops.size(); k-- > 0;) m = m * OpMatrix(n.ops[k], time);
            p.nodeLocal[i] = m;
        } else {
            p.nodeLocal[i] = n.local;  // static: computed on the first evaluation
        }
        Value vis;
        p.nodeVisible[i] = !(n.visibility.IsValid() && n.visibility.Get(time, vis) && vis.Str() == "invisible");
    }

    p.joints.resize(skeletons.size());
    p.weights.resize(skeletons.size());
    for (size_t s = 0; s < skeletons.size(); ++s) {
        const Skeleton& k = skeletons[s];
        std::vector<TRS>& joints = p.joints[s];
        if (!k.valid) {
            joints.clear();
            continue;
        }
        joints = k.restTRS;
        if (!k.anim) continue;
        Value T, R, S, W;
        const bool hasT = k.translations.Get(time, T), hasR = k.rotations.Get(time, R), hasS = k.scales.Get(time, S);
        for (size_t a = 0; a < k.animToSkel.size(); ++a) {
            const int j = k.animToSkel[a];
            if (j < 0 || size_t(j) >= joints.size()) continue;
            TRS& x = joints[size_t(j)];  // components the animation leaves out keep their rest values
            if (hasT && T.nums.size() >= a * 3 + 3)
                for (int i = 0; i < 3; ++i) x.t[i] = T.nums[a * 3 + size_t(i)];
            if (hasR && R.nums.size() >= a * 4 + 4) {
                const double* q = R.nums.data() + a * 4;
                const double len = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
                for (int i = 0; i < 4; ++i) x.q[i] = len > 0 ? q[i] / len : (i == 3 ? 1.0 : 0.0);
                for (double& c : x.s) c = std::abs(c);  // a mirrored rest pose does not survive a new rotation
            }
            if (hasS && S.nums.size() >= a * 3 + 3)
                for (int i = 0; i < 3; ++i) x.s[i] = S.nums[a * 3 + size_t(i)];
        }
        if (k.blendShapeWeights.Get(time, W)) p.weights[s] = ToFloats(W);
        else p.weights[s].clear();
    }

    p.points.resize(meshes.size());
    for (size_t i = 0; i < meshes.size(); ++i) {
        const Mesh& m = meshes[i];
        p.points[i].clear();
        Value v;
        if (m.pointsAttr.IsAnimated() && m.pointsAttr.Get(time, v) && v.nums.size() == m.restPoints.size())
            p.points[i] = ToFloats(v);
    }
}

void Scene::BlendPoses(const Pose& a, const Pose& b, double w, Pose& out) const {
    const size_t nn = b.nodeLocal.size();
    out.nodeLocal.resize(nn);
    for (size_t i = 0; i < nn; ++i) {
        const bool differ = i < a.nodeLocal.size() && std::memcmp(a.nodeLocal[i].m, b.nodeLocal[i].m, sizeof(b.nodeLocal[i].m)) != 0;
        out.nodeLocal[i] = differ ? TRS::Blend(TRS::FromMatrix(a.nodeLocal[i]), TRS::FromMatrix(b.nodeLocal[i]), w).ToMatrix()
                                  : b.nodeLocal[i];
    }
    out.nodeVisible = b.nodeVisible;  // visibility switches; it does not fade

    out.joints.resize(b.joints.size());
    for (size_t s = 0; s < b.joints.size(); ++s) {
        const std::vector<TRS>& jb = b.joints[s];
        if (s >= a.joints.size() || a.joints[s].size() != jb.size()) {
            out.joints[s] = jb;
            continue;
        }
        out.joints[s].resize(jb.size());
        for (size_t j = 0; j < jb.size(); ++j) out.joints[s][j] = TRS::Blend(a.joints[s][j], jb[j], w);
    }

    static const std::vector<float> none;
    auto lerp = [&](const std::vector<float>& x, const std::vector<float>& y, std::vector<float>& o) {
        if (x.size() != y.size() || y.empty()) {
            o = y;
            return;
        }
        o.resize(y.size());
        const float fw = float(w);
        for (size_t i = 0; i < y.size(); ++i) o[i] = x[i] + (y[i] - x[i]) * fw;
    };
    out.weights.resize(b.weights.size());
    for (size_t s = 0; s < b.weights.size(); ++s) lerp(s < a.weights.size() ? a.weights[s] : none, b.weights[s], out.weights[s]);
    out.points.resize(b.points.size());
    for (size_t i = 0; i < b.points.size(); ++i) lerp(i < a.points.size() ? a.points[i] : none, b.points[i], out.points[i]);
}

void Scene::ApplyPose(const Pose& p, double time) {
    currentTime = time;
    for (size_t i = 0; i < nodes.size(); ++i) {
        Node& n = nodes[i];
        n.local = p.nodeLocal[i];
        const Node* parent = n.parent >= 0 ? &nodes[size_t(n.parent)] : nullptr;
        n.world = (n.resetXformStack || !parent) ? n.local * RootTransform() : n.local * parent->world;
        n.visible = p.nodeVisible[i] && !n.userHidden && (parent ? parent->visible : !hidden);
    }

    for (size_t s = 0; s < skeletons.size(); ++s) {
        Skeleton& k = skeletons[s];
        if (!k.valid) continue;
        k.weights = p.weights[s];
        const std::vector<TRS>& joints = p.joints[s];
        const size_t n = std::min(k.joints.size(), joints.size());
        // Skeleton space: local * parent (joints are normally ordered parents first).
        k.skelSpace.assign(k.joints.size(), Mat4::Identity());
        std::vector<char> done(n, 0);
        std::function<void(size_t, int)> solve = [&](size_t j, int depth) {
            if (done[j]) return;
            const int parent = k.parents[j];
            const Mat4 local = joints[j].ToMatrix();
            if (parent >= 0 && size_t(parent) < n && depth < 1000) {
                solve(size_t(parent), depth + 1);
                k.skelSpace[j] = local * k.skelSpace[size_t(parent)];
            } else {
                k.skelSpace[j] = local;
            }
            done[j] = 1;
        };
        for (size_t j = 0; j < n; ++j) solve(j, 0);
    }

    EvaluateInstancers(time);
    for (size_t i = 0; i < meshes.size(); ++i) EvaluateMesh(meshes[i], p.points[i]);
    firstEvaluation_ = false;
}

void Scene::Evaluate(double time) {
    SamplePose(time, applied_);
    ApplyPose(applied_, time);
}

void Scene::EvaluateBlend(double fromTime, double time, double weight) {
    SamplePose(fromTime, sampleA_);
    SamplePose(time, sampleB_);
    BlendPoses(sampleA_, sampleB_, std::clamp(weight, 0.0, 1.0), applied_);
    ApplyPose(applied_, time);
}

void Scene::EvaluateBlend(const Pose& from, double time, double weight) {
    SamplePose(time, sampleB_);
    BlendPoses(from, sampleB_, std::clamp(weight, 0.0, 1.0), applied_);
    ApplyPose(applied_, time);
}

void Scene::EvaluateInstancers(double time) {
    for (auto& inst : instancers) {
        inst.protoInstances.assign(inst.prototypes.size(), {});
        Value ids, pos, ori, scl, inv, idv;
        if (!inst.prim->GetAttribute("protoIndices").Get(time, ids) || !inst.prim->GetAttribute("positions").Get(time, pos))
            continue;
        bool haveOri = inst.prim->GetAttribute("orientations").Get(time, ori) ||
                       inst.prim->GetAttribute("orientationsf").Get(time, ori);
        bool haveScl = inst.prim->GetAttribute("scales").Get(time, scl);
        std::unordered_set<long long> hiddenIds;
        if (inst.prim->GetAttribute("invisibleIds").Get(time, inv))
            for (double d : inv.nums) hiddenIds.insert((long long)d);
        bool haveIds = inst.prim->GetAttribute("ids").Get(time, idv);
        for (size_t i = 0; i < ids.nums.size() && i * 3 + 2 < pos.nums.size(); ++i) {
            long long id = haveIds && i < idv.nums.size() ? (long long)idv.nums[i] : (long long)i;
            if (hiddenIds.count(id)) continue;
            int proto = int(ids.nums[i]);
            if (proto < 0 || size_t(proto) >= inst.prototypes.size()) continue;
            Mat4 x = Mat4::Identity();
            if (haveScl && scl.nums.size() >= i * 3 + 3) x = Mat4::Scale(scl.nums[i * 3], scl.nums[i * 3 + 1], scl.nums[i * 3 + 2]);
            if (haveOri && ori.nums.size() >= i * 4 + 4)
                x = x * Mat4::FromQuat(ori.nums[i * 4], ori.nums[i * 4 + 1], ori.nums[i * 4 + 2], ori.nums[i * 4 + 3]);
            x = x * Mat4::Translate(pos.nums[i * 3], pos.nums[i * 3 + 1], pos.nums[i * 3 + 2]);
            inst.protoInstances[size_t(proto)].push_back(x);
        }
    }
}

void Scene::EvaluateMesh(Mesh& m, const std::vector<float>& sampledPoints) {
    const Node& node = nodes[size_t(m.node)];
    m.visible = node.visible;

    // CPU deformation: point animation and blend shapes.
    if (m.HasCpuDeformation()) {
        const bool sampled = !sampledPoints.empty() && sampledPoints.size() == m.restPoints.size();
        std::vector<float> pts = sampled ? sampledPoints : m.restPoints;
        bool changed = sampled;
        if (m.skeleton >= 0 && !m.blendShapes.empty()) {
            const Skeleton& k = skeletons[size_t(m.skeleton)];
            for (const BlendShape& b : m.blendShapes) {
                if (b.channel < 0 || size_t(b.channel) >= k.weights.size()) continue;
                const float w = k.weights[size_t(b.channel)];
                if (w == 0.0f) continue;
                changed = true;
                if (b.pointIndices.empty()) {
                    for (size_t i = 0; i < pts.size() && i < b.offsets.size(); ++i) pts[i] += w * b.offsets[i];
                } else {
                    for (size_t i = 0; i < b.pointIndices.size(); ++i) {
                        uint32_t p = b.pointIndices[i];
                        if (p >= m.pointCount) continue;
                        for (int c = 0; c < 3; ++c) pts[p * 3 + c] += w * b.offsets[i * 3 + size_t(c)];
                    }
                }
            }
        }
        if (changed || !m.points.empty()) {
            m.points = std::move(pts);
            if (m.normals.empty() || m.pointsAttr.IsAnimated()) ComputeNormals(m.points, m.cornerPoint, m.smoothNormals, m.cpuNormals);
            m.pointsDirty = true;
        }
    }

    // Skinning palette (row-vector matrices taking mesh points to world space).
    if (m.skeleton >= 0 && !m.paletteJoints.empty()) {
        const Skeleton& k = skeletons[size_t(m.skeleton)];
        const Mat4 skelWorld = k.node >= 0 ? nodes[size_t(k.node)].world : RootTransform();
        m.palette.resize(m.paletteJoints.size());
        for (size_t i = 0; i < m.paletteJoints.size(); ++i) {
            int j = m.paletteJoints[i];
            m.palette[i] = (j >= 0 && size_t(j) < k.skelSpace.size())
                               ? m.geomBind * k.bindInverse[size_t(j)] * k.skelSpace[size_t(j)] * skelWorld
                               : m.geomBind * skelWorld;
        }
    } else {
        m.palette.assign(1, node.world);
    }

    if (m.instancer >= 0) {
        const Instancer& inst = instancers[size_t(m.instancer)];
        const Mat4 relative = node.world * nodes[size_t(m.protoParentNode)].world.Inverse();
        const Mat4& instWorld = nodes[size_t(inst.node)].world;
        m.instances.clear();
        if (size_t(m.protoIndex) < inst.protoInstances.size())
            for (const Mat4& x : inst.protoInstances[size_t(m.protoIndex)]) m.instances.push_back(relative * x * instWorld);
    }
}

PoseDifference ComparePoses(const Pose& a, const Pose& b) {
    PoseDifference d;
    auto add = [&](const TRS& x, const TRS& y) {
        double dot = 0, dist = 0, len = 0;
        for (int i = 0; i < 4; ++i) dot += x.q[i] * y.q[i];
        for (int i = 0; i < 3; ++i) {
            dist += (x.t[i] - y.t[i]) * (x.t[i] - y.t[i]);
            len += y.t[i] * y.t[i];
        }
        d.angle = std::max(d.angle, 2 * std::acos(std::min(1.0, std::abs(dot))));
        d.distance = std::max(d.distance, std::sqrt(dist));
        d.extent = std::max(d.extent, std::sqrt(len));
    };
    for (size_t i = 0; i < a.nodeLocal.size() && i < b.nodeLocal.size(); ++i)
        if (std::memcmp(a.nodeLocal[i].m, b.nodeLocal[i].m, sizeof(b.nodeLocal[i].m)) != 0)
            add(TRS::FromMatrix(a.nodeLocal[i]), TRS::FromMatrix(b.nodeLocal[i]));
    for (size_t s = 0; s < a.joints.size() && s < b.joints.size(); ++s)
        if (a.joints[s].size() == b.joints[s].size())
            for (size_t j = 0; j < b.joints[s].size(); ++j) add(a.joints[s][j], b.joints[s][j]);
    for (size_t s = 0; s < a.weights.size() && s < b.weights.size(); ++s)
        if (a.weights[s].size() == b.weights[s].size())
            for (size_t i = 0; i < b.weights[s].size(); ++i) d.weight = std::max(d.weight, double(std::abs(a.weights[s][i] - b.weights[s][i])));
    for (size_t m = 0; m < a.points.size() && m < b.points.size(); ++m) {
        const std::vector<float>& x = a.points[m];
        const std::vector<float>& y = b.points[m];
        if (x.size() != y.size()) continue;
        for (size_t i = 0; i + 2 < y.size(); i += 3) {
            const double dx = x[i] - y[i], dy = x[i + 1] - y[i + 1], dz = x[i + 2] - y[i + 2];
            d.distance = std::max(d.distance, std::sqrt(dx * dx + dy * dy + dz * dz));
            d.extent = std::max(d.extent, double(std::max({std::abs(y[i]), std::abs(y[i + 1]), std::abs(y[i + 2])})));
        }
    }
    return d;
}

void Scene::ComputeWorldPoints(const Mesh& m, std::vector<float>& out) const {
    const std::vector<float>& pts = m.CurrentPoints();
    out.resize(pts.size());
    const bool skinned = m.skeleton >= 0 && !m.jointWeights.empty() && m.palette.size() > 1;
    for (uint32_t p = 0; p < m.pointCount; ++p) {
        Vec3 v{pts[p * 3], pts[p * 3 + 1], pts[p * 3 + 2]};
        Vec3 r;
        if (skinned) {
            for (int k = 0; k < 4; ++k) {
                float w = m.jointWeights[p * 4 + size_t(k)];
                if (w <= 0) continue;
                size_t j = m.jointIndices[p * 4 + size_t(k)];
                if (j < m.palette.size()) r = r + m.palette[j].TransformPoint(v) * w;
            }
        } else {
            r = m.palette.empty() ? v : m.palette[0].TransformPoint(v);
        }
        out[p * 3] = float(r.x);
        out[p * 3 + 1] = float(r.y);
        out[p * 3 + 2] = float(r.z);
    }
}

Bounds Scene::ComputeBindPoseBounds() const {
    Bounds b;
    for (const Mesh& m : meshes) {
        if (!m.visible) continue;
        std::vector<Mat4> xforms;
        if (m.instancer >= 0) {
            xforms = m.instances;
        } else if (m.skeleton >= 0 && !m.paletteJoints.empty()) {
            const Skeleton& k = skeletons[size_t(m.skeleton)];
            xforms.push_back(m.geomBind * (k.node >= 0 ? nodes[size_t(k.node)].world : RootTransform()));
        } else if (!m.palette.empty()) {
            xforms.push_back(m.palette[0]);
        }
        for (const Mat4& x : xforms)
            for (size_t i = 0; i + 2 < m.restPoints.size(); i += 3)
                b.Add(x.TransformPoint({m.restPoints[i], m.restPoints[i + 1], m.restPoints[i + 2]}));
    }
    return b;
}

Bounds Scene::ComputeBounds(const std::vector<char>* meshMask) const {
    Bounds b;
    std::vector<float> pts;
    for (size_t mi = 0; mi < meshes.size(); ++mi) {
        const Mesh& m = meshes[mi];
        if (!m.visible) continue;
        if (meshMask && (mi >= meshMask->size() || !(*meshMask)[mi])) continue;
        if (m.instancer >= 0) {
            for (const Mat4& x : m.instances)
                for (size_t i = 0; i + 2 < m.restPoints.size(); i += 3)
                    b.Add(x.TransformPoint({m.restPoints[i], m.restPoints[i + 1], m.restPoints[i + 2]}));
            continue;
        }
        ComputeWorldPoints(m, pts);
        for (size_t i = 0; i + 2 < pts.size(); i += 3) b.Add(Vec3{pts[i], pts[i + 1], pts[i + 2]});
    }
    return b;
}

}  // namespace scene
