#!/usr/bin/env bash
# build_blender.sh - Blender (blenderphi, branch main-anari) with the ANARI Cycles device.
#
# Needs: scripts/build_anari_sdk.sh and scripts/fetch_blender_libs.sh.
# Result: build/blender/bin/blender (run it through scripts/blender.sh).
#
# The node has no display and no X11 / Wayland development packages, so Blender is
# built headless (WITH_HEADLESS=ON): background rendering and Python only, no window.
# BLENDER_HEADLESS=OFF builds the windowed Blender on a machine which has the headers.
#
# The OpenGL back end is switched off as well: a headless Blender creates its OpenGL
# context with EGL, and the EGL headers (EGL/eglplatform.h, KHR/khrplatform.h; the
# package mesa-libEGL-devel / libglvnd-devel) are not installed on the node. The
# Vulkan back end is built from Blender's precompiled libraries and stays on.
# BLENDER_OPENGL=ON builds the OpenGL back end where the headers exist.
#
# OpenAL is switched off too (BLENDER_OPENAL=ON keeps it). It is the only audio device
# back end the precompiled libraries bring, and it probes the audio servers of the
# machine when Blender starts, also with -noaudio: that started the PipeWire services
# of the user and wrote ~/.local/state/wireplumber. Audaspace itself stays (sound in
# rendered videos is mixed without a device).
#
# Usage: build_blender.sh [configure|build|install]...   (default: all three)
#   After C++ changes:               build_blender.sh build install
#   After add-on (.py) changes only: build_blender.sh install
# On Linux `install` copies the scripts and the bundled shared libraries (also
# libanari.so.0) next to the executable, into build/blender/bin: Blender does not run
# without it.
set -euo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/env.sh"

B="$ANARI_BUILD/blender"
BLENDER_JOBS="${BLENDER_JOBS:-$JOBS}"
steps=("$@")
[ "${#steps[@]}" = 0 ] && steps=(configure build install)

for step in "${steps[@]}"; do
  case "$step" in
    configure)
      # The options of the Windows build (README 4.11), plus the headless switch.
      # Blender's own Cycles is the reference renderer of the smoke tests and runs on
      # the CPU there, so its CUDA / OptiX kernels are not built.
      log_run blender-configure cmake -S "$ANARI_WS/blenderphi" -B "$B" "${ANARI_CMAKE_BASE[@]}" \
        -DWITH_HEADLESS="${BLENDER_HEADLESS:-ON}" \
        -DWITH_OPENGL_BACKEND="${BLENDER_OPENGL:-OFF}" \
        -DWITH_OPENAL="${BLENDER_OPENAL:-OFF}" \
        -DWITH_CYCLES_DEVICE_ANARI=ON \
        -Danari_DIR="$ANARI_SDK_CMAKE_DIR" \
        -DWITH_GTESTS=OFF \
        -DWITH_CYCLES_CUDA_BINARIES=OFF \
        -DWITH_CYCLES_DEVICE_HIP=OFF -DWITH_CYCLES_DEVICE_ONEAPI=OFF \
        -DNINJA_MAX_NUM_PARALLEL_COMPILE_HEAVY_JOBS=8 \
        -DNINJA_MAX_NUM_PARALLEL_LINK_JOBS=2
      ;;
    build)
      log_run blender-build cmake --build "$B" --parallel "$BLENDER_JOBS"
      ;;
    install)
      log_run blender-install cmake --install "$B"
      ;;
    *)
      echo "unknown step: $step" >&2
      exit 2
      ;;
  esac
done
ls -l "$B/bin/blender" 2>/dev/null || true
