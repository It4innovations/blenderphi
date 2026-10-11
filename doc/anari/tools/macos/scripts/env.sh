# env.sh - common environment of the ANARI workspace on macOS. Source it, do not run it.
#
# Everything lives under the workspace root (the parent directory of scripts/):
#   <repository>/     source mirrors, made by sync_apply.sh
#   build/<name>/     build directories, build/logs, build/test_out
#   install/          install prefixes (same layout as on Windows)
#   external/         downloaded third party releases (OSPRay, RPR SDK, Kokkos, ...)
#   tools/            downloaded build tools (ispc, ...)
#   cache/            pip, XDG and Python byte code caches
#   sync/             manifests and patches received from Windows
#
# Nothing may be written to the home directory: the caches of the tools used here
# are redirected below, and Blender is always run through scripts/blender.sh.

# The physical path: ~/scratch is a symbolic link, and CMake resolves links.
ANARI_WS="$(cd "$(dirname "${BASH_SOURCE[0]:-$0}")/.." && pwd -P)"
export ANARI_WS
export ANARI_BUILD="$ANARI_WS/build"
export ANARI_INSTALL="$ANARI_WS/install"
export ANARI_EXTERNAL="$ANARI_WS/external"
export ANARI_TOOLS="$ANARI_WS/tools"
export ANARI_LOGS="$ANARI_WS/build/logs"
export ANARI_SDK_CMAKE_DIR="$ANARI_INSTALL/lib/cmake/anari-0.17.0"
export BLENDER_LIB_DIR="$ANARI_WS/blenderphi/lib/macos_arm64"

# A non-interactive ssh shell only has the system PATH. Homebrew is used read-only.
export PATH="$ANARI_TOOLS/bin:/opt/homebrew/bin:/usr/bin:/bin:/usr/sbin:/sbin"

# Caches and per-user data, away from the home directory.
export XDG_CACHE_HOME="$ANARI_WS/cache/xdg/cache"
export XDG_CONFIG_HOME="$ANARI_WS/cache/xdg/config"
export XDG_DATA_HOME="$ANARI_WS/cache/xdg/data"
export XDG_STATE_HOME="$ANARI_WS/cache/xdg/state"
export PIP_CACHE_DIR="$ANARI_WS/cache/pip"
export PIP_DISABLE_PIP_VERSION_CHECK=1
export PIP_NO_INPUT=1
export PYTHONUSERBASE="$ANARI_WS/cache/pyuserbase"
export PYTHONNOUSERSITE=1
export PYTHONPYCACHEPREFIX="$ANARI_WS/cache/pycache"
export MPLCONFIGDIR="$ANARI_WS/cache/matplotlib"
# Dr.Jit (Mitsuba): the kernel cache of the LLVM and Metal back ends, by default ~/.drjit.
export DRJIT_CACHE_DIR="$ANARI_WS/cache/drjit"
export GIT_TERMINAL_PROMPT=0
export HOMEBREW_NO_AUTO_UPDATE=1

# 10 cores and 16 GB of memory: moderate defaults. Override with JOBS=<n>.
export JOBS="${JOBS:-8}"

mkdir -p "$ANARI_BUILD" "$ANARI_LOGS" "$ANARI_INSTALL" "$ANARI_EXTERNAL" "$ANARI_TOOLS/bin" \
         "$ANARI_WS/cache"

# CMake arguments shared by all components: an initial cache which switches the user
# package registry (~/.cmake/packages) off in both directions, and Ninja.
ANARI_CMAKE_BASE=(-G Ninja -C "$ANARI_WS/scripts/common.cmake")

# The ANARI SDK and the devices also get one deployment target. Blender is built for
# macOS 13.0 (its own default, build_blender.sh leaves it alone); 13.3 is the first
# version with the floating point std::to_chars which some devices use. Without a
# setting, each project takes its own default (the host version, or 11.2 for Cycles).
ANARI_CMAKE_COMMON=("${ANARI_CMAKE_BASE[@]}" -DCMAKE_OSX_DEPLOYMENT_TARGET=13.3)

# log_run <log name> <command...>: run a command, keep its output in build/logs.
log_run() {
  local name="$1"
  shift
  echo "+ $* (log: $ANARI_LOGS/$name.log)"
  if "$@" >"$ANARI_LOGS/$name.log" 2>&1; then
    return 0
  fi
  echo "FAILED: $*" >&2
  tail -n 40 "$ANARI_LOGS/$name.log" >&2
  return 1
}

# An APFS volume for builds which do not work on exFAT (MoonRay): a sparse disk image
# stored in the workspace. hdiutil does not mount it on a directory of the exFAT volume,
# so it is mounted in /Volumes; <workspace>/mnt is a symbolic link to it.
export ANARI_APFS_IMAGE="$ANARI_WS/apfs.sparsebundle"
export ANARI_APFS="/Volumes/anari-apfs"
anari_apfs_mount() {
  if ! mount | grep -q " on $ANARI_APFS "; then
    if [ ! -d "$ANARI_APFS_IMAGE" ]; then
      hdiutil create -type SPARSEBUNDLE -fs APFS -size 200g -volname anari-apfs \
        "$ANARI_APFS_IMAGE" >/dev/null
    fi
    hdiutil attach -nobrowse "$ANARI_APFS_IMAGE" >/dev/null
  fi
  [ -e "$ANARI_WS/mnt" ] || ln -s "$ANARI_APFS" "$ANARI_WS/mnt"
}

# The exFAT volume stores extended attributes as "._<name>" files. Remove the ones
# left in a directory tree (they confuse globbing build steps and `git status`).
clean_appledouble() {
  find "$@" -name '._*' -type f -delete 2>/dev/null || true
}
