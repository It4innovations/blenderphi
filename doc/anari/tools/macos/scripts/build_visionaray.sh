#!/usr/bin/env bash
# build_visionaray.sh - Visionaray (header-only install) and anari-visionaray, CPU device
# only (library name `visionaray`). The CUDA device does not exist on macOS.
#
# anari-visionaray compiles against the *installed* Visionaray headers
# (install/include/visionaray), so this script always installs Visionaray first.
#
# Needs: scripts/build_anari_sdk.sh. Installs to install/lib.
set -euo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/env.sh"

B="$ANARI_BUILD/visionaray"
log_run visionaray-configure cmake -S "$ANARI_WS/visionaray" -B "$B" "${ANARI_CMAKE_COMMON[@]}" \
  -DCMAKE_INSTALL_PREFIX="$ANARI_INSTALL" \
  -DVSNRAY_ENABLE_EXAMPLES=OFF -DVSNRAY_ENABLE_VIEWER=OFF -DVSNRAY_ENABLE_COMMON=OFF \
  -DVSNRAY_ENABLE_CUDA=OFF -DVSNRAY_ENABLE_TBB=OFF -DBUILD_TESTING=OFF
log_run visionaray-install cmake --build "$B" --target install

B="$ANARI_BUILD/anari-visionaray"
log_run anari-visionaray-configure cmake -S "$ANARI_WS/anari-visionaray" -B "$B" "${ANARI_CMAKE_COMMON[@]}" \
  -DCMAKE_INSTALL_PREFIX="$ANARI_INSTALL" -DCMAKE_PREFIX_PATH="$ANARI_INSTALL" \
  -Danari_DIR="$ANARI_SDK_CMAKE_DIR" \
  -DBUILD_SHARED_LIBS=ON \
  -DANARI_VISIONARAY_ENABLE_CUDA=OFF -DANARI_VISIONARAY_ENABLE_NANOVDB=ON
log_run anari-visionaray-build cmake --build "$B" --parallel "$JOBS"
log_run anari-visionaray-install cmake --install "$B"
ls -l "$ANARI_INSTALL/lib"/libanari_library_visionaray*.dylib
