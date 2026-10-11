#!/usr/bin/env bash
# build_barney.sh - barney with its OptiX back end (library name `barney`), the
# default configuration and the one of Windows. The OptiX headers are the ones of
# barney/submodules/optix (9.0); the CUDA architecture is the one of the GPUs of the
# node (ANARI_CUDA_ARCH). MPI is left off.
#
# Needs: scripts/build_anari_sdk.sh. Installs to install/lib.
set -euo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/env.sh"

B="$ANARI_BUILD/barney"
log_run barney-configure cmake -S "$ANARI_WS/barney" -B "$B" "${ANARI_CMAKE_COMMON[@]}" \
  -DCMAKE_INSTALL_PREFIX="$ANARI_INSTALL" -DCMAKE_PREFIX_PATH="$ANARI_INSTALL" \
  -Danari_DIR="$ANARI_SDK_CMAKE_DIR" \
  -DCMAKE_CUDA_ARCHITECTURES="$ANARI_CUDA_ARCH" \
  -DBARNEY_MPI=OFF \
  -DPython3_EXECUTABLE=/usr/bin/python3
log_run barney-build cmake --build "$B" --parallel "$JOBS"
log_run barney-install cmake --install "$B"
ls -l "$ANARI_INSTALL/lib"/libanari_library_barney*.so
check_undefined "$ANARI_INSTALL/lib/libanari_library_barney.so"
