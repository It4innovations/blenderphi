#!/usr/bin/env bash
# build_photon.sh - Photon (Kokkos path tracer), CPU build: Kokkos Threads with Embree.
# Library name `photon`, installed to install/photon_cpu/lib (the name of the Windows
# workspace; the GPU build of Windows needs CUDA and OptiX and does not exist on macOS).
#
# Kokkos 5.2.2 is cloned to external/kokkos if it is not there and installed to
# install/kokkos-host (Threads and Serial back ends, static). Photon links the Embree of
# Blender's precompiled libraries: inside Blender it uses the copy Blender has loaded,
# other programs need Blender.app/Contents/Resources/lib on DYLD_LIBRARY_PATH.
#
# Needs: scripts/build_anari_sdk.sh, scripts/fetch_blender_libs.sh.
set -euo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/env.sh"

KOKKOS_SRC="$ANARI_EXTERNAL/kokkos"
KOKKOS="$ANARI_INSTALL/kokkos-host"
if [ ! -f "$KOKKOS_SRC/CMakeLists.txt" ]; then
  git clone -q --depth 1 --branch 5.2.2 https://github.com/kokkos/kokkos.git "$KOKKOS_SRC"
fi
if [ ! -d "$KOKKOS/lib/cmake/Kokkos" ]; then
  log_run kokkos-host-configure cmake -S "$KOKKOS_SRC" -B "$ANARI_BUILD/kokkos-host" "${ANARI_CMAKE_COMMON[@]}" \
    -DCMAKE_INSTALL_PREFIX="$KOKKOS" \
    -DCMAKE_CXX_STANDARD=20 -DCMAKE_CXX_EXTENSIONS=OFF \
    -DKokkos_ENABLE_THREADS=ON -DKokkos_ENABLE_SERIAL=ON \
    -DKokkos_ENABLE_TESTS=OFF -DKokkos_ENABLE_EXAMPLES=OFF -DKokkos_ENABLE_BENCHMARKS=OFF \
    -DBUILD_SHARED_LIBS=OFF -DCMAKE_POSITION_INDEPENDENT_CODE=ON
  log_run kokkos-host-build cmake --build "$ANARI_BUILD/kokkos-host" --parallel "$JOBS"
  log_run kokkos-host-install cmake --install "$ANARI_BUILD/kokkos-host"
fi

# Blender's macOS libraries have Embree (headers and libembree4.dylib) but no CMake
# package for it, which Photon looks for. A minimal one is written here.
EMBREE="$BLENDER_LIB_DIR/embree"
EMBREE_DIR="$ANARI_BUILD/embree-blender-cmake"
EMBREE_VERSION="$(sed -n 's/^#define RTC_VERSION_STRING "\(.*\)"/\1/p' "$EMBREE/include/embree4/rtcore_config.h")"
mkdir -p "$EMBREE_DIR"
cat >"$EMBREE_DIR/embree-config.cmake" <<EOF
# Written by scripts/build_photon.sh: the Embree of Blender's precompiled libraries.
set(EMBREE_INCLUDE_DIRS "$EMBREE/include")
set(EMBREE_LIBRARIES "$EMBREE/lib/libembree4.dylib")
set(EMBREE_VERSION "$EMBREE_VERSION")
EOF
cat >"$EMBREE_DIR/embree-config-version.cmake" <<EOF
set(PACKAGE_VERSION "$EMBREE_VERSION")
if(PACKAGE_VERSION VERSION_LESS PACKAGE_FIND_VERSION)
  set(PACKAGE_VERSION_COMPATIBLE FALSE)
else()
  set(PACKAGE_VERSION_COMPATIBLE TRUE)
endif()
EOF

B="$ANARI_BUILD/photon-cpu"
PREFIX="$ANARI_INSTALL/photon_cpu"
log_run photon-cpu-configure cmake -S "$ANARI_WS/photon" -B "$B" "${ANARI_CMAKE_COMMON[@]}" \
  -DOPENCODE_ENABLE_CUDA=OFF \
  -DKokkos_DIR="$KOKKOS/lib/cmake/Kokkos" \
  -Danari_DIR="$ANARI_SDK_CMAKE_DIR" \
  -DPHOTON_ENABLE_EMBREE=ON -Dembree_DIR="$EMBREE_DIR" \
  -DPHOTON_ENABLE_OIDN=OFF \
  -DPHOTON_STB_IMAGE_DIR="$ANARI_WS/ANARI-SDK/external/stb_image" \
  -DPHOTON_BUILD_TESTS=OFF -DPHOTON_BUILD_APPS=OFF -DPHOTON_BUILD_TOOLS=OFF \
  -DCMAKE_INSTALL_PREFIX="$PREFIX"
log_run photon-cpu-build cmake --build "$B" --parallel "$JOBS"
log_run photon-cpu-install cmake --install "$B"
ls -l "$PREFIX"/lib/*.dylib
