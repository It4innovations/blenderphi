#!/usr/bin/env bash
# build_moonray.sh [deps] [moonray] [device] [stage] - MoonRay and openmoonray-anari.
#
# Unlike the other components, MoonRay is not built on the exFAT volume: clang's
# `#pragma once` fails there for headers which are reached through differently spelled
# paths (Embree, scene_rdl2, moonray), and the autotools projects of the dependency chain
# depend on file times which exFAT does not keep. The build happens on an APFS volume
# instead, a sparse disk image which is stored inside the workspace:
#
#   ~/scratch/anari/apfs.sparsebundle     the image (created on first use, grows on demand)
#   /Volumes/anari-apfs                   where it is mounted (~/scratch/anari/mnt links to it;
#                                         hdiutil refuses a mount point on the exFAT volume)
#   /Volumes/anari-apfs/moonray/openmoonray-anari   a copy of the source mirror, the build
#                                         directories (build/) and the install prefixes
#                                         (external/installs) of the scripts of that repository
#   /Volumes/anari-apfs/tmp               TMPDIR of the build
#
# The image has to be mounted again after a restart; this script does that
# (`hdiutil attach -nobrowse`), scripts/env.sh has the function `anari_apfs_mount`.
#
# Steps (default: all, in this order):
#   deps      the dependency chain of upstream MoonRay (building/macOS): Boost, TBB, Embree,
#             OpenVDB, OpenImageIO, OpenSubdiv, ISPC, ... -> external/installs
#   moonray   scene_rdl2, mcrt_denoise, moonray and moonshine -> external/installs/openmoonray
#   device    anari_library_moonray and its DSO
#   stage     device and MoonRay runtime -> ~/scratch/anari/install/moonray
#
# Every step runs the scripts of openmoonray-anari (scripts/build-moonray-deps-macos.sh,
# scripts/build-moonray-macos.sh, ...) in the copy on the APFS volume.
set -euo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/env.sh"

anari_apfs_mount
APFS="$ANARI_APFS"
SRC="$APFS/moonray/openmoonray-anari"
mkdir -p "$APFS/tmp" "$APFS/moonray" "$ANARI_WS/cache/home"

# The build gets temporary files on the APFS volume and a home directory of its own.
export TMPDIR="$APFS/tmp"
export HOME="$ANARI_WS/cache/home"
# Tools of the dependency chain (ispc, luac, ...) once they are built.
export PATH="$SRC/external/installs/bin:$PATH"

# The copy follows the mirror. Build results are kept: they are not part of the mirror.
sync_sources() {
  log_run moonray-rsync rsync -a --delete \
    --exclude '.git' --exclude '/build/' --exclude '/external/installs/' \
    --exclude '/external/install/' --exclude '/external/tools/' --exclude '/external/vcpkg/' \
    --exclude '._*' \
    "$ANARI_WS/openmoonray-anari/" "$SRC/"
}

INSTALLS="$SRC/external/installs"          # the dependency chain
MOONRAY="$INSTALLS/openmoonray"            # scene_rdl2, moonray, moonshine, the device DSO
DEVICE_BUILD="$SRC/build/anari-device"
STAGE="$ANARI_INSTALL/moonray"

# The ANARI device, against the ANARI SDK of the workspace. Makefiles like the rest of
# the MoonRay build (see scripts/build-moonray-macos.sh of the repository for why).
build_device() {
  cmake -S "$SRC" -B "$DEVICE_BUILD" -G "Unix Makefiles" -C "$ANARI_WS/scripts/common.cmake" \
    -DCMAKE_OSX_DEPLOYMENT_TARGET= \
    -Danari_DIR="$ANARI_SDK_CMAKE_DIR" \
    -DOPENMOONRAY_ANARI_BUILD_TESTS=OFF -DOPENMOONRAY_ANARI_BUILD_EXAMPLES=OFF \
    -DCMAKE_INSTALL_RPATH="@loader_path" \
    -DCMAKE_INSTALL_PREFIX="$STAGE"
  cmake --build "$DEVICE_BUILD" -j"$JOBS"
}

# stage_fix <file>: make a staged Mach-O file independent of the build location.
#   * the absolute run paths to the build tree are removed (@loader_path and
#     @loader_path/../lib stay, they resolve to lib/ of the staged tree),
#   * absolute install names of libraries of the chain become @rpath names,
#   * the references to TBB follow its new name.
stage_fix() {
  local file="$1" rpath
  otool -l "$file" 2>/dev/null | awk '/LC_RPATH/ {getline; getline; print $2}' |
    while IFS= read -r rpath; do
      case "$rpath" in
        "$APFS"/*) install_name_tool -delete_rpath "$rpath" "$file" 2>/dev/null || true ;;
      esac
    done
  # The autotools libraries of the chain (log4cplus, cppunit, microhttpd) have absolute
  # install names.
  local dep
  otool -L "$file" 2>/dev/null | awk 'NR > 1 {print $1}' |
    while IFS= read -r dep; do
      case "$dep" in
        "$INSTALLS"/lib/*)
          install_name_tool -change "$dep" "@rpath/$(basename "$dep")" "$file" 2>/dev/null || true ;;
      esac
    done
  # TBB under another name, see stage_runtime.
  local name
  for name in tbb tbbmalloc tbbmalloc_proxy; do
    install_name_tool -change "@rpath/lib$name.dylib" "@rpath/lib${name}_moonray.dylib" \
      "$file" 2>/dev/null || true
  done
  case "$(otool -D "$file" 2>/dev/null | tail -n 1)" in
    "$INSTALLS"/lib/* | @rpath/libtbb.dylib | @rpath/libtbbmalloc.dylib | @rpath/libtbbmalloc_proxy.dylib)
      install_name_tool -id "@rpath/$(basename "$file")" "$file" 2>/dev/null || true ;;
  esac
  # install_name_tool invalidates the ad-hoc signature, arm64 needs a valid one.
  codesign -f -s - "$file" >/dev/null 2>&1 || true
}

# The device and the MoonRay runtime in install/moonray, the layout of the Windows
# workspace: lib/ (bin/ on Windows) with the device, the MoonRay libraries and the
# libraries of the dependency chain, and rdl2dso/, coredata/ next to it. The device
# finds rdl2dso/ relative to its own location.
stage_runtime() {
  cmake --install "$DEVICE_BUILD"
  mkdir -p "$STAGE/lib" "$STAGE/bin"
  # `cp -a` keeps the symbolic links of the versioned library names.
  cp -a "$MOONRAY"/lib/*.dylib "$STAGE/lib/"
  cp -a "$INSTALLS"/lib/*.dylib "$STAGE/lib/"
  # MoonRay needs TBB 2020, and its install name is the one of Blender's oneTBB
  # (@rpath/libtbb.dylib). dyld gives a library which asks for an install name that is
  # already loaded the loaded one: inside Blender the device then fails to load with
  # "Symbol not found: tbb::spin_rw_mutex_v3::internal_upgrade ... Expected in:
  # Blender.app/.../libtbb.dylib". The staged TBB therefore gets a name of its own;
  # stage_fix rewrites the references. No other library of the chain shares an install
  # name with a library of Blender.
  local name
  for name in tbb tbbmalloc tbbmalloc_proxy; do
    mv -f "$STAGE/lib/lib$name.dylib" "$STAGE/lib/lib${name}_moonray.dylib"
  done
  cp -a "$MOONRAY/bin/moonray" "$MOONRAY/bin/rdl2_print" "$STAGE/bin/"
  local dir
  for dir in rdl2dso coredata shaders; do
    if [ -d "$MOONRAY/$dir" ]; then
      rsync -a --delete "$MOONRAY/$dir/" "$STAGE/$dir/"
    fi
  done
  local file count=0
  while IFS= read -r file; do
    stage_fix "$file"
    count=$((count + 1))
  done < <(find "$STAGE/lib" "$STAGE/bin" "$STAGE/rdl2dso" -type f \
             \( -name '*.dylib' -o -name '*.so' -o -perm +111 \) ! -name '*.proxy' ! -name '._*')
  echo "staged to $STAGE: $count binaries, $(ls "$STAGE/rdl2dso" | grep -c '\.so$') DSOs"
}

steps=("$@")
[ "${#steps[@]}" = 0 ] && steps=(deps moonray device stage)
sync_sources

for step in "${steps[@]}"; do
  case "$step" in
    deps)
      log_run moonray-deps bash "$SRC/scripts/build-moonray-deps-macos.sh"
      ;;
    moonray)
      log_run moonray-core bash "$SRC/scripts/build-moonray-macos.sh"
      ;;
    device)
      log_run moonray-device build_device
      ;;
    stage)
      log_run moonray-stage stage_runtime
      ;;
    *)
      echo "unknown step: $step" >&2
      exit 2
      ;;
  esac
done
