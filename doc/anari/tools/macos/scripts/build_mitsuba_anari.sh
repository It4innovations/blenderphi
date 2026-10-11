#!/usr/bin/env bash
# build_mitsuba_anari.sh - mitsuba-anari (library name `mitsuba`) with Mitsuba 3 from its
# submodule ext/mitsuba3, variants scalar_rgb, llvm_ad_rgb and metal_ad_rgb.
#
# On Apple the device replaces cuda_ad_rgb by metal_ad_rgb by itself (Dr.Jit's Metal
# back end). Mitsuba and Dr.Jit are built by the configure step of the device, in
# build/mitsuba-anari/_deps; that takes a while the first time. The device, Mitsuba, Dr.Jit
# and a copy of the ANARI front-end library are installed to install/mitsuba/lib.
#
# Runtime notes:
#   * llvm_ad_rgb loads libLLVM.dylib at runtime. The one of scripts/build_llvm_c.sh
#     (C API only, see there for why) is installed next to the device, where Dr.Jit finds
#     it by name before it looks for the LLVM of Homebrew. DRJIT_LIBLLVM_PATH selects
#     another one.
#   * Dr.Jit caches compiled kernels in DRJIT_CACHE_DIR, by default ~/.drjit.
#     scripts/env.sh sets it to cache/drjit.
#
# exFAT: clang's `#pragma once` can misfire on this volume under a parallel build
# ("redefinition" errors). The build is resumed up to MITSUBA_BUILD_TRIES times (3).
#
# Needs: scripts/build_anari_sdk.sh, scripts/fetch_blender_libs.sh (for build_llvm_c.sh).
set -euo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/env.sh"

LLVM_C="$ANARI_INSTALL/llvm-c/lib/libLLVM.dylib"
[ -f "$LLVM_C" ] || bash "$ANARI_WS/scripts/build_llvm_c.sh"

B="$ANARI_BUILD/mitsuba-anari"
PREFIX="$ANARI_INSTALL/mitsuba"
# The Mitsuba build inside the configure step runs `cmake --build --parallel` without a
# number; this bounds it (16 GB of memory).
export CMAKE_BUILD_PARALLEL_LEVEL="${MITSUBA_JOBS:-6}"

tries="${MITSUBA_BUILD_TRIES:-3}"
n=1
until log_run mitsuba-anari-configure cmake -S "$ANARI_WS/mitsuba-anari" -B "$B" "${ANARI_CMAKE_COMMON[@]}" \
  -DCMAKE_INSTALL_PREFIX="$PREFIX" -DCMAKE_PREFIX_PATH="$ANARI_INSTALL" \
  -Danari_DIR="$ANARI_SDK_CMAKE_DIR" \
  -DMITSUBA_ANARI_USE_SYSTEM_ANARI_SDK=ON \
  -DMITSUBA_ANARI_DEP_CONFIGS=Release \
  -DMITSUBA_ANARI_LLVM_RUNTIME="$LLVM_C" \
  -DPython3_EXECUTABLE="$(command -v python3)" \
  -DBUILD_TESTING=OFF; do
  cp "$ANARI_LOGS/mitsuba-anari-configure.log" "$ANARI_LOGS/mitsuba-anari-configure-try$n.log"
  if [ "$n" -ge "$tries" ]; then
    echo "the configure step (which builds Mitsuba) failed $n times" >&2
    exit 1
  fi
  n=$((n + 1))
  echo "resuming the Mitsuba build, try $n"
done
log_run mitsuba-anari-build cmake --build "$B" --parallel "$JOBS"
log_run mitsuba-anari-install cmake --install "$B"
ls -l "$PREFIX/lib" | head -40
