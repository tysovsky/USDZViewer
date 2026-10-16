"""Reference world-space mesh points (UsdSkel linear blend skinning) using Pixar's USD Python API:

    blender --background --factory-startup --python tests/pxr_eval.py -- INPUT TIME OUT.json

Writes {"time": t, "meshes": {path: [x, y, z, ...]}} like `usdzinfo INPUT --eval TIME OUT.json`.
Blend shapes are not applied (the reference uses only joint skinning); the stage must be Y-up.
"""

import json
import sys

from pxr import Gf, Usd, UsdGeom, UsdSkel


def main():
    argv = sys.argv[sys.argv.index("--") + 1:]
    src, t, out = argv[0], float(argv[1]), argv[2]
    stage = Usd.Stage.Open(src)
    time = Usd.TimeCode(t)
    xcache = UsdGeom.XformCache(time)
    meshes = {}
    skinned = set()

    skel_cache = UsdSkel.Cache()
    for prim in stage.Traverse():
        if not prim.IsA(UsdSkel.Root):
            continue
        root = UsdSkel.Root(prim)
        skel_cache.Populate(root, Usd.PrimDefaultPredicate)
        for binding in skel_cache.ComputeSkelBindings(root, Usd.PrimDefaultPredicate):
            skel = binding.GetSkeleton()
            query = skel_cache.GetSkelQuery(skel)
            xforms = query.ComputeSkinningTransforms(time)
            skel_to_world = xcache.GetLocalToWorldTransform(skel.GetPrim())
            anim_query = query.GetAnimQuery()
            for target in binding.GetSkinningTargets():
                mesh_prim = target.GetPrim()
                points = UsdGeom.Mesh(mesh_prim).GetPointsAttr().Get(time)
                if target.HasBlendShapes() and anim_query:
                    weights = anim_query.ComputeBlendShapeWeights(time)
                    mapped = target.GetBlendShapeMapper().Remap(weights)
                    targets = target.GetBlendShapeTargetsRel().GetTargets()
                    for i, path in enumerate(targets):
                        if i >= len(mapped) or mapped[i] == 0:
                            continue
                        shape = UsdSkel.BlendShape(stage.GetPrimAtPath(path))
                        offsets = shape.GetOffsetsAttr().Get()
                        indices = shape.GetPointIndicesAttr().Get() or range(len(points))
                        for k, p in enumerate(indices):
                            points[p] = points[p] + offsets[k] * mapped[i]
                if not target.ComputeSkinnedPoints(xforms, points, time):
                    print("skinning failed for", mesh_prim.GetPath())
                    continue
                flat = []
                for p in points:
                    w = skel_to_world.Transform(Gf.Vec3d(p))
                    flat += [w[0], w[1], w[2]]
                meshes[str(mesh_prim.GetPath())] = flat
                skinned.add(str(mesh_prim.GetPath()))

    # PointInstancer prototypes: all instances of each prototype mesh, concatenated.
    for prim in stage.Traverse():
        if not prim.IsA(UsdGeom.PointInstancer):
            continue
        pi = UsdGeom.PointInstancer(prim)
        xforms = pi.ComputeInstanceTransformsAtTime(time, time)
        proto_indices = pi.GetProtoIndicesAttr().Get(time)
        inst_to_world = xcache.GetLocalToWorldTransform(prim)
        for proto_index, proto_path in enumerate(pi.GetPrototypesRel().GetTargets()):
            proto = stage.GetPrimAtPath(proto_path)
            root_inv = xcache.GetLocalToWorldTransform(proto).GetInverse()
            for desc in Usd.PrimRange(proto):
                if not desc.IsA(UsdGeom.Mesh):
                    continue
                rel = xcache.GetLocalToWorldTransform(desc) * root_inv
                pts = UsdGeom.Mesh(desc).GetPointsAttr().Get(time)
                flat = []
                for i, x in enumerate(xforms):
                    if proto_indices[i] != proto_index:
                        continue
                    m = rel * x * inst_to_world
                    for p in pts:
                        w = m.Transform(Gf.Vec3d(p))
                        flat += [w[0], w[1], w[2]]
                meshes[str(desc.GetPath())] = flat
                skinned.add(str(desc.GetPath()))

    for prim in stage.Traverse():
        if prim.IsA(UsdGeom.Mesh) and str(prim.GetPath()) not in skinned:
            m = xcache.GetLocalToWorldTransform(prim)
            flat = []
            for p in UsdGeom.Mesh(prim).GetPointsAttr().Get(time) or []:
                w = m.Transform(Gf.Vec3d(p))
                flat += [w[0], w[1], w[2]]
            meshes[str(prim.GetPath())] = flat

    with open(out, "w") as f:
        json.dump({"time": t, "meshes": meshes}, f)
    print("evaluated %d meshes" % len(meshes))


main()
