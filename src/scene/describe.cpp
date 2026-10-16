#include "scene/scene.h"

#include <cstdarg>
#include <cstdio>
#include <functional>

namespace scene {

namespace {

std::string Num(double v) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.6g", v);
    return buf;
}

std::string Fmt(const char* fmt, ...) {
    char buf[1024];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    return buf;
}

}  // namespace

std::string SummarizeValue(const usd::Value& v, size_t maxElements) {
    using K = usd::Value::Kind;
    std::string out;
    switch (v.kind) {
        case K::None: return "(no value)";
        case K::Blocked: return "None";
        case K::Number: {
            const size_t tuple = v.tupleSize ? v.tupleSize : 1;
            const size_t count = v.nums.size() / tuple;
            if (v.isArray) out = "[" + std::to_string(count) + "] ";
            for (size_t i = 0; i < count && i < (v.isArray ? maxElements : 1); ++i) {
                if (i) out += ", ";
                if (tuple > 1) out += "(";
                for (size_t k = 0; k < tuple; ++k) {
                    if (k) out += ", ";
                    out += Num(v.nums[i * tuple + k]);
                }
                if (tuple > 1) out += ")";
            }
            if (v.isArray && count > maxElements) out += ", ...";
            return out;
        }
        case K::String:
        case K::Token:
        case K::Asset:
        case K::Path: {
            const char* open = v.kind == K::String ? "\"" : v.kind == K::Asset ? "@" : v.kind == K::Path ? "<" : "";
            const char* close = v.kind == K::String ? "\"" : v.kind == K::Asset ? "@" : v.kind == K::Path ? ">" : "";
            if (v.isArray) out = "[" + std::to_string(v.strs.size()) + "] ";
            for (size_t i = 0; i < v.strs.size() && i < maxElements; ++i) {
                if (i) out += ", ";
                out += open + v.strs[i] + close;
            }
            if (v.strs.size() > maxElements) out += ", ...";
            return out;
        }
        case K::Dictionary: {
            out = "{";
            size_t i = 0;
            if (v.dict)
                for (auto& [k, val] : *v.dict) {
                    if (i++) out += ", ";
                    if (i > maxElements) {
                        out += "...";
                        break;
                    }
                    out += k + ": " + SummarizeValue(val, 3);
                }
            return out + "}";
        }
        case K::References: {
            if (v.refs)
                for (size_t i = 0; i < v.refs->size(); ++i) {
                    const usd::Reference& r = (*v.refs)[i];
                    if (i) out += ", ";
                    if (!r.assetPath.empty()) out += "@" + r.assetPath + "@";
                    if (!r.primPath.empty()) out += "<" + r.primPath + ">";
                }
            return out;
        }
        case K::TimeSamples:
            return std::to_string(v.samples ? v.samples->size() : 0) + " time samples";
    }
    return out;
}

std::string Describe(const usd::Stage& stage, const Scene& sc) {
    std::string s;
    s += "File: " + std::string(stage.RootLayer().location.Describe()) + "\r\n\r\n";
    s += "Layers:\r\n";
    for (auto& l : stage.Layers())
        s += Fmt("  %s  (%s %s, %zu specs)\r\n", l->location.Describe().c_str(), l->format.c_str(), l->version.c_str(),
                 l->specs.size());
    s += "\r\nStage:\r\n";
    s += Fmt("  upAxis = %s, metersPerUnit = %s\r\n", stage.upAxis.c_str(), Num(stage.metersPerUnit).c_str());
    s += Fmt("  timeCodesPerSecond = %s, framesPerSecond = %s\r\n", Num(stage.timeCodesPerSecond).c_str(),
             Num(stage.framesPerSecond).c_str());
    if (stage.hasTimeRange)
        s += Fmt("  startTimeCode = %s, endTimeCode = %s\r\n", Num(stage.startTimeCode).c_str(), Num(stage.endTimeCode).c_str());
    if (const usd::Value* dp = stage.RootMetadata("defaultPrim")) s += "  defaultPrim = " + dp->Str() + "\r\n";
    if (const usd::Value* doc = stage.RootMetadata("documentation")) s += "  doc = " + doc->Str() + "\r\n";

    size_t prims = 0;
    std::function<void(const usd::Prim*)> count = [&](const usd::Prim* p) {
        for (auto& c : p->Children()) {
            ++prims;
            count(c.get());
        }
    };
    count(stage.PseudoRoot());
    s += Fmt("\r\nScene: %zu prims, %zu meshes, %zu triangles, %zu materials, %zu textures, %zu skeletons\r\n", prims,
             sc.meshes.size(), sc.triangleCount, sc.materials.size() - 1, sc.images.size(), sc.skeletons.size());
    if (sc.animated)
        s += Fmt("Animation: time codes %s to %s (%.2f s at %s time codes per second)\r\n", Num(sc.startTime).c_str(),
                 Num(sc.endTime).c_str(), (sc.endTime - sc.startTime) / sc.timeCodesPerSecond,
                 Num(sc.timeCodesPerSecond).c_str());
    else
        s += "Animation: none\r\n";

    if (!sc.clips.empty()) {
        s += "\r\nClips:\r\n";
        for (auto& c : sc.clips)
            s += Fmt("  %-32s %8s - %-8s (%.2f s)\r\n", c.name.c_str(), Num(c.start).c_str(), Num(c.end).c_str(),
                     (c.end - c.start) / sc.timeCodesPerSecond);
    }
    if (!sc.skeletons.empty()) {
        s += "\r\nSkeletons:\r\n";
        for (auto& k : sc.skeletons)
            s += Fmt("  %s: %zu joints, animation %s\r\n", k.path.c_str(), k.joints.size(),
                     k.anim ? k.anim->Path().c_str() : "(none)");
    }
    s += "\r\nMeshes:\r\n";
    for (auto& m : sc.meshes) {
        std::string flags;
        if (m.skeleton >= 0) flags += ", skinned";
        if (!m.blendShapes.empty()) flags += ", " + std::to_string(m.blendShapes.size()) + " blend shapes";
        if (m.pointsAttr.IsAnimated()) flags += ", point animation";
        if (m.instancer >= 0) flags += ", instanced";
        if (m.normals.empty()) flags += m.smoothNormals ? ", smooth normals (computed)" : ", flat normals (computed)";
        if (m.uvs.empty()) flags += ", no texture coordinates";
        if (m.doubleSided) flags += ", double-sided";
        s += Fmt("  %s: %u points, %zu triangles%s\r\n", m.path.c_str(), m.pointCount, m.cornerPoint.size() / 3, flags.c_str());
        for (auto& sub : m.subsets)
            s += Fmt("      %u triangles -> %s\r\n", sub.count / 3,
                     sub.material == 0 ? "(display color)" : sc.materials[size_t(sub.material)].path.c_str());
    }
    if (sc.materials.size() > 1) {
        s += "\r\nMaterials:\r\n";
        for (size_t i = 1; i < sc.materials.size(); ++i) {
            const Material& m = sc.materials[i];
            auto tex = [&](const TextureSlot& t) {
                return t.image >= 0 ? sc.images[size_t(t.image)].name : std::string();
            };
            s += "  " + m.path + "\r\n";
            if (m.usesVertexColor) s += "      uses display color\r\n";
            s += m.diffuseTex.image >= 0 ? "      diffuse: " + tex(m.diffuseTex) + "\r\n"
                                          : Fmt("      diffuse: (%.3g, %.3g, %.3g)\r\n", m.diffuse[0], m.diffuse[1], m.diffuse[2]);
            s += Fmt("      metallic %s, roughness %s, opacity %s", m.metallicTex.image >= 0 ? "textured" : Num(m.metallic).c_str(),
                     m.roughnessTex.image >= 0 ? "textured" : Num(m.roughness).c_str(),
                     m.opacityTex.image >= 0 ? tex(m.opacityTex).c_str() : Num(m.opacity).c_str());
            if (m.opacityThreshold > 0) s += Fmt(" (cut-out at %s)", Num(m.opacityThreshold).c_str());
            else if (m.IsTranslucent()) s += " (blended)";
            s += "\r\n";
            if (m.normalTex.image >= 0) s += "      normal map: " + tex(m.normalTex) + "\r\n";
            if (m.emissiveTex.image >= 0) s += "      emissive: " + tex(m.emissiveTex) + "\r\n";
            if (m.occlusionTex.image >= 0) s += "      occlusion: " + tex(m.occlusionTex) + "\r\n";
        }
    }
    if (!sc.images.empty()) {
        s += "\r\nTextures:\r\n";
        for (auto& img : sc.images) s += "  " + img.location.Describe() + "\r\n";
    }
    if (!sc.warnings.empty()) {
        s += "\r\nWarnings:\r\n";
        for (auto& w : sc.warnings) s += "  " + w + "\r\n";
    }
    return s;
}

}  // namespace scene
