#!/usr/bin/env bash
# build_visionaray.sh - Visionaray (header-only install) and anari-visionaray with both
# of its devices: `visionaray` (CPU) and `visionaray_cuda`, one library each, as on
# Windows.
#
# Visionaray is installed first, with CUDA enabled so that its package exports the CUDA
# run time dependency, without viewer, examples and the `common` library. anari-visionaray
# compiles against the *installed* headers (install/include/visionaray), which is why
# this script always does both.
#
# ANARI_VISIONARAY_CUDA=OFF builds the CPU device only.
#
# Needs: scripts/build_anari_sdk.sh. Installs to install/lib.
set -euo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/env.sh"

CUDA="${ANARI_VISIONARAY_CUDA:-ON}"

B="$ANARI_BUILD/visionaray"
log_run visionaray-configure cmake -S "$ANARI_WS/visionaray" -B "$B" "${ANARI_CMAKE_COMMON[@]}" \
  -DCMAKE_INSTALL_PREFIX="$ANARI_INSTALL" \
  -DVSNRAY_ENABLE_EXAMPLES=OFF -DVSNRAY_ENABLE_VIEWER=OFF -DVSNRAY_ENABLE_COMMON=OFF \
  -DVSNRAY_ENABLE_CUDA="$CUDA" -DVSNRAY_ENABLE_TBB=OFF -DBUILD_TESTING=OFF \
  -DCMAKE_CUDA_ARCHITECTURES="$ANARI_CUDA_ARCH"
log_run visionaray-install cmake --build "$B" --target install

B="$ANARI_BUILD/anari-visionaray"
log_run anari-visionaray-configure cmake -S "$ANARI_WS/anari-visionaray" -B "$B" "${ANARI_CMAKE_COMMON[@]}" \
  -DCMAKE_INSTALL_PREFIX="$ANARI_INSTALL" -DCMAKE_PREFIX_PATH="$ANARI_INSTALL" \
  -Danari_DIR="$ANARI_SDK_CMAKE_DIR" \
  -DBUILD_SHARED_LIBS=ON \
  -DANARI_VISIONARAY_ENABLE_CUDA="$CUDA" -DANARI_VISIONARAY_ENABLE_NANOVDB=ON \
  -DCMAKE_CUDA_ARCHITECTURES="$ANARI_CUDA_ARCH"
log_run anari-visionaray-build cmake --build "$B" --parallel "$JOBS"
log_run anari-visionaray-install cmake --install "$B"
ls -l "$ANARI_INSTALL/lib"/libanari_library_visionaray*.so
for lib in "$ANARI_INSTALL/lib"/libanari_library_visionaray*.so; do
  check_undefined "$lib" "$ANARI_INSTALL/lib"
done
