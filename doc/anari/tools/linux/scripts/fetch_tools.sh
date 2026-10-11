#!/usr/bin/env bash
# fetch_tools.sh - download the build tools which the machine does not have into tools/.
#
#   cmake    official binary release            tools/cmake-<version>-linux-x86_64
#   ninja    official binary release            tools/bin/ninja
#   git-lfs  official binary release (x86_64)   tools/bin/git-lfs
#
# tools/bin is first on the PATH of scripts/env.sh. Nothing is installed: the archives
# are unpacked, and `git lfs install` (which would edit ~/.gitconfig) is not run:
# env.sh passes the LFS filter settings to git through the environment.
#
# Re-running skips what is already there. Downloads are kept in tools/dl.
set -euo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/env.sh"

CMAKE_VERSION="${CMAKE_VERSION:-4.2.1}"     # the version used on Windows
NINJA_VERSION="${NINJA_VERSION:-1.12.1}"
GIT_LFS_VERSION="${GIT_LFS_VERSION:-3.7.1}"

DL="$ANARI_TOOLS/dl"
mkdir -p "$DL" "$ANARI_TOOLS/bin"

download() {   # download <url> <file>
  [ -s "$2" ] && return 0
  echo "downloading $1"
  curl -sS -L --fail -o "$2.part" "$1"
  mv "$2.part" "$2"
}

# CMake
CMAKE_DIR="$ANARI_TOOLS/cmake-$CMAKE_VERSION-linux-x86_64"
if [ ! -x "$CMAKE_DIR/bin/cmake" ]; then
  download "https://github.com/Kitware/CMake/releases/download/v$CMAKE_VERSION/cmake-$CMAKE_VERSION-linux-x86_64.tar.gz" \
    "$DL/cmake-$CMAKE_VERSION-linux-x86_64.tar.gz"
  tar -xzf "$DL/cmake-$CMAKE_VERSION-linux-x86_64.tar.gz" -C "$ANARI_TOOLS"
fi
for tool in cmake ctest cpack; do
  ln -sfn "$CMAKE_DIR/bin/$tool" "$ANARI_TOOLS/bin/$tool"
done

# Ninja
if [ ! -x "$ANARI_TOOLS/bin/ninja" ]; then
  download "https://github.com/ninja-build/ninja/releases/download/v$NINJA_VERSION/ninja-linux.zip" \
    "$DL/ninja-linux-$NINJA_VERSION.zip"
  unzip -q -o "$DL/ninja-linux-$NINJA_VERSION.zip" -d "$ANARI_TOOLS/bin"
  chmod +x "$ANARI_TOOLS/bin/ninja"
fi

# Git LFS
if [ ! -x "$ANARI_TOOLS/bin/git-lfs" ]; then
  download "https://github.com/git-lfs/git-lfs/releases/download/v$GIT_LFS_VERSION/git-lfs-linux-amd64-v$GIT_LFS_VERSION.tar.gz" \
    "$DL/git-lfs-linux-amd64-v$GIT_LFS_VERSION.tar.gz"
  rm -rf "$DL/git-lfs-unpack"
  mkdir -p "$DL/git-lfs-unpack"
  tar -xzf "$DL/git-lfs-linux-amd64-v$GIT_LFS_VERSION.tar.gz" -C "$DL/git-lfs-unpack"
  cp "$DL/git-lfs-unpack/git-lfs-$GIT_LFS_VERSION/git-lfs" "$ANARI_TOOLS/bin/git-lfs"
  chmod +x "$ANARI_TOOLS/bin/git-lfs"
  rm -rf "$DL/git-lfs-unpack"
fi

echo "cmake:   $(cmake --version | head -n 1) ($(command -v cmake))"
echo "ninja:   $(ninja --version) ($(command -v ninja))"
echo "git-lfs: $(git lfs version) ($(command -v git-lfs))"
echo "nvcc:    $("$CUDACXX" --version | tail -n 2 | head -n 1) ($CUDACXX)"
echo "gcc:     $("$CC" --version | head -n 1)"
