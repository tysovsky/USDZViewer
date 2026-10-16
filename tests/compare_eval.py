"""Compares world-space mesh points: python tests/compare_eval.py OURS.json REFERENCE.json [TOLERANCE]

The tolerance is relative to the scene size (default 1e-4)."""

import json
import math
import sys


def main():
    ours = json.load(open(sys.argv[1]))["meshes"]
    ref = json.load(open(sys.argv[2]))["meshes"]
    tol = float(sys.argv[3]) if len(sys.argv) > 3 else 1e-4
    extent = 1e-9
    for pts in ref.values():
        for v in pts:
            extent = max(extent, abs(v))
    failed = False
    for path in sorted(set(ours) | set(ref)):
        if path not in ref:
            print("%s: not in the reference (implicit shape?)" % path)
            continue
        if path not in ours:
            print("%s: missing from ours" % path)
            failed = True
            continue
        a, b = ours[path], ref[path]
        if len(a) != len(b):
            print("%s: %d values vs reference %d" % (path, len(a), len(b)))
            failed = True
            continue
        worst, worst_i = 0.0, -1
        for i in range(0, len(a), 3):
            d = math.sqrt(sum((a[i + k] - b[i + k]) ** 2 for k in range(3)))
            if d > worst:
                worst, worst_i = d, i // 3
        status = "ok" if worst <= tol * extent else "MISMATCH"
        if status != "ok":
            failed = True
            i = worst_i * 3
            print("%s: max error %.3g at point %d: %s vs reference %s  %s" % (
                path, worst, worst_i, a[i:i + 3], b[i:i + 3], status))
        else:
            print("%s: max error %.3g  ok" % (path, worst))
    print("FAILED" if failed else "OK (scene extent %.3g)" % extent)
    sys.exit(1 if failed else 0)


main()
