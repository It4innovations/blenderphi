#!/usr/bin/env bash
# bg.sh <name> <command...> - run a long command detached from the ssh session.
#   Output:    build/logs/<name>.out
#   Exit code: build/logs/<name>.done (written when the command has finished)
# bg.sh --wait <name> [seconds]  waits for it and prints the end of the output.
set -uo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/env.sh"

if [ "${1:-}" = "--wait" ]; then
  name="$2"
  limit="${3:-540}"
  start=$(date +%s)
  while [ ! -f "$ANARI_LOGS/$name.done" ]; do
    if [ $(($(date +%s) - start)) -ge "$limit" ]; then
      echo "still running after $limit s:"
      tail -n 3 "$ANARI_LOGS/$name.out" | cut -c1-300
      exit 3
    fi
    sleep 5
  done
  tail -n "${BG_TAIL:-25}" "$ANARI_LOGS/$name.out"
  echo "exit code: $(cat "$ANARI_LOGS/$name.done")"
  exit "$(cat "$ANARI_LOGS/$name.done")"
fi

name="${1:?usage: bg.sh <name> <command...>}"
shift
rm -f "$ANARI_LOGS/$name.done"
nohup bash -c '"$@"; echo $? > "$0"' "$ANARI_LOGS/$name.done" "$@" \
  >"$ANARI_LOGS/$name.out" 2>&1 </dev/null &
echo "started $name (pid $!)"
