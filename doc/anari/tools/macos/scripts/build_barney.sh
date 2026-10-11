#!/usr/bin/env bash
# build_barney.sh - barney with its CPU (Embree) back end, library name `barney`.
# There is no CUDA / OptiX on macOS, so only the CPU back end is built. MPI is left off.
#
# Needs: scripts/build_anari_sdk.sh. Installs to install/lib.
set -euo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/env.sh"

B="$ANARI_BUILD/barney"
log_run barney-configure cmake -S "$ANARI_WS/barney" -B "$B" "${ANARI_CMAKE_COMMON[@]}" \
  -DCMAKE_INSTALL_PREFIX="$ANARI_INSTALL" -DCMAKE_PREFIX_PATH="$ANARI_INSTALL" \
  -Danari_DIR="$ANARI_SDK_CMAKE_DIR" \
  -DBARNEY_BACKEND_CPU=ON -DBARNEY_BACKEND_OPTIX=OFF -DBARNEY_BACKEND_CUDA=OFF \
  -DBARNEY_MPI=OFF \
  -DPython3_EXECUTABLE="$(command -v python3)"
log_run barney-build cmake --build "$B" --parallel "$JOBS"
log_run barney-install cmake --install "$B"
ls -l "$ANARI_INSTALL/lib"/libanari_library_barney*.dylib
