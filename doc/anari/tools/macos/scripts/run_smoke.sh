#!/usr/bin/env bash
# run_smoke.sh [configuration ...] - Blender ANARI smoke tests on macOS.
#
# The macOS counterpart of blenderphi/doc/anari/tools/run_smoke_all.sh. Without
# arguments, every configuration whose ANARI library is installed is run.
# Logs: build/logs/smoke-<configuration>.txt, images: build/test_out/<configuration>.
# Extra test names can be given after `--`:
#   run_smoke.sh helide -- AnariSmokeTest.test_lights
#
# Environment: SMOKE_TIMEOUT=<seconds> (default 3600), SMOKE_SAMPLES=<n>.
set -uo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/env.sh"

TESTS="$ANARI_WS/blenderphi/tests/python/anari_smoke_tests.py"
LIB="$ANARI_INSTALL/lib"
TIMEOUT="${SMOKE_TIMEOUT:-3600}"

selected=()
extra=()
while [ $# -gt 0 ]; do
  if [ "$1" = "--" ]; then shift; extra=("$@"); break; fi
  selected+=("$1")
  shift
done

is_selected() {
  [ "${#selected[@]}" = 0 ] && return 0
  local s
  for s in "${selected[@]}"; do [ "$s" = "$1" ] && return 0; done
  return 1
}

# run <configuration> <library> <device parameters> <library directory> [<directory> ...]
run() {
  local name="$1" library="$2" params="$3"
  shift 3
  is_selected "$name" || return 0
  if [ ! -f "$1/libanari_library_$library.dylib" ]; then
    # Not built: only an error when it was asked for by name.
    [ "${#selected[@]}" = 0 ] || echo "$name: $1/libanari_library_$library.dylib is not installed"
    return 0
  fi
  local log="$ANARI_LOGS/smoke-$name.txt"
  local args=(--library "$library" --outdir "$ANARI_BUILD/test_out/$name")
  local path
  for path in "$@"; do args+=(--library-path "$path"); done
  [ -n "$params" ] && args+=(--device-parameters "$params")
  [ -n "${SMOKE_SAMPLES:-}" ] && args+=(--samples "$SMOKE_SAMPLES")
  local start status
  start=$(date +%s)
  # macOS has no timeout(1): a watchdog in the background stops a hanging run.
  bash "$ANARI_WS/scripts/blender.sh" --background --factory-startup -noaudio \
    --python "$TESTS" -- "${args[@]}" ${extra[@]+"${extra[@]}"} >"$log" 2>&1 &
  local pid=$!
  (sleep "$TIMEOUT" && kill -9 "$pid" 2>/dev/null && echo "TIMEOUT after $TIMEOUT s" >>"$log")     </dev/null >/dev/null 2>&1 &
  local watchdog=$!
  wait "$pid"
  status=$?
  # The sleep first: once the watchdog is gone it is no longer its child.
  pkill -P "$watchdog" 2>/dev/null
  kill "$watchdog" 2>/dev/null
  wait "$watchdog" 2>/dev/null
  echo "$name exit=$status time=$(($(date +%s) - start))s $(grep -E '^(OK|FAILED|Ran )' "$log" | tr '\n' ' ')"
}

run cycles cycles "computeDevice=cpu" "$LIB"
run cycles_metal cycles "computeDevice=metal" "$LIB"
run helide helide "" "$LIB"
run barney barney "" "$LIB"
run visionaray visionaray "" "$LIB"
run ospray ospray "" "$ANARI_INSTALL/ospray/lib" "$LIB"
run rpr rpr "" "$ANARI_INSTALL/rpr/lib" "$LIB"
run rpr_cpu rpr "computeDevice=cpu" "$ANARI_INSTALL/rpr/lib" "$LIB"
run photon_cpu photon "" "$ANARI_INSTALL/photon_cpu/lib" "$LIB"
run mitsuba mitsuba "mitsuba.variant=llvm_ad_rgb" "$ANARI_INSTALL/mitsuba/lib" "$LIB"
run mitsuba_metal mitsuba "mitsuba.variant=metal_ad_rgb" "$ANARI_INSTALL/mitsuba/lib" "$LIB"
run mitsuba_scalar mitsuba "mitsuba.variant=scalar_rgb" "$ANARI_INSTALL/mitsuba/lib" "$LIB"
run moonray moonray "" "$ANARI_INSTALL/moonray/lib" "$LIB"
echo "all done"
