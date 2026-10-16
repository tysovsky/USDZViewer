"""Material coverage test (run inside Blender): blender -b --factory-startup --python tests/make_material_test.py -- OUTDIR

Writes materials.usdz: a row of spheres/quads using UsdPreviewSurface features — metallic, rough plastic,
normal map + UsdTransform2d tiling, blended transparency, emissive, and an RGBA cut-out texture.
"""

import math
import os
import shutil
import struct
import sys
import tempfile
import zlib

from pxr import Gf, Sdf, Usd, UsdGeom, UsdShade, UsdUtils

OUT = sys.argv[sys.argv.index("--") + 1]
WORK = tempfile.mkdtemp(prefix="usdz_mat_")
os.makedirs(os.path.join(WORK, "tex"), exist_ok=True)


def png(path, w, h, pixel, channels):
    rows = []
    for y in range(h):
        row = bytearray([0])
        for x in range(w):
            row += bytes(pixel(x, y))
        rows.append(bytes(row))
    color_type = {3: 2, 4: 6}[channels]

    def chunk(t, d):
        return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xffffffff)

    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, color_type, 0, 0, 0)) +
                chunk(b"IDAT", zlib.compress(b"".join(rows))) + chunk(b"IEND", b""))


def bumps(x, y):  # tangent-space normal map of a grid of domes
    u, v = (x % 32) / 16.0 - 1, (y % 32) / 16.0 - 1
    r2 = u * u + v * v
    if r2 < 0.8:
        nz = math.sqrt(1 - min(r2, 0.99))
        n = (u * 0.8, -v * 0.8, nz)
    else:
        n = (0, 0, 1)
    l = math.sqrt(sum(c * c for c in n))
    return [int((c / l * 0.5 + 0.5) * 255) for c in n]


png(os.path.join(WORK, "tex", "bumps.png"), 128, 128, bumps, 3)
png(os.path.join(WORK, "tex", "checker.png"), 64, 64,
    lambda x, y: [230, 230, 230] if (x // 8 + y // 8) % 2 else [60, 90, 160], 3)
png(os.path.join(WORK, "tex", "leaf.png"), 64, 64,
    lambda x, y: [60, 170, 70, 255 if ((x - 32) ** 2 + (y - 32) ** 2 < 900 and (x // 6) % 2) else 0], 4)

stage = Usd.Stage.CreateNew(os.path.join(WORK, "materials.usda"))
UsdGeom.SetStageUpAxis(stage, UsdGeom.Tokens.y)
UsdGeom.SetStageMetersPerUnit(stage, 1)
root = UsdGeom.Xform.Define(stage, "/Materials")
stage.SetDefaultPrim(root.GetPrim())


def sphere_mesh(path, x, rings=24, segs=48, r=0.4):
    m = UsdGeom.Mesh.Define(stage, path)
    pts, uv, idx, cnt, uvidx = [], [], [], [], []
    for i in range(rings + 1):
        a = -math.pi / 2 + math.pi * i / rings
        for j in range(segs + 1):
            b = 2 * math.pi * j / segs
            pts.append(Gf.Vec3f(x + r * math.cos(a) * math.sin(b), 0.5 + r * math.sin(a), r * math.cos(a) * math.cos(b)))
            uv.append(Gf.Vec2f(j / segs, i / rings))
    for i in range(rings):
        for j in range(segs):
            a = i * (segs + 1) + j
            q = [a, a + 1, a + segs + 2, a + segs + 1]
            idx += q
            cnt.append(4)
    m.CreatePointsAttr(pts)
    m.CreateFaceVertexCountsAttr(cnt)
    m.CreateFaceVertexIndicesAttr(idx)
    m.CreateSubdivisionSchemeAttr("none")
    m.CreateOrientationAttr(UsdGeom.Tokens.leftHanded)  # this winding is clockwise seen from outside
    st = UsdGeom.PrimvarsAPI(m).CreatePrimvar("st", Sdf.ValueTypeNames.TexCoord2fArray, UsdGeom.Tokens.vertex)
    st.Set(uv)
    # smooth normals
    nrm = [Gf.Vec3f(*(Gf.Vec3f(p) - Gf.Vec3f(x, 0.5, 0)).GetNormalized()) for p in pts]
    m.CreateNormalsAttr(nrm)
    m.SetNormalsInterpolation(UsdGeom.Tokens.vertex)
    return m


def quad_mesh(path, x):
    m = UsdGeom.Mesh.Define(stage, path)
    m.CreatePointsAttr([Gf.Vec3f(x - 0.4, 0.1, 0), Gf.Vec3f(x + 0.4, 0.1, 0), Gf.Vec3f(x + 0.4, 0.9, 0), Gf.Vec3f(x - 0.4, 0.9, 0)])
    m.CreateFaceVertexCountsAttr([4])
    m.CreateFaceVertexIndicesAttr([0, 1, 2, 3])
    m.CreateDoubleSidedAttr(True)
    UsdGeom.PrimvarsAPI(m).CreatePrimvar("st", Sdf.ValueTypeNames.TexCoord2fArray, UsdGeom.Tokens.vertex).Set(
        [Gf.Vec2f(0, 0), Gf.Vec2f(1, 0), Gf.Vec2f(1, 1), Gf.Vec2f(0, 1)])
    return m


def material(name, **inputs):
    mat = UsdShade.Material.Define(stage, "/Materials/Looks/" + name)
    pbr = UsdShade.Shader.Define(stage, mat.GetPath().AppendChild("PBR"))
    pbr.CreateIdAttr("UsdPreviewSurface")
    for k, v in inputs.items():
        if isinstance(v, tuple):
            pbr.CreateInput(k, Sdf.ValueTypeNames.Color3f).Set(Gf.Vec3f(*v))
        else:
            pbr.CreateInput(k, Sdf.ValueTypeNames.Float).Set(v)
    mat.CreateSurfaceOutput().ConnectToSource(pbr.ConnectableAPI(), "surface")
    return mat, pbr


def texture(mat, name, file, colorspace="auto", transform=None):
    reader = UsdShade.Shader.Get(stage, mat.GetPath().AppendChild("Reader"))
    if not reader:
        reader = UsdShade.Shader.Define(stage, mat.GetPath().AppendChild("Reader"))
        reader.CreateIdAttr("UsdPrimvarReader_float2")
        reader.CreateInput("varname", Sdf.ValueTypeNames.String).Set("st")
    tex = UsdShade.Shader.Define(stage, mat.GetPath().AppendChild(name))
    tex.CreateIdAttr("UsdUVTexture")
    tex.CreateInput("file", Sdf.ValueTypeNames.Asset).Set(file)
    tex.CreateInput("sourceColorSpace", Sdf.ValueTypeNames.Token).Set(colorspace)
    src = reader
    if transform:
        xf = UsdShade.Shader.Define(stage, mat.GetPath().AppendChild(name + "Xform"))
        xf.CreateIdAttr("UsdTransform2d")
        xf.CreateInput("in", Sdf.ValueTypeNames.Float2).ConnectToSource(reader.ConnectableAPI(), "result")
        xf.CreateInput("scale", Sdf.ValueTypeNames.Float2).Set(Gf.Vec2f(*transform[0]))
        xf.CreateInput("rotation", Sdf.ValueTypeNames.Float).Set(transform[1])
        tex.CreateInput("st", Sdf.ValueTypeNames.Float2).ConnectToSource(xf.ConnectableAPI(), "result")
    else:
        tex.CreateInput("st", Sdf.ValueTypeNames.Float2).ConnectToSource(reader.ConnectableAPI(), "result")
    return tex


def bind(mesh, mat):
    UsdShade.MaterialBindingAPI.Apply(mesh.GetPrim()).Bind(mat)


m, _ = material("Gold", diffuseColor=(1.0, 0.76, 0.33), metallic=1.0, roughness=0.25)
bind(sphere_mesh("/Materials/Gold", -2.5), m)
m, _ = material("Plastic", diffuseColor=(0.8, 0.1, 0.1), roughness=0.85)
bind(sphere_mesh("/Materials/Plastic", -1.5), m)
m, pbr = material("Bumpy", roughness=0.45)
t = texture(m, "Diffuse", "tex/checker.png", transform=((4, 2), 30))
pbr.CreateInput("diffuseColor", Sdf.ValueTypeNames.Color3f).ConnectToSource(t.ConnectableAPI(), "rgb")
n = texture(m, "Normal", "tex/bumps.png", colorspace="raw", transform=((4, 2), 30))
n.CreateInput("scale", Sdf.ValueTypeNames.Float4).Set(Gf.Vec4f(2, 2, 2, 1))
n.CreateInput("bias", Sdf.ValueTypeNames.Float4).Set(Gf.Vec4f(-1, -1, -1, 0))
pbr.CreateInput("normal", Sdf.ValueTypeNames.Normal3f).ConnectToSource(n.ConnectableAPI(), "rgb")
bind(sphere_mesh("/Materials/Bumpy", -0.5), m)
m, _ = material("Glass", diffuseColor=(0.2, 0.45, 0.9), opacity=0.35, roughness=0.1)
bind(sphere_mesh("/Materials/Glass", 0.5), m)
m, _ = material("Glow", diffuseColor=(0.1, 0.1, 0.1), emissiveColor=(4.0, 1.6, 0.3))
bind(sphere_mesh("/Materials/Glow", 1.5), m)
m, pbr = material("Leaf", roughness=0.6, opacityThreshold=0.5)
t = texture(m, "Albedo", "tex/leaf.png")
pbr.CreateInput("diffuseColor", Sdf.ValueTypeNames.Color3f).ConnectToSource(t.ConnectableAPI(), "rgb")
pbr.CreateInput("opacity", Sdf.ValueTypeNames.Float).ConnectToSource(t.ConnectableAPI(), "a")
bind(quad_mesh("/Materials/Leaf", 2.5), m)
stage.GetRootLayer().Save()

out = os.path.join(OUT, "materials.usdz")
if os.path.exists(out):
    os.remove(out)
print("materials.usdz", UsdUtils.CreateNewUsdzPackage(Sdf.AssetPath(os.path.join(WORK, "materials.usda")), out))
shutil.rmtree(WORK, ignore_errors=True)
