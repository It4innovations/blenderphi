#!/usr/bin/env bash
# build_anari_sdk.sh - ANARI SDK 0.17 with the helide device.
# Installs to install/ (libraries in install/lib, CMake package in
# install/lib/cmake/anari-0.17.0). All devices and Blender are built against it.
set -euo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/env.sh"

# The GCC of Rocky Linux 10 compiles for x86-64-v3 (AVX2) by default. helide builds its
# own Embree for SSE4.2 only (external/embree, as on Windows); with AVX defined that
# Embree references its 8-wide primitives, which only exist in the AVX libraries it
# does not build, and helide fails to load ("undefined symbol
# embree::CurveNi<8>::Type::name()"). x86-64-v2 is the SSE4.2 level. Embree only takes
# CMAKE_CXX_FLAGS with EMBREE_IGNORE_CMAKE_CXX_FLAGS=OFF.
SDK_ARCH_FLAGS="${ANARI_SDK_ARCH_FLAGS:--march=x86-64-v2}"

B="$ANARI_BUILD/anari-sdk"
log_run anari-sdk-configure cmake -S "$ANARI_WS/ANARI-SDK" -B "$B" "${ANARI_CMAKE_COMMON[@]}" \
  -DCMAKE_INSTALL_PREFIX="$ANARI_INSTALL" \
  -DCMAKE_C_FLAGS="$SDK_ARCH_FLAGS" -DCMAKE_CXX_FLAGS="$SDK_ARCH_FLAGS" \
  -DEMBREE_IGNORE_CMAKE_CXX_FLAGS=OFF \
  -DBUILD_TESTING=OFF -DBUILD_EXAMPLES=ON \
  -DBUILD_HELIDE_DEVICE=ON -DBUILD_HELIDE_GPU_DEVICE=OFF \
  -DBUILD_VIEWER=OFF -DBUILD_CTS=OFF
log_run anari-sdk-build cmake --build "$B" --parallel "$JOBS"
log_run anari-sdk-install cmake --install "$B"
ls "$ANARI_INSTALL/lib"/libanari*.so
# A device library with unresolved symbols only fails when it is loaded.
if ldd -r "$ANARI_INSTALL/lib/libanari_library_helide.so" 2>&1 | grep -q "undefined symbol"; then
  echo "libanari_library_helide.so has undefined symbols:" >&2
  ldd -r "$ANARI_INSTALL/lib/libanari_library_helide.so" 2>&1 | grep "undefined symbol" | head -n 5 >&2
  exit 1
fi
