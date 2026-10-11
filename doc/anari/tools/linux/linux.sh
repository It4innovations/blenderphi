#!/usr/bin/env bash
# linux.sh <command> - run a command on the Linux machine, in bash, inside the ANARI
# workspace and with its environment (scripts/env.sh).
#
#   bash linux.sh 'bash scripts/build_anari_sdk.sh'
#   bash linux.sh 'bash scripts/bg.sh blender bash scripts/build_blender.sh'   # detached
#   bash linux.sh 'bash scripts/bg.sh --wait blender 500'
#
# Environment: LINUX_HOST (the ssh name of the machine), LINUX_ROOT (the workspace there,
# an absolute path). Both have to be set.
# Uses Windows OpenSSH (the ssh of Git Bash has no access to the Windows ssh-agent).
set -euo pipefail
LINUX_HOST="${LINUX_HOST:?set LINUX_HOST to the ssh name of the Linux machine}"
LINUX_ROOT="${LINUX_ROOT:?set LINUX_ROOT to the absolute path of the workspace there}"
SSH="${SSH:-/c/Windows/System32/OpenSSH/ssh.exe}"
# The command travels base64 encoded, so no quoting is lost on the way.
encoded="$(printf '%s' "$*" | base64 -w 0)"
MSYS_NO_PATHCONV=1 "$SSH" -o BatchMode=yes -o ConnectTimeout=20 -o ServerAliveInterval=15 \
  -o ServerAliveCountMax=4 "$LINUX_HOST" \
  "cd $LINUX_ROOT && mkdir -p cache && echo $encoded | base64 -d > cache/.linux-command-\$\$.sh && bash -c 'source scripts/env.sh; source \"\$1\"; rc=\$?; rm -f \"\$1\"; exit \$rc' linux.sh cache/.linux-command-\$\$.sh" </dev/null 2> >(grep -v -E 'post-quantum|store now, decrypt later|server may need to be upgraded' >&2)
