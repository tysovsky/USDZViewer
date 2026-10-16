// usdzinfo: command line inspector for USD/USDZ files.
//
//   usdzinfo FILE                         summary: layers, stage metadata, prim tree and scene contents
//   usdzinfo FILE --dump-layer OUT.json   every spec/field of the root layer as JSON (parser tests)
//   usdzinfo FILE --eval TIME OUT.json    world-space (skinned) mesh points at TIME as JSON (scene tests)
//     --blend-from FROM WEIGHT            ... crossfaded from the pose at FROM (WEIGHT 0 = FROM, 1 = TIME)
#include "scene/scene.h"
#include "usd/fileio.h"
#include "usd/stage.h"

#include <cmath>
#include <cstdio>
#include <functional>
#include <string>

using namespace usd;

namespace {

void JsonString(std::string& out, const std::string& s) {
    out += '"';
    for (unsigned char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out += char(c);
                }
        }
    }
    out += '"';
}

void JsonNumber(std::string& out, double v) {
    if (std::isnan(v)) {
        out += "NaN";
    } else if (std::isinf(v)) {
        out += v > 0 ? "Infinity" : "-Infinity";
    } else {
        char buf[40];
        std::snprintf(buf, sizeof(buf), "%.17g", v);
        out += buf;
    }
}

void JsonValue(std::string& out, const Value& v) {
    using K = Value::Kind;
    switch (v.kind) {
        case K::None: out += "null"; break;
        case K::Blocked: out += "{\"blocked\":true}"; break;
        case K::Number: {
            out += "{\"n\":[";
            for (size_t i = 0; i < v.nums.size(); ++i) {
                if (i) out += ',';
                JsonNumber(out, v.nums[i]);
            }
            out += "],\"t\":" + std::to_string(v.tupleSize) + ",\"a\":" + (v.isArray ? "true" : "false") + "}";
            break;
        }
        case K::String:
        case K::Token:
        case K::Asset:
        case K::Path: {
            out += "{\"s\":[";
            for (size_t i = 0; i < v.strs.size(); ++i) {
                if (i) out += ',';
                JsonString(out, v.strs[i]);
            }
            out += "],\"a\":";
            out += v.isArray ? "true" : "false";
            out += "}";
            break;
        }
        case K::Dictionary: {
            out += "{\"d\":{";
            bool first = true;
            if (v.dict)
                for (auto& [k, val] : *v.dict) {
                    if (!first) out += ',';
                    first = false;
                    JsonString(out, k);
                    out += ':';
                    JsonValue(out, val);
                }
            out += "}}";
            break;
        }
        case K::References: {
            out += "{\"r\":[";
            if (v.refs)
                for (size_t i = 0; i < v.refs->size(); ++i) {
                    const Reference& r = (*v.refs)[i];
                    if (i) out += ',';
                    out += '[';
                    JsonString(out, r.assetPath);
                    out += ',';
                    JsonString(out, r.primPath);
                    out += ',';
                    JsonNumber(out, r.layerOffset.offset);
                    out += ',';
                    JsonNumber(out, r.layerOffset.scale);
                    out += ']';
                }
            out += "]}";
            break;
        }
        case K::TimeSamples: {
            out += "{\"ts\":[";
            if (v.samples)
                for (size_t i = 0; i < v.samples->size(); ++i) {
                    if (i) out += ',';
                    out += '[';
                    JsonNumber(out, (*v.samples)[i].time);
                    out += ',';
                    JsonValue(out, (*v.samples)[i].value);
                    out += ']';
                }
            out += "]}";
            break;
        }
    }
}

bool DumpLayer(const Layer& layer, const std::wstring& outPath) {
    std::string out = "{\"specs\":{";
    bool first = true;
    for (auto& [path, spec] : layer.specs) {
        if (!first) out += ",\n";
        first = false;
        JsonString(out, path);
        out += ":{\"type\":" + std::to_string(int(spec.type)) + ",\"fields\":{";
        for (size_t i = 0; i < spec.fields.size(); ++i) {
            if (i) out += ',';
            JsonString(out, spec.fields[i].first);
            out += ':';
            JsonValue(out, spec.fields[i].second);
        }
        out += "}}";
    }
    out += "}}\n";
    FILE* f = _wfopen(outPath.c_str(), L"wb");
    if (!f) return false;
    fwrite(out.data(), 1, out.size(), f);
    fclose(f);
    return true;
}

void PrintTree(const Prim* prim, int indent) {
    for (auto& c : prim->Children()) {
        std::printf("%*s%s [%s]%s%s\n", indent * 2, "", c->Name().c_str(),
                    c->TypeName().empty() ? "-" : c->TypeName().c_str(), c->IsActive() ? "" : " (inactive)",
                    c->IsDefined() ? "" : (c->IsAbstract() ? " (class)" : " (over)"));
        PrintTree(c.get(), indent + 1);
    }
}

}  // namespace

bool DumpEval(const scene::Scene& sc, const std::wstring& outPath) {
    std::string out = "{\"time\":";
    JsonNumber(out, sc.currentTime);
    out += ",\"meshes\":{";
    std::vector<float> pts;
    bool first = true;
    const scene::Mat4 toStage = sc.upAxisFix.Inverse();  // report in the stage's own up axis
    for (const scene::Mesh& m : sc.meshes) {
        if (m.instancer >= 0) {
            pts.clear();
            for (const scene::Mat4& x : m.instances)
                for (size_t i = 0; i + 2 < m.restPoints.size(); i += 3) {
                    scene::Vec3 p = (x * toStage).TransformPoint({m.restPoints[i], m.restPoints[i + 1], m.restPoints[i + 2]});
                    pts.insert(pts.end(), {float(p.x), float(p.y), float(p.z)});
                }
        } else {
            sc.ComputeWorldPoints(m, pts);
            for (size_t i = 0; i + 2 < pts.size(); i += 3) {
                scene::Vec3 p = toStage.TransformPoint({pts[i], pts[i + 1], pts[i + 2]});
                pts[i] = float(p.x);
                pts[i + 1] = float(p.y);
                pts[i + 2] = float(p.z);
            }
        }
        if (!first) out += ",\n";
        first = false;
        JsonString(out, m.path);
        out += ":[";
        for (size_t i = 0; i < pts.size(); ++i) {
            if (i) out += ',';
            char buf[32];
            std::snprintf(buf, sizeof(buf), "%.7g", pts[i]);
            out += buf;
        }
        out += "]";
    }
    out += "}}\n";
    FILE* f = _wfopen(outPath.c_str(), L"wb");
    if (!f) return false;
    fwrite(out.data(), 1, out.size(), f);
    fclose(f);
    return true;
}

int wmain(int argc, wchar_t** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: usdzinfo FILE [--dump-layer OUT.json] [--eval TIME OUT.json]\n");
        return 2;
    }
    std::wstring file = argv[1];
    std::wstring dumpLayer, evalOut;
    double evalTime = 0, blendFrom = 0, blendWeight = -1;
    for (int i = 2; i < argc; ++i) {
        std::wstring a = argv[i];
        if (a == L"--dump-layer" && i + 1 < argc) dumpLayer = argv[++i];
        if (a == L"--eval" && i + 2 < argc) {
            evalTime = _wtof(argv[++i]);
            evalOut = argv[++i];
        }
        if (a == L"--blend-from" && i + 2 < argc) {
            blendFrom = _wtof(argv[++i]);
            blendWeight = _wtof(argv[++i]);
        }
    }

    std::string err;
    auto stage = Stage::Open(file, err);
    if (!stage) {
        std::fprintf(stderr, "error: %s\n", err.c_str());
        return 1;
    }
    if (!dumpLayer.empty()) {
        if (!DumpLayer(stage->RootLayer(), dumpLayer)) {
            std::fprintf(stderr, "error: cannot write %s\n", WideToUtf8(dumpLayer).c_str());
            return 1;
        }
        return 0;
    }
    scene::Scene sc;
    sc.Build(*stage);
    if (!evalOut.empty()) {
        if (blendWeight >= 0) sc.EvaluateBlend(blendFrom, evalTime, blendWeight);
        else sc.Evaluate(evalTime);
        if (!DumpEval(sc, evalOut)) {
            std::fprintf(stderr, "error: cannot write %s\n", WideToUtf8(evalOut).c_str());
            return 1;
        }
        return 0;
    }

    std::string report = scene::Describe(*stage, sc);
    for (size_t pos; (pos = report.find('\r')) != std::string::npos;) report.erase(pos, 1);
    std::fputs(report.c_str(), stdout);
    auto b = sc.ComputeBounds();
    if (!b.IsEmpty())
        std::printf("\nBounds at time %g: (%.4g, %.4g, %.4g) - (%.4g, %.4g, %.4g)\n", sc.currentTime, b.min.x, b.min.y,
                    b.min.z, b.max.x, b.max.y, b.max.z);
    std::printf("\nPrims:\n");
    PrintTree(stage->PseudoRoot(), 1);
    return 0;
}
