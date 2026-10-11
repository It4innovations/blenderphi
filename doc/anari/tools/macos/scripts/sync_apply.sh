#!/usr/bin/env bash
# sync_apply.sh <repository> - Mac side of sync_to_mac.sh (run by it over ssh).
#
# Reads sync/<repository>/manifest.tsv, one line per repository / submodule:
#   path <TAB> base commit <TAB> url <TAB> branch <TAB> patch file or - <TAB> tree id
#   <TAB> HEAD commit on Windows <TAB> other commits to fetch (MERGE_HEAD) or -
# and makes <repository>/ an exact mirror of the Windows working tree:
#   * the base commit is fetched from the remote (read-only use of git, no commits).
#     It is the Windows HEAD, or the newest ancestor of it which is pushed,
#   * HEAD is moved to it and the patch is applied to the index: the commits which
#     are not pushed and the uncommitted changes together,
#   * the working tree is updated from the index. Only files whose content differs
#     are written, so a repeated sync does not force a rebuild of everything,
#   * files which are neither in the index nor ignored are removed.
# The uncommitted Windows changes therefore show as staged changes on the Mac
# (`git diff --cached`), and `git diff` shows what was edited on the Mac since. Such
# local edits are saved to sync/mac-local/<repository>-*.patch before they are
# overwritten: the Windows tree is the source of truth.
#
# The tree id of the resulting index is compared with the one of the Windows state.
#
# Environment:
#   SYNC_DEPTH=<n>   history depth of the fetch (default 1; 0 = full history)

set -euo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/env.sh"

repo="${1:?usage: sync_apply.sh <repository>}"
sync="$ANARI_WS/sync/$repo"
manifest="$sync/manifest.tsv"
depth="${SYNC_DEPTH:-1}"
stamp="$(date +%Y%m%d-%H%M%S)"
[ -f "$manifest" ] || { echo "no manifest: $manifest" >&2; exit 1; }

# Submodules which are not needed on the Mac (they are checked out on Windows).
skip_path() {
  case "$repo/$1" in
    blenderphi/lib/windows_x64*|blenderphi/lib/windows_arm64*|blenderphi/lib/linux_x64*) return 0 ;;
    openmoonray-anari/external/vcpkg*) return 0 ;;
  esac
  return 1
}

# Where Git LFS objects are taken from when the fork does not have them.
lfs_fallback_url() {
  case "$repo/$1" in
    blenderphi/.) echo "https://projects.blender.org/blender/blender.git" ;;
  esac
}

paths=()
while IFS=$'\t' read -r p _; do paths+=("$p"); done <"$manifest"

# parent_of <path>: the manifest entry which contains the submodule <path>.
parent_of() {
  local path="$1" best="" p
  [ "$path" = "." ] && return 0
  best="."
  for p in "${paths[@]}"; do
    [ "$p" = "." ] && continue
    case "$path" in
      "$p"/*) [ "${#p}" -gt "${#best}" ] || [ "$best" = "." ] && best="$p" ;;
    esac
  done
  echo "$best"
}

work_dir() { if [ "$1" = "." ]; then echo "$ANARI_WS/$repo"; else echo "$ANARI_WS/$repo/$1"; fi; }

fetch_commit() {
  local dir="$1" commit="$2"
  git -C "$dir" cat-file -e "$commit^{commit}" 2>/dev/null && return 0
  if [ "$depth" = 0 ]; then
    git -C "$dir" fetch -q origin || true
    git -C "$dir" cat-file -e "$commit^{commit}" 2>/dev/null && return 0
    git -C "$dir" fetch -q origin "$commit"
  else
    git -C "$dir" fetch -q --depth "$depth" origin "$commit"
  fi
}

# changed_files <work tree>: the files whose content differs from the index.
# `git diff` alone is not enough: for a file which .gitattributes gives to Git LFS
# but which is committed as a plain blob, it compares the LFS pointer the clean
# filter would make and always reports a difference.
changed_files() {
  local dir="$1" f
  git -C "$dir" diff --name-only --ignore-submodules=all | while IFS= read -r f; do
    if [ -f "$dir/$f" ] && git -C "$dir" cat-file blob ":$f" 2>/dev/null | cmp -s - "$dir/$f"; then
      continue
    fi
    echo "$f"
  done
}

apply_entry() {
  local path="$1" commit="$2" url="$3" branch="$4" patch="$5" tree="$6"
  local whead="${7:-$2}" extra="${8:--}"
  local dir parent pdir rel gitdir child ccommit got save changed
  dir="$(work_dir "$path")"

  if [ ! -e "$dir/.git" ]; then
    mkdir -p "$dir"
    if [ "$path" = "." ]; then
      git init -q "$dir"
    else
      # A submodule: its git directory goes into the one of its parent, as git does.
      parent="$(parent_of "$path")"
      pdir="$(work_dir "$parent")"
      if [ "$parent" = "." ]; then rel="$path"; else rel="${path#"$parent"/}"; fi
      gitdir="$(git -C "$pdir" rev-parse --absolute-git-dir)/modules/$rel"
      mkdir -p "$(dirname "$gitdir")"
      git init -q --separate-git-dir "$gitdir" "$dir"
      git -C "$pdir" config "submodule.$rel.url" "$url"
      git -C "$pdir" config "submodule.$rel.active" true
    fi
    git -C "$dir" remote add origin "$url"
    # exFAT: inode numbers and change times are not stable, only trust size and mtime.
    git -C "$dir" config core.checkStat minimal
    git -C "$dir" config core.trustctime false
    git -C "$dir" config core.autocrlf false
    git -C "$dir" config advice.detachedHead false
  else
    git -C "$dir" remote set-url origin "$url"
    # Edits made on the Mac since the last sync are saved before they are overwritten.
    if git -C "$dir" rev-parse -q --verify HEAD >/dev/null; then
      git -C "$dir" update-index -q --refresh >/dev/null 2>&1 || true
      changed="$(changed_files "$dir")"
      if [ -n "$changed" ]; then
        mkdir -p "$ANARI_WS/sync/mac-local"
        save="$ANARI_WS/sync/mac-local/$repo-$(echo "$path" | tr '/.' '__')-$stamp.patch"
        echo "$changed" | while IFS= read -r f; do
          git -C "$dir" diff --binary -- "$f"
        done >"$save"
        echo "   WARNING: $repo/$path had local changes on the Mac, saved to $save:" >&2
        echo "$changed" | sed 's/^/      /' | head -n 20 >&2
      fi
    fi
  fi

  fetch_commit "$dir" "$commit"
  # The other side of a merge in progress: the patch does not need it, but the
  # history of the merge is then available on the Mac as well.
  if [ "$extra" != "-" ]; then
    local other
    for other in $extra; do
      fetch_commit "$dir" "$other" 2>/dev/null ||
        echo "   note: $repo/$path: the merge head ${other:0:12} is not on the remote"
    done
  fi

  # Move HEAD without touching the index or the working tree.
  if [ "$branch" != "HEAD" ]; then
    git -C "$dir" update-ref "refs/heads/$branch" "$commit"
    git -C "$dir" symbolic-ref HEAD "refs/heads/$branch"
  else
    git -C "$dir" update-ref --no-deref HEAD "$commit"
  fi

  # The index becomes the Windows state: HEAD, the patch, and the commits of the
  # submodules as they are checked out on Windows.
  git -C "$dir" read-tree --reset HEAD
  if [ "$patch" != "-" ]; then
    git -C "$dir" apply --cached --binary --whitespace=nowarn "$sync/$patch"
  fi
  local c3 c4 c5 c6 chead
  while IFS=$'\t' read -r child ccommit c3 c4 c5 c6 chead _; do
    [ "$child" = "." ] && continue
    [ "$(parent_of "$child")" = "$path" ] || continue
    if [ "$path" = "." ]; then rel="$child"; else rel="${child#"$path"/}"; fi
    # The HEAD of the submodule on Windows, which may be a commit that is not pushed.
    git -C "$dir" update-index --cacheinfo "160000,${chead:-$ccommit},$rel"
  done <"$manifest"

  got="$(git -C "$dir" write-tree)"
  if [ "$got" != "$tree" ]; then
    echo "   ERROR: $repo/$path: tree $got differs from the Windows tree $tree" >&2
    return 1
  fi

  # Update the working tree. Large files are fetched afterwards, in one go.
  git -C "$dir" update-index -q --refresh >/dev/null 2>&1 || true
  GIT_LFS_SKIP_SMUDGE=1 git -C "$dir" checkout-index -a -f
  git -C "$dir" clean -fdq
  if [ -f "$dir/.gitattributes" ] && grep -q 'filter=lfs' "$dir/.gitattributes"; then
    local fallback
    fallback="$(lfs_fallback_url "$path")"
    if ! git -C "$dir" lfs fetch origin "$commit" >"$sync/lfs-fetch.log" 2>&1; then
      if [ -n "$fallback" ]; then
        git -C "$dir" remote get-url lfs-fallback >/dev/null 2>&1 ||
          git -C "$dir" remote add lfs-fallback "$fallback"
        git -C "$dir" lfs fetch lfs-fallback "$commit" >>"$sync/lfs-fetch.log" 2>&1 ||
          echo "   WARNING: $repo/$path: some Git LFS objects are missing, see $sync/lfs-fetch.log" >&2
      else
        echo "   WARNING: $repo/$path: git lfs fetch failed, see $sync/lfs-fetch.log" >&2
      fi
    fi
    git -C "$dir" lfs checkout >>"$sync/lfs-fetch.log" 2>&1 || true
  fi
  git -C "$dir" update-index -q --refresh >/dev/null 2>&1 || true
  changed="$(changed_files "$dir")"
  if [ -n "$changed" ]; then
    echo "   ERROR: $repo/$path: the working tree does not match the index:" >&2
    echo "$changed" | sed 's/^/      /' | head -n 20 >&2
    return 1
  fi
  local note=""
  [ "$whead" != "$commit" ] && note=" (the Windows HEAD ${whead:0:12} is not pushed)"
  printf '   %-40s %s tree %s staged files: %s%s\n' "$path" "${commit:0:12}" "${tree:0:12}" \
    "$(git -C "$dir" diff --cached --name-only --ignore-submodules=all | wc -l | tr -d ' ')" "$note"
}

status=0
while IFS=$'\t' read -r path commit url branch patch tree whead extra; do
  if [ "$path" != "." ] && skip_path "$path"; then
    echo "   $path: skipped (not used on the Mac)"
    continue
  fi
  # A submodule inside a skipped or failed parent has no place to go.
  if [ "$path" != "." ] && [ ! -d "$(work_dir "$(parent_of "$path")")" ]; then
    continue
  fi
  # Not in a `||` list: bash would ignore `set -e` inside the function there.
  set +e
  (set -e; apply_entry "$path" "$commit" "$url" "$branch" "$patch" "$tree" "$whead" "$extra") </dev/null
  rc=$?
  set -e
  [ "$rc" = 0 ] || status=1
done <"$manifest"
exit $status
