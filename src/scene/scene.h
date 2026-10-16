// Render-oriented scene built from a composed USD stage: transform hierarchy, triangulated meshes,
// UsdPreviewSurface materials, UsdSkel skeletons/animations and blend shapes, evaluated per frame.
#pragma once

#include "scene/math.h"
#include "usd/stage.h"

#include <memory>
#include <string>
#include <vector>

namespace scene {

enum class Wrap : uint8_t { Repeat, Mirror, Clamp, Black };

// A texture input of a material (UsdUVTexture feeding a UsdPreviewSurface input).
struct TextureSlot {
    int image = -1;    // index into Scene::images; -1 = constant input
    int channel = -1;  // -1 = rgb, 0..3 = r, g, b, a
    float scale[4] = {1, 1, 1, 1};
    float bias[4] = {0, 0, 0, 0};
    Wrap wrapS = Wrap::Repeat, wrapT = Wrap::Repeat;
    bool srgb = false;
    // st' = (uv[0]*s + uv[1]*t + uv[2], uv[3]*s + uv[4]*t + uv[5])  (UsdTransform2d)
    float uv[6] = {1, 0, 0, 0, 1, 0};
};

struct ImageRef {
    usd::AssetLocation location;
    std::string name;  // asset path as authored
};

struct Material {
    std::string path;
    std::string name;
    bool usesVertexColor = false;  // no UsdPreviewSurface: shade with displayColor
    float diffuse[3] = {0.18f, 0.18f, 0.18f};
    float emissive[3] = {0, 0, 0};
    float specularColor[3] = {0, 0, 0};
    bool useSpecularWorkflow = false;
    float metallic = 0.0f;
    float roughness = 0.5f;
    float opacity = 1.0f;
    float opacityThreshold = 0.0f;
    float occlusion = 1.0f;
    float clearcoat = 0.0f;
    float clearcoatRoughness = 0.01f;
    float ior = 1.5f;
    TextureSlot diffuseTex, emissiveTex, metallicTex, roughnessTex, opacityTex, normalTex, occlusionTex, specularTex;
    std::string uvSet;  // primvar the textures read ("st" if unspecified)
    bool IsTranslucent() const { return opacityThreshold <= 0.0f && (opacity < 0.999f || opacityTex.image >= 0); }
};

struct XformOp {
    enum Type { Translate, Scale, RotateX, RotateY, RotateZ, RotateXYZ, RotateXZY, RotateYXZ, RotateYZX, RotateZXY, RotateZYX,
                Orient, Transform } type;
    usd::Attribute attr;
    bool invert = false;
};

struct Node {
    const usd::Prim* prim = nullptr;
    int parent = -1;
    bool resetXformStack = false;
    bool animated = false;  // any op or visibility time-sampled
    std::vector<XformOp> ops;
    usd::Attribute visibility;
    Mat4 local = Mat4::Identity();
    Mat4 world = Mat4::Identity();
    bool visible = true;   // evaluated, including ancestors
    bool userHidden = false;  // toggled from the UI
};

struct BlendShape {
    std::string name;
    std::vector<uint32_t> pointIndices;  // empty = all points
    std::vector<float> offsets;          // 3 per entry
    int channel = -1;                    // index into the skeleton animation's blend shape weights
};

struct SubMesh {
    uint32_t first = 0;  // first corner (3 per triangle)
    uint32_t count = 0;  // corner count
    int material = 0;
};

struct Mesh {
    const usd::Prim* prim = nullptr;
    std::string path;
    int node = -1;
    uint32_t pointCount = 0;
    std::vector<uint32_t> cornerPoint;  // per triangle corner: index into points
    std::vector<float> normals;         // 3 per corner, authored normals (empty: computed)
    std::vector<float> uvs;             // 2 per corner (empty: none)
    std::vector<float> colors;          // 4 per corner (displayColor/displayOpacity; empty: none)
    std::vector<SubMesh> subsets;
    std::vector<float> restPoints;  // 3 per point
    bool smoothNormals = true;
    bool doubleSided = false;

    // Skinning (UsdSkel). Palette entry k maps to skeleton joint paletteJoints[k].
    int skeleton = -1;
    std::vector<uint16_t> jointIndices;  // 4 per point
    std::vector<float> jointWeights;     // 4 per point
    std::vector<int> paletteJoints;
    Mat4 geomBind = Mat4::Identity();
    std::vector<BlendShape> blendShapes;

    // Time-sampled points (vertex animation).
    usd::Attribute pointsAttr;

    // PointInstancer prototypes: drawn once per instance instead of at their own location.
    int instancer = -1;
    int protoIndex = -1;
    int protoParentNode = -1;

    // ---- evaluated per frame ----
    std::vector<Mat4> palette;      // skinning matrices to world space (one entry for rigid meshes)
    std::vector<Mat4> instances;    // world matrices for instanced meshes
    std::vector<float> points;      // current points when deformed on the CPU (blend shapes / point animation)
    std::vector<float> cpuNormals;  // current per-corner normals when they must be recomputed
    bool pointsDirty = true;        // `points` / `cpuNormals` changed since the renderer last uploaded them
    bool visible = true;
    bool highlighted = false;       // UI selection (tinted by the renderer)
    bool HasCpuDeformation() const { return pointsAttr.IsAnimated() || !blendShapes.empty(); }
    const std::vector<float>& CurrentPoints() const { return points.empty() ? restPoints : points; }
};

struct Skeleton {
    const usd::Prim* prim = nullptr;
    std::string path;
    int node = -1;
    std::vector<std::string> joints;
    std::vector<int> parents;
    std::vector<Mat4> bindInverse;  // inverse world-space bind transforms
    std::vector<Mat4> rest;         // local rest transforms
    bool hasRest = false;
    std::vector<Mat4> bind;
    std::vector<TRS> restTRS;       // local pose without animation (rest, or derived from bind)

    const usd::Prim* anim = nullptr;
    usd::Attribute translations, rotations, scales, blendShapeWeights;
    std::vector<int> animToSkel;
    std::vector<std::string> animBlendShapes;

    // evaluated
    std::vector<Mat4> skelSpace;  // joint transforms in skeleton space
    std::vector<float> weights;   // blend shape weights
    bool valid = false;
};

struct Instancer {
    const usd::Prim* prim = nullptr;
    int node = -1;
    std::vector<std::string> prototypes;
    std::vector<std::vector<Mat4>> protoInstances;  // evaluated: per prototype, instancer-local matrices
};

struct Clip {
    std::string name;
    double start = 0, end = 0;  // time codes
};

// The animated state of a scene at one time, before world transforms, skinning and deformation are
// derived from it. Two poses can be blended (crossfades between animation clips).
struct Pose {
    std::vector<Mat4> nodeLocal;              // per node
    std::vector<char> nodeVisible;            // per node, authored visibility only
    std::vector<std::vector<TRS>> joints;     // per skeleton: local joint transforms
    std::vector<std::vector<float>> weights;  // per skeleton: blend shape weights
    std::vector<std::vector<float>> points;   // per mesh: time-sampled points (empty if not animated)
};

class Scene {
public:
    bool Build(const usd::Stage& stage);
    void Evaluate(double time);
    // Crossfade: the pose at `fromTime` (or a captured pose) blended towards the pose at `time` by `weight`
    // (0 = from, 1 = time). Transforms blend as translation/rotation/scale, rotations by slerp.
    void EvaluateBlend(double fromTime, double time, double weight);
    void EvaluateBlend(const Pose& from, double time, double weight);
    const Pose& CurrentPose() const { return applied_; }  // what the last Evaluate produced
    // The animated values at `time`, without applying them (for comparing poses).
    void SamplePose(double time, Pose& out) const;
    // World space at the last evaluated time; `meshMask` (one entry per mesh) limits it to some meshes.
    Bounds ComputeBounds(const std::vector<char>* meshMask = nullptr) const;
    // World-space bounds with skinned meshes in their bind pose (a stable framing for animated characters).
    Bounds ComputeBindPoseBounds() const;
    // World-space skinned/deformed positions of a mesh at the last evaluated time (3 per point).
    void ComputeWorldPoints(const Mesh& mesh, std::vector<float>& out) const;

    const usd::Stage* stage = nullptr;
    std::vector<Node> nodes;
    std::vector<Mesh> meshes;
    std::vector<Skeleton> skeletons;
    std::vector<Instancer> instancers;
    std::vector<Material> materials;  // [0] is the default (displayColor) material
    std::vector<ImageRef> images;
    std::vector<Clip> clips;
    std::vector<std::string> warnings;

    double startTime = 0, endTime = 0;
    double timeCodesPerSecond = 24, framesPerSecond = 24;
    bool animated = false;
    Mat4 upAxisFix = Mat4::Identity();
    // Where the whole scene sits in the viewer (several files can be open side by side) and whether it
    // is shown. Both take effect on the next Evaluate().
    Mat4 placement = Mat4::Identity();
    bool hidden = false;
    Mat4 RootTransform() const { return upAxisFix * placement; }
    size_t triangleCount = 0;
    double currentTime = 0;

private:
    struct Builder;
    friend struct Builder;
    void BlendPoses(const Pose& a, const Pose& b, double w, Pose& out) const;
    void ApplyPose(const Pose& pose, double time);
    void EvaluateInstancers(double time);
    void EvaluateMesh(Mesh& mesh, const std::vector<float>& sampledPoints);
    bool firstEvaluation_ = true;
    Pose sampleA_, sampleB_, applied_;
};

// Largest differences between two poses of one scene: rotation (radians) and translation of joints and animated
// nodes (plus point movement), and blend shape weight. `extent` is the largest translation / point coordinate
// in `b`, a length scale for `distance`.
struct PoseDifference {
    double angle = 0, distance = 0, weight = 0, extent = 0;
};
PoseDifference ComparePoses(const Pose& a, const Pose& b);

// Normals for a triangle list (3 corners per triangle). Smooth normals average per point.
void ComputeNormals(const std::vector<float>& points, const std::vector<uint32_t>& cornerPoint, bool smooth,
                    std::vector<float>& out);

// Human-readable report: layers, stage metadata, meshes, materials, textures, skeletons, clips, warnings.
std::string Describe(const usd::Stage& stage, const Scene& scene);

// One-line summary of a value for display ("[993] (0.1, 2, 3), (...)...", "\"text\"", "</path>").
std::string SummarizeValue(const usd::Value& v, size_t maxElements = 6);

}  // namespace scene
