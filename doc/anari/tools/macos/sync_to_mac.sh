#!/usr/bin/env bash
# sync_to_mac.sh - mirror the Windows working trees of F:\work\anari on the Mac.
#
# The Windows state is not all on the remotes (uncommitted changes, commits and
# merges which are not pushed, merges in progress), so it is carried over as
#   1. a base commit of each repository (and of each initialized submodule) which
#      exists on the remote `origin` and which the Mac fetches from there: HEAD if
#      it is pushed, else the merge base of HEAD with its upstream branch, else the
#      newest ancestor of HEAD which a remote branch contains, and
#   2. one binary patch per repository / submodule from that base commit to the
#      working tree: the commits which are not pushed and everything which is not
#      committed (modified, deleted and untracked files; ignored files are left out).
#
# The patch is made with a temporary index and a temporary object directory:
# neither the working tree, the index, MERGE_HEAD nor the object database of the
# Windows repository is written to, and nothing is fetched on Windows (the base is
# found with the remote-tracking branches as they are). Line endings are normalized
# by git (the Windows trees are CRLF check-outs, the patches and the Mac trees are
# LF). A repository in the middle of a merge is synchronized with the content of
# its working tree; if a file still has conflict markers, the repository is not
# sent (SYNC_ALLOW_MARKERS=1 sends it anyway).
#
# Usage (Git Bash):
#   bash sync_to_mac.sh [options] <repository> [<repository> ...]
#   bash sync_to_mac.sh all                 the default list below
#   bash sync_to_mac.sh --scripts           only copy mac/scripts to the Mac
# Options:
#   --no-apply     make the patches and copy them, but do not apply them on the Mac
#   --local        only make the patches (in build/sync/macos), copy nothing
#   --rehash       start from `git read-tree` instead of a copy of the index
#                  (slower; use it if the result is ever in doubt)
#
# From PowerShell:
#   & "C:\Program Files\Git\bin\bash.exe" F:\work\anari\blenderphi\doc\anari\tools\macos\sync_to_mac.sh blenderphi
#
# Environment:
#   MAC_HOST       ssh host (default: the ssh alias `mac`)
#   MAC_ROOT       the workspace on the Mac, relative to the home directory (default scratch/anari)
#   ANARI_WS       the Windows workspace (default: the directory this repository is in)
#   SYNC_OUT       where the patches are written (default: <workspace>/build/sync/macos)
#
# The Mac side is scripts/sync_apply.sh (copied to the Mac on every run). It prints
# the tree id of the resulting state; this script compares it with the tree id of
# the Windows state, so a sync that ends with "OK" is exact.

set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
WS="${ANARI_WS:-$(cd "$HERE/../../../../.." && pwd)}"   # the workspace, F:\work\anari
OUT="${SYNC_OUT:-$WS/build/sync/macos}"
MAC_HOST="${MAC_HOST:-mac}"
MAC_ROOT="${MAC_ROOT:-scratch/anari}"             # relative to the home directory on the Mac
# The ssh of Git Bash has no access to the Windows ssh-agent; use Windows OpenSSH.
SSH="${SSH:-/c/Windows/System32/OpenSSH/ssh.exe}"
SCP="${SCP:-/c/Windows/System32/OpenSSH/scp.exe}"
# A lost connection fails after a minute instead of hanging.
SSH_OPTS=(-o BatchMode=yes -o ConnectTimeout=20 -o ServerAliveInterval=15 -o ServerAliveCountMax=4)

DEFAULT_REPOS=(ANARI-SDK blenderphi cyclesphi-anari barney visionaray anari-visionaray
               anari-ospray RadeonProRenderANARI photon mitsuba-anari pynari)

usage() { awk 'NR > 1 && /^#/ { print; next } NR > 1 { exit }' "${BASH_SOURCE[0]}"; }

apply=1
copy=1
rehash=0
scripts_only=0
repos=()
for arg in "$@"; do
  case "$arg" in
    --no-apply) apply=0 ;;
    --local) copy=0; apply=0 ;;
    --rehash) rehash=1 ;;
    --scripts) scripts_only=1 ;;
    all) repos+=("${DEFAULT_REPOS[@]}") ;;
    -h|--help) usage; exit 0 ;;
    -*) echo "unknown option: $arg" >&2; exit 2 ;;
    *) repos+=("$arg") ;;
  esac
done

mac_ssh() { MSYS_NO_PATHCONV=1 "$SSH" "${SSH_OPTS[@]}" "$MAC_HOST" "$@" </dev/null; }

copy_scripts() {
  mac_ssh "mkdir -p $MAC_ROOT/scripts $MAC_ROOT/sync"
  # scp keeps the files byte for byte (a pipe from PowerShell would add a BOM).
  MSYS_NO_PATHCONV=1 "$SCP" "${SSH_OPTS[@]}" -q -r "$(cygpath -w "$HERE/scripts")" "$MAC_HOST:$MAC_ROOT/"
}

if [ "$scripts_only" = 1 ]; then
  copy_scripts
  echo "scripts copied to $MAC_HOST:$MAC_ROOT/scripts"
  exit 0
fi
if [ "${#repos[@]}" = 0 ]; then
  usage
  exit 2
fi

# All temporary files of a run live in one directory, removed at exit.
TMP_ROOT="$(mktemp -d)"
trap 'cd /; rm -rf "$TMP_ROOT"' EXIT

# base_commit <work tree> [<pinned commit>]: the commit the Mac checks out, see the top
# of this file. For a submodule, <pinned commit> is the commit which the base commit of
# its superproject records for it: that one is taken to be on the remote as well.
base_commit() {
  local wt="$1" pinned="${2:-}" head base candidates
  head="$(git -C "$wt" rev-parse HEAD)"
  if [ "$head" = "$pinned" ] ||
     [ -z "$(git -C "$wt" rev-list -n 1 HEAD --not --remotes=origin)" ]; then
    echo "$head"   # HEAD is on the remote
    return 0
  fi
  # The newest commits in the history of HEAD which the remote has.
  candidates="$(git -C "$wt" rev-list --boundary HEAD --not --remotes=origin | sed -n 's/^-//p')"
  base="$(git -C "$wt" merge-base HEAD '@{u}' 2>/dev/null || true)"
  if [ -n "$base" ] && echo "$candidates" | grep -q "^$base\$"; then
    echo "$base"
    return 0
  fi
  if [ -n "$candidates" ]; then
    # shellcheck disable=SC2086
    git -C "$wt" rev-list -n 1 --date-order $candidates
    return 0
  fi
  if [ -n "$pinned" ] && git -C "$wt" merge-base --is-ancestor "$pinned" HEAD 2>/dev/null; then
    echo "$pinned"
    return 0
  fi
  # No remote-tracking branch says anything about HEAD (a shallow or single-commit
  # clone, for instance). Try HEAD itself: the Mac fails if it cannot fetch it.
  echo "$head"
}

# make_patch <work tree> <patch file> <base commit>
# Writes the patch from the base commit to the working tree (possibly empty) and
# prints the tree id of the working tree state.
make_patch() {
  local wt="$1" patch="$2" base="$3" tmp gitdir objdir
  tmp="$(mktemp -d "$TMP_ROOT/XXXXXX")"
  gitdir="$(git -C "$wt" rev-parse --absolute-git-dir)"
  objdir="$(cd "$wt" && cd "$(git rev-parse --git-path objects)" && pwd)"
  mkdir -p "$tmp/objects"
  if [ "$rehash" = 0 ] && [ -f "$gitdir/index" ]; then
    # A copy of the index keeps the cached stat data, so only changed files are read.
    # Entries of a merge in progress are resolved by `git add` below, in the copy.
    cp "$gitdir/index" "$tmp/index"
  else
    GIT_INDEX_FILE="$tmp/index" git -C "$wt" read-tree "$base"
  fi
  (
    export GIT_INDEX_FILE="$tmp/index"
    export GIT_OBJECT_DIRECTORY="$tmp/objects"
    export GIT_ALTERNATE_OBJECT_DIRECTORIES="$objdir"
    cd "$wt"
    git add -A . >/dev/null 2>"$tmp/add.log" || { cat "$tmp/add.log" >&2; exit 1; }
    git diff --cached --binary --full-index --no-renames --ignore-submodules=all "$base" >"$patch"
    git write-tree
  ) >"$tmp/tree" || return 1
  tail -n 1 "$tmp/tree"
}

# conflict_markers <patch file>: the files to which the patch adds conflict markers.
conflict_markers() {
  LC_ALL=C awk '
    /^\+\+\+ b\// { file = substr($0, 7) }
    /^\+(<<<<<<<|>>>>>>>|\|\|\|\|\|\|\|) / { print file }
  ' "$1" | sort -u
}

# collect <repository> <work tree> <path inside the repository> <manifest> [<pinned commit>]
# One manifest line per repository / submodule, parents first:
#   path <TAB> base commit <TAB> url <TAB> branch <TAB> patch file or - <TAB> tree id
#   <TAB> HEAD commit <TAB> other commits to fetch (MERGE_HEAD) or -
collect() {
  local repo="$1" wt="$2" rel="$3" manifest="$4" pinned="${5:-}"
  local head base url branch patch tree n sub gitdir extra markers subpin
  head="$(git -C "$wt" rev-parse HEAD)"
  base="$(base_commit "$wt" "$pinned")" || return 1
  url="$(git -C "$wt" config --get remote.origin.url || echo -)"
  branch="$(git -C "$wt" rev-parse --abbrev-ref HEAD)"
  gitdir="$(git -C "$wt" rev-parse --absolute-git-dir)"
  extra="-"
  if [ -f "$gitdir/MERGE_HEAD" ]; then
    extra="$(tr '\n' ' ' <"$gitdir/MERGE_HEAD" | sed 's/ *$//')"
    echo "   NOTE: $repo/$rel is in the middle of a merge: the content of the working tree is sent" >&2
  fi
  if [ "$base" != "$head" ]; then
    echo "   NOTE: $repo/$rel: HEAD ${head:0:12} is not on the remote, the patch is made against" \
      "${base:0:12} ($(git -C "$wt" rev-list --count "$base..HEAD") commits not pushed)" >&2
  fi
  n="$(wc -l <"$manifest")"
  patch="$n.patch"
  tree="$(make_patch "$wt" "$OUT/$repo/$patch" "$base")" || return 1
  if [ ! -s "$OUT/$repo/$patch" ]; then
    patch="-"   # an empty file stays behind, the Mac side ignores it
  else
    if grep -a -q '^+oid sha256:' "$OUT/$repo/$patch"; then
      echo "WARNING: $repo/$rel: the patch changes Git LFS pointers; their content is not carried over" >&2
    fi
    markers="$(conflict_markers "$OUT/$repo/$patch")"
    if [ -n "$markers" ] && [ "${SYNC_ALLOW_MARKERS:-0}" != 1 ]; then
      echo "ERROR: $repo/$rel: conflict markers in:" >&2
      echo "$markers" | sed 's/^/         /' >&2
      echo "       resolve them, or set SYNC_ALLOW_MARKERS=1 if they belong there" >&2
      return 1
    fi
  fi
  printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' "$rel" "$base" "$url" "$branch" "$patch" "$tree" \
    "$head" "$extra" >>"$manifest"
  # Initialized submodules only (a leading '-' marks one that is not checked out).
  git -C "$wt" submodule status 2>/dev/null | while read -r line; do
    case "$line" in -*) continue ;; esac
    sub="$(echo "$line" | sed -e 's/^[+U ]*//' | cut -d' ' -f2)"
    subpin="$(git -C "$wt" rev-parse -q --verify "$base:$sub" 2>/dev/null || true)"
    if [ "$rel" = "." ]; then
      collect "$repo" "$wt/$sub" "$sub" "$manifest" "$subpin" || : >"$OUT/$repo/failed"
    else
      collect "$repo" "$wt/$sub" "$rel/$sub" "$manifest" "$subpin" || : >"$OUT/$repo/failed"
    fi
  done
}

[ "$copy" = 1 ] && copy_scripts

mkdir -p "$OUT"
status=0
for repo in "${repos[@]}"; do
  wt="$WS/$repo"
  if ! git -C "$wt" rev-parse --git-dir >/dev/null 2>&1; then
    echo "== $repo: not a git repository in $WS" >&2
    status=1
    continue
  fi
  echo "== $repo"
  # A fresh output directory for every run; the previous one is kept as <repo>.old.
  if [ -d "$OUT/$repo" ]; then
    [ -d "$OUT/$repo.old" ] && (cd "$OUT" && rm -rf -- "./$repo.old")
    mv "$OUT/$repo" "$OUT/$repo.old"
  fi
  mkdir -p "$OUT/$repo"
  : >"$OUT/$repo/manifest.tsv"
  collect "$repo" "$wt" "." "$OUT/$repo/manifest.tsv" || : >"$OUT/$repo/failed"
  if [ -f "$OUT/$repo/failed" ]; then
    echo "   $repo: FAILED, nothing was sent" >&2
    status=1
    continue
  fi
  awk -F'\t' -v dir="$OUT/$repo" '{
    size = "-"
    if ($5 != "-") { cmd = "wc -c < \"" dir "/" $5 "\""; cmd | getline size; close(cmd) }
    printf "   %-40s %s %s patch=%s bytes%s\n", $1, substr($2, 1, 12), $4, size,
      ($2 != $7 ? " (base of " substr($7, 1, 12) ")" : "")
  }' "$OUT/$repo/manifest.tsv"

  [ "$copy" = 1 ] || continue
  mac_ssh "cd $MAC_ROOT/sync && if [ -d $repo ]; then mv $repo .old-$repo-\$\$ && rm -rf .old-$repo-*; fi"
  MSYS_NO_PATHCONV=1 "$SCP" "${SSH_OPTS[@]}" -q -r "$(cygpath -w "$OUT/$repo")" "$MAC_HOST:$MAC_ROOT/sync/"
  [ "$apply" = 1 ] || continue
  if mac_ssh "bash $MAC_ROOT/scripts/sync_apply.sh $repo"; then
    echo "   $repo: OK"
  else
    echo "   $repo: FAILED" >&2
    status=1
  fi
done
exit $status
