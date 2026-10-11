#!/usr/bin/env bash
# blender.sh [blender arguments] - run the Blender of this workspace.
#
# Blender and the ANARI devices must not write to the home directory, so the process
# gets a home of its own inside the workspace (cache/home):
#   * BLENDER_USER_RESOURCES: Blender's configuration, scripts and extensions,
#   * HOME: what POSIX code uses (~/.drjit of Mitsuba, ~/.cache, ...),
#   * CFFIXED_USER_HOME: what the macOS frameworks use (~/Library/Caches, ...).
# For tests, also pass --factory-startup.
set -euo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/env.sh"

export BLENDER_BIN="${BLENDER_BIN:-$ANARI_BUILD/blender/bin/Blender.app/Contents/MacOS/Blender}"
ANARI_HOME="$ANARI_WS/cache/home"
mkdir -p "$ANARI_HOME" "$ANARI_WS/cache/blender-user"
export BLENDER_USER_RESOURCES="$ANARI_WS/cache/blender-user"
export HOME="$ANARI_HOME"
export CFFIXED_USER_HOME="$ANARI_HOME"
exec "$BLENDER_BIN" "$@"
