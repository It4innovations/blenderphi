#!/usr/bin/env bash
# build_moonray.sh [scene_rdl2] [mcrt_denoise] [moonray] [moonshine] [anari_dso] [device]
#
# MoonRay (the forks under openmoonray-anari/external/openmoonray) and the ANARI device
# openmoonray-anari (library name `moonray`). Without arguments all steps are done, in
# this order; each one installs into install/moonray:
#
#   scene_rdl2    the scene description library
#   mcrt_denoise  the denoiser (Open Image Denoise)
#   moonray       the renderer, its command line tools and the shader DSOs it ships
#   moonshine     the shader library: the Dwa materials and the pattern maps
#   anari_dso     the RDL2 DSOs of the device repository (dso/: AnariBumpNormalMap)
#   device        libanari_library_moonray.so
#
# As on Windows and macOS the projects are built standalone, not through the
# openmoonray superproject (which also wants arras, hydra and USD), with Unix Makefiles
# (the generator of upstream's Linux build; Ninja's ISPC support does not work with
# these projects on POSIX, see cmake_modules/cmake/OMR_Platform.cmake).
#
# Where the libraries come from:
#   * Blender's precompiled libraries (blenderphi/lib/linux_x64): TBB, Embree, OpenVDB,
#     OpenEXR, Imath, OpenImageIO, OpenSubdiv, Open Image Denoise, jpeg. MoonRay is
#     built against exactly the copies which Blender loads: inside Blender a shared
#     library whose soname is already loaded is not loaded a second time, so MoonRay
#     gets Blender's copy in any case, and it has to be built for it. Outside of
#     Blender these libraries are found in build/blender/bin/lib.
#   * install/moonray-deps (scripts/build_moonray_deps.sh): Boost, JsonCpp, Log4cplus,
#     CppUnit, Random123, and ISPC in tools/. Blender has none of them.
#   * the system: Lua 5.4 (lua-devel, luac), OpenSSL, zlib, flex, bison, libatomic.
#
# MOONRAY_XPU=ON builds MoonRay with its GPU path (XPU: CUDA + OptiX) as a second,
# complete set: build trees build/moonray-xpu-*, installed to install/moonray_xpu. It
# uses the OptiX 8.1 headers, the version of the Windows build (the 9.0 headers of
# barney/submodules/optix were not tried); they are cloned from
# github.com/NVIDIA/optix-dev to external/optix-8.1.
#
# Needs: scripts/build_anari_sdk.sh, scripts/fetch_blender_libs.sh,
# scripts/build_moonray_deps.sh; for the `device` step nothing else.
set -euo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/env.sh"

SRC="$ANARI_WS/openmoonray-anari"
OMR="$SRC/external/openmoonray"
XPU="${MOONRAY_XPU:-OFF}"
if [ "$XPU" = ON ]; then
  VARIANT="-xpu"
  PREFIX="$ANARI_INSTALL/moonray_xpu"
  OPTIX81="$ANARI_EXTERNAL/optix-8.1"
  if [ ! -f "$OPTIX81/include/optix.h" ]; then
    log_run moonray-optix-headers git clone -q --depth 1 --branch v8.1.0       https://github.com/NVIDIA/optix-dev.git "$OPTIX81"
  fi
else
  VARIANT=""
  PREFIX="$ANARI_INSTALL/moonray"
fi
DEPS="$ANARI_INSTALL/moonray-deps"
BL="$BLENDER_LIB_DIR"
ISPC_BIN="$ANARI_TOOLS/bin/ispc"

steps=("$@")
[ "${#steps[@]}" = 0 ] && steps=(scene_rdl2 mcrt_denoise moonray moonshine anari_dso device)

export CMAKE_POLICY_VERSION_MINIMUM=3.5
# Hints of MoonRay's own find modules (cmake_modules/cmake/Find*.cmake).
export Boost_ROOT="$DEPS" CppUnit_ROOT="$DEPS" JsonCpp_ROOT="$DEPS" Log4cplus_ROOT="$DEPS"
export Random123_ROOT="$DEPS"
export OpenSubDiv_ROOT="$BL/opensubdiv" OpenVDB_ROOT="$BL/openvdb" TBB_ROOT="$BL/tbb"
export ISPC="$ISPC_BIN"
export OPTIX_ROOT="${OPTIX81:-$ANARI_OPTIX_DIR}"
# MoonrayDso.cmake finds the ispc_dso_generate helper through this variable in
# standalone builds.
export CMAKE_MODULES_ROOT="$OMR/cmake_modules"
# Code generators run while building (rdl2_json_exporter, ...): the installed tools and
# the libraries they need.
export PATH="$PREFIX/bin:$PATH"
export LD_LIBRARY_PATH="$PREFIX/lib:$DEPS/lib:$ANARI_BUILD/blender/bin/lib:$BL/tbb/lib:$BL/embree/lib:$BL/openvdb/lib:$BL/openexr/lib:$BL/imath/lib:$BL/openimageio/lib:$BL/opencolorio/lib:$BL/opensubdiv/lib:$BL/openimagedenoise/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

EMBREE_PACKAGE="$(embree_blender_package)"
PREFIX_PATH="$PREFIX;$DEPS;$BL/tbb;$BL/openexr;$BL/imath;$BL/openimageio;$BL/opencolorio;$BL/openimagedenoise;$BL/jpeg;$BL/png;$BL/webp;$BL/openjpeg;$BL/openjph;$BL/fmt;$BL/zstd"

common=(-G "Unix Makefiles" -C "$ANARI_WS/scripts/common.cmake"
  -DCMAKE_INSTALL_PREFIX="$PREFIX"
  -DCMAKE_PREFIX_PATH="$PREFIX_PATH"
  -DCMAKE_MODULE_PATH="$OMR/cmake_modules/cmake"
  -DCMAKE_ISPC_COMPILER="$ISPC_BIN"
  -DEmbree_DIR="$EMBREE_PACKAGE" -Dembree_DIR="$EMBREE_PACKAGE"
  -DPython_EXECUTABLE=/usr/bin/python3
  -DBUILD_TESTING=NO)
gpu=(-DMOONRAY_USE_OPTIX="$XPU")
if [ "$XPU" = ON ]; then
  gpu+=(-DCMAKE_CUDA_ARCHITECTURES="$ANARI_CUDA_ARCH")
fi

# project <name> <source directory> <cmake arguments...>
project() {
  local name="$1" source="$2"
  shift 2
  local B="$ANARI_BUILD/moonray$VARIANT-$name"
  log_run "moonray$VARIANT-$name-configure" cmake -S "$source" -B "$B" "${common[@]}" "$@"
  log_run "moonray$VARIANT-$name-build" cmake --build "$B" --parallel "$JOBS"
  log_run "moonray$VARIANT-$name-install" cmake --install "$B"
}

for step in "${steps[@]}"; do
  case "$step" in
    scene_rdl2)
      project scene_rdl2 "$OMR/moonray/scene_rdl2" \
        -DSCENERDL2_BUILD_TESTING=NO -DSCENERDL2_BUILD_PYTHON_BINDINGS=NO
      ;;
    mcrt_denoise)
      # Always the CPU denoiser (Open Image Denoise), as on Windows: the OptiX denoiser
      # of mcrt_denoise is written for OptiX 7 (OptixDenoiserParams::denoiseAlpha).
      project mcrt_denoise "$OMR/moonray/mcrt_denoise"         -DMCRTDENOISE_BUILD_TESTING=NO -DMOONRAY_USE_OPTIX=NO
      ;;
    moonray)
      project moonray "$OMR/moonray/moonray" \
        -DMOONRAY_BUILD_TESTING=NO "${gpu[@]}"
      ;;
    moonshine)
      project moonshine "$OMR/moonray/moonshine" \
        -DMOONSHINE_BUILD_TESTING=NO "${gpu[@]}"
      ;;
    anari_dso)
      project anari-dso "$SRC/dso" "${gpu[@]}"
      ;;
    device)
      B="$ANARI_BUILD/openmoonray-anari$VARIANT"
      log_run "openmoonray-anari$VARIANT-configure" cmake -S "$SRC" -B "$B" "${common[@]}" \
        -Danari_DIR="$ANARI_SDK_CMAKE_DIR" \
        -DOPENMOONRAY_ANARI_BUILD_TESTS=OFF -DOPENMOONRAY_ANARI_BUILD_EXAMPLES=OFF \
        -DCMAKE_INSTALL_RPATH='$ORIGIN' -DCMAKE_INSTALL_RPATH_USE_LINK_PATH=ON \
        "${gpu[@]}"
      log_run "openmoonray-anari$VARIANT-build" cmake --build "$B" --parallel "$JOBS"
      log_run "openmoonray-anari$VARIANT-install" cmake --install "$B"
      ls -l "$PREFIX/lib/libanari_library_moonray.so"
      check_undefined "$PREFIX/lib/libanari_library_moonray.so" \
        "$PREFIX/lib" "$DEPS/lib" "$ANARI_BUILD/blender/bin/lib" "$ANARI_INSTALL/lib"
      ;;
    *)
      echo "unknown step: $step" >&2
      exit 2
      ;;
  esac
done
