#include "render/renderer.h"

#include "render/shaders.h"

#include <d2d1_1.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <dwrite.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace render {

using Microsoft::WRL::ComPtr;
using scene::Mat4;
using scene::Vec3;

std::wstring FormatLength(double meters, bool imperial) {
    wchar_t buf[96];
    const double a = std::fabs(meters);
    if (a < 0.01) std::swprintf(buf, 96, L"%.1f mm", meters * 1000);
    else if (a < 1) std::swprintf(buf, 96, L"%.1f cm", meters * 100);
    else if (a < 1000) std::swprintf(buf, 96, L"%.2f m", meters);
    else std::swprintf(buf, 96, L"%.2f km", meters / 1000);
    std::wstring s = buf;
    if (imperial) {
        const double inches = a / 0.0254;
        if (inches < 12) {
            std::swprintf(buf, 96, L"  (%.1f in)", inches);
        } else if (inches < 12 * 5280) {
            double feet = std::floor(inches / 12), rest = inches - feet * 12;
            if (rest >= 11.95) {
                feet += 1;
                rest = 0;
            }
            std::swprintf(buf, 96, L"  (%.0f ft %.1f in)", feet, rest);
        } else {
            std::swprintf(buf, 96, L"  (%.2f mi)", inches / (12 * 5280));
        }
        s += buf;
    }
    return s;
}

// ---- camera ----------------------------------------------------------------------------------

Vec3 Camera::Back() const {
    return {std::cos(pitch) * std::sin(yaw), std::sin(pitch), std::cos(pitch) * std::cos(yaw)};
}
Vec3 Camera::Eye() const { return target + Back() * distance; }
Vec3 Camera::Right() const { return Vec3{0, 1, 0}.Cross(Back()).Normalized(); }
Vec3 Camera::Up() const { return Back().Cross(Right()); }

Mat4 Camera::View() const {
    Vec3 z = Back(), x = Right(), y = Up(), e = Eye();
    Mat4 v = Mat4::Identity();
    v.m[0] = x.x; v.m[1] = y.x; v.m[2] = z.x;
    v.m[4] = x.y; v.m[5] = y.y; v.m[6] = z.y;
    v.m[8] = x.z; v.m[9] = y.z; v.m[10] = z.z;
    v.m[12] = -x.Dot(e);
    v.m[13] = -y.Dot(e);
    v.m[14] = -z.Dot(e);
    return v;
}

Mat4 Camera::Projection(double aspect) const {
    double ys = 1.0 / std::tan(fovY / 2), xs = ys / aspect;
    double n = nearClip, f = farClip;
    Mat4 p;
    std::memset(p.m, 0, sizeof(p.m));
    p.m[0] = xs;
    p.m[5] = ys;
    p.m[10] = f / (n - f);
    p.m[11] = -1;
    p.m[14] = n * f / (n - f);
    return p;
}

namespace {

struct FrameCB {
    float viewProj[16];
    float cameraPos[4];
    float lightDir[3][4];
    float lightColor[3][4];
    float skyColor[4];
    float groundColor[4];
    float frameParams[4];
};
struct DrawCB {
    uint32_t paletteOffset;
    uint32_t flags;
    float pad[2];
    float tint[4];
};
struct MaterialCB {
    float baseColor[4];
    float emissive[4];
    float pbr[4];
    float specular[4];
    uint32_t flags[4];
    int32_t channels0[4];
    int32_t channels1[4];
    float texScale[8][4];
    float texBias[8][4];
    float uvRow0[8][4];
    float uvRow1[8][4];
};
struct PaletteEntry {
    float r[4][4];
    float n[3][4];
    float pad[4];
};
struct VertexAttr {
    float uv[2];
    float color[4];
    uint16_t joints[4];
    float weights[4];
};
struct LineVertex {
    float pos[3];
    float color[4];
};
struct GridCB {
    float viewProj[16];
    float shadowViewProj[16];
    float params[4];
    float cameraPos[4];
    float shadowParams[4];
    float groundFlags[4];
};
constexpr UINT kShadowSize = 2048;
struct CompositeCB {
    float params[4];
    float bgTop[4];
    float bgBottom[4];
};
struct LineCB {
    float viewProj[16];
};
static_assert(sizeof(PaletteEntry) == 128, "palette entry layout");
static_assert(sizeof(VertexAttr) == 48, "vertex layout");
static_assert(sizeof(FrameCB) % 16 == 0 && sizeof(MaterialCB) % 16 == 0, "constant buffer layout");

void ToFloats(const Mat4& m, float* out) {
    for (int i = 0; i < 16; ++i) out[i] = float(m.m[i]);
}

void FillPalette(const Mat4& m, PaletteEntry& e) {
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c) e.r[r][c] = float(m.m[r * 4 + c]);
    // Normal matrix = inverse transpose of the upper 3x3 = cofactor matrix / determinant.
    Vec3 r0{m.m[0], m.m[1], m.m[2]}, r1{m.m[4], m.m[5], m.m[6]}, r2{m.m[8], m.m[9], m.m[10]};
    Vec3 c0 = r1.Cross(r2), c1 = r2.Cross(r0), c2 = r0.Cross(r1);
    double det = r0.Dot(c0);
    double inv = std::fabs(det) > 1e-30 ? 1.0 / det : 1.0;
    const Vec3 rows[3] = {c0 * inv, c1 * inv, c2 * inv};
    for (int r = 0; r < 3; ++r) {
        e.n[r][0] = float(rows[r].x);
        e.n[r][1] = float(rows[r].y);
        e.n[r][2] = float(rows[r].z);
        e.n[r][3] = 0;
    }
    std::memset(e.pad, 0, sizeof(e.pad));
}

float SrgbToLinear(float c) { return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f); }

// A "nice" tick spacing (1, 2 or 5 times a power of ten) giving about 4-8 ticks over `length`.
double NiceStep(double length) {
    if (length <= 0) return 1;
    const double raw = length / 8;
    const double p = std::pow(10.0, std::floor(std::log10(raw)));
    for (double m : {1.0, 2.0, 5.0, 10.0})
        if (p * m >= raw) return p * m;
    return p * 10;
}

struct TextLabel {
    std::wstring text;
    float x, y;
    int anchor;  // 0 centered, 1 left edge at x, 2 right edge at x
    bool compact;  // smaller font
    bool background;
    float color[3];  // sRGB
};

enum DrawKind { KindOpaque = 0, KindMasked = 1, KindBlend = 2 };

}  // namespace

struct Renderer::Impl {
    HWND hwnd = nullptr;
    ComPtr<ID3D11Device> dev;
    ComPtr<ID3D11DeviceContext> ctx;
    ComPtr<IDXGISwapChain1> swap;
    ComPtr<ID3D11RenderTargetView> backRTV;
    ComPtr<ID3D11Texture2D> hdrMs, hdrResolved, depthMs;
    ComPtr<ID3D11RenderTargetView> hdrRTV;
    ComPtr<ID3D11ShaderResourceView> hdrSRV;
    ComPtr<ID3D11DepthStencilView> dsv;
    int width = 0, height = 0, samples = 1;

    ComPtr<ID3D11VertexShader> meshVS, lineVS, gridVS, compVS;
    ComPtr<ID3D11PixelShader> psOpaque, psMasked, psBlend, psWire, linePS, gridPS, compPS;
    ComPtr<ID3D11InputLayout> meshLayout, lineLayout;
    ComPtr<ID3D11RasterizerState> rsCullBack, rsCullNone, rsWire, rsWireOverlay, rsLines;
    ComPtr<ID3D11DepthStencilState> dsDefault, dsReadOnly, dsNone;
    ComPtr<ID3D11BlendState> bsOpaque, bsPremul;
    ComPtr<ID3D11SamplerState> samplers[16];
    ComPtr<ID3D11Buffer> frameCB, drawCB, gridCB, compCB, lineCB;
    ComPtr<ID3D11Buffer> paletteBuf;
    ComPtr<ID3D11ShaderResourceView> paletteSRV;
    size_t paletteCap = 0;
    ComPtr<ID3D11Buffer> lineVB;
    size_t lineCap = 0;

    // Ground contact shadow: top-down depth map.
    ComPtr<ID3D11Texture2D> shadowTex;
    ComPtr<ID3D11DepthStencilView> shadowDSV;
    ComPtr<ID3D11ShaderResourceView> shadowSRV;
    ComPtr<ID3D11SamplerState> shadowCmp, shadowPoint;
    bool shadowActive = false;
    bool imageValid = false;  // hdrResolved holds a complete scene image

    // Direct2D / DirectWrite text drawn on top of the back buffer.
    ComPtr<ID2D1Factory1> d2dFactory;
    ComPtr<ID2D1Device> d2dDevice;
    ComPtr<ID2D1DeviceContext> d2dCtx;
    ComPtr<ID2D1Bitmap1> d2dTarget;
    ComPtr<IDWriteFactory> dwFactory;
    ComPtr<IDWriteTextFormat> labelFormat, smallFormat;
    ComPtr<ID2D1SolidColorBrush> textBrush, backBrush;
    float textScale = 0;
    Mat4 shadowVP = Mat4::Identity();
    double shadowRange = 1, shadowUnitsPerUV = 1, shadowMaxHeight = 1;

    struct GpuMesh {
        ComPtr<ID3D11Buffer> pos, nrm, attr;
        bool dynamicPos = false, dynamicNrm = false;
        UINT vertexCount = 0;
    };
    struct GpuTexture {
        ComPtr<ID3D11Texture2D> tex;
        ComPtr<ID3D11ShaderResourceView> linear, srgb;
    };
    struct GpuMaterial {
        ComPtr<ID3D11Buffer> cb;
        ID3D11ShaderResourceView* srv[8] = {};
        ID3D11SamplerState* samp[8] = {};
        int kind = KindOpaque;
    };
    // GPU resources of one loaded file; several can be shown together.
    struct GpuScene {
        scene::Scene* scene = nullptr;
        std::vector<GpuMesh> meshes;
        std::vector<GpuTexture> textures;
        std::vector<GpuMaterial> materials;
        size_t textureBytes = 0;
        bool shown = true;  // drawn (scenes of other tabs stay uploaded but hidden)
    };
    std::vector<std::unique_ptr<GpuScene>> scenes;
    std::string adapterName;
    std::vector<std::string> warnings;
    bool lost = false;

    bool Compile(const char* src, const char* name, const char* entry, const char* target, ComPtr<ID3DBlob>& out,
                 std::string& err) {
        ComPtr<ID3DBlob> errors;
        HRESULT hr = D3DCompile(src, std::strlen(src), name, nullptr, nullptr, entry, target,
                                D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &out, &errors);
        if (FAILED(hr)) {
            err = std::string("shader compilation failed (") + name + ":" + entry + ")";
            if (errors) err += ": " + std::string(static_cast<const char*>(errors->GetBufferPointer()), errors->GetBufferSize());
            return false;
        }
        return true;
    }

    template <class T>
    ComPtr<ID3D11Buffer> MakeCB() {
        D3D11_BUFFER_DESC d = {};
        d.ByteWidth = UINT((sizeof(T) + 15) & ~size_t(15));
        d.Usage = D3D11_USAGE_DYNAMIC;
        d.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        d.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        ComPtr<ID3D11Buffer> b;
        dev->CreateBuffer(&d, nullptr, &b);
        return b;
    }

    template <class T>
    void Upload(ID3D11Buffer* b, const T& data) {
        D3D11_MAPPED_SUBRESOURCE m;
        if (SUCCEEDED(ctx->Map(b, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
            std::memcpy(m.pData, &data, sizeof(T));
            ctx->Unmap(b, 0);
        }
    }

    ComPtr<ID3D11Buffer> MakeVB(const void* data, size_t bytes, bool dynamic) {
        D3D11_BUFFER_DESC d = {};
        d.ByteWidth = UINT(std::max<size_t>(bytes, 16));
        d.Usage = dynamic ? D3D11_USAGE_DYNAMIC : D3D11_USAGE_IMMUTABLE;
        d.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        d.CPUAccessFlags = dynamic ? D3D11_CPU_ACCESS_WRITE : 0;
        D3D11_SUBRESOURCE_DATA init = {data, 0, 0};
        ComPtr<ID3D11Buffer> b;
        dev->CreateBuffer(&d, data ? &init : nullptr, &b);
        return b;
    }

    bool Init(HWND h, std::string& err) {
        hwnd = h;
        const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
        UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
        HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, levels, 2, D3D11_SDK_VERSION,
                                       &dev, nullptr, &ctx);
        if (FAILED(hr))
            hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, levels + 1, 1, D3D11_SDK_VERSION,
                                   &dev, nullptr, &ctx);
        if (FAILED(hr))
            hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, flags, levels + 1, 1, D3D11_SDK_VERSION, &dev,
                                   nullptr, &ctx);
        if (FAILED(hr)) {
            err = "Direct3D 11 is not available on this system";
            return false;
        }
        ComPtr<IDXGIDevice> dxgiDev;
        ComPtr<IDXGIAdapter> adapter;
        ComPtr<IDXGIFactory2> factory;
        dev.As(&dxgiDev);
        dxgiDev->GetAdapter(&adapter);
        adapter->GetParent(IID_PPV_ARGS(&factory));
        DXGI_ADAPTER_DESC ad;
        if (SUCCEEDED(adapter->GetDesc(&ad))) {
            char name[256];
            WideCharToMultiByte(CP_UTF8, 0, ad.Description, -1, name, sizeof(name), nullptr, nullptr);
            adapterName = name;
        }
        DXGI_SWAP_CHAIN_DESC1 sd = {};
        sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;  // BGRA for Direct2D text interop
        sd.SampleDesc.Count = 1;
        sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        sd.BufferCount = 2;
        sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        sd.Scaling = DXGI_SCALING_STRETCH;
        sd.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
        if (FAILED(factory->CreateSwapChainForHwnd(dev.Get(), hwnd, &sd, nullptr, nullptr, &swap))) {
            err = "cannot create the swap chain";
            return false;
        }
        factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);

        // Text overlay (optional: the viewer works without it, just without labels).
        D2D1_FACTORY_OPTIONS fo = {};
        if (SUCCEEDED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory1), &fo,
                                        reinterpret_cast<void**>(d2dFactory.GetAddressOf()))) &&
            SUCCEEDED(d2dFactory->CreateDevice(dxgiDev.Get(), &d2dDevice)) &&
            SUCCEEDED(d2dDevice->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &d2dCtx)) &&
            SUCCEEDED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                          reinterpret_cast<IUnknown**>(dwFactory.GetAddressOf())))) {
            d2dCtx->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, 1), &textBrush);
            d2dCtx->CreateSolidColorBrush(D2D1::ColorF(0.06f, 0.065f, 0.08f, 0.78f), &backBrush);
            d2dCtx->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
        } else {
            d2dCtx.Reset();
        }

        UINT q = 0;
        if (SUCCEEDED(dev->CheckMultisampleQualityLevels(DXGI_FORMAT_R16G16B16A16_FLOAT, 4, &q)) && q > 0) {
            UINT qd = 0;
            if (SUCCEEDED(dev->CheckMultisampleQualityLevels(DXGI_FORMAT_D32_FLOAT, 4, &qd)) && qd > 0) samples = 4;
        }

        // Shaders.
        ComPtr<ID3DBlob> vs, ps;
        if (!Compile(kMeshShader, "mesh", "VSMain", "vs_5_0", vs, err)) return false;
        dev->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &meshVS);
        const D3D11_INPUT_ELEMENT_DESC meshElems[] = {
            {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
            {"NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 1, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
            {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 2, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
            {"COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 2, 8, D3D11_INPUT_PER_VERTEX_DATA, 0},
            {"BLENDINDICES", 0, DXGI_FORMAT_R16G16B16A16_UINT, 2, 24, D3D11_INPUT_PER_VERTEX_DATA, 0},
            {"BLENDWEIGHT", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 2, 32, D3D11_INPUT_PER_VERTEX_DATA, 0},
        };
        dev->CreateInputLayout(meshElems, 6, vs->GetBufferPointer(), vs->GetBufferSize(), &meshLayout);
        const std::pair<const char*, ComPtr<ID3D11PixelShader>*> meshPS[] = {
            {"PSOpaque", &psOpaque}, {"PSMasked", &psMasked}, {"PSBlend", &psBlend}, {"PSWire", &psWire}};
        for (auto& [entry, target] : meshPS) {
            if (!Compile(kMeshShader, "mesh", entry, "ps_5_0", ps, err)) return false;
            dev->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, target->GetAddressOf());
        }
        if (!Compile(kLineShader, "line", "VSMain", "vs_5_0", vs, err)) return false;
        dev->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &lineVS);
        const D3D11_INPUT_ELEMENT_DESC lineElems[] = {
            {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
            {"COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0},
        };
        dev->CreateInputLayout(lineElems, 2, vs->GetBufferPointer(), vs->GetBufferSize(), &lineLayout);
        if (!Compile(kLineShader, "line", "PSMain", "ps_5_0", ps, err)) return false;
        dev->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, &linePS);
        if (!Compile(kGridShader, "grid", "VSMain", "vs_5_0", vs, err)) return false;
        dev->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &gridVS);
        if (!Compile(kGridShader, "grid", "PSMain", "ps_5_0", ps, err)) return false;
        dev->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, &gridPS);
        if (!Compile(kCompositeShader, "composite", "VSMain", "vs_5_0", vs, err)) return false;
        dev->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &compVS);
        if (!Compile(kCompositeShader, "composite", "PSMain", "ps_5_0", ps, err)) return false;
        dev->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, &compPS);

        // Fixed-function state.
        D3D11_RASTERIZER_DESC rd = {};
        rd.FillMode = D3D11_FILL_SOLID;
        rd.CullMode = D3D11_CULL_BACK;
        rd.FrontCounterClockwise = TRUE;  // USD's default (rightHanded) winding
        rd.DepthClipEnable = TRUE;
        rd.MultisampleEnable = TRUE;
        rd.AntialiasedLineEnable = FALSE;
        dev->CreateRasterizerState(&rd, &rsCullBack);
        rd.CullMode = D3D11_CULL_NONE;
        dev->CreateRasterizerState(&rd, &rsCullNone);
        rd.FillMode = D3D11_FILL_WIREFRAME;
        dev->CreateRasterizerState(&rd, &rsWire);
        rd.DepthBias = -20;
        rd.SlopeScaledDepthBias = -1.0f;
        rd.DepthBiasClamp = 0;
        dev->CreateRasterizerState(&rd, &rsWireOverlay);
        rd.FillMode = D3D11_FILL_SOLID;
        rd.DepthBias = 0;
        rd.SlopeScaledDepthBias = 0;
        rd.MultisampleEnable = FALSE;
        rd.AntialiasedLineEnable = TRUE;
        dev->CreateRasterizerState(&rd, &rsLines);

        D3D11_DEPTH_STENCIL_DESC dd = {};
        dd.DepthEnable = TRUE;
        dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
        dd.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;
        dev->CreateDepthStencilState(&dd, &dsDefault);
        dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
        dev->CreateDepthStencilState(&dd, &dsReadOnly);
        dd.DepthEnable = FALSE;
        dev->CreateDepthStencilState(&dd, &dsNone);

        D3D11_BLEND_DESC bd = {};
        bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        dev->CreateBlendState(&bd, &bsOpaque);
        auto& rt = bd.RenderTarget[0];
        rt.BlendEnable = TRUE;
        rt.SrcBlend = D3D11_BLEND_ONE;
        rt.DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
        rt.BlendOp = D3D11_BLEND_OP_ADD;
        rt.SrcBlendAlpha = D3D11_BLEND_ONE;
        rt.DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
        rt.BlendOpAlpha = D3D11_BLEND_OP_ADD;
        dev->CreateBlendState(&bd, &bsPremul);

        static const D3D11_TEXTURE_ADDRESS_MODE modes[4] = {D3D11_TEXTURE_ADDRESS_WRAP, D3D11_TEXTURE_ADDRESS_MIRROR,
                                                            D3D11_TEXTURE_ADDRESS_CLAMP, D3D11_TEXTURE_ADDRESS_BORDER};
        for (int s = 0; s < 4; ++s) {
            for (int t = 0; t < 4; ++t) {
                D3D11_SAMPLER_DESC smp = {};
                smp.Filter = D3D11_FILTER_ANISOTROPIC;
                smp.MaxAnisotropy = 8;
                smp.AddressU = modes[s];
                smp.AddressV = modes[t];
                smp.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
                smp.BorderColor[3] = 1.0f;
                smp.MaxLOD = FLT_MAX;
                smp.ComparisonFunc = D3D11_COMPARISON_NEVER;
                dev->CreateSamplerState(&smp, &samplers[s * 4 + t]);
            }
        }
        // Shadow map + samplers.
        D3D11_TEXTURE2D_DESC sd2 = {};
        sd2.Width = sd2.Height = kShadowSize;
        sd2.MipLevels = 1;
        sd2.ArraySize = 1;
        sd2.Format = DXGI_FORMAT_R32_TYPELESS;
        sd2.SampleDesc.Count = 1;
        sd2.Usage = D3D11_USAGE_DEFAULT;
        sd2.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;
        if (SUCCEEDED(dev->CreateTexture2D(&sd2, nullptr, &shadowTex))) {
            D3D11_DEPTH_STENCIL_VIEW_DESC dvd = {};
            dvd.Format = DXGI_FORMAT_D32_FLOAT;
            dvd.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
            dev->CreateDepthStencilView(shadowTex.Get(), &dvd, &shadowDSV);
            D3D11_SHADER_RESOURCE_VIEW_DESC svd = {};
            svd.Format = DXGI_FORMAT_R32_FLOAT;
            svd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
            svd.Texture2D.MipLevels = 1;
            dev->CreateShaderResourceView(shadowTex.Get(), &svd, &shadowSRV);
        }
        D3D11_SAMPLER_DESC cmp = {};
        cmp.Filter = D3D11_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT;
        cmp.AddressU = cmp.AddressV = cmp.AddressW = D3D11_TEXTURE_ADDRESS_BORDER;
        cmp.BorderColor[0] = cmp.BorderColor[1] = cmp.BorderColor[2] = cmp.BorderColor[3] = 1.0f;
        cmp.ComparisonFunc = D3D11_COMPARISON_LESS_EQUAL;
        cmp.MaxLOD = 0;
        dev->CreateSamplerState(&cmp, &shadowCmp);
        D3D11_SAMPLER_DESC pt = cmp;
        pt.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
        pt.ComparisonFunc = D3D11_COMPARISON_NEVER;
        dev->CreateSamplerState(&pt, &shadowPoint);

        frameCB = MakeCB<FrameCB>();
        drawCB = MakeCB<DrawCB>();
        gridCB = MakeCB<GridCB>();
        compCB = MakeCB<CompositeCB>();
        lineCB = MakeCB<LineCB>();

        RECT rc;
        GetClientRect(hwnd, &rc);
        Resize(rc.right - rc.left, rc.bottom - rc.top);
        return true;
    }

    void Resize(int w, int h) {
        if (!swap || w <= 0 || h <= 0) return;
        if (w == width && h == height && backRTV) return;
        ctx->OMSetRenderTargets(0, nullptr, nullptr);
        ID3D11ShaderResourceView* nullSrv = nullptr;
        ctx->PSSetShaderResources(0, 1, &nullSrv);
        if (d2dCtx) d2dCtx->SetTarget(nullptr);  // all back buffer references must go before ResizeBuffers
        d2dTarget.Reset();
        backRTV.Reset();
        hdrRTV.Reset();
        hdrSRV.Reset();
        dsv.Reset();
        hdrMs.Reset();
        hdrResolved.Reset();
        depthMs.Reset();
        ctx->Flush();
        HRESULT hr = swap->ResizeBuffers(0, UINT(w), UINT(h), DXGI_FORMAT_UNKNOWN, 0);
        if (FAILED(hr)) {
            lost = true;
            return;
        }
        width = w;
        height = h;
        imageValid = false;
        ComPtr<ID3D11Texture2D> back;
        swap->GetBuffer(0, IID_PPV_ARGS(&back));
        D3D11_RENDER_TARGET_VIEW_DESC rvd = {};
        rvd.Format = DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
        rvd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
        dev->CreateRenderTargetView(back.Get(), &rvd, &backRTV);
        if (d2dCtx) {
            ComPtr<IDXGISurface> surface;
            back.As(&surface);
            D2D1_BITMAP_PROPERTIES1 bp = D2D1::BitmapProperties1(
                D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
                D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE), 96.0f, 96.0f);
            if (FAILED(d2dCtx->CreateBitmapFromDxgiSurface(surface.Get(), &bp, &d2dTarget))) d2dTarget.Reset();
        }

        D3D11_TEXTURE2D_DESC td = {};
        td.Width = UINT(w);
        td.Height = UINT(h);
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE | (samples == 1 ? D3D11_BIND_RENDER_TARGET : 0);
        dev->CreateTexture2D(&td, nullptr, &hdrResolved);
        dev->CreateShaderResourceView(hdrResolved.Get(), nullptr, &hdrSRV);
        if (samples > 1) {
            td.SampleDesc.Count = UINT(samples);
            td.BindFlags = D3D11_BIND_RENDER_TARGET;
            dev->CreateTexture2D(&td, nullptr, &hdrMs);
            dev->CreateRenderTargetView(hdrMs.Get(), nullptr, &hdrRTV);
        } else {
            dev->CreateRenderTargetView(hdrResolved.Get(), nullptr, &hdrRTV);
        }
        td.Format = DXGI_FORMAT_D32_FLOAT;
        td.SampleDesc.Count = UINT(samples);
        td.BindFlags = D3D11_BIND_DEPTH_STENCIL;
        dev->CreateTexture2D(&td, nullptr, &depthMs);
        dev->CreateDepthStencilView(depthMs.Get(), nullptr, &dsv);
    }

    void ClearScenes() {
        scenes.clear();
        imageValid = false;
    }

    void RemoveScene(const scene::Scene* sc) {
        scenes.erase(std::remove_if(scenes.begin(), scenes.end(), [&](const auto& g) { return g->scene == sc; }),
                     scenes.end());
        imageValid = false;
    }

    void ShowScenes(const std::vector<const scene::Scene*>& shown) {
        for (auto& gs : scenes) {
            const bool show = std::find(shown.begin(), shown.end(), gs->scene) != shown.end();
            if (show != gs->shown) {
                gs->shown = show;
                imageValid = false;
            }
        }
    }

    void AddScene(scene::Scene* sc, std::vector<DecodedImage> images) {
        if (!sc) return;
        RemoveScene(sc);
        auto gs = std::make_unique<GpuScene>();
        gs->scene = sc;
        std::vector<GpuTexture>& textures = gs->textures;
        std::vector<GpuMaterial>& materials = gs->materials;
        std::vector<GpuMesh>& meshes = gs->meshes;
        size_t& textureBytes = gs->textureBytes;

        // Which images are sampled as sRGB.
        std::vector<char> wantSrgb(sc->images.size(), 0);
        for (auto& m : sc->materials)
            for (const scene::TextureSlot* s : {&m.diffuseTex, &m.emissiveTex, &m.metallicTex, &m.roughnessTex,
                                                &m.opacityTex, &m.normalTex, &m.occlusionTex, &m.specularTex})
                if (s->image >= 0 && size_t(s->image) < wantSrgb.size() && s->srgb) wantSrgb[size_t(s->image)] = 1;

        textures.resize(sc->images.size());
        for (size_t i = 0; i < sc->images.size() && i < images.size(); ++i) {
            DecodedImage& img = images[i];
            if (!img.Ok()) {
                warnings.push_back("texture " + sc->images[i].name + ": " + (img.error.empty() ? "not loaded" : img.error));
                continue;
            }
            if (img.width > D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION || img.height > D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION) {
                warnings.push_back("texture " + sc->images[i].name + " is larger than the GPU supports");
                continue;
            }
            D3D11_TEXTURE2D_DESC td = {};
            td.Width = UINT(img.width);
            td.Height = UINT(img.height);
            td.MipLevels = 0;
            td.ArraySize = 1;
            td.Format = DXGI_FORMAT_R8G8B8A8_TYPELESS;
            td.SampleDesc.Count = 1;
            td.Usage = D3D11_USAGE_DEFAULT;
            td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
            td.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;
            GpuTexture& t = textures[i];
            if (FAILED(dev->CreateTexture2D(&td, nullptr, &t.tex))) {
                warnings.push_back("cannot create GPU texture for " + sc->images[i].name);
                continue;
            }
            ctx->UpdateSubresource(t.tex.Get(), 0, nullptr, img.rgba.data(), UINT(img.width * 4), 0);
            D3D11_SHADER_RESOURCE_VIEW_DESC vd = {};
            vd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
            vd.Texture2D.MipLevels = UINT(-1);
            vd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            dev->CreateShaderResourceView(t.tex.Get(), &vd, &t.linear);
            vd.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
            dev->CreateShaderResourceView(t.tex.Get(), &vd, &t.srgb);
            ctx->GenerateMips(wantSrgb[i] ? t.srgb.Get() : t.linear.Get());
            textureBytes += size_t(img.width) * img.height * 4 * 4 / 3;
        }

        materials.resize(sc->materials.size());
        for (size_t i = 0; i < sc->materials.size(); ++i) {
            const scene::Material& m = sc->materials[i];
            GpuMaterial& g = materials[i];
            MaterialCB cb = {};
            cb.baseColor[0] = m.diffuse[0];
            cb.baseColor[1] = m.diffuse[1];
            cb.baseColor[2] = m.diffuse[2];
            cb.baseColor[3] = m.opacity;
            cb.emissive[0] = m.emissive[0];
            cb.emissive[1] = m.emissive[1];
            cb.emissive[2] = m.emissive[2];
            cb.emissive[3] = m.opacityThreshold;
            cb.pbr[0] = m.metallic;
            cb.pbr[1] = m.roughness;
            cb.pbr[2] = m.occlusion;
            cb.pbr[3] = m.ior;
            cb.specular[0] = m.specularColor[0];
            cb.specular[1] = m.specularColor[1];
            cb.specular[2] = m.specularColor[2];
            cb.specular[3] = m.useSpecularWorkflow ? 1.0f : 0.0f;
            const scene::TextureSlot* slots[8] = {&m.diffuseTex, &m.emissiveTex, &m.metallicTex, &m.roughnessTex,
                                                  &m.opacityTex, &m.normalTex, &m.occlusionTex, &m.specularTex};
            bool textured = false;
            for (int s = 0; s < 8; ++s) {
                const scene::TextureSlot& ts = *slots[s];
                int32_t* ch = s < 4 ? &cb.channels0[s] : &cb.channels1[s - 4];
                *ch = ts.channel;
                for (int k = 0; k < 4; ++k) {
                    cb.texScale[s][k] = ts.scale[k];
                    cb.texBias[s][k] = ts.bias[k];
                }
                for (int k = 0; k < 3; ++k) {
                    cb.uvRow0[s][k] = ts.uv[k];
                    cb.uvRow1[s][k] = ts.uv[3 + k];
                }
                if (ts.image < 0 || size_t(ts.image) >= textures.size() || !textures[size_t(ts.image)].tex) continue;
                const GpuTexture& t = textures[size_t(ts.image)];
                cb.flags[0] |= 1u << s;
                g.srv[s] = ts.srgb ? t.srgb.Get() : t.linear.Get();
                g.samp[s] = samplers[int(ts.wrapS) * 4 + int(ts.wrapT)].Get();
                textured = true;
            }
            for (int s = 0; s < 8; ++s)
                if (!g.samp[s]) g.samp[s] = samplers[0].Get();
            // A missing diffuse texture would otherwise render the 18% grey default: use a light grey.
            if (m.diffuseTex.image >= 0 && !(cb.flags[0] & 1u)) cb.baseColor[0] = cb.baseColor[1] = cb.baseColor[2] = 0.7f;
            (void)textured;
            if (m.usesVertexColor) cb.flags[1] |= 1;
            if (m.opacityThreshold > 0) {
                cb.flags[1] |= 2;
                g.kind = KindMasked;
            } else if (m.IsTranslucent()) {
                cb.flags[1] |= 4;
                g.kind = KindBlend;
            }
            D3D11_BUFFER_DESC bd = {};
            bd.ByteWidth = sizeof(MaterialCB);
            bd.Usage = D3D11_USAGE_IMMUTABLE;
            bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
            D3D11_SUBRESOURCE_DATA init = {&cb, 0, 0};
            dev->CreateBuffer(&bd, &init, &g.cb);
        }

        meshes.resize(sc->meshes.size());
        std::vector<float> pos, nrm, computed;
        std::vector<VertexAttr> attr;
        for (size_t i = 0; i < sc->meshes.size(); ++i) {
            scene::Mesh& m = sc->meshes[i];
            GpuMesh& g = meshes[i];
            const size_t n = m.cornerPoint.size();
            g.vertexCount = UINT(n);
            const std::vector<float>& pts = m.CurrentPoints();
            pos.resize(n * 3);
            for (size_t c = 0; c < n; ++c) std::memcpy(&pos[c * 3], &pts[size_t(m.cornerPoint[c]) * 3], 12);
            const std::vector<float>* normals = &m.normals;
            if (!m.cpuNormals.empty()) {
                normals = &m.cpuNormals;
            } else if (m.normals.empty()) {
                scene::ComputeNormals(m.restPoints, m.cornerPoint, m.smoothNormals, computed);
                normals = &computed;
            }
            attr.resize(n);
            for (size_t c = 0; c < n; ++c) {
                VertexAttr& a = attr[c];
                const uint32_t p = m.cornerPoint[c];
                a.uv[0] = m.uvs.empty() ? 0.f : m.uvs[c * 2];
                a.uv[1] = m.uvs.empty() ? 0.f : m.uvs[c * 2 + 1];
                for (int k = 0; k < 4; ++k) a.color[k] = m.colors.empty() ? (k == 3 ? 1.0f : 0.6f) : m.colors[c * 4 + size_t(k)];
                if (!m.jointWeights.empty()) {
                    for (int k = 0; k < 4; ++k) {
                        a.joints[k] = m.jointIndices[size_t(p) * 4 + size_t(k)];
                        a.weights[k] = m.jointWeights[size_t(p) * 4 + size_t(k)];
                    }
                } else {
                    std::memset(a.joints, 0, sizeof(a.joints));
                    a.weights[0] = 1;
                    a.weights[1] = a.weights[2] = a.weights[3] = 0;
                }
            }
            g.dynamicPos = m.HasCpuDeformation();
            g.dynamicNrm = g.dynamicPos && (m.normals.empty() || m.pointsAttr.IsAnimated());
            g.pos = MakeVB(pos.data(), pos.size() * 4, g.dynamicPos);
            g.nrm = MakeVB(normals->data(), normals->size() * 4, g.dynamicNrm);
            g.attr = MakeVB(attr.data(), attr.size() * sizeof(VertexAttr), false);
            m.pointsDirty = false;
        }
        scenes.push_back(std::move(gs));
        imageValid = false;
    }

    void UpdateDynamic(GpuScene& gs) {
        for (size_t i = 0; i < gs.meshes.size() && i < gs.scene->meshes.size(); ++i) {
            scene::Mesh& m = gs.scene->meshes[i];
            GpuMesh& g = gs.meshes[i];
            if (!m.pointsDirty || !g.dynamicPos) continue;
            m.pointsDirty = false;
            D3D11_MAPPED_SUBRESOURCE map;
            const std::vector<float>& pts = m.CurrentPoints();
            if (SUCCEEDED(ctx->Map(g.pos.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &map))) {
                float* out = static_cast<float*>(map.pData);
                for (size_t c = 0; c < m.cornerPoint.size(); ++c)
                    std::memcpy(out + c * 3, &pts[size_t(m.cornerPoint[c]) * 3], 12);
                ctx->Unmap(g.pos.Get(), 0);
            }
            if (g.dynamicNrm && m.cpuNormals.size() == m.cornerPoint.size() * 3 &&
                SUCCEEDED(ctx->Map(g.nrm.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &map))) {
                std::memcpy(map.pData, m.cpuNormals.data(), m.cpuNormals.size() * 4);
                ctx->Unmap(g.nrm.Get(), 0);
            }
        }
    }

    void EnsurePalette(size_t count) {
        if (count <= paletteCap && paletteBuf) return;
        size_t cap = std::max<size_t>(256, paletteCap);
        while (cap < count) cap *= 2;
        D3D11_BUFFER_DESC bd = {};
        bd.ByteWidth = UINT(cap * sizeof(PaletteEntry));
        bd.Usage = D3D11_USAGE_DYNAMIC;
        bd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
        bd.StructureByteStride = sizeof(PaletteEntry);
        paletteBuf.Reset();
        paletteSRV.Reset();
        dev->CreateBuffer(&bd, nullptr, &paletteBuf);
        D3D11_SHADER_RESOURCE_VIEW_DESC vd = {};
        vd.Format = DXGI_FORMAT_UNKNOWN;
        vd.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
        vd.Buffer.NumElements = UINT(cap);
        dev->CreateShaderResourceView(paletteBuf.Get(), &vd, &paletteSRV);
        paletteCap = cap;
    }

    void DrawLines(const std::vector<LineVertex>& verts, const Mat4& viewProj) {
        if (verts.empty()) return;
        if (verts.size() > lineCap || !lineVB) {
            lineCap = std::max<size_t>(verts.size(), 1024);
            lineVB = MakeVB(nullptr, lineCap * sizeof(LineVertex), true);
        }
        D3D11_MAPPED_SUBRESOURCE map;
        if (FAILED(ctx->Map(lineVB.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &map))) return;
        std::memcpy(map.pData, verts.data(), verts.size() * sizeof(LineVertex));
        ctx->Unmap(lineVB.Get(), 0);
        LineCB cb;
        ToFloats(viewProj, cb.viewProj);
        Upload(lineCB.Get(), cb);
        UINT stride = sizeof(LineVertex), offset = 0;
        ctx->IASetVertexBuffers(0, 1, lineVB.GetAddressOf(), &stride, &offset);
        ctx->IASetInputLayout(lineLayout.Get());
        ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_LINELIST);
        ctx->VSSetShader(lineVS.Get(), nullptr, 0);
        ctx->VSSetConstantBuffers(0, 1, lineCB.GetAddressOf());
        ctx->PSSetShader(linePS.Get(), nullptr, 0);
        ctx->RSSetState(rsLines.Get());
        ctx->Draw(UINT(verts.size()), 0);
    }

    void Render(const Camera& cam, const RenderOptions& opt, bool redrawScene) {
        if (!swap || lost || width <= 0 || height <= 0 || !backRTV) return;
        ctx->ClearState();  // Direct2D shares this context; every pass binds what it needs
        if (redrawScene || !imageValid) {
            RenderScene(cam, opt);
            imageValid = true;
        }
        Composite(cam, opt);
    }

    // Draws the scene into the HDR target and resolves it into hdrResolved.
    void RenderScene(const Camera& cam, const RenderOptions& opt) {
        struct Item {
            uint32_t scene, mesh, subset, palette;
            int material, kind;
            float depth;
        };
        std::vector<Item> items;
        std::vector<PaletteEntry> pal;
        const Vec3 eye = cam.Eye();
        const Vec3 back = cam.Back();
        for (size_t si = 0; si < scenes.size(); ++si) {
            GpuScene& gs = *scenes[si];
            if (!gs.shown) continue;
            UpdateDynamic(gs);
            for (size_t i = 0; i < gs.scene->meshes.size() && i < gs.meshes.size(); ++i) {
                const scene::Mesh& m = gs.scene->meshes[i];
                if (!m.visible || gs.meshes[i].vertexCount == 0) continue;
                auto addItems = [&](uint32_t paletteOffset, const Mat4& placement) {
                    Vec3 origin = placement.TransformPoint({0, 0, 0});
                    float depth = float(-(origin - eye).Dot(back));
                    for (size_t s = 0; s < m.subsets.size(); ++s) {
                        int mat = m.subsets[s].material;
                        int kind = size_t(mat) < gs.materials.size() ? gs.materials[size_t(mat)].kind : KindOpaque;
                        items.push_back({uint32_t(si), uint32_t(i), uint32_t(s), paletteOffset, mat, kind, depth});
                    }
                };
                if (m.instancer >= 0) {
                    for (const Mat4& x : m.instances) {
                        uint32_t off = uint32_t(pal.size());
                        pal.emplace_back();
                        FillPalette(x, pal.back());
                        addItems(off, x);
                    }
                } else if (!m.palette.empty()) {
                    uint32_t off = uint32_t(pal.size());
                    for (const Mat4& x : m.palette) {
                        pal.emplace_back();
                        FillPalette(x, pal.back());
                    }
                    addItems(off, m.palette.back());
                }
            }
        }
        if (!pal.empty()) {
            EnsurePalette(pal.size());
            D3D11_MAPPED_SUBRESOURCE map;
            if (SUCCEEDED(ctx->Map(paletteBuf.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &map))) {
                std::memcpy(map.pData, pal.data(), pal.size() * sizeof(PaletteEntry));
                ctx->Unmap(paletteBuf.Get(), 0);
            }
        }

        const double aspect = double(width) / double(height);
        const Mat4 view = cam.View();
        const Mat4 proj = cam.Projection(aspect);
        const Mat4 viewProj = view * proj;

        FrameCB fc = {};
        ToFloats(viewProj, fc.viewProj);
        fc.cameraPos[0] = float(eye.x);
        fc.cameraPos[1] = float(eye.y);
        fc.cameraPos[2] = float(eye.z);
        // Studio rig that follows the camera: key (upper left), fill (right), rim (behind).
        const Vec3 right = cam.Right(), up = cam.Up();
        const double dirs[3][3] = {{-0.45, 0.65, 0.62}, {0.75, 0.12, 0.45}, {0.15, 0.55, -0.82}};
        const float colors[3][3] = {{2.9f, 2.78f, 2.6f}, {0.55f, 0.62f, 0.75f}, {1.7f, 1.75f, 1.85f}};
        for (int l = 0; l < 3; ++l) {
            Vec3 d = (right * dirs[l][0] + up * dirs[l][1] + back * dirs[l][2]).Normalized();
            fc.lightDir[l][0] = float(d.x);
            fc.lightDir[l][1] = float(d.y);
            fc.lightDir[l][2] = float(d.z);
            for (int k = 0; k < 3; ++k) fc.lightColor[l][k] = colors[l][k];
        }
        fc.skyColor[0] = 0.34f; fc.skyColor[1] = 0.37f; fc.skyColor[2] = 0.42f;
        fc.groundColor[0] = 0.13f; fc.groundColor[1] = 0.12f; fc.groundColor[2] = 0.11f;
        fc.frameParams[0] = float(opt.shading);
        fc.frameParams[1] = float(samples);
        fc.frameParams[2] = 1.0f;

        // Mesh pass helpers.
        auto bindMeshPipeline = [&]() {
            ctx->IASetInputLayout(meshLayout.Get());
            ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            ctx->VSSetShader(meshVS.Get(), nullptr, 0);
            ID3D11Buffer* cbs[3] = {frameCB.Get(), drawCB.Get(), nullptr};
            ctx->VSSetConstantBuffers(0, 2, cbs);
            ctx->PSSetConstantBuffers(0, 2, cbs);
            ctx->VSSetShaderResources(0, 1, paletteSRV.GetAddressOf());
        };
        const GpuMesh* boundMesh = nullptr;
        const GpuMaterial* boundMaterial = nullptr;
        auto drawItem = [&](const Item& it, bool bindMaterial) {
            const GpuScene& gs = *scenes[it.scene];
            const scene::Mesh& m = gs.scene->meshes[it.mesh];
            const GpuMesh& g = gs.meshes[it.mesh];
            if (boundMesh != &g) {
                ID3D11Buffer* vbs[3] = {g.pos.Get(), g.nrm.Get(), g.attr.Get()};
                UINT strides[3] = {12, 12, sizeof(VertexAttr)};
                UINT offsets[3] = {0, 0, 0};
                ctx->IASetVertexBuffers(0, 3, vbs, strides, offsets);
                boundMesh = &g;
            }
            if (bindMaterial && size_t(it.material) < gs.materials.size() && boundMaterial != &gs.materials[size_t(it.material)]) {
                const GpuMaterial& gm = gs.materials[size_t(it.material)];
                ctx->PSSetConstantBuffers(2, 1, gm.cb.GetAddressOf());
                ctx->PSSetShaderResources(0, 8, gm.srv);
                ctx->PSSetSamplers(0, 8, gm.samp);
                boundMaterial = &gm;
            }
            DrawCB dc = {};
            dc.paletteOffset = it.palette;
            if (m.highlighted) {
                dc.tint[0] = 1.0f;
                dc.tint[1] = 0.62f;
                dc.tint[2] = 0.18f;
                dc.tint[3] = 0.35f;
            }
            Upload(drawCB.Get(), dc);
            const scene::SubMesh& sm = m.subsets[it.subset];
            ctx->Draw(sm.count, sm.first);
        };
        auto cullFor = [&](const Item& it) {
            const scene::Mesh& m = scenes[it.scene]->scene->meshes[it.mesh];
            return (m.doubleSided || opt.showBackfaces) ? rsCullNone.Get() : rsCullBack.Get();
        };

        // Ground shadow: orthographic depth map looking straight down over the shadow bounds.
        shadowActive = opt.groundShadow && shadowDSV && !items.empty() && !opt.shadowBounds.IsEmpty() &&
                       opt.wireframe != 1;
        if (shadowActive) {
            const scene::Bounds& b = opt.shadowBounds;
            const Vec3 c = b.Center();
            const double half = std::max(b.max.x - b.min.x, b.max.z - b.min.z) * 0.5 * 1.3 + 1e-4;
            const double top = b.max.y + half * 0.05 + 1e-4;
            const double bottom = std::min(b.min.y, opt.gridHeight) - half * 0.05 - 1e-4;
            shadowRange = top - bottom;
            shadowUnitsPerUV = 2 * half;
            shadowMaxHeight = std::max(b.max.y - opt.gridHeight, 1e-4) * 1.6;
            const double s = 1.0 / half;
            Mat4 m;
            std::memset(m.m, 0, sizeof(m.m));
            m.m[0] = s;                    // clip.x from world x
            m.m[9] = s;                    // clip.y from world z
            m.m[6] = -1.0 / shadowRange;   // depth from height
            m.m[12] = -c.x * s;
            m.m[13] = -c.z * s;
            m.m[14] = top / shadowRange;
            m.m[15] = 1.0;
            shadowVP = m;

            FrameCB sfc = fc;
            ToFloats(shadowVP, sfc.viewProj);
            sfc.frameParams[1] = 1.0f;  // no MSAA in the shadow map: masked coverage becomes an alpha test
            Upload(frameCB.Get(), sfc);
            ID3D11ShaderResourceView* nullSrv = nullptr;
            ctx->PSSetShaderResources(0, 1, &nullSrv);
            ctx->OMSetRenderTargets(0, nullptr, shadowDSV.Get());
            ctx->ClearDepthStencilView(shadowDSV.Get(), D3D11_CLEAR_DEPTH, 1.0f, 0);
            D3D11_VIEWPORT svp = {0, 0, float(kShadowSize), float(kShadowSize), 0, 1};
            ctx->RSSetViewports(1, &svp);
            const float blendFactor[4] = {0, 0, 0, 0};
            ctx->OMSetBlendState(bsOpaque.Get(), blendFactor, 0xffffffff);
            ctx->OMSetDepthStencilState(dsDefault.Get(), 0);
            ctx->RSSetState(rsCullNone.Get());
            bindMeshPipeline();
            for (const Item& it : items) {
                if (it.kind == KindBlend) continue;
                ctx->PSSetShader(it.kind == KindMasked ? psMasked.Get() : nullptr, nullptr, 0);
                drawItem(it, it.kind == KindMasked);
            }
            boundMesh = nullptr;
            boundMaterial = nullptr;
        }

        Upload(frameCB.Get(), fc);
        D3D11_VIEWPORT vp = {0, 0, float(width), float(height), 0, 1};
        ctx->RSSetViewports(1, &vp);
        const float clear[4] = {0, 0, 0, 0};
        ctx->OMSetRenderTargets(1, hdrRTV.GetAddressOf(), dsv.Get());
        ctx->ClearRenderTargetView(hdrRTV.Get(), clear);
        ctx->ClearDepthStencilView(dsv.Get(), D3D11_CLEAR_DEPTH, 1.0f, 0);
        const bool ground = opt.grid || shadowActive;

        if (!items.empty()) {
            bindMeshPipeline();
            std::vector<const Item*> opaque, blended;
            for (const Item& it : items) (it.kind == KindBlend ? blended : opaque).push_back(&it);
            std::stable_sort(opaque.begin(), opaque.end(), [](const Item* a, const Item* b) { return a->scene != b->scene ? a->scene < b->scene : a->material < b->material; });
            std::stable_sort(blended.begin(), blended.end(), [](const Item* a, const Item* b) { return a->depth > b->depth; });
            const float blendFactor[4] = {0, 0, 0, 0};
            if (opt.wireframe != 1) {
                ctx->OMSetBlendState(bsOpaque.Get(), blendFactor, 0xffffffff);
                ctx->OMSetDepthStencilState(dsDefault.Get(), 0);
                for (const Item* it : opaque) {
                    ctx->RSSetState(cullFor(*it));
                    ctx->PSSetShader(it->kind == KindMasked ? psMasked.Get() : psOpaque.Get(), nullptr, 0);
                    drawItem(*it, true);
                }
            } else {
                ctx->OMSetBlendState(bsOpaque.Get(), blendFactor, 0xffffffff);
                ctx->OMSetDepthStencilState(dsDefault.Get(), 0);
                ctx->RSSetState(rsWire.Get());
                ctx->PSSetShader(psWire.Get(), nullptr, 0);
                for (const Item& it : items) drawItem(it, false);
            }

            if (ground) DrawGrid(viewProj, eye, opt);
            bindMeshPipeline();
            boundMesh = nullptr;
            boundMaterial = nullptr;  // the ground pass rebinds texture slot 0

            if (opt.wireframe != 1 && !blended.empty()) {
                ctx->OMSetBlendState(bsPremul.Get(), blendFactor, 0xffffffff);
                ctx->OMSetDepthStencilState(dsReadOnly.Get(), 0);
                ctx->PSSetShader(psBlend.Get(), nullptr, 0);
                for (const Item* it : blended) {
                    ctx->RSSetState(cullFor(*it));
                    drawItem(*it, true);
                }
            }
            if (opt.wireframe == 2) {
                ctx->OMSetBlendState(bsOpaque.Get(), blendFactor, 0xffffffff);
                ctx->OMSetDepthStencilState(dsReadOnly.Get(), 0);
                ctx->RSSetState(rsWireOverlay.Get());
                ctx->PSSetShader(psWire.Get(), nullptr, 0);
                for (const Item& it : items) drawItem(it, false);
            }
        } else if (ground) {
            DrawGrid(viewProj, eye, opt);
        }

        // Skeleton overlay (drawn through the meshes).
        if (opt.skeleton) {
            std::vector<LineVertex> lines;
            for (const auto& gs : scenes)
            for (const scene::Skeleton& k : gs->scene->skeletons) {
                const scene::Scene* sc = gs->scene;
                if (!k.valid || sc->hidden || !gs->shown) continue;
                const Mat4 world = k.node >= 0 ? sc->nodes[size_t(k.node)].world : sc->RootTransform();
                if (k.node >= 0 && !sc->nodes[size_t(k.node)].visible) continue;
                std::vector<Vec3> joints(k.joints.size());
                for (size_t j = 0; j < joints.size(); ++j) joints[j] = (k.skelSpace[j] * world).TransformPoint({0, 0, 0});
                for (size_t j = 0; j < joints.size(); ++j) {
                    int p = k.parents[j];
                    if (p < 0) continue;
                    const Vec3& a = joints[size_t(p)];
                    const Vec3& b = joints[j];
                    lines.push_back({{float(a.x), float(a.y), float(a.z)}, {1.0f, 0.75f, 0.2f, 1}});
                    lines.push_back({{float(b.x), float(b.y), float(b.z)}, {0.3f, 1.0f, 0.5f, 1}});
                }
            }
            const float blendFactor[4] = {0, 0, 0, 0};
            ctx->OMSetBlendState(bsPremul.Get(), blendFactor, 0xffffffff);
            ctx->OMSetDepthStencilState(dsNone.Get(), 0);
            DrawLines(lines, viewProj);
        }

        if (samples > 1) ctx->ResolveSubresource(hdrResolved.Get(), 0, hdrMs.Get(), 0, DXGI_FORMAT_R16G16B16A16_FLOAT);
    }

    // Background + tonemap of the last rendered scene image, axis gizmo, present. Cheap enough to run every
    // refresh, which keeps the frame rate steady (variable-refresh displays flicker when it jumps).
    void Composite(const Camera& cam, const RenderOptions& opt) {
        const float uiScale = std::max(opt.uiScale, 1.0f);
        const Mat4 view = cam.View();
        D3D11_VIEWPORT vp = {0, 0, float(width), float(height), 0, 1};
        ctx->RSSetViewports(1, &vp);
        ctx->OMSetRenderTargets(1, backRTV.GetAddressOf(), nullptr);
        CompositeCB cc = {};
        cc.params[0] = opt.exposure;
        cc.params[1] = float(height);
        const float top[3] = {0.29f, 0.31f, 0.35f}, bottom[3] = {0.11f, 0.115f, 0.13f};
        for (int k = 0; k < 3; ++k) {
            cc.bgTop[k] = SrgbToLinear(top[k]);
            cc.bgBottom[k] = SrgbToLinear(bottom[k]);
        }
        Upload(compCB.Get(), cc);
        const float blendFactor[4] = {0, 0, 0, 0};
        ctx->OMSetBlendState(bsOpaque.Get(), blendFactor, 0xffffffff);
        ctx->OMSetDepthStencilState(dsNone.Get(), 0);
        ctx->RSSetState(rsCullNone.Get());
        ctx->IASetInputLayout(nullptr);
        ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ctx->VSSetShader(compVS.Get(), nullptr, 0);
        ctx->PSSetShader(compPS.Get(), nullptr, 0);
        ctx->PSSetConstantBuffers(0, 1, compCB.GetAddressOf());
        ctx->PSSetShaderResources(0, 1, hdrSRV.GetAddressOf());
        ctx->Draw(3, 0);
        ID3D11ShaderResourceView* nullSrv[8] = {};
        ctx->PSSetShaderResources(0, 8, nullSrv);

        std::vector<TextLabel> labels;
        const Mat4 viewProj = view * cam.Projection(double(width) / double(height));

        // Ruler (drawn over the scene so it stays readable).
        if (opt.ruler && !opt.rulers.empty()) {
            std::vector<LineVertex> lines;
            for (const RenderOptions::RulerBox& box : opt.rulers)
                if (!box.bounds.IsEmpty()) AddRuler(cam, box, viewProj, lines, labels);
            ctx->OMSetBlendState(bsPremul.Get(), blendFactor, 0xffffffff);
            DrawLines(lines, viewProj);
        }

        // Orientation gizmo in the lower-left corner.
        if (opt.axes) {
            const float size = 70.0f * uiScale;
            D3D11_VIEWPORT gvp = {8.0f * uiScale, float(height) - size - 8.0f * uiScale, size, size, 0, 1};
            ctx->RSSetViewports(1, &gvp);
            Mat4 rot = view;
            rot.m[12] = rot.m[13] = rot.m[14] = 0;
            Mat4 ortho = Mat4::Scale(0.8, 0.8, -0.1) * Mat4::Translate(0, 0, 0.5);
            const float colors[3][3] = {{0.95f, 0.3f, 0.3f}, {0.4f, 0.9f, 0.35f}, {0.35f, 0.55f, 1.0f}};
            std::vector<LineVertex> axes;
            for (int a = 0; a < 3; ++a) {
                float tip[3] = {0, 0, 0};
                tip[a] = 1;
                axes.push_back({{0, 0, 0}, {colors[a][0], colors[a][1], colors[a][2], 1}});
                axes.push_back({{tip[0], tip[1], tip[2]}, {colors[a][0], colors[a][1], colors[a][2], 1}});
                // Axis letter just past the tip (sRGB text color ~ the linear line color brightened).
                Vec3 p = (rot * ortho).TransformPoint({tip[0] * 1.22, tip[1] * 1.22, tip[2] * 1.22});
                labels.push_back({std::wstring(1, L"XYZ"[a]), gvp.TopLeftX + float(p.x * 0.5 + 0.5) * size,
                                  gvp.TopLeftY + float(0.5 - p.y * 0.5) * size, 0, true, false,
                                  {std::sqrt(colors[a][0]), std::sqrt(colors[a][1]), std::sqrt(colors[a][2])}});
            }
            ctx->OMSetBlendState(bsPremul.Get(), blendFactor, 0xffffffff);
            DrawLines(axes, rot * ortho);
            ctx->RSSetViewports(1, &vp);
        }

        DrawLabels(labels, uiScale);

        HRESULT hr = swap->Present(1, 0);
        if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) lost = true;
    }

    // Projects a world point to viewport pixels; false if it is behind the camera.
    bool Project(const Mat4& vp, const Vec3& p, float& sx, float& sy) const {
        const double* m = vp.m;
        const double x = p.x * m[0] + p.y * m[4] + p.z * m[8] + m[12];
        const double y = p.x * m[1] + p.y * m[5] + p.z * m[9] + m[13];
        const double w = p.x * m[3] + p.y * m[7] + p.z * m[11] + m[15];
        if (w <= 1e-9) return false;
        sx = float((x / w * 0.5 + 0.5) * width);
        sy = float((0.5 - y / w * 0.5) * height);
        return true;
    }

    // Bounding box with width / depth dimension lines along the bottom edges facing the camera and a
    // height ruler (ticks + labels) on the vertical edge at the screen's left side of the box.
    void AddRuler(const Camera& cam, const RenderOptions::RulerBox& box, const Mat4& viewProj,
                  std::vector<LineVertex>& lines, std::vector<TextLabel>& labels) const {
        const scene::Bounds& b = box.bounds;
        const Vec3 mn = b.min, mx = b.max, c = b.Center();
        const double size[3] = {mx.x - mn.x, mx.y - mn.y, mx.z - mn.z};
        const double maxDim = std::max({size[0], size[1], size[2], 1e-9});
        const double off = maxDim * 0.07;
        const double mpu = box.metersPerUnit > 0 ? box.metersPerUnit : 1.0;
        const Vec3 eye = cam.Eye();
        const float accent[4] = {1.0f, 0.62f, 0.12f, 1.0f};  // linear (the back buffer view is sRGB)
        const float faint[4] = {1.0f, 0.62f, 0.12f, 0.35f};
        const float labelColor[3] = {1.0f, 0.86f, 0.55f};
        auto line = [&](const Vec3& p0, const Vec3& p1, const float* col) {
            lines.push_back({{float(p0.x), float(p0.y), float(p0.z)}, {col[0], col[1], col[2], col[3]}});
            lines.push_back({{float(p1.x), float(p1.y), float(p1.z)}, {col[0], col[1], col[2], col[3]}});
        };
        auto label = [&](const std::wstring& text, const Vec3& at, int anchor, bool compact) {
            float sx, sy;
            if (Project(viewProj, at, sx, sy))
                labels.push_back({text, sx, sy, anchor, compact, true, {labelColor[0], labelColor[1], labelColor[2]}});
        };

        // Box.
        Vec3 corner[8];
        for (int i = 0; i < 8; ++i) corner[i] = {i & 1 ? mx.x : mn.x, i & 2 ? mx.y : mn.y, i & 4 ? mx.z : mn.z};
        for (int i = 0; i < 8; ++i)
            for (int bit : {1, 2, 4})
                if (!(i & bit)) line(corner[i], corner[i | bit], faint);

        // One dimension line from p0 to p1, ticks pointing along `outward`.
        auto dimension = [&](const Vec3& p0, const Vec3& p1, double lengthUnits, const Vec3& outward, bool tickLabels) {
            line(p0, p1, accent);
            const Vec3 tick = outward * (off * 0.28);
            line(p0 - tick, p0 + tick * 1.8, accent);  // end caps
            line(p1 - tick, p1 + tick * 1.8, accent);
            if (lengthUnits <= 0) return;
            const double stepM = NiceStep(lengthUnits * mpu);
            const double stepU = stepM / mpu;
            const Vec3 dir = (p1 - p0) * (1.0 / lengthUnits);
            const int count = int(std::floor(lengthUnits / stepU + 1e-6));
            const int labelEvery = std::max(1, (count + 5) / 6);
            // Which side of the line the outward direction points to on screen.
            float ax, ay, bx, by;
            int anchor = 1;
            if (Project(viewProj, p0, ax, ay) && Project(viewProj, p0 + outward, bx, by)) anchor = bx < ax ? 2 : 1;
            for (int i = 1; i <= count; ++i) {
                const Vec3 p = p0 + dir * (i * stepU);
                if ((p1 - p).Length() < stepU * 0.25) continue;  // too close to the end cap
                line(p, p + tick, accent);
                if (tickLabels && i % labelEvery == 0) {
                    const double v = i * stepM;
                    wchar_t buf[32];
                    if (stepM >= 1) std::swprintf(buf, 32, L"%g m", std::round(v * 1000) / 1000);
                    else if (stepM >= 0.01) std::swprintf(buf, 32, L"%g cm", std::round(v * 100 * 100) / 100);
                    else std::swprintf(buf, 32, L"%g mm", std::round(v * 1000 * 100) / 100);
                    label(buf, p + tick * 2.2, anchor, true);
                }
            }
        };

        // Bottom edges facing the camera: width (X) and depth (Z).
        const double xs = eye.x >= c.x ? mx.x : mn.x, zs = eye.z >= c.z ? mx.z : mn.z;
        const double dx = xs == mx.x ? 1.0 : -1.0, dz = zs == mx.z ? 1.0 : -1.0;
        const double y0 = mn.y;
        const Vec3 wa{mn.x, y0, zs + dz * off}, wb{mx.x, y0, zs + dz * off};
        const Vec3 da{xs + dx * off, y0, mn.z}, db{xs + dx * off, y0, mx.z};
        line({mn.x, y0, zs}, wa + Vec3{0, 0, dz * off * 0.25}, faint);  // extension lines
        line({mx.x, y0, zs}, wb + Vec3{0, 0, dz * off * 0.25}, faint);
        line({xs, y0, mn.z}, da + Vec3{dx * off * 0.25, 0, 0}, faint);
        line({xs, y0, mx.z}, db + Vec3{dx * off * 0.25, 0, 0}, faint);
        dimension(wa, wb, size[0], {0, 0, dz}, false);
        dimension(da, db, size[2], {dx, 0, 0}, false);
        label(L"Width " + FormatLength(size[0] * mpu, true), (wa + wb) * 0.5 + Vec3{0, 0, dz * off * 1.1}, 0, false);
        label(L"Depth " + FormatLength(size[2] * mpu, true), (da + db) * 0.5 + Vec3{dx * off * 1.1, 0, 0}, 0, false);

        // Height ruler on the bottom corner that projects furthest to the left.
        int best = 0;
        float bestX = 1e30f;
        for (int i = 0; i < 4; ++i) {
            const Vec3 p{i & 1 ? mx.x : mn.x, y0, i & 2 ? mx.z : mn.z};
            float sx, sy;
            if (Project(viewProj, p, sx, sy) && sx < bestX) {
                bestX = sx;
                best = i;
            }
        }
        const double hx0 = best & 1 ? mx.x : mn.x, hz0 = best & 2 ? mx.z : mn.z;
        const Vec3 out = Vec3{hx0 >= c.x ? 1.0 : -1.0, 0, hz0 >= c.z ? 1.0 : -1.0}.Normalized();
        const Vec3 ha = Vec3{hx0, mn.y, hz0} + out * off, hb = Vec3{hx0, mx.y, hz0} + out * off;
        line({hx0, mn.y, hz0}, ha + out * (off * 0.25), faint);
        line({hx0, mx.y, hz0}, hb + out * (off * 0.25), faint);
        dimension(ha, hb, size[1], out, true);
        label(L"Height " + FormatLength(size[1] * mpu, true), hb + Vec3{0, off * 0.9, 0}, 0, false);
    }

    void EnsureTextFormats(float scale) {
        if (!dwFactory || (labelFormat && textScale == scale)) return;
        labelFormat.Reset();
        smallFormat.Reset();
        dwFactory->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STYLE_NORMAL,
                                    DWRITE_FONT_STRETCH_NORMAL, 13.0f * scale, L"en-us", &labelFormat);
        dwFactory->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
                                    DWRITE_FONT_STRETCH_NORMAL, 11.0f * scale, L"en-us", &smallFormat);
        for (IDWriteTextFormat* f : {labelFormat.Get(), smallFormat.Get()})
            if (f) f->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        textScale = scale;
    }

    void DrawLabels(const std::vector<TextLabel>& labels, float scale) {
        if (!d2dCtx || !d2dTarget || labels.empty()) return;
        EnsureTextFormats(scale);
        if (!labelFormat || !smallFormat) return;
        d2dCtx->SetTarget(d2dTarget.Get());
        d2dCtx->BeginDraw();
        for (const TextLabel& l : labels) {
            IDWriteTextFormat* fmt = l.compact ? smallFormat.Get() : labelFormat.Get();
            ComPtr<IDWriteTextLayout> layout;
            if (FAILED(dwFactory->CreateTextLayout(l.text.c_str(), UINT32(l.text.size()), fmt, 4096.0f, 4096.0f, &layout)))
                continue;
            DWRITE_TEXT_METRICS tm;
            layout->GetMetrics(&tm);
            const float left = l.anchor == 0 ? l.x - tm.width / 2 : l.anchor == 1 ? l.x : l.x - tm.width;
            const float top = l.y - tm.height / 2;
            if (l.background) {
                const float px = 5.0f * scale, py = 1.5f * scale;
                D2D1_ROUNDED_RECT rr = {D2D1::RectF(left - px, top - py, left + tm.width + px, top + tm.height + py),
                                        3.0f * scale, 3.0f * scale};
                d2dCtx->FillRoundedRectangle(rr, backBrush.Get());
            }
            textBrush->SetColor(D2D1::ColorF(l.color[0], l.color[1], l.color[2], 1.0f));
            d2dCtx->DrawTextLayout(D2D1::Point2F(left, top), layout.Get(), textBrush.Get());
        }
        d2dCtx->EndDraw();
    }

    void DrawGrid(const Mat4& viewProj, const Vec3& eye, const RenderOptions& opt) {
        GridCB gc = {};
        ToFloats(viewProj, gc.viewProj);
        gc.params[0] = float(opt.gridCell);
        gc.params[1] = float(opt.gridExtent);
        gc.params[2] = float(opt.gridHeight);
        gc.params[3] = float(opt.gridExtent);
        gc.cameraPos[0] = float(eye.x);
        gc.cameraPos[1] = float(eye.y);
        gc.cameraPos[2] = float(eye.z);
        ToFloats(shadowVP, gc.shadowViewProj);
        gc.shadowParams[0] = shadowActive ? 1.0f : 0.0f;
        gc.shadowParams[1] = float(shadowRange);
        gc.shadowParams[2] = float(shadowUnitsPerUV);
        gc.shadowParams[3] = float(shadowMaxHeight);
        gc.groundFlags[0] = opt.grid ? 1.0f : 0.0f;
        gc.groundFlags[1] = 0.75f;
        Upload(gridCB.Get(), gc);
        ID3D11ShaderResourceView* srv = shadowActive ? shadowSRV.Get() : nullptr;
        ctx->PSSetShaderResources(0, 1, &srv);
        ID3D11SamplerState* samps[2] = {shadowCmp.Get(), shadowPoint.Get()};
        ctx->PSSetSamplers(0, 2, samps);
        const float blendFactor[4] = {0, 0, 0, 0};
        ctx->OMSetBlendState(bsPremul.Get(), blendFactor, 0xffffffff);
        ctx->OMSetDepthStencilState(dsReadOnly.Get(), 0);
        ctx->RSSetState(rsCullNone.Get());
        ctx->IASetInputLayout(nullptr);
        ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ctx->VSSetShader(gridVS.Get(), nullptr, 0);
        ctx->VSSetConstantBuffers(0, 1, gridCB.GetAddressOf());
        ctx->PSSetShader(gridPS.Get(), nullptr, 0);
        ctx->PSSetConstantBuffers(0, 1, gridCB.GetAddressOf());
        ctx->Draw(6, 0);
    }
};

Renderer::Renderer() : impl_(std::make_unique<Impl>()) {}
Renderer::~Renderer() = default;
bool Renderer::Init(HWND hwnd, std::string& err) { return impl_->Init(hwnd, err); }
void Renderer::Resize(int w, int h) { impl_->Resize(w, h); }
void Renderer::AddScene(scene::Scene* sc, std::vector<DecodedImage> images) { impl_->AddScene(sc, std::move(images)); }
void Renderer::RemoveScene(const scene::Scene* sc) { impl_->RemoveScene(sc); }
void Renderer::ClearScenes() { impl_->ClearScenes(); }
void Renderer::ShowScenes(const std::vector<const scene::Scene*>& shown) { impl_->ShowScenes(shown); }
void Renderer::Render(const Camera& c, const RenderOptions& o, bool redrawScene) { impl_->Render(c, o, redrawScene); }
const std::string& Renderer::AdapterName() const { return impl_->adapterName; }
int Renderer::MsaaSamples() const { return impl_->samples; }
bool Renderer::DeviceLost() const { return impl_->lost; }
size_t Renderer::TextureMemory() const {
    size_t n = 0;
    for (const auto& s : impl_->scenes) n += s->textureBytes;
    return n;
}
std::vector<std::string> Renderer::TakeWarnings() {
    std::vector<std::string> w;
    w.swap(impl_->warnings);
    return w;
}

}  // namespace render
