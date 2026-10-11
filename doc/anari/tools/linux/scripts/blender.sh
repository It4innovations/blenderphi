#!/usr/bin/env bash
# blender.sh [blender arguments] - run the Blender of this workspace.
#
# Blender and the ANARI devices must not write to the home directory, so the process
# gets a home of its own inside the workspace (cache/home):
#   * BLENDER_USER_RESOURCES: Blender's configuration, scripts and extensions,
#   * HOME: what POSIX code uses (~/.drjit of Mitsuba, ~/.nv, ...),
#   * XDG_*, CUDA_CACHE_PATH, OPTIX_CACHE_PATH, TMPDIR: set by scripts/env.sh.
# For tests, also pass --factory-startup.
#
# ANARI_GPU=<index> limits the process to one GPU (CUDA_VISIBLE_DEVICES); the default
# is GPU 0, so that test loops never occupy both GPUs of the node. ANARI_GPU=all lifts it.
set -euo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/env.sh"

export BLENDER_BIN="${BLENDER_BIN:-$ANARI_BUILD/blender/bin/blender}"
ANARI_HOME="$ANARI_CACHE/home"
mkdir -p "$ANARI_HOME" "$ANARI_CACHE/blender-user"
export BLENDER_USER_RESOURCES="$ANARI_CACHE/blender-user"
export HOME="$ANARI_HOME"
if [ "${ANARI_GPU:-0}" != "all" ]; then
  export CUDA_VISIBLE_DEVICES="${ANARI_GPU:-0}"
fi
exec "$BLENDER_BIN" "$@"
