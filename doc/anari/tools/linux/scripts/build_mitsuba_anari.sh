#!/usr/bin/env bash
# build_mitsuba_anari.sh - mitsuba-anari (library name `mitsuba`) with Mitsuba 3 and
# Dr.Jit built from the ext/mitsuba3 submodule, variants scalar_rgb, llvm_ad_rgb and
# cuda_ad_rgb (the default of the device), against the ANARI SDK of the workspace.
#
# Mitsuba is built while the device is *configured* (cmake/Dependencies.cmake runs its
# build with all cores), so the configure step takes most of the time (about half an
# hour) and is confined to JOBS cores with taskset.
#
# -DMITSUBA_ANARI_MI_EXTRA_CXX_FLAGS=-march=x86-64-v2: the GCC of Rocky Linux 10
# compiles for x86-64-v3 by default, with which the Embree inside the Mitsuba build
# does not link (as scripts/build-cs-p12.sh of the repository notes).
#
# Dr.Jit needs no CUDA toolkit to build: cuda_ad_rgb talks to the driver and to OptiX
# at run time. llvm_ad_rgb loads the LLVM of the system (DRJIT_LIBLLVM_PATH, set by
# scripts/env.sh when /usr/lib64/libLLVM.so.* exists).
#
# The device and the Mitsuba run time are installed to install/mitsuba/lib.
#
# Needs: scripts/build_anari_sdk.sh.
set -euo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/env.sh"

B="$ANARI_BUILD/mitsuba-anari"
PREFIX="$ANARI_INSTALL/mitsuba"
log_run mitsuba-anari-configure taskset -c "0-$((JOBS - 1))" \
  cmake -S "$ANARI_WS/mitsuba-anari" -B "$B" "${ANARI_CMAKE_COMMON[@]}" \
  -DCMAKE_INSTALL_PREFIX="$PREFIX" \
  -DCMAKE_PREFIX_PATH="$ANARI_INSTALL" -Danari_DIR="$ANARI_SDK_CMAKE_DIR" \
  -DMITSUBA_ANARI_USE_SYSTEM_ANARI_SDK=ON \
  -DMITSUBA_ANARI_DEP_CONFIGS=Release \
  -DMITSUBA_ANARI_MI_EXTRA_CXX_FLAGS=-march=x86-64-v2 \
  -DMITSUBA_ANARI_BUILD_TESTS=OFF -DMITSUBA_ANARI_BUILD_EXAMPLES=OFF -DBUILD_TESTING=OFF \
  -DPython3_EXECUTABLE=/usr/bin/python3
log_run mitsuba-anari-build cmake --build "$B" --parallel "$JOBS"
log_run mitsuba-anari-install cmake --install "$B"
ls -l "$PREFIX/lib"/libanari_library_mitsuba.so
check_undefined "$PREFIX/lib/libanari_library_mitsuba.so" "$PREFIX/lib" "$ANARI_INSTALL/lib"
