#!/usr/bin/env bash
# build_photon.sh [kokkos] [gpu] [cpu] - Photon (library name `photon`), a wavefront
# path tracer written with Kokkos. Two builds of its ANARI device are made, as on
# Windows:
#   gpu   Kokkos Cuda,    OptiX (the 9.0 headers of barney)   -> install/photon/lib
#   cpu   Kokkos Threads, Embree (the one of Blender)         -> install/photon_cpu/lib
# `kokkos` clones Kokkos 5.2.2 to external/kokkos and installs it twice, to
# install/kokkos-cuda and install/kokkos-host. Without arguments all three are done.
#
# The CPU build uses the Embree of Blender's precompiled libraries, so that the same
# Embree is used inside and outside of Blender. That directory has no CMake package on
# Linux; a small one is written to build/embree-blender. Outside of Blender the device
# needs build/blender/bin/lib on LD_LIBRARY_PATH (libembree4.so.4, libtbb.so.12).
#
# Open Image Denoise is switched off (Photon would download a prebuilt one), and the
# tests and command line apps are not built, as on Windows.
#
# Needs: scripts/build_anari_sdk.sh, scripts/fetch_blender_libs.sh, the barney sources.
set -euo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/env.sh"

KOKKOS_VERSION="${KOKKOS_VERSION:-5.2.2}"
KOKKOS_SRC="$ANARI_EXTERNAL/kokkos"
# The Kokkos name of the GPU architecture (Kokkos_ARCH_<name>), see
# external/kokkos/cmake/kokkos_arch.cmake.
KOKKOS_GPU_ARCH="${KOKKOS_GPU_ARCH:-BLACKWELL120}"

steps=("$@")
[ "${#steps[@]}" = 0 ] && steps=(kokkos gpu cpu)

kokkos_common=(-DCMAKE_CXX_STANDARD=20 -DCMAKE_CXX_EXTENSIONS=OFF
  -DKokkos_ENABLE_SERIAL=ON
  -DKokkos_ENABLE_TESTS=OFF -DKokkos_ENABLE_EXAMPLES=OFF -DKokkos_ENABLE_BENCHMARKS=OFF
  -DBUILD_SHARED_LIBS=OFF -DCMAKE_POSITION_INDEPENDENT_CODE=ON)
photon_common=(-Danari_DIR="$ANARI_SDK_CMAKE_DIR"
  -DPHOTON_ENABLE_OIDN=OFF -DPHOTON_BUILD_TESTS=OFF -DPHOTON_BUILD_APPS=OFF
  -DPHOTON_STB_IMAGE_DIR="$ANARI_WS/ANARI-SDK/external/stb_image"
  -DPython3_EXECUTABLE=/usr/bin/python3)

for step in "${steps[@]}"; do
  case "$step" in
    kokkos)
      if [ ! -f "$KOKKOS_SRC/CMakeLists.txt" ]; then
        log_run kokkos-clone git clone -q --depth 1 --branch "$KOKKOS_VERSION" \
          https://github.com/kokkos/kokkos.git "$KOKKOS_SRC"
      fi
      B="$ANARI_BUILD/kokkos-cuda"
      log_run kokkos-cuda-configure cmake -S "$KOKKOS_SRC" -B "$B" "${ANARI_CMAKE_COMMON[@]}" \
        -DCMAKE_INSTALL_PREFIX="$ANARI_INSTALL/kokkos-cuda" "${kokkos_common[@]}" \
        -DKokkos_ENABLE_CUDA=ON -DKokkos_ENABLE_COMPILE_AS_CMAKE_LANGUAGE=ON \
        "-DKokkos_ARCH_$KOKKOS_GPU_ARCH=ON" \
        -DCMAKE_CUDA_ARCHITECTURES="$ANARI_CUDA_ARCH"
      log_run kokkos-cuda-build cmake --build "$B" --parallel "$JOBS"
      log_run kokkos-cuda-install cmake --install "$B"
      B="$ANARI_BUILD/kokkos-host"
      log_run kokkos-host-configure cmake -S "$KOKKOS_SRC" -B "$B" "${ANARI_CMAKE_COMMON[@]}" \
        -DCMAKE_INSTALL_PREFIX="$ANARI_INSTALL/kokkos-host" "${kokkos_common[@]}" \
        -DKokkos_ENABLE_THREADS=ON
      log_run kokkos-host-build cmake --build "$B" --parallel "$JOBS"
      log_run kokkos-host-install cmake --install "$B"
      ;;
    gpu)
      B="$ANARI_BUILD/photon"
      log_run photon-configure cmake -S "$ANARI_WS/photon" -B "$B" "${ANARI_CMAKE_COMMON[@]}" \
        -DCMAKE_INSTALL_PREFIX="$ANARI_INSTALL/photon" "${photon_common[@]}" \
        -DKokkos_DIR="$ANARI_INSTALL/kokkos-cuda/lib/cmake/Kokkos" \
        -DPHOTON_ENABLE_OPTIX=ON -DOptiX_INSTALL_DIR="$ANARI_OPTIX_DIR" \
        -DPHOTON_ENABLE_EMBREE=OFF \
        -DCMAKE_CUDA_ARCHITECTURES="$ANARI_CUDA_ARCH"
      log_run photon-build cmake --build "$B" --parallel "$JOBS"
      log_run photon-install cmake --install "$B"
      ls -l "$ANARI_INSTALL/photon/lib/libanari_library_photon.so"
      check_undefined "$ANARI_INSTALL/photon/lib/libanari_library_photon.so" "$ANARI_INSTALL/lib"
      ;;
    cpu)
      # A CMake package for the Embree of Blender (headers and libembree4.so only).
      E="$(embree_blender_package)"
      B="$ANARI_BUILD/photon-cpu"
      log_run photon-cpu-configure cmake -S "$ANARI_WS/photon" -B "$B" "${ANARI_CMAKE_COMMON[@]}" \
        -DCMAKE_INSTALL_PREFIX="$ANARI_INSTALL/photon_cpu" "${photon_common[@]}" \
        -DOPENCODE_ENABLE_CUDA=OFF \
        -DKokkos_DIR="$ANARI_INSTALL/kokkos-host/lib/cmake/Kokkos" \
        -DPHOTON_ENABLE_EMBREE=ON -Dembree_DIR="$E"
      log_run photon-cpu-build cmake --build "$B" --parallel "$JOBS"
      log_run photon-cpu-install cmake --install "$B"
      ls -l "$ANARI_INSTALL/photon_cpu/lib/libanari_library_photon.so"
      check_undefined "$ANARI_INSTALL/photon_cpu/lib/libanari_library_photon.so" \
        "$ANARI_INSTALL/lib" "$ANARI_BUILD/blender/bin/lib"
      ;;
    *)
      echo "unknown step: $step" >&2
      exit 2
      ;;
  esac
done
