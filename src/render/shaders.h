// HLSL sources, compiled at startup with D3DCompile (d3dcompiler_47.dll ships with Windows 10/11).
#pragma once

namespace render {

// Mesh shading: GPU skinning from a palette of row-vector matrices, UsdPreviewSurface-style PBR.
inline const char* kMeshShader = R"HLSL(
#define PI 3.14159265

cbuffer FrameCB : register(b0) {
    row_major float4x4 viewProj;
    float4 cameraPos;
    float4 lightDir[3];    // xyz: direction towards the light
    float4 lightColor[3];  // rgb: radiance
    float4 skyColor;
    float4 groundColor;
    float4 frameParams;    // x: shading mode (0 lit, 1 base color, 2 normals), y: MSAA samples, z: env strength
};

cbuffer DrawCB : register(b1) {
    uint paletteOffset;
    uint drawFlags;
    float2 drawPad;
    float4 tint;           // rgb: highlight color, a: amount
};

cbuffer MaterialCB : register(b2) {
    float4 baseColor;      // diffuse rgb, opacity
    float4 emissive;       // rgb, opacityThreshold
    float4 pbr;            // metallic, roughness, occlusion, ior
    float4 specular;       // specularColor rgb, useSpecularWorkflow
    uint4 matFlags;        // x: texture slot mask, y: 1 = vertex color, 2 = alpha test, 4 = translucent
    int4 channels0;        // channel select for slots 0..3 (-1 = rgb)
    int4 channels1;        // slots 4..7
    float4 texScale[8];
    float4 texBias[8];
    float4 uvRow0[8];
    float4 uvRow1[8];
};

struct PaletteEntry { float4 r0, r1, r2, r3, n0, n1, n2, pad; };
StructuredBuffer<PaletteEntry> palette : register(t0);

Texture2D texDiffuse : register(t0);
Texture2D texEmissive : register(t1);
Texture2D texMetallic : register(t2);
Texture2D texRoughness : register(t3);
Texture2D texOpacity : register(t4);
Texture2D texNormal : register(t5);
Texture2D texOcclusion : register(t6);
Texture2D texSpecular : register(t7);
SamplerState sDiffuse : register(s0);
SamplerState sEmissive : register(s1);
SamplerState sMetallic : register(s2);
SamplerState sRoughness : register(s3);
SamplerState sOpacity : register(s4);
SamplerState sNormal : register(s5);
SamplerState sOcclusion : register(s6);
SamplerState sSpecular : register(s7);

struct VSIn {
    float3 pos : POSITION;
    float3 nrm : NORMAL;
    float2 uv : TEXCOORD0;
    float4 col : COLOR0;
    uint4 joints : BLENDINDICES0;
    float4 weights : BLENDWEIGHT0;
};

struct VSOut {
    float4 pos : SV_Position;
    float3 wpos : TEXCOORD1;
    float3 nrm : TEXCOORD2;
    float2 uv : TEXCOORD0;
    float4 col : COLOR0;
};

VSOut VSMain(VSIn i) {
    float3 p = 0, n = 0;
    float4 hp = float4(i.pos, 1);
    [unroll] for (uint k = 0; k < 4; ++k) {
        float w = i.weights[k];
        if (w > 0) {
            PaletteEntry e = palette[paletteOffset + i.joints[k]];
            p += w * mul(hp, float4x4(e.r0, e.r1, e.r2, e.r3)).xyz;
            n += w * mul(i.nrm, float3x3(e.n0.xyz, e.n1.xyz, e.n2.xyz));
        }
    }
    VSOut o;
    o.wpos = p;
    o.pos = mul(float4(p, 1), viewProj);
    o.nrm = n;
    o.uv = i.uv;
    o.col = i.col;
    return o;
}

bool HasTex(uint slot) { return ((matFlags.x >> slot) & 1) != 0; }

float2 XformUV(uint s, float2 uv) {
    float3 h = float3(uv, 1);
    return float2(dot(uvRow0[s].xyz, h), dot(uvRow1[s].xyz, h));
}

float4 SampleSlot(Texture2D t, SamplerState ss, uint s, float2 uv) {
    float2 st = XformUV(s, uv);
    return t.Sample(ss, float2(st.x, 1 - st.y)) * texScale[s] + texBias[s];  // USD st origin is bottom-left
}

float Pick(float4 v, int ch) { return ch == 1 ? v.g : ch == 2 ? v.b : ch == 3 ? v.a : v.r; }
float3 PickRGB(float4 v, int ch) { return ch < 0 ? v.rgb : Pick(v, ch).xxx; }

// Tangent frame from screen-space derivatives (no precomputed tangents needed).
float3x3 CotangentFrame(float3 N, float3 p, float2 uv) {
    float3 dp1 = ddx(p), dp2 = ddy(p);
    float2 duv1 = ddx(uv), duv2 = ddy(uv);
    float3 dp2perp = cross(dp2, N);
    float3 dp1perp = cross(N, dp1);
    float3 T = dp2perp * duv1.x + dp1perp * duv2.x;
    float3 B = dp2perp * duv1.y + dp1perp * duv2.y;
    float invmax = rsqrt(max(max(dot(T, T), dot(B, B)), 1e-20));
    return float3x3(T * invmax, B * invmax, N);
}

float D_GGX(float NoH, float a) {
    float a2 = a * a;
    float d = NoH * NoH * (a2 - 1) + 1;
    return a2 / (PI * d * d);
}

float V_SmithGGX(float NoV, float NoL, float a) {
    float a2 = a * a;
    float gv = NoL * sqrt(NoV * NoV * (1 - a2) + a2);
    float gl = NoV * sqrt(NoL * NoL * (1 - a2) + a2);
    return 0.5 / max(gv + gl, 1e-5);
}

float3 F_Schlick(float3 F0, float VoH) { return F0 + (1 - F0) * pow(1 - VoH, 5); }

// Analytic fit of the pre-integrated environment BRDF (Karis, "Physically Based Shading on Mobile").
float3 EnvBRDFApprox(float3 F0, float roughness, float NoV) {
    const float4 c0 = float4(-1, -0.0275, -0.572, 0.022);
    const float4 c1 = float4(1, 0.0425, 1.04, -0.04);
    float4 r = roughness * c0 + c1;
    float a004 = min(r.x * r.x, exp2(-9.28 * NoV)) * r.x + r.y;
    float2 AB = float2(-1.04, 1.04) * a004 + r.zw;
    return F0 * AB.x + AB.y;
}

struct Surface {
    float3 color;
    float alpha;
};

Surface ShadeSurface(VSOut i, bool front) {
    float3 N = normalize(i.nrm);
    if (!front) N = -N;
    float3 V = normalize(cameraPos.xyz - i.wpos);
    float2 uv = i.uv;

    float3 albedo = baseColor.rgb;
    float opacity = baseColor.a;
    if (matFlags.y & 1) { albedo *= i.col.rgb; opacity *= i.col.a; }
    if (HasTex(0)) albedo = PickRGB(SampleSlot(texDiffuse, sDiffuse, 0, uv), channels0.x);
    float3 emis = emissive.rgb;
    if (HasTex(1)) emis = PickRGB(SampleSlot(texEmissive, sEmissive, 1, uv), channels0.y);
    float metallic = pbr.x;
    if (HasTex(2)) metallic = Pick(SampleSlot(texMetallic, sMetallic, 2, uv), channels0.z);
    float roughness = pbr.y;
    if (HasTex(3)) roughness = Pick(SampleSlot(texRoughness, sRoughness, 3, uv), channels0.w);
    if (HasTex(4)) opacity = Pick(SampleSlot(texOpacity, sOpacity, 4, uv), channels1.x);
    float occlusion = pbr.z;
    if (HasTex(6)) occlusion = Pick(SampleSlot(texOcclusion, sOcclusion, 6, uv), channels1.z);
    float3 specColor = specular.rgb;
    if (HasTex(7)) specColor = PickRGB(SampleSlot(texSpecular, sSpecular, 7, uv), channels1.w);
    if (HasTex(5)) {
        float3 tn = SampleSlot(texNormal, sNormal, 5, uv).xyz;
        float3x3 tbn = CotangentFrame(N, i.wpos, XformUV(5, uv));
        float3 mapped = mul(tn, tbn);
        if (dot(mapped, mapped) > 1e-8) N = normalize(mapped);
    }

    Surface s;
    s.alpha = saturate(opacity);
    int mode = (int)frameParams.x;
    if (mode == 1) { s.color = albedo + emis; return s; }
    if (mode == 2) { s.color = N * 0.5 + 0.5; return s; }

    metallic = saturate(metallic);
    roughness = clamp(roughness, 0.02, 1.0);
    occlusion = saturate(occlusion);
    float3 diffuseColor;
    float3 F0;
    if (specular.w > 0.5) {
        diffuseColor = albedo;
        F0 = specColor;
    } else {
        float ior = max(pbr.w, 1.0);
        float f = (ior - 1) / (ior + 1);
        diffuseColor = albedo * (1 - metallic);
        F0 = lerp(f * f, albedo, metallic);
    }
    float a = roughness * roughness;
    float NoV = abs(dot(N, V)) + 1e-4;
    float3 color = 0;
    [unroll] for (int l = 0; l < 3; ++l) {
        float3 L = lightDir[l].xyz;
        float NoL = saturate(dot(N, L));
        if (NoL <= 0) continue;
        float3 H = normalize(V + L);
        float NoH = saturate(dot(N, H));
        float VoH = saturate(dot(V, H));
        float3 F = F_Schlick(F0, VoH);
        float3 spec = D_GGX(NoH, a) * V_SmithGGX(NoV, NoL, a) * F;
        color += (diffuseColor * (1 - F) / PI + spec) * lightColor[l].rgb * NoL;
    }
    // Hemisphere ambient plus a soft gradient environment for reflections.
    float3 amb = lerp(groundColor.rgb, skyColor.rgb, N.y * 0.5 + 0.5);
    float3 R = reflect(-V, N);
    float3 env = lerp(groundColor.rgb, skyColor.rgb * 1.6, smoothstep(-0.3, 0.7, R.y));
    env = lerp(env, amb, roughness);
    float3 Fa = EnvBRDFApprox(F0, roughness, saturate(NoV));
    color += (diffuseColor * amb + env * Fa * frameParams.z) * occlusion;
    color += emis;
    s.color = color;
    return s;
}

float3 ApplyTint(float3 c) { return lerp(c, tint.rgb, tint.a); }

float4 PSOpaque(VSOut i, bool front : SV_IsFrontFace) : SV_Target {
    Surface s = ShadeSurface(i, front);
    return float4(ApplyTint(s.color), 1);
}

struct MaskedOut {
    float4 color : SV_Target0;
    uint coverage : SV_Coverage;
};

// Alpha cut-outs: sharpened alpha converted to an MSAA coverage mask for smooth edges.
MaskedOut PSMasked(VSOut i, bool front : SV_IsFrontFace) {
    Surface s = ShadeSurface(i, front);
    float threshold = max(emissive.w, 1e-3);
    float a = saturate((s.alpha - threshold) / max(fwidth(s.alpha), 1e-4) + 0.5);
    uint samples = max((uint)frameParams.y, 1);
    uint n = (uint)round(a * samples);
    if (n == 0) discard;
    MaskedOut o;
    o.color = float4(ApplyTint(s.color), 1);
    o.coverage = (1u << n) - 1u;
    return o;
}

// Translucency: premultiplied alpha.
float4 PSBlend(VSOut i, bool front : SV_IsFrontFace) : SV_Target {
    Surface s = ShadeSurface(i, front);
    return float4(ApplyTint(s.color) * s.alpha, s.alpha);
}

float4 PSWire(VSOut i) : SV_Target {
    return float4(0.85, 0.9, 1.0, 1.0) * 0.9;
}
)HLSL";

// Colored lines (skeleton overlay, axis gizmo).
inline const char* kLineShader = R"HLSL(
cbuffer LineCB : register(b0) {
    row_major float4x4 viewProj;
};
struct VSIn { float3 pos : POSITION; float4 col : COLOR0; };
struct VSOut { float4 pos : SV_Position; float4 col : COLOR0; };
VSOut VSMain(VSIn i) {
    VSOut o;
    o.pos = mul(float4(i.pos, 1), viewProj);
    o.col = i.col;
    return o;
}
float4 PSMain(VSOut i) : SV_Target { return float4(i.col.rgb * i.col.a, i.col.a); }
)HLSL";

// Ground plane at y = height: procedural grid plus a soft contact shadow from a top-down depth map.
inline const char* kGridShader = R"HLSL(
cbuffer GridCB : register(b0) {
    row_major float4x4 viewProj;
    row_major float4x4 shadowViewProj;  // world -> top-down orthographic shadow clip space
    float4 gridParams;    // x: cell size, y: extent, z: plane height, w: fade radius
    float4 cameraPos;
    float4 shadowParams;  // x: enabled, y: depth range (world units), z: world units per shadow uv, w: max strength height
    float4 groundFlags;   // x: draw grid lines, y: shadow strength
};
Texture2D<float> shadowDepth : register(t0);
SamplerComparisonState shadowCmp : register(s0);
SamplerState shadowPoint : register(s1);

struct VSOut { float4 pos : SV_Position; float3 wpos : TEXCOORD0; };
VSOut VSMain(uint id : SV_VertexID) {
    float2 c[6] = { float2(-1,-1), float2(1,-1), float2(1,1), float2(-1,-1), float2(1,1), float2(-1,1) };
    float2 xz = c[id] * gridParams.y;
    VSOut o;
    o.wpos = float3(xz.x, gridParams.z, xz.y);
    o.pos = mul(float4(o.wpos, 1), viewProj);
    return o;
}
float GridLine(float2 coord) {
    float2 d = fwidth(coord);
    float2 g = abs(frac(coord - 0.5) - 0.5) / max(d, 1e-6);
    return 1 - min(min(g.x, g.y), 1);
}

static const float2 kPoisson[16] = {
    float2(-0.94201624, -0.39906216), float2(0.94558609, -0.76890725), float2(-0.09418410, -0.92938870),
    float2(0.34495938, 0.29387760), float2(-0.91588581, 0.45771432), float2(-0.81544232, -0.87912464),
    float2(-0.38277543, 0.27676845), float2(0.97484398, 0.75648379), float2(0.44323325, -0.97511554),
    float2(0.53742981, -0.47373420), float2(-0.26496911, -0.41893023), float2(0.79197514, 0.19090188),
    float2(-0.24188840, 0.99706507), float2(-0.81409955, 0.91437590), float2(0.19984126, 0.78641367),
    float2(0.14383161, -0.14100790)};

// Fraction of light blocked at a ground point, with a penumbra that widens with occluder height.
float GroundShadow(float3 wpos) {
    float4 sc = mul(float4(wpos, 1), shadowViewProj);
    float2 uv = float2(sc.x * 0.5 + 0.5, 0.5 - sc.y * 0.5);
    if (any(uv < 0) || any(uv > 1)) return 0;
    float depth = sc.z;
    // Blocker search.
    float searchUV = 0.025;
    float blocker = 0, count = 0;
    [unroll] for (int i = 0; i < 16; ++i) {
        float d = shadowDepth.SampleLevel(shadowPoint, uv + kPoisson[i] * searchUV, 0);
        if (d < depth - 1e-4) { blocker += d; count += 1; }
    }
    if (count == 0) return 0;
    blocker /= count;
    float height = (depth - blocker) * shadowParams.y;       // occluder height above the ground
    float radiusUV = clamp(height * 0.12 / shadowParams.z, 0.0015, 0.03);
    float lit = 0;
    [unroll] for (int k = 0; k < 16; ++k)
        lit += shadowDepth.SampleCmpLevelZero(shadowCmp, uv + kPoisson[k] * radiusUV, depth - 1e-4);
    float shadow = 1 - lit / 16;
    return shadow * saturate(1 - height / shadowParams.w);
}

float4 PSMain(VSOut i) : SV_Target {
    float fade = saturate(1 - length(i.wpos.xz) / gridParams.w);
    fade *= fade;
    float alpha = 0;
    float3 color = 0;
    if (groundFlags.x > 0) {
        float2 coord = i.wpos.xz / gridParams.x;
        float minor = GridLine(coord);
        float major = GridLine(coord / 10);
        color = float3(0.55, 0.57, 0.62);
        alpha = max(minor * 0.18, major * 0.42);
        float2 d = fwidth(i.wpos.xz) * 1.5;
        if (abs(i.wpos.z) < d.y) { color = float3(0.85, 0.25, 0.25); alpha = 0.8; }   // X axis
        if (abs(i.wpos.x) < d.x) { color = float3(0.25, 0.45, 0.95); alpha = 0.8; }   // Z axis
        alpha *= fade;
    }
    float s = shadowParams.x > 0 ? GroundShadow(i.wpos) * groundFlags.y : 0;
    // Premultiplied: grid lines over a black shadow layer.
    return float4(color * alpha, alpha + s * (1 - alpha));
}
)HLSL";

// Final pass: background gradient + exposure + ACES tonemap, written to an sRGB back buffer.
inline const char* kCompositeShader = R"HLSL(
cbuffer CompositeCB : register(b0) {
    float4 params;   // x: exposure, y: viewport height
    float4 bgTop;    // linear
    float4 bgBottom; // linear
};
Texture2D<float4> hdr : register(t0);
float4 VSMain(uint id : SV_VertexID) : SV_Position {
    float2 uv = float2((id << 1) & 2, id & 2);
    return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
}
float3 ACES(float3 x) {
    return saturate((x * (2.51 * x + 0.03)) / (x * (2.43 * x + 0.59) + 0.14));
}
float4 PSMain(float4 pos : SV_Position) : SV_Target {
    float4 c = hdr.Load(int3(pos.xy, 0));
    float t = saturate(pos.y / max(params.y, 1));
    float3 bg = lerp(bgTop.rgb, bgBottom.rgb, t);
    float a = saturate(c.a);
    float3 scene = a > 1e-4 ? ACES(c.rgb / a * params.x) : 0;
    return float4(lerp(bg, scene, a), 1);
}
)HLSL";

}  // namespace render
