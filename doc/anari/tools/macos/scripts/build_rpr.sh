#!/usr/bin/env bash
# build_rpr.sh - RadeonProRenderANARI (library name `rpr`) with the RadeonProRender SDK,
# whose repository contains the macOS binaries (universal: arm64 and x86_64).
#
# The SDK version is 3.1.6 on macOS, not 3.1.7.1 as on Windows: the tags v3.1.7 and
# v3.1.7.1 of the SDK repository only updated the Windows and Linux binaries. Their
# macOS libraries are still the build of 3.1.6, and they refuse a context which is
# created with the API version of the 3.1.7 headers (RPR_ERROR_INVALID_API_VERSION).
# v3.1.6 is the newest tag whose headers match its macOS libraries.
#
# The SDK is cloned to external/rpr-<tag> if it is not there. Its `hipbin` submodule (the
# precompiled HIP and CUDA kernels) is not needed on macOS: Northstar renders with Metal.
#
# The device and the RPR runtime are installed to install/rpr/lib.
#
# Needs: scripts/build_anari_sdk.sh.
set -euo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/env.sh"

RPR_TAG="${RPR_TAG:-v3.1.6}"
RPR="$ANARI_EXTERNAL/rpr-$RPR_TAG"
if [ ! -f "$RPR/RadeonProRender/inc/RadeonProRender.h" ]; then
  GIT_LFS_SKIP_SMUDGE=1 git clone -q --depth 1 --branch "$RPR_TAG" \
    https://github.com/GPUOpen-LibrariesAndSDKs/RadeonProRenderSDK.git "$RPR"
fi

B="$ANARI_BUILD/rpr-anari"
PREFIX="$ANARI_INSTALL/rpr"
log_run rpr-anari-configure cmake -S "$ANARI_WS/RadeonProRenderANARI" -B "$B" "${ANARI_CMAKE_COMMON[@]}" \
  -DCMAKE_INSTALL_PREFIX="$PREFIX" \
  -Danari_DIR="$ANARI_SDK_CMAKE_DIR" \
  -DRPR_SDK_ROOT="$RPR"
log_run rpr-anari-build cmake --build "$B" --parallel "$JOBS"
log_run rpr-anari-install cmake --install "$B"
ls -l "$PREFIX/lib"
