#!/usr/bin/env bash
# build_visrtx.sh - VisRTX, RTX device only (library name `visrtx`), as on Windows.
# The OpenGL device is not built. The OptiX headers are the ones of
# barney/submodules/optix (9.0).
#
# VISRTX_MIN_ARCH is the compute capability the PTX of the device is generated for.
# It stays at 75 (the default with CUDA 13, and the value of Windows): OptiX and the
# driver compile that PTX for the GPU which is there. With the compute capability of
# the Blackwell GPUs (120) the build fails: for that target the nvcc of CUDA 13.3 runs
# ptxas over the PTX of the OptiX programs, which stops at the OptiX intrinsics
# ("Call to '_optix_get_launch_index_x' requires call prototype").
#
# Needs: scripts/build_anari_sdk.sh and the barney sources. Installs to install/lib.
set -euo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/env.sh"

B="$ANARI_BUILD/visrtx"
log_run visrtx-configure cmake -S "$ANARI_WS/VisRTX" -B "$B" "${ANARI_CMAKE_COMMON[@]}" \
  -DCMAKE_INSTALL_PREFIX="$ANARI_INSTALL" \
  -Danari_DIR="$ANARI_SDK_CMAKE_DIR" \
  -DVISRTX_BUILD_GL_DEVICE=OFF \
  -DOptiX_ROOT_DIR="$ANARI_OPTIX_DIR" \
  -DVISRTX_MIN_ARCH="${VISRTX_MIN_ARCH:-75}"
log_run visrtx-build cmake --build "$B" --parallel "$JOBS"
log_run visrtx-install cmake --install "$B"
ls -l "$ANARI_INSTALL/lib"/libanari_library_visrtx*.so
check_undefined "$ANARI_INSTALL/lib/libanari_library_visrtx.so"
