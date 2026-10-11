#!/usr/bin/env bash
# build_blender.sh - Blender (blenderphi, branch main-anari) with the ANARI Cycles device.
#
# Needs: scripts/build_anari_sdk.sh and scripts/fetch_blender_libs.sh.
# Result: build/blender/bin/Blender.app (run it through scripts/blender.sh).
#
# 16 GB of memory: 6 compile jobs, 2 heavy compile jobs and 1 link job by default.
# Override with BLENDER_JOBS=<n>.
#
# Usage: build_blender.sh [configure|build|install]...   (default: all three)
#   After C++ changes:               build_blender.sh build install
#   After add-on (.py) changes only: build_blender.sh install
set -euo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/env.sh"

B="$ANARI_BUILD/blender"
BLENDER_JOBS="${BLENDER_JOBS:-6}"
steps=("$@")
[ "${#steps[@]}" = 0 ] && steps=(configure build install)

for step in "${steps[@]}"; do
  case "$step" in
    configure)
      log_run blender-configure cmake -S "$ANARI_WS/blenderphi" -B "$B" "${ANARI_CMAKE_BASE[@]}" \
        -DWITH_CYCLES_DEVICE_ANARI=ON \
        -Danari_DIR="$ANARI_SDK_CMAKE_DIR" \
        -DWITH_GTESTS=OFF \
        -DNINJA_MAX_NUM_PARALLEL_COMPILE_HEAVY_JOBS=2 \
        -DNINJA_MAX_NUM_PARALLEL_LINK_JOBS=1
      ;;
    build)
      log_run blender-build cmake --build "$B" --parallel "$BLENDER_JOBS"
      ;;
    install)
      log_run blender-install cmake --install "$B"
      ;;
    *)
      echo "unknown step: $step" >&2
      exit 2
      ;;
  esac
done
ls -d "$B/bin/Blender.app" 2>/dev/null || true
