#!/usr/bin/env bash
# build_cyclesphi_anari.sh - Cycles as an ANARI device (library name `cycles`).
#
# Builds the Cycles of the `cyclesphi` submodule against Blender's precompiled
# libraries (the same ones Blender itself loads) with the CPU and the Metal device,
# and Open Image Denoise. There is no CUDA / OptiX on macOS.
#
# Needs: scripts/build_anari_sdk.sh and scripts/fetch_blender_libs.sh.
# Installs install/lib/libanari_library_cycles.dylib and install/lib/cycles/.
set -euo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/env.sh"

B="$ANARI_BUILD/cyclesphi-anari"
log_run cyclesphi-anari-configure cmake -S "$ANARI_WS/cyclesphi-anari" -B "$B" "${ANARI_CMAKE_COMMON[@]}" \
  -DCMAKE_INSTALL_PREFIX="$ANARI_INSTALL" \
  -Danari_DIR="$ANARI_SDK_CMAKE_DIR" \
  -DCYCLES_LIB_DIR="$BLENDER_LIB_DIR" \
  -DWITH_CYCLESPHI=OFF \
  -DANARI_CYCLES_USE_OPTIX=OFF -DANARI_CYCLES_USE_OIDN=ON \
  -DANARI_CYCLES_USE_METAL="${ANARI_CYCLES_USE_METAL:-ON}"
log_run cyclesphi-anari-build cmake --build "$B" --parallel "$JOBS"
log_run cyclesphi-anari-install cmake --install "$B"
ls -l "$ANARI_INSTALL/lib/libanari_library_cycles.dylib"
