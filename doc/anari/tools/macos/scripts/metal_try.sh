#!/usr/bin/env bash
# metal_try.sh [test ...] - compile the Metal kernels of cyclesphi-anari and render.
#
# For work on the Metal kernels: runs smoke tests (default: test_basic_render) with
# computeDevice=metal and prints the kernel compile errors, if any. The kernel sources
# are read from the source tree (CYCLES_KERNEL_PATH), so kernel headers can be edited
# and tried without building or installing the device.
# Log: build/logs/metal-try.txt. METAL_TRY_INSTALLED=1 uses the installed sources.
set -uo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/env.sh"

tests=("$@")
[ "${#tests[@]}" = 0 ] && tests=(AnariSmokeTest.test_basic_render)
if [ "${METAL_TRY_INSTALLED:-0}" != 1 ]; then
  export CYCLES_KERNEL_PATH="$ANARI_WS/cyclesphi-anari/cyclesphi/src"
fi
log="$ANARI_LOGS/metal-try.txt"
start=$(date +%s)
bash "$ANARI_WS/scripts/blender.sh" --background --factory-startup -noaudio \
  --python "$ANARI_WS/blenderphi/tests/python/anari_smoke_tests.py" -- \
  --library cycles --library-path "$ANARI_INSTALL/lib" \
  --outdir "$ANARI_BUILD/test_out/cycles_metal" \
  --device-parameters computeDevice=metal "${tests[@]}" >"$log" 2>&1
echo "exit=$? time=$(($(date +%s) - start))s"
grep -E '^ANARI|^OK|^FAILED|^Ran ' "$log" |
  sed -E 's/mean \[.*\], Cycles mean \[.*\], /basic: /' | cut -c1-200
if grep -q 'could not be loaded' "$log"; then
  echo "Metal kernels failed to compile, $(grep -c 'error:' "$log") error lines:"
  grep -A200 'could not be loaded' "$log" | grep 'error:' | sed -E 's/^[ |]*//' | sort -u |
    cut -c1-220 | head -n "${METAL_TRY_ERRORS:-40}"
fi
