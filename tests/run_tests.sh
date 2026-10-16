#!/usr/bin/env bash
# Regression tests: compares the viewer's USD parsing and scene evaluation against Pixar's USD (bundled with
# Blender). Usage: tests/run_tests.sh [BLENDER_EXE] [EXTRA_FILES...]
set -u -o pipefail
cd "$(dirname "$0")/.."
BLENDER="${1:-/c/Program Files/Blender Foundation/Blender 5.2/blender.exe}"
shift || true
INFO=build/Release/usdzinfo.exe
TMP=$(mktemp -d)
FAIL=0

blender_py() { "$BLENDER" --background --factory-startup --python "$@" 2>&1 | grep -E "Traceback|Error" ; }

"$BLENDER" --background --factory-startup --python tests/make_test_assets.py -- tests/assets >/dev/null 2>&1

check_layer() {
    "$INFO" "$1" --dump-layer "$TMP/ours.json" || { echo "FAIL load $1"; FAIL=1; return; }
    blender_py tests/pxr_dump_layer.py -- "$1" "$TMP/ref.json"
    echo "-- layer: $1"
    python tests/compare_layers.py "$TMP/ours.json" "$TMP/ref.json" | tail -12 || FAIL=1
}

check_eval() {
    "$INFO" "$1" --eval "$2" "$TMP/ours_eval.json" || { echo "FAIL eval $1"; FAIL=1; return; }
    blender_py tests/pxr_eval.py -- "$1" "$2" "$TMP/ref_eval.json"
    echo "-- eval: $1 @ $2"
    python tests/compare_eval.py "$TMP/ours_eval.json" "$TMP/ref_eval.json" | grep -E "MISMATCH|missing|OK|FAILED" || FAIL=1
}

for f in tests/assets/composition.usdz tests/assets/composition_c.usdz tests/assets/skel_blend.usdz tests/assets/zup.usda "$@"; do
    check_layer "$f"
done
for t in 0 13.5 37 60; do check_eval tests/assets/composition.usdz $t; check_eval tests/assets/composition_c.usdz $t; done
for t in 0 12.25 48; do check_eval tests/assets/skel_blend.usdz $t; done
check_eval tests/assets/zup.usda 0
for f in "$@"; do
    check_eval "$f" 0
    check_eval "$f" 47.5
done
rm -rf "$TMP"
[ $FAIL -eq 0 ] && echo "ALL TESTS PASSED" || echo "SOME TESTS FAILED"
exit $FAIL
