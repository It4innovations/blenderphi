#!/usr/bin/env bash
# build_anari_ospray.sh - anari-ospray (library name `ospray`) with the OSPRay 3.2.0
# binary release for macOS arm64, which brings Embree, Open VKL, Open Image Denoise and
# TBB with it. The release is downloaded to external/ospray if it is not there.
#
# The device and the OSPRay runtime are installed to their own directory,
# install/ospray/lib, like on Windows: Blender ships libraries of the same kind.
#
# Needs: scripts/build_anari_sdk.sh.
set -euo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/env.sh"

VERSION=3.2.0
NAME="ospray-$VERSION.arm64.macosx"
OSPRAY="$ANARI_EXTERNAL/ospray/$NAME"
if [ ! -d "$OSPRAY/lib" ]; then
  mkdir -p "$ANARI_EXTERNAL/ospray"
  (
    cd "$ANARI_EXTERNAL/ospray"
    curl -sS -L --fail -o "$NAME.zip" \
      "https://github.com/RenderKit/ospray/releases/download/v$VERSION/$NAME.zip"
    unzip -q -o "$NAME.zip"
  )
fi

B="$ANARI_BUILD/anari-ospray"
PREFIX="$ANARI_INSTALL/ospray"
log_run anari-ospray-configure cmake -S "$ANARI_WS/anari-ospray" -B "$B" "${ANARI_CMAKE_COMMON[@]}" \
  -DCMAKE_INSTALL_PREFIX="$PREFIX" \
  -Danari_DIR="$ANARI_SDK_CMAKE_DIR" \
  -Dospray_DIR="$OSPRAY/lib/cmake/ospray-$VERSION" -DCMAKE_PREFIX_PATH="$OSPRAY" \
  -DPython3_EXECUTABLE="$(command -v python3)"
log_run anari-ospray-build cmake --build "$B" --parallel "$JOBS"
log_run anari-ospray-install cmake --install "$B"

# The OSPRay runtime next to the device (found through its rpath @loader_path). The
# test, example and GUI libraries of the release are not needed. `cp -a` keeps the
# version symbolic links.
for lib in "$OSPRAY"/lib/*.dylib; do
  case "$(basename "$lib")" in
    *testing*|*imgui*|*mpi*) continue ;;
  esac
  cp -a "$lib" "$PREFIX/lib/"
done
ls -l "$PREFIX/lib/libanari_library_ospray.dylib"
