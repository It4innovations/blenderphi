# env.sh - common environment of the ANARI workspace on Linux. Source it, do not run it.
#
# Everything lives under the workspace root (the parent directory of scripts/):
#   <repository>/     source mirrors, made by sync_apply.sh
#   build/<name>/     build directories, build/logs, build/test_out
#   install/          install prefixes (same layout as on Windows, libraries in lib/)
#   external/         downloaded third party releases (OSPRay, RPR SDK, Kokkos, ...)
#   tools/            downloaded build tools (cmake, ninja, git-lfs, ...), see fetch_tools.sh
#   cache/            pip, XDG, Python, CUDA and OptiX caches, tmp/, home/ for test runs
#   sync/             manifests and patches received from Windows
#
# Nothing may be written to the home directory or installed system wide: the caches
# of the tools used here are redirected below, and Blender and the samples are always
# run through scripts/blender.sh and scripts/run_pynari.sh, which also replace HOME.

# The physical path: CMake resolves symbolic links.
ANARI_WS="$(cd "$(dirname "${BASH_SOURCE[0]:-$0}")/.." && pwd -P)"
export ANARI_WS
export ANARI_BUILD="$ANARI_WS/build"
export ANARI_INSTALL="$ANARI_WS/install"
export ANARI_EXTERNAL="$ANARI_WS/external"
export ANARI_TOOLS="$ANARI_WS/tools"
export ANARI_LOGS="$ANARI_WS/build/logs"
export ANARI_CACHE="$ANARI_WS/cache"
export ANARI_SDK_CMAKE_DIR="$ANARI_INSTALL/lib/cmake/anari-0.17.0"
export BLENDER_LIB_DIR="$ANARI_WS/blenderphi/lib/linux_x64"
# The OptiX 9.0 headers, as on Windows (barney, VisRTX, cyclesphi-anari, Photon).
export ANARI_OPTIX_DIR="$ANARI_WS/barney/submodules/optix"

# Caches and per-user data, away from the home directory. Set before the environment
# modules are loaded, so that Lmod has no reason to write a cache to the home either.
export XDG_CACHE_HOME="$ANARI_CACHE/xdg/cache"
export XDG_CONFIG_HOME="$ANARI_CACHE/xdg/config"
export XDG_DATA_HOME="$ANARI_CACHE/xdg/data"
export XDG_STATE_HOME="$ANARI_CACHE/xdg/state"
export PIP_CACHE_DIR="$ANARI_CACHE/pip"
export PIP_DISABLE_PIP_VERSION_CHECK=1
export PIP_NO_INPUT=1
export PYTHONUSERBASE="$ANARI_CACHE/pyuserbase"
export PYTHONNOUSERSITE=1
export PYTHONPYCACHEPREFIX="$ANARI_CACHE/pycache"
export MPLCONFIGDIR="$ANARI_CACHE/matplotlib"
export GIT_TERMINAL_PROMPT=0
# The Git LFS filters, so that they do not depend on `git lfs install` in ~/.gitconfig.
export GIT_CONFIG_COUNT=4
export GIT_CONFIG_KEY_0=filter.lfs.clean GIT_CONFIG_VALUE_0='git-lfs clean -- %f'
export GIT_CONFIG_KEY_1=filter.lfs.smudge GIT_CONFIG_VALUE_1='git-lfs smudge -- %f'
export GIT_CONFIG_KEY_2=filter.lfs.process GIT_CONFIG_VALUE_2='git-lfs filter-process'
export GIT_CONFIG_KEY_3=filter.lfs.required GIT_CONFIG_VALUE_3=true
# Temporary files of compilers, pip and Blender (the default is /tmp).
export TMPDIR="$ANARI_CACHE/tmp"
# NVIDIA: the JIT cache of CUDA (default ~/.nv/ComputeCache), the OptiX program cache
# (default /var/tmp/OptixCache_<user>) and the GL shader cache (default ~/.cache/nvidia).
export CUDA_CACHE_PATH="$ANARI_CACHE/nv/ComputeCache"
export OPTIX_CACHE_PATH="$ANARI_CACHE/optix"
export __GL_SHADER_DISK_CACHE_PATH="$ANARI_CACHE/nv/GLCache"
# Lmod only writes its spider cache (~/.cache/lmod) when building it took this long.
export LMOD_SHORT_TIME=86400

mkdir -p "$ANARI_BUILD" "$ANARI_LOGS" "$ANARI_INSTALL" "$ANARI_EXTERNAL" "$ANARI_TOOLS/bin" \
         "$ANARI_CACHE" "$TMPDIR" "$CUDA_CACHE_PATH" "$OPTIX_CACHE_PATH" \
         "$__GL_SHADER_DISK_CACHE_PATH"

# No session services. A program which opens an audio device (Blender's OpenAL did,
# even with -noaudio) connects to the PipeWire socket of the login session in
# XDG_RUNTIME_DIR; systemd then starts pipewire, wireplumber, pipewire-pulse and the
# D-Bus broker for the user, and wireplumber writes ~/.local/state/wireplumber. With a
# runtime directory of its own and no session bus address, nothing started from this
# environment can reach those sockets.
export XDG_RUNTIME_DIR="$ANARI_CACHE/xdg/runtime"
mkdir -p "$XDG_RUNTIME_DIR" && chmod 700 "$XDG_RUNTIME_DIR"
unset DBUS_SESSION_BUS_ADDRESS PIPEWIRE_RUNTIME_DIR PULSE_SERVER PULSE_RUNTIME_PATH
export ALSOFT_DRIVERS=null
export SDL_AUDIODRIVER=dummy

# Environment modules. A non-interactive shell does not have `module`; /etc/profile
# defines it. ANARI_MODULES is the list to load (space separated); by default none is
# needed, see the CUDA toolkit below.
ANARI_MODULES="${ANARI_MODULES:-}"
# /etc/profile and Lmod do not run with `set -eu`. The flags are read from $-: a
# `$(set +o)` would report errexit as off, because a command substitution clears it.
_anari_restore=""
case $- in *e*) _anari_restore="$_anari_restore -e" ;; esac
case $- in *u*) _anari_restore="$_anari_restore -u" ;; esac
set +eu
if ! type module >/dev/null 2>&1; then
  source /etc/profile >/dev/null 2>&1
fi
if [ -n "$ANARI_MODULES" ]; then
  # shellcheck disable=SC2086
  module load $ANARI_MODULES || echo "env.sh: 'module load $ANARI_MODULES' failed" >&2
fi
# shellcheck disable=SC2086
[ -z "$_anari_restore" ] || set $_anari_restore
unset _anari_restore

# The CUDA toolkit. The one installed on the node itself (/usr/local/cuda-13.3, from
# NVIDIA's RPMs) is used; the CUDA 12 environment module of the node could not be
# used when this was set up. Override with ANARI_CUDA_ROOT=<toolkit>, or with
# ANARI_MODULES=CUDA/<version> (EBROOTCUDA is used).
if [ -n "$ANARI_MODULES" ] && [ -n "${EBROOTCUDA:-}" ] && [ -x "$EBROOTCUDA/bin/nvcc" ]; then
  ANARI_CUDA_ROOT="${ANARI_CUDA_ROOT:-$EBROOTCUDA}"
fi
export ANARI_CUDA_ROOT="${ANARI_CUDA_ROOT:-/usr/local/cuda-13.3}"
export CUDA_HOME="$ANARI_CUDA_ROOT" CUDA_PATH="$ANARI_CUDA_ROOT"
export CUDAToolkit_ROOT="$ANARI_CUDA_ROOT"
export CUDA_TOOLKIT_ROOT_DIR="$ANARI_CUDA_ROOT"
export CUDACXX="$ANARI_CUDA_ROOT/bin/nvcc"
export PATH="$ANARI_CUDA_ROOT/bin:$PATH"
# The GPUs of the node are RTX PRO 6000 Blackwell: compute capability 12.0
# (sm_120, known to nvcc since CUDA 12.8).
export ANARI_CUDA_ARCH="${ANARI_CUDA_ARCH:-120}"

# Tools of the workspace first. $HOME/git-lfs/bin on the default PATH of this account
# holds an aarch64 git-lfs (the home is shared with an Arm cluster); tools/bin has the
# x86_64 one and must come before it.
export PATH="$ANARI_TOOLS/bin:$PATH"
export CC="${CC:-/usr/bin/gcc}"
export CXX="${CXX:-/usr/bin/g++}"

# A shared machine with 64 cores: moderate default. Override with JOBS=<n>.
export JOBS="${JOBS:-32}"
ulimit -n 8192 2>/dev/null || true
# No core files: the node hands them to systemd-coredump, outside of the workspace.
ulimit -c 0 2>/dev/null || true

# CMake arguments shared by all components: an initial cache which switches the user
# package registry (~/.cmake/packages) off in both directions, and Ninja.
ANARI_CMAKE_BASE=(-G Ninja -C "$ANARI_WS/scripts/common.cmake")
ANARI_CMAKE_COMMON=("${ANARI_CMAKE_BASE[@]}")

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

# Mitsuba's llvm_ad_rgb variant: Dr.Jit loads the LLVM of the system.
if [ -z "${DRJIT_LIBLLVM_PATH:-}" ]; then
  for _anari_llvm in /usr/lib64/libLLVM.so.[0-9]*; do
    [ -e "$_anari_llvm" ] && export DRJIT_LIBLLVM_PATH="$_anari_llvm"
  done
  unset _anari_llvm
fi

# embree_blender_package: writes a CMake package for the Embree of Blender's precompiled
# libraries (lib/linux_x64/embree only holds the headers and libembree4.so) to
# build/embree-blender and prints that directory. Photon and MoonRay are built against
# it (-Dembree_DIR / -DEmbree_DIR), so that they use the Embree which Blender loads.
embree_blender_package() {
  local dir="$ANARI_BUILD/embree-blender" header
  # The include directory: Blender's headers plus, when build_moonray_deps.sh has
  # fetched them, the ISPC headers (*.isph) of the same Embree version, which Blender's
  # build does not install and MoonRay's ISPC code includes.
  mkdir -p "$dir/include/embree4"
  for header in "$BLENDER_LIB_DIR"/embree/include/embree4/*.h                 "$ANARI_EXTERNAL"/moonray-deps/embree-4.4.1/include/embree4/*.isph; do
    [ -e "$header" ] && ln -sfn "$header" "$dir/include/embree4/$(basename "$header")"
  done
  cat >"$dir/embree-config.cmake" <<CONFIG
# Written by scripts/env.sh: the Embree of Blender's precompiled libraries.
set(EMBREE_INCLUDE_DIRS "$dir/include")
set(EMBREE_LIBRARY "$BLENDER_LIB_DIR/embree/lib/libembree4.so")
set(EMBREE_LIBRARIES "\${EMBREE_LIBRARY}")
if(NOT TARGET embree)
  add_library(embree SHARED IMPORTED GLOBAL)
  set_target_properties(embree PROPERTIES
    IMPORTED_LOCATION "\${EMBREE_LIBRARY}"
    IMPORTED_SONAME "libembree4.so.4"
    INTERFACE_INCLUDE_DIRECTORIES "\${EMBREE_INCLUDE_DIRS}")
endif()
CONFIG
  cat >"$dir/embree-config-version.cmake" <<'CONFIG'
set(PACKAGE_VERSION "4.4.1")
if(NOT PACKAGE_FIND_VERSION OR
   (PACKAGE_FIND_VERSION_MAJOR EQUAL 4 AND NOT PACKAGE_FIND_VERSION VERSION_GREATER PACKAGE_VERSION))
  set(PACKAGE_VERSION_COMPATIBLE TRUE)
endif()
CONFIG
  echo "$dir"
}

# check_undefined <shared library> [<library directory> ...]: a device library with
# unresolved symbols only fails when it is loaded, so look for them after the install.
check_undefined() {
  local lib="$1" out
  shift
  out="$(LD_LIBRARY_PATH="$(IFS=:; echo "$*")${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" ldd -r "$lib" 2>&1 |
    grep -E "undefined symbol|not found" | head -n 8 || true)"
  if [ -n "$out" ]; then
    echo "$lib has unresolved dependencies:" >&2
    echo "$out" >&2
    return 1
  fi
}
