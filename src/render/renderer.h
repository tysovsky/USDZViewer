// Direct3D 11 renderer for a scene::Scene: GPU skinning, PBR shading into an HDR MSAA target,
// alpha-tested/blended materials, ground grid, skeleton overlay and an ACES-tonemapped composite.
#pragma once

#include "render/image.h"
#include "scene/scene.h"

#include <memory>
#include <string>
#include <vector>

#include <windows.h>

namespace render {

struct Camera {
    scene::Vec3 target;
    double yaw = 0.5;     // radians around +Y
    double pitch = 0.25;  // radians, positive looks down
    double distance = 5.0;
    double fovY = 0.7853981633974483;  // 45 degrees
    double nearClip = 0.01, farClip = 1000.0;

    scene::Vec3 Eye() const;
    scene::Vec3 Right() const;
    scene::Vec3 Up() const;
    scene::Vec3 Back() const;  // from the target towards the eye
    scene::Mat4 View() const;
    scene::Mat4 Projection(double aspect) const;
};

struct RenderOptions {
    bool grid = true;
    bool groundShadow = true;
    scene::Bounds shadowBounds;  // region covered by the ground shadow (normally the animation's bounds)
    bool skeleton = false;
    bool axes = true;
    int wireframe = 0;           // 0 shaded, 1 wireframe, 2 shaded + wireframe
    bool showBackfaces = false;  // draw single-sided meshes without back-face culling
    int shading = 0;             // 0 lit, 1 base color, 2 normals
    float exposure = 1.0f;
    double gridCell = 0.1;
    double gridExtent = 10.0;
    double gridHeight = 0.0;
    float uiScale = 1.0f;

    // Ruler: dimension lines and labels around each box (world space), lengths shown in meters.
    struct RulerBox {
        scene::Bounds bounds;
        double metersPerUnit = 1.0;
    };
    bool ruler = false;
    std::vector<RulerBox> rulers;
};

// "1.43 m", "12.5 cm", "8.0 mm"; with `imperial`, also " (4 ft 8.3 in)".
std::wstring FormatLength(double meters, bool imperial);

class Renderer {
public:
    Renderer();
    ~Renderer();
    bool Init(HWND hwnd, std::string& err);
    void Resize(int width, int height);
    // Uploads a scene's meshes, materials and textures (`images` is indexed like scene->images) and draws it
    // from then on, together with any other added scenes. The scene must outlive its registration.
    void AddScene(scene::Scene* scene, std::vector<DecodedImage> images);
    void RemoveScene(const scene::Scene* scene);
    void ClearScenes();
    // Draws only these of the added scenes (the others keep their GPU resources).
    void ShowScenes(const std::vector<const scene::Scene*>& shown);
    // Renders and presents a frame. With redrawScene = false the previous scene image is presented again
    // (only the cheap final pass runs), which lets the app keep a steady frame rate while nothing changes.
    void Render(const Camera& camera, const RenderOptions& options, bool redrawScene = true);

    const std::string& AdapterName() const;
    int MsaaSamples() const;
    bool DeviceLost() const;
    size_t TextureMemory() const;
    std::vector<std::string> TakeWarnings();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace render
