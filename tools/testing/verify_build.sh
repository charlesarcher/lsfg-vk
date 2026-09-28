#!/usr/bin/env bash
# Build every tree and prove which code is actually going to run.
#
# This exists because build-frameddb was once 36 minutes stale and served a
# layer that did not contain the fix being tested, which cost a debugging
# session. Every Vulkan object in the app, the layer and the backend comes
# from a different build directory, so "I rebuilt" means nothing unless the
# binary, the .so and the git hash are all shown together.
#
#   ./tools/testing/verify_build.sh
#
# Exits non-zero if any tree fails to build. The staleness check is advisory
# and reported, not enforced, because an uncommitted tree is legitimate.
set -uo pipefail

cd "$(dirname "$0")/../.." || exit 1
ROOT="$PWD"
HASH="$(git rev-parse --short HEAD 2>/dev/null || echo unknown)"
DIRTY="$(git status --porcelain -- . ':!thirdparty' | grep -q . && echo ' DIRTY' || echo '')"
J="$(nproc)"

echo "repo  $ROOT"
echo "git   $HASH$DIRTY"
echo

# build each tree, then report the mtime of what it actually produced
build_one() {
    local dir="$1" label="$2" artifact="$3"
    local start end
    start=$(date +%s)
    if ! cmake --build "$dir" -j"$J" >"/tmp/verify_build_${dir}.log" 2>&1; then
        echo "  $label: BUILD FAILED  (see /tmp/verify_build_${dir}.log)"
        grep -E "error:" "/tmp/verify_build_${dir}.log" | head -5
        return 1
    fi
    end=$(date +%s)
    if [ -e "$artifact" ]; then
        printf "  %-9s ok  %2ds  %s  %s\n" "$label" "$((end - start))" \
            "$(date -r "$artifact" '+%H:%M:%S')" "${artifact#"$ROOT"/}"
    else
        printf "  %-9s ok  %2ds  (artifact not found: %s)\n" "$label" "$((end - start))" "$artifact"
    fi
}

rc=0
echo "builds:"
build_one build         app      "build/lsfg-vk-app/lsfg-vk-app" || rc=1
build_one build-framedbg app-dbg  "build-framedbg/lsfg-vk-app/lsfg-vk-app" || rc=1
build_one build         layer    "build/lsfg-vk-layer/liblsfg-vk-layer.so" || rc=1
build_one build-framedbg layer-dbg "build-framedbg/lsfg-vk-layer/liblsfg-vk-layer.so" || rc=1

# The layer that VK_LAYER_PATH points at is the one that loads. If that is a
# different tree from the one you just built, everything below it is moot.
echo
echo "layer the compositor will load:"
LAYER_ENV="${VK_LAYER_PATH:-$ROOT/build-framedbg/lsfg-vk-layer}"
for so in "$LAYER_ENV"/*.so; do
    [ -e "$so" ] || continue
    printf "  %s  %s\n" "$(date -r "$so" '+%H:%M:%S')" "$so"
done
echo "  manifest:"
for j in "$LAYER_ENV"/*.json; do
    [ -e "$j" ] || continue
    grep -o '"library_path"[^,]*' "$j" 2>/dev/null | head -2 | sed 's/^/    /'
    # an absolute path that no longer exists is the classic stale-manifest trap
    grep -o '"library_path"[[:space:]]*:[[:space:]]*"[^"]*"' "$j" 2>/dev/null \
        | sed 's/.*"\(.*\)"/\1/' | while read -r lp; do
            [ -e "$lp" ] || echo "    MISSING: $lp"
        done
done

echo
if [ $rc -ne 0 ]; then
    echo "RESULT: BUILD FAILED - not running anything"
    exit 1
fi
echo "RESULT: all trees built at $HASH$DIRTY"
echo "run tests from build-framedbg/ for frame diagnostics; build/ is the clean tree."
