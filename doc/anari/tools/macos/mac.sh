#!/usr/bin/env bash
# mac.sh <command> - run a command on the Mac, in bash, inside the ANARI workspace
# (~/scratch/anari) and with its environment (scripts/env.sh).
#
#   bash mac.sh 'bash scripts/build_anari_sdk.sh'
#   bash mac.sh 'bash scripts/bg.sh blender bash scripts/build_blender.sh'   # detached
#   bash mac.sh 'bash scripts/bg.sh --wait blender 500'
#
# Uses Windows OpenSSH (the ssh of Git Bash has no access to the Windows ssh-agent).
set -euo pipefail
MAC_HOST="${MAC_HOST:-mac}"
MAC_ROOT="${MAC_ROOT:-scratch/anari}"
SSH="${SSH:-/c/Windows/System32/OpenSSH/ssh.exe}"
# The command travels base64 encoded, so no quoting is lost in the login shell (zsh).
encoded="$(printf '%s' "$*" | base64 -w 0)"
MSYS_NO_PATHCONV=1 "$SSH" -o BatchMode=yes -o ConnectTimeout=20 -o ServerAliveInterval=15 \
  -o ServerAliveCountMax=4 "$MAC_HOST" \
  "cd $MAC_ROOT && echo $encoded | base64 -d > cache/.mac-command-\$\$.sh && bash -c 'source scripts/env.sh; source \"\$1\"; rc=\$?; rm -f \"\$1\"; exit \$rc' mac.sh cache/.mac-command-\$\$.sh" </dev/null
