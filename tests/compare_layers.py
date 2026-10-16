"""Compares two canonical layer dumps (usdzinfo --dump-layer vs. tests/pxr_dump_layer.py).

    python tests/compare_layers.py OURS.json REFERENCE.json

Exits with status 1 if any spec or field differs (numbers are compared with a small relative tolerance).
"""

import json
import math
import sys


def is_empty(v):
    if v is None:
        return True
    if v.get("empty"):
        return True
    if "n" in v and not v["n"] and v.get("a"):
        return True
    if "s" in v and not v["s"] and v.get("a"):
        return True
    return False


def close(a, b):
    if isinstance(a, float) and math.isnan(a):
        return isinstance(b, float) and math.isnan(b)
    if a == b:
        return True
    return abs(a - b) <= 1e-6 * max(1.0, abs(a), abs(b))


def diff(ours, ref, where, out):
    if len(out) > 60:
        return
    if is_empty(ours) and is_empty(ref):
        return
    if ours is None or ref is None:
        out.append("%s: %r vs reference %r" % (where, summarize(ours), summarize(ref)))
        return
    if "unsupported" in ref:
        return
    if "d" in ref or "d" in ours:
        a, b = ours.get("d", {}), ref.get("d", {})
        for k in sorted(set(a) | set(b)):
            diff(a.get(k), b.get(k), where + "." + k, out)
        return
    if "ts" in ref or "ts" in ours:
        a, b = ours.get("ts", []), ref.get("ts", [])
        if len(a) != len(b):
            out.append("%s: %d samples vs reference %d" % (where, len(a), len(b)))
            return
        for (ta, va), (tb, vb) in zip(a, b):
            if not close(ta, tb):
                out.append("%s: sample time %r vs reference %r" % (where, ta, tb))
                return
            diff(va, vb, "%s[t=%g]" % (where, tb), out)
        return
    if "r" in ref or "r" in ours:
        a, b = ours.get("r", []), ref.get("r", [])
        if a != b:
            out.append("%s: references %r vs reference %r" % (where, a, b))
        return
    if "s" in ref or "s" in ours:
        if ours.get("s") != ref.get("s"):
            out.append("%s: %r vs reference %r" % (where, summarize(ours), summarize(ref)))
        return
    if "n" in ref or "n" in ours:
        a, b = ours.get("n"), ref.get("n")
        if a is None or b is None or len(a) != len(b):
            out.append("%s: %r vs reference %r" % (where, summarize(ours), summarize(ref)))
            return
        if ref.get("a") != ours.get("a"):
            out.append("%s: array flag %r vs reference %r" % (where, ours.get("a"), ref.get("a")))
        if ref.get("t") and ours.get("t") and ref["t"] != ours["t"] and len(a) > 0:
            out.append("%s: tuple size %r vs reference %r" % (where, ours.get("t"), ref.get("t")))
        for i, (x, y) in enumerate(zip(a, b)):
            if not close(x, y):
                out.append("%s[%d]: %r vs reference %r" % (where, i, x, y))
                return
        return
    if ours.get("blocked") != ref.get("blocked"):
        out.append("%s: %r vs reference %r" % (where, ours, ref))


def summarize(v):
    s = json.dumps(v)
    return s if len(s) < 160 else s[:160] + "..."


def main():
    ours = json.load(open(sys.argv[1]))["specs"]
    ref = json.load(open(sys.argv[2]))["specs"]
    problems = []
    for path in sorted(set(ours) | set(ref)):
        if path not in ours:
            problems.append("missing spec %s" % path)
            continue
        if path not in ref:
            problems.append("extra spec %s" % path)
            continue
        a, b = ours[path], ref[path]
        if b["type"] and a["type"] != b["type"]:
            problems.append("%s: spec type %d vs reference %d" % (path, a["type"], b["type"]))
        fa, fb = a["fields"], b["fields"]
        for name in sorted(set(fa) | set(fb)):
            if name in ("variantSetChildren", "variantChildren") and name not in fb:
                continue  # children fields the reference API does not list
            if name in ("custom", "variability", "specifier") and name not in fa and len(fb[name].get("n", [])) == 1:
                continue  # the reference API reports schema fallbacks as if they were authored
            if name not in fa and not is_empty(fb.get(name)):
                problems.append("%s: missing field %s = %s" % (path, name, summarize(fb[name])))
            elif name not in fb and not is_empty(fa.get(name)):
                problems.append("%s: extra field %s = %s" % (path, name, summarize(fa[name])))
            elif name in fa and name in fb:
                diff(fa[name], fb[name], "%s:%s" % (path, name), problems)
    nfields = sum(len(s["fields"]) for s in ref.values())
    if problems:
        for p in problems[:80]:
            print(p)
        print("FAILED: %d problems (%d specs, %d fields in reference)" % (len(problems), len(ref), nfields))
        sys.exit(1)
    print("OK: %d specs, %d fields match" % (len(ref), nfields))


main()
