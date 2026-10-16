"""Reference layer dump using Pixar's USD Python API (run with Blender's bundled Python, which ships pxr):

    blender --background --factory-startup --python tests/pxr_dump_layer.py -- INPUT OUT.json

Writes the same canonical JSON as `usdzinfo INPUT --dump-layer OUT.json` so the two can be compared with
tests/compare_layers.py. List ops are flattened like the viewer does (explicit items, or prepended + added +
appended items); quaternions are written as (x, y, z, w).
"""

import json
import math
import sys

from pxr import Gf, Sdf, Vt

SPEC_TYPES = {
    "PrimSpec": 6, "AttributeSpec": 1, "RelationshipSpec": 8, "VariantSpec": 10, "VariantSetSpec": 11,
    "PseudoRootSpec": 7,
}


def num(x):
    return {"n": [float(v) for v in x], "t": 0, "a": False}


def flatten_listop(op):
    if op.isExplicit:
        return list(op.explicitItems)
    return list(op.prependedItems) + list(op.addedItems) + list(op.appendedItems)


def quat(q):
    i = q.GetImaginary()
    return [float(i[0]), float(i[1]), float(i[2]), float(q.GetReal())]


def matrix(m):
    return [float(m[r][c]) for r in range(len(m)) for c in range(len(m))]


def element(v):
    """Returns (numbers, tuple size) for a single numeric element, or None."""
    if isinstance(v, bool):
        return [1.0 if v else 0.0], 1
    if isinstance(v, (int, float)):
        return [float(v)], 1
    if isinstance(v, (Gf.Quatf, Gf.Quatd, Gf.Quath)):
        return quat(v), 4
    if isinstance(v, (Gf.Matrix4d, Gf.Matrix3d, Gf.Matrix2d, Gf.Matrix4f, Gf.Matrix3f, Gf.Matrix2f)):
        m = matrix(v)
        return m, len(m)
    if hasattr(v, "__len__") and hasattr(v, "__getitem__") and type(v).__module__.startswith("pxr.Gf"):
        return [float(c) for c in v], len(v)
    return None


def convert(v):
    if v is None:
        return None
    if isinstance(v, Sdf.ValueBlock):
        return {"blocked": True}
    if isinstance(v, str):
        return {"s": [v], "a": False}
    if isinstance(v, Sdf.AssetPath):
        return {"s": [v.path], "a": False}
    if isinstance(v, Sdf.Path):
        return {"s": [str(v)], "a": False}
    if isinstance(v, (Sdf.Specifier, Sdf.Variability, Sdf.Permission)):
        return {"n": [float(v.value)], "t": 1, "a": False}
    if isinstance(v, (Sdf.TokenListOp, Sdf.StringListOp)):
        return {"s": [str(x) for x in flatten_listop(v)], "a": True}
    if isinstance(v, Sdf.PathListOp):
        return {"s": [str(x) for x in flatten_listop(v)], "a": True}
    if isinstance(v, (Sdf.ReferenceListOp, Sdf.PayloadListOp)):
        return {"r": [[r.assetPath, str(r.primPath), r.layerOffset.offset, r.layerOffset.scale]
                      for r in flatten_listop(v)]}
    if isinstance(v, Sdf.Payload):
        return {"r": [[v.assetPath, str(v.primPath), v.layerOffset.offset, v.layerOffset.scale]]}
    if isinstance(v, (Sdf.IntListOp, Sdf.Int64ListOp, Sdf.UIntListOp, Sdf.UInt64ListOp)):
        return {"n": [float(x) for x in flatten_listop(v)], "t": 1, "a": True}
    if isinstance(v, dict):
        return {"d": {k: convert(x) for k, x in v.items()}}
    if isinstance(v, Sdf.LayerOffset):
        return {"n": [v.offset, v.scale], "t": 2, "a": False}
    e = element(v)
    if e is not None:
        return {"n": e[0], "t": e[1], "a": False}
    # Sequences: Vt arrays, python lists/tuples (token vectors, path vectors, layer offsets, ...)
    try:
        items = list(v)
    except TypeError:
        return {"unsupported": type(v).__name__}
    if not items:
        return {"empty": True}
    first = items[0]
    if isinstance(first, Sdf.LayerOffset):
        out = []
        for o in items:
            out += [o.offset, o.scale]
        return {"n": out, "t": 2, "a": True}
    if isinstance(first, (str, Sdf.Path, Sdf.AssetPath)):
        return {"s": [x.path if isinstance(x, Sdf.AssetPath) else str(x) for x in items], "a": True}
    nums, tuple_size = [], 1
    for x in items:
        e = element(x)
        if e is None:
            return {"unsupported": type(x).__name__}
        nums += e[0]
        tuple_size = e[1]
    return {"n": nums, "t": tuple_size, "a": True}


def main():
    argv = sys.argv[sys.argv.index("--") + 1:]
    src, out = argv[0], argv[1]
    layer = Sdf.Layer.FindOrOpen(src)
    specs = {}

    def visit(path):
        obj = layer.GetObjectAtPath(path)
        if obj is None:
            return
        spec_type = SPEC_TYPES.get(type(obj).__name__, 0)
        if path == Sdf.Path.absoluteRootPath:
            spec_type = 7
        fields = {}
        # ListInfoKeys omits the "children" fields; add the ones the viewer reads explicitly.
        for name in obj.ListInfoKeys():
            if name == "timeSamples":
                samples = []
                for t in layer.ListTimeSamplesForPath(path):
                    samples.append([t, convert(layer.QueryTimeSample(path, t))])
                fields[name] = {"ts": samples}
            elif name == "subLayerOffsets":  # no Python converter for GetInfo
                flat = []
                for o in layer.subLayerOffsets:
                    flat += [o.offset, o.scale]
                fields[name] = {"n": flat, "t": 2, "a": True}
            else:
                fields[name] = convert(obj.GetInfo(name))
        prim = obj.primSpec if isinstance(obj, Sdf.VariantSpec) else obj
        if isinstance(prim, Sdf.PrimSpec) or spec_type == 7:
            kids = [c.name for c in prim.nameChildren] if hasattr(prim, "nameChildren") else []
            if spec_type == 7:
                kids = [p.name for p in layer.rootPrims]
            if kids:
                fields["primChildren"] = {"s": kids, "a": True}
            props = [p.name for p in prim.properties] if hasattr(prim, "properties") else []
            if props:
                fields["properties"] = {"s": props, "a": True}
        specs[str(path)] = {"type": spec_type, "fields": fields}

    layer.Traverse(Sdf.Path.absoluteRootPath, visit)
    with open(out, "w") as f:
        json.dump({"specs": specs}, f)
    print("dumped %d specs" % len(specs))


main()
