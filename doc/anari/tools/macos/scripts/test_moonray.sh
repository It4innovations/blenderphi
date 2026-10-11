#!/usr/bin/env bash
# test_moonray.sh [prefix] - render scripts/moonray_test.rdla with the standalone `moonray`.
#
# Default prefix: the MoonRay build on the APFS volume
# (/Volumes/anari-apfs/moonray/openmoonray-anari/external/installs/openmoonray). Pass
# ~/scratch/anari/install/moonray to test the staged runtime instead.
# Output: build/test_out/moonray_standalone/moonray_test.exr, and the mean of the image.
set -euo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/env.sh"

anari_apfs_mount
PREFIX="${1:-$ANARI_APFS/moonray/openmoonray-anari/external/installs/openmoonray}"
OUT="$ANARI_BUILD/test_out/moonray_standalone"
mkdir -p "$OUT" "$ANARI_WS/cache/home"

export HOME="$ANARI_WS/cache/home"
export TMPDIR="$ANARI_APFS/tmp"
export RDL2_DSO_PATH="$PREFIX/rdl2dso"
export REZ_MOONRAY_ROOT="$PREFIX"

start=$(date +%s)
"$PREFIX/bin/moonray" -in "$ANARI_WS/scripts/moonray_test.rdla" -out "$OUT/moonray_test.exr" \
  -threads "$JOBS" >"$ANARI_LOGS/moonray-standalone.log" 2>&1 || {
  echo "moonray failed, see $ANARI_LOGS/moonray-standalone.log" >&2
  tail -n 20 "$ANARI_LOGS/moonray-standalone.log" >&2
  exit 1
}
echo "moonray: $(($(date +%s) - start)) s, $(ls -l "$OUT/moonray_test.exr" | awk '{print $5}') bytes"
grep -i -E 'exec.*mode|vectorized|scalar|xpu' "$ANARI_LOGS/moonray-standalone.log" | head -n 3 || true
