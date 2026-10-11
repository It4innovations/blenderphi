#!/usr/bin/env bash
# build_rpr.sh - RadeonProRenderANARI (library name `rpr`) with the RadeonProRender SDK
# 3.1.7.1, the version of Windows. The SDK repository contains the Linux binaries
# (RadeonProRender/binUbuntu20) and, in its `hipbin` submodule, the precompiled
# Northstar kernels (*.hipbin for AMD, *.cudabin for NVIDIA GPUs). It is cloned to
# external/rpr if it is not there.
#
# On NVIDIA GPUs Northstar renders with CUDA and loads the CUDA 13 run time
# (libcudart.so.13). The node has it in /usr/local/cuda-13.3/lib64; it is linked next
# to the device, whose directory Northstar searches (rpath $ORIGIN).
#
# The device, the RPR run time and the kernels are installed to install/rpr/lib; RPR
# writes its kernel cache to install/rpr/lib/cache.
#
# Needs: scripts/build_anari_sdk.sh.
set -euo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/env.sh"

RPR_TAG="${RPR_TAG:-v3.1.7.1}"
RPR="$ANARI_EXTERNAL/rpr"
if [ ! -f "$RPR/RadeonProRender/inc/RadeonProRender.h" ]; then
  log_run rpr-sdk-clone git clone -q --depth 1 --branch "$RPR_TAG" \
    https://github.com/GPUOpen-LibrariesAndSDKs/RadeonProRenderSDK.git "$RPR"
fi
if [ -z "$(ls "$RPR/hipbin" 2>/dev/null)" ]; then
  log_run rpr-sdk-hipbin git -C "$RPR" submodule update --init --depth 1 hipbin
fi

B="$ANARI_BUILD/rpr-anari"
PREFIX="$ANARI_INSTALL/rpr"
log_run rpr-anari-configure cmake -S "$ANARI_WS/RadeonProRenderANARI" -B "$B" "${ANARI_CMAKE_COMMON[@]}" \
  -DCMAKE_INSTALL_PREFIX="$PREFIX" \
  -Danari_DIR="$ANARI_SDK_CMAKE_DIR" \
  -DRPR_SDK_ROOT="$RPR"
log_run rpr-anari-build cmake --build "$B" --parallel "$JOBS"
log_run rpr-anari-install cmake --install "$B"

# The CUDA 13 run time for Northstar.
cudart="$(ls "$ANARI_CUDA_ROOT"/lib64/libcudart.so.13 /usr/local/cuda-13*/lib64/libcudart.so.13 2>/dev/null | head -n 1 || true)"
if [ -n "$cudart" ]; then
  ln -sfn "$cudart" "$PREFIX/lib/libcudart.so.13"
else
  echo "libcudart.so.13 not found: Northstar will not render on NVIDIA GPUs"
fi
ls -l "$PREFIX/lib" | head -n 20
check_undefined "$PREFIX/lib/libanari_library_rpr.so" "$PREFIX/lib"
