#!/usr/bin/env bash
# fetch_blender_libs.sh - Blender's precompiled Linux x86_64 libraries.
#
# Checks out the submodule blenderphi/lib/linux_x64 at the commit recorded in the HEAD
# of blenderphi, like `make update` does, but without `make update` (which also pulls
# the Blender sources). The repository uses Git LFS; a shallow fetch of one commit is
# enough. Re-run it after blenderphi moved to a commit with newer libraries.
set -euo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/env.sh"

SRC="$ANARI_WS/blenderphi"
SUB="lib/linux_x64"
DIR="$SRC/$SUB"

commit="$(git -C "$SRC" ls-tree HEAD "$SUB" | awk '{print $3}')"
url="$(git -C "$SRC" config -f "$SRC/.gitmodules" "submodule.$SUB.url")"
[ -n "$commit" ] || { echo "no submodule $SUB in the HEAD of blenderphi" >&2; exit 1; }
echo "$SUB: $commit from $url"

if [ ! -e "$DIR/.git" ]; then
  mkdir -p "$DIR"
  gitdir="$(git -C "$SRC" rev-parse --absolute-git-dir)/modules/$SUB"
  mkdir -p "$(dirname "$gitdir")"
  git init -q --separate-git-dir "$gitdir" "$DIR"
  git -C "$DIR" remote add origin "$url"
  git -C "$DIR" config advice.detachedHead false
fi

if [ "$(git -C "$DIR" rev-parse -q --verify HEAD 2>/dev/null || true)" != "$commit" ]; then
  if ! git -C "$DIR" cat-file -e "$commit^{commit}" 2>/dev/null; then
    git -C "$DIR" fetch --depth 1 origin "$commit" ||
      git -C "$DIR" fetch origin
  fi
  # Pointer files first, the large files are downloaded in one batch below.
  GIT_LFS_SKIP_SMUDGE=1 git -C "$DIR" checkout -q -f --detach "$commit"
fi
git -C "$DIR" lfs fetch origin "$commit"
git -C "$DIR" lfs checkout
git -C "$DIR" update-index -q --refresh || true
if ! git -C "$DIR" diff --quiet; then
  echo "$SUB: the working tree is not complete:" >&2
  git -C "$DIR" diff --stat | tail -n 5 >&2
  exit 1
fi
echo "$SUB: complete, $(git -C "$DIR" ls-files | wc -l | tr -d ' ') files"
