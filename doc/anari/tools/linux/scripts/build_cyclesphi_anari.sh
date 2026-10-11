#!/usr/bin/env bash
# build_cyclesphi_anari.sh - Cycles as an ANARI device (library name `cycles`).
#
# Builds the Cycles of the `cyclesphi` submodule against Blender's precompiled
# libraries (the same ones Blender itself loads) with the CPU, CUDA and OptiX devices
# and Open Image Denoise. The OptiX headers are the ones of barney/submodules/optix
# (9.0), as on Windows.
#
# The GPU kernels are precompiled at build time. ANARI_CYCLES_CUDA_ARCHS selects for
# which architectures:
#   sm_120      a native kernel (cubin) for the Blackwell GPUs of this node,
#   compute_75  a PTX kernel which the driver compiles for any GPU from Turing on
#               (Cycles falls back to it when there is no cubin for the GPU).
# The OptiX kernels do not depend on the architecture.
#
# Needs: scripts/build_anari_sdk.sh, scripts/fetch_blender_libs.sh and the barney
# sources (for the OptiX headers).
# Installs install/lib/libanari_library_cycles.so and install/lib/cycles/.
set -euo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/env.sh"

ARCHS="${ANARI_CYCLES_CUDA_ARCHS:-sm_${ANARI_CUDA_ARCH};compute_75}"
B="$ANARI_BUILD/cyclesphi-anari"
log_run cyclesphi-anari-configure cmake -S "$ANARI_WS/cyclesphi-anari" -B "$B" "${ANARI_CMAKE_COMMON[@]}" \
  -DCMAKE_INSTALL_PREFIX="$ANARI_INSTALL" \
  -Danari_DIR="$ANARI_SDK_CMAKE_DIR" \
  -DCYCLES_LIB_DIR="$BLENDER_LIB_DIR" \
  -DWITH_CYCLESPHI=OFF \
  -DANARI_CYCLES_USE_OPTIX=ON -DANARI_CYCLES_USE_OIDN=ON \
  -DOPTIX_ROOT_DIR="$ANARI_OPTIX_DIR" \
  -DCUDA_TOOLKIT_ROOT_DIR="$ANARI_CUDA_ROOT" \
  "-DANARI_CYCLES_CUDA_ARCHS=$ARCHS"
log_run cyclesphi-anari-build cmake --build "$B" --parallel "$JOBS"
log_run cyclesphi-anari-install cmake --install "$B"
ls -l "$ANARI_INSTALL/lib/libanari_library_cycles.so"
ls "$ANARI_INSTALL/lib/cycles/lib" 2>/dev/null || true
