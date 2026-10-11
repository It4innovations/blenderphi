#!/usr/bin/env bash
# build_anari_sdk.sh - ANARI SDK 0.17 with the helide device.
# Installs to install/ (libraries in install/lib, CMake package in
# install/lib/cmake/anari-0.17.0). All devices and Blender are built against it.
set -euo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/env.sh"

B="$ANARI_BUILD/anari-sdk"
log_run anari-sdk-configure cmake -S "$ANARI_WS/ANARI-SDK" -B "$B" "${ANARI_CMAKE_COMMON[@]}" \
  -DCMAKE_INSTALL_PREFIX="$ANARI_INSTALL" \
  -DBUILD_TESTING=OFF -DBUILD_EXAMPLES=ON \
  -DBUILD_HELIDE_DEVICE=ON -DBUILD_HELIDE_GPU_DEVICE=OFF \
  -DBUILD_VIEWER=OFF -DBUILD_CTS=OFF
log_run anari-sdk-build cmake --build "$B" --parallel "$JOBS"
log_run anari-sdk-install cmake --install "$B"
ls "$ANARI_INSTALL/lib"/libanari*.dylib
