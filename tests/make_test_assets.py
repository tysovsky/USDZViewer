"""Builds feature-coverage test files with Pixar's USD Python API (run inside Blender, which bundles pxr):

    blender --background --factory-startup --python tests/make_test_assets.py -- OUTDIR

Writes:
  composition.usdz  references (external + internal), variants, sublayer with a time offset, inherits,
                    animated xform ops (translate / rotateXYZ / orient / scale), GeomSubsets with two materials,
                    point animation, implicit shapes, a PointInstancer
  skel_blend.usdz   UsdSkel skinning + blend shapes driven by a SkelAnimation
  zup.usda          Z-up stage with a leftHanded mesh and uniform displayColor
"""

import math
import os
import shutil
import sys
import tempfile

from pxr import Gf, Sdf, Usd, UsdGeom, UsdShade, UsdSkel, UsdUtils, Vt

OUT = sys.argv[sys.argv.index("--") + 1]
os.makedirs(OUT, exist_ok=True)
WORK = tempfile.mkdtemp(prefix="usdz_tests_")


def box_mesh(stage, path, size=1.0, center=(0, 0, 0)):
    m = UsdGeom.Mesh.Define(stage, path)
    s = size / 2
    cx, cy, cz = center
    pts = [(cx + x * s, cy + y * s, cz + z * s) for x in (-1, 1) for y in (-1, 1) for z in (-1, 1)]
    m.CreatePointsAttr([Gf.Vec3f(*p) for p in pts])
    faces = [(0, 1, 3, 2), (4, 6, 7, 5), (0, 4, 5, 1), (2, 3, 7, 6), (0, 2, 6, 4), (1, 5, 7, 3)]
    m.CreateFaceVertexCountsAttr([4] * 6)
    m.CreateFaceVertexIndicesAttr([i for f in faces for i in f])
    m.CreateSubdivisionSchemeAttr("none")
    return m


def preview_material(stage, path, color, texture=None):
    mat = UsdShade.Material.Define(stage, path)
    pbr = UsdShade.Shader.Define(stage, path + "/PBR")
    pbr.CreateIdAttr("UsdPreviewSurface")
    pbr.CreateInput("diffuseColor", Sdf.ValueTypeNames.Color3f).Set(Gf.Vec3f(*color))
    pbr.CreateInput("roughness", Sdf.ValueTypeNames.Float).Set(0.4)
    mat.CreateSurfaceOutput().ConnectToSource(pbr.ConnectableAPI(), "surface")
    return mat


def write_png(path, w=8, h=8):
    import struct, zlib
    rows = []
    for y in range(h):
        row = bytearray([0])
        for x in range(w):
            c = 255 if (x + y) % 2 else 40
            row += bytes([c, 120, 255 - c])
        rows.append(bytes(row))
    data = zlib.compress(b"".join(rows))

    def chunk(t, d):
        return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xffffffff)

    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)) + chunk(b"IDAT", data) +
                chunk(b"IEND", b""))


def composition():
    d = os.path.join(WORK, "composition")
    os.makedirs(os.path.join(d, "textures"), exist_ok=True)
    write_png(os.path.join(d, "textures", "checker.png"))

    # External asset referenced twice.
    asset = Usd.Stage.CreateNew(os.path.join(d, "asset.usda"))
    model = UsdGeom.Xform.Define(asset, "/Model")
    asset.SetDefaultPrim(model.GetPrim())
    body = box_mesh(asset, "/Model/Body", 1.0, (0, 0.5, 0))
    mat = UsdShade.Material.Define(asset, "/Model/Looks/Checker")
    pbr = UsdShade.Shader.Define(asset, "/Model/Looks/Checker/PBR")
    pbr.CreateIdAttr("UsdPreviewSurface")
    tex = UsdShade.Shader.Define(asset, "/Model/Looks/Checker/Tex")
    tex.CreateIdAttr("UsdUVTexture")
    tex.CreateInput("file", Sdf.ValueTypeNames.Asset).Set("textures/checker.png")
    reader = UsdShade.Shader.Define(asset, "/Model/Looks/Checker/Reader")
    reader.CreateIdAttr("UsdPrimvarReader_float2")
    mat_input = mat.CreateInput("frame:stPrimvarName", Sdf.ValueTypeNames.Token)
    mat_input.Set("st")
    reader.CreateInput("varname", Sdf.ValueTypeNames.Token).ConnectToSource(mat_input)
    tex.CreateInput("st", Sdf.ValueTypeNames.Float2).ConnectToSource(reader.ConnectableAPI(), "result")
    pbr.CreateInput("diffuseColor", Sdf.ValueTypeNames.Color3f).ConnectToSource(tex.ConnectableAPI(), "rgb")
    mat.CreateSurfaceOutput().ConnectToSource(pbr.ConnectableAPI(), "surface")
    UsdShade.MaterialBindingAPI.Apply(body.GetPrim()).Bind(mat)
    st = UsdGeom.PrimvarsAPI(body).CreatePrimvar("st", Sdf.ValueTypeNames.TexCoord2fArray, UsdGeom.Tokens.faceVarying)
    st.Set([Gf.Vec2f(u, v) for _ in range(6) for (u, v) in ((0, 0), (1, 0), (1, 1), (0, 1))])
    asset.GetRootLayer().Save()

    # Sublayer with animation, applied with a +10 time offset and 2x scale.
    sub = Usd.Stage.CreateNew(os.path.join(d, "anim.usda"))
    spin = UsdGeom.Xform.Define(sub, "/World/Spinner")
    op = spin.AddRotateXYZOp()
    op.Set(Gf.Vec3f(0, 0, 0), 0)
    op.Set(Gf.Vec3f(0, 360, 45), 20)
    sub.GetRootLayer().Save()

    root = Usd.Stage.CreateNew(os.path.join(d, "composition.usda"))
    root.GetRootLayer().subLayerPaths.append("anim.usda")
    root.GetRootLayer().subLayerOffsets[0] = Sdf.LayerOffset(10, 2)
    root.SetStartTimeCode(0)
    root.SetEndTimeCode(60)
    root.SetTimeCodesPerSecond(24)
    UsdGeom.SetStageUpAxis(root, UsdGeom.Tokens.y)
    world = UsdGeom.Xform.Define(root, "/World")
    root.SetDefaultPrim(world.GetPrim())

    a = UsdGeom.Xform.Define(root, "/World/AssetA")
    a.GetPrim().GetReferences().AddReference("asset.usda")
    a.AddTranslateOp().Set(Gf.Vec3d(-2, 0, 0))
    b = UsdGeom.Xform.Define(root, "/World/AssetB")
    b.GetPrim().GetReferences().AddReference("asset.usda", "/Model")
    t = b.AddTranslateOp()
    t.Set(Gf.Vec3d(2, 0, 0), 0)
    t.Set(Gf.Vec3d(2, 3, 0), 60)
    o = b.AddOrientOp()
    o.Set(Gf.Quatf(1, 0, 0, 0), 0)
    o.Set(Gf.Quatf(math.cos(math.pi / 4), 0, math.sin(math.pi / 4), 0), 60)
    b.AddScaleOp().Set(Gf.Vec3f(1, 2, 1))

    # Payload, and a reference with a layer offset onto an animated asset.
    bob = Usd.Stage.CreateNew(os.path.join(d, "bob.usda"))
    bob_root = UsdGeom.Xform.Define(bob, "/Bob")
    bob.SetDefaultPrim(bob_root.GetPrim())
    bt = bob_root.AddTranslateOp()
    bt.Set(Gf.Vec3d(0, 0, 0), 0)
    bt.Set(Gf.Vec3d(0, 2, 0), 10)
    box_mesh(bob, "/Bob/Geo", 0.4)
    bob.GetRootLayer().Save()
    pay = UsdGeom.Xform.Define(root, "/World/Payload")
    pay.GetPrim().GetPayloads().AddPayload("bob.usda")
    pay.AddTranslateOp(UsdGeom.XformOp.PrecisionDouble, "place").Set(Gf.Vec3d(-6, 0, 0))
    off = UsdGeom.Xform.Define(root, "/World/OffsetRef")
    off.GetPrim().GetReferences().AddReference(Sdf.Reference("bob.usda", layerOffset=Sdf.LayerOffset(20, 3)))
    off.AddTranslateOp(UsdGeom.XformOp.PrecisionDouble, "place").Set(Gf.Vec3d(-7, 0, 0))

    # Internal reference to a sibling prim.
    inner = UsdGeom.Xform.Define(root, "/World/InternalRef")
    inner.GetPrim().GetReferences().AddInternalReference("/World/AssetA")
    inner.AddTranslateOp(UsdGeom.XformOp.PrecisionDouble, "offset").Set(Gf.Vec3d(0, 0, -3))

    # Spinner child (the animated rotation comes from the offset sublayer).
    box_mesh(root, "/World/Spinner/Box", 0.5, (1, 0, 0))

    # Variants.
    var = UsdGeom.Xform.Define(root, "/World/Variant")
    vs = var.GetPrim().GetVariantSets().AddVariantSet("size")
    for name, size in (("small", 0.4), ("large", 1.6)):
        vs.AddVariant(name)
        vs.SetVariantSelection(name)
        with vs.GetVariantEditContext():
            box_mesh(root, "/World/Variant/Geo", size, (0, 0, 3))
    vs.SetVariantSelection("large")

    # Inherits: class provides an xform offset.
    cls = root.CreateClassPrim("/_class_Lifted")
    UsdGeom.Xformable(cls).AddTranslateOp().Set(Gf.Vec3d(0, 4, 0))
    inh = UsdGeom.Xform.Define(root, "/World/Inherited")
    inh.GetPrim().GetInherits().AddInherit("/_class_Lifted")
    box_mesh(root, "/World/Inherited/Box", 0.3)

    # GeomSubsets with two materials.
    sub_mesh = box_mesh(root, "/World/Subsets", 1.0, (4, 0.5, 0))
    red = preview_material(root, "/World/Looks/Red", (0.9, 0.1, 0.1))
    blue = preview_material(root, "/World/Looks/Blue", (0.1, 0.2, 0.9))
    UsdShade.MaterialBindingAPI.Apply(sub_mesh.GetPrim()).Bind(red)
    gs = UsdGeom.Subset.CreateGeomSubset(sub_mesh, "top", UsdGeom.Tokens.face, Vt.IntArray([2, 3]),
                                         UsdShade.Tokens.materialBind)
    UsdShade.MaterialBindingAPI.Apply(gs.GetPrim()).Bind(blue)

    # Point animation.
    wave = box_mesh(root, "/World/Wave", 1.0, (-4, 0.5, 0))
    pts = wave.GetPointsAttr().Get()
    for frame in (0, 30, 60):
        k = frame / 60.0
        wave.GetPointsAttr().Set([Gf.Vec3f(p[0], p[1] * (1 + k) if p[1] > 0.5 else p[1], p[2]) for p in pts], frame)

    # Implicit shapes.
    for i, kind in enumerate(("Cube", "Sphere", "Cylinder", "Cone", "Capsule")):
        prim = root.DefinePrim("/World/Shapes/%s" % kind, kind)
        UsdGeom.Xformable(prim).AddTranslateOp().Set(Gf.Vec3d(-4 + i * 2, 0.5, 5))
        UsdGeom.Gprim(prim).CreateDisplayColorAttr([Gf.Vec3f(0.2 + 0.15 * i, 0.6, 0.3)])

    # PointInstancer with two prototypes.
    pi = UsdGeom.PointInstancer.Define(root, "/World/Instancer")
    pi.AddTranslateOp().Set(Gf.Vec3d(0, 0, 8))
    box_mesh(root, "/World/Instancer/Prototypes/Small", 0.3)
    proto2 = UsdGeom.Xform.Define(root, "/World/Instancer/Prototypes/Tall")
    proto2.AddScaleOp().Set(Gf.Vec3f(0.2, 1.0, 0.2))
    box_mesh(root, "/World/Instancer/Prototypes/Tall/Geo", 1.0)
    pi.CreatePrototypesRel().SetTargets([Sdf.Path("/World/Instancer/Prototypes/Small"), Sdf.Path("/World/Instancer/Prototypes/Tall")])
    pi.CreateProtoIndicesAttr([0, 1, 0, 1])
    pi.CreatePositionsAttr([Gf.Vec3f(x, 0, 0) for x in (-1.5, -0.5, 0.5, 1.5)])
    pi.CreateOrientationsAttr([Gf.Quath(1, 0, 0, 0), Gf.Quath(0.7071, 0, 0.7071, 0), Gf.Quath(1, 0, 0, 0), Gf.Quath(1, 0, 0, 0)])
    pi.CreateScalesAttr([Gf.Vec3f(1, 1, 1), Gf.Vec3f(1, 1, 1), Gf.Vec3f(2, 2, 2), Gf.Vec3f(1, 1, 1)])
    root.GetRootLayer().Save()

    out = os.path.join(OUT, "composition.usdz")
    if os.path.exists(out):
        os.remove(out)
    ok = UsdUtils.CreateNewUsdzPackage(Sdf.AssetPath(os.path.join(d, "composition.usda")), out)
    print("composition.usdz", ok)

    # The same root layer in the binary crate format (list ops, variant selections, references in USDC).
    root.GetRootLayer().Export(os.path.join(d, "composition_c.usdc"))
    out = os.path.join(OUT, "composition_c.usdz")
    if os.path.exists(out):
        os.remove(out)
    print("composition_c.usdz", UsdUtils.CreateNewUsdzPackage(Sdf.AssetPath(os.path.join(d, "composition_c.usdc")), out))


def skel_blend():
    d = os.path.join(WORK, "skel")
    os.makedirs(d, exist_ok=True)
    path = os.path.join(d, "skel_blend.usdc")
    stage = Usd.Stage.CreateNew(path)
    stage.SetStartTimeCode(0)
    stage.SetEndTimeCode(48)
    stage.SetTimeCodesPerSecond(24)
    UsdGeom.SetStageUpAxis(stage, UsdGeom.Tokens.y)
    root = UsdSkel.Root.Define(stage, "/Character")
    stage.SetDefaultPrim(root.GetPrim())
    skel = UsdSkel.Skeleton.Define(stage, "/Character/Skel")
    joints = ["Root", "Root/Mid", "Root/Mid/Tip"]
    skel.CreateJointsAttr(joints)
    bind = [Gf.Matrix4d(1).SetTranslate(Gf.Vec3d(0, y, 0)) for y in (0, 1, 2)]
    skel.CreateBindTransformsAttr(bind)
    rest = [Gf.Matrix4d(1), Gf.Matrix4d(1).SetTranslate(Gf.Vec3d(0, 1, 0)), Gf.Matrix4d(1).SetTranslate(Gf.Vec3d(0, 1, 0))]
    skel.CreateRestTransformsAttr(rest)

    anim = UsdSkel.Animation.Define(stage, "/Character/Skel/Anim")
    anim.CreateJointsAttr(["Root/Mid", "Root/Mid/Tip"])  # subset, different order handling
    anim.CreateTranslationsAttr([Gf.Vec3f(0, 1, 0), Gf.Vec3f(0, 1, 0)])
    anim.CreateScalesAttr([Gf.Vec3h(1, 1, 1), Gf.Vec3h(1, 1, 1)])
    rot = anim.CreateRotationsAttr()
    for f in (0, 24, 48):
        a = math.radians(f / 48.0 * 90)
        q = Gf.Quatf(math.cos(a / 2), 0, 0, math.sin(a / 2))
        rot.Set([q, q], f)
    anim.CreateBlendShapesAttr(["bulge"])
    w = anim.CreateBlendShapeWeightsAttr()
    w.Set([0.0], 0)
    w.Set([1.0], 48)
    binding = UsdSkel.BindingAPI.Apply(skel.GetPrim())
    binding.CreateAnimationSourceRel().SetTargets([anim.GetPath()])

    # A column mesh with 3 rings of 4 points.
    mesh = UsdGeom.Mesh.Define(stage, "/Character/Column")
    pts, idx, cnt = [], [], []
    for ring in range(4):
        for k in range(4):
            a = k * math.pi / 2
            pts.append(Gf.Vec3f(0.3 * math.cos(a), ring * 2 / 3.0, 0.3 * math.sin(a)))
    for ring in range(3):
        for k in range(4):
            a, b = ring * 4 + k, ring * 4 + (k + 1) % 4
            idx += [a, b, b + 4, a + 4]
            cnt.append(4)
    mesh.CreatePointsAttr(pts)
    mesh.CreateFaceVertexCountsAttr(cnt)
    mesh.CreateFaceVertexIndicesAttr(idx)
    mb = UsdSkel.BindingAPI.Apply(mesh.GetPrim())
    mb.CreateSkeletonRel().SetTargets([skel.GetPath()])
    ji, jw = [], []
    for p in pts:
        y = p[1]
        if y < 1:
            ji += [0, 1]
            jw += [1 - y, y]
        else:
            ji += [1, 2]
            jw += [2 - y, y - 1]
    mb.CreateJointIndicesPrimvar(False, 2).Set(ji)
    mb.CreateJointWeightsPrimvar(False, 2).Set(jw)
    mb.CreateGeomBindTransformAttr(Gf.Matrix4d(1))
    shape = UsdSkel.BlendShape.Define(stage, "/Character/Column/bulge")
    shape.CreateOffsetsAttr([Gf.Vec3f(0.2, 0, 0)] * 4)
    shape.CreatePointIndicesAttr([4, 5, 6, 7])
    mb.CreateBlendShapesAttr(["bulge"])
    mb.CreateBlendShapeTargetsRel().SetTargets([shape.GetPath()])
    stage.GetRootLayer().Save()
    out = os.path.join(OUT, "skel_blend.usdz")
    if os.path.exists(out):
        os.remove(out)
    print("skel_blend.usdz", UsdUtils.CreateNewUsdzPackage(Sdf.AssetPath(path), out))


def zup():
    path = os.path.join(OUT, "zup.usda")
    stage = Usd.Stage.CreateNew(path) if not os.path.exists(path) else Usd.Stage.Open(path)
    stage.GetRootLayer().Clear() if os.path.exists(path) else None
    UsdGeom.SetStageUpAxis(stage, UsdGeom.Tokens.z)
    UsdGeom.SetStageMetersPerUnit(stage, 0.01)
    m = box_mesh(stage, "/Tower", 100.0, (0, 0, 50))
    m.CreateOrientationAttr(UsdGeom.Tokens.leftHanded)
    pv = UsdGeom.PrimvarsAPI(m).CreatePrimvar("displayColor", Sdf.ValueTypeNames.Color3fArray, UsdGeom.Tokens.uniform)
    pv.Set([Gf.Vec3f(i / 6.0, 0.5, 1 - i / 6.0) for i in range(6)])
    stage.SetDefaultPrim(m.GetPrim())
    stage.GetRootLayer().Save()
    print("zup.usda written")


composition()
skel_blend()
zup()
shutil.rmtree(WORK, ignore_errors=True)
