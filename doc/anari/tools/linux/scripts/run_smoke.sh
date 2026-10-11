#!/usr/bin/env bash
# run_smoke.sh [configuration ...] - Blender ANARI smoke tests on Linux.
#
# The Linux counterpart of blenderphi/doc/anari/tools/run_smoke_all.sh. Without
# arguments, every configuration whose ANARI library is installed is run, one after
# the other. Logs: build/logs/smoke-<configuration>.txt, images:
# build/test_out/<configuration>. Extra test names can be given after `--`:
#   run_smoke.sh helide -- AnariSmokeTest.test_lights
#
# Environment: SMOKE_TIMEOUT=<seconds> (default 3600), SMOKE_SAMPLES=<n>,
# ANARI_GPU=<index> (default 0: the tests only use one GPU of the node).
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
  if [ ! -f "$1/libanari_library_$library.so" ]; then
    # Not built: only an error when it was asked for by name.
    [ "${#selected[@]}" = 0 ] || echo "$name: $1/libanari_library_$library.so is not installed"
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
  timeout --signal=KILL "$TIMEOUT" bash "$ANARI_WS/scripts/blender.sh" \
    --background --factory-startup -noaudio \
    --python "$TESTS" -- "${args[@]}" ${extra[@]+"${extra[@]}"} >"$log" 2>&1
  status=$?
  [ "$status" = 137 ] && echo "TIMEOUT after $TIMEOUT s" >>"$log"
  echo "$name exit=$status time=$(($(date +%s) - start))s $(grep -E '^(OK|FAILED|Ran )' "$log" | tr '\n' ' ')"
  # A crash of Blender ends the whole suite. Keep that log as smoke-<name>-crash.txt and
  # run the suite again without the test which crashed, so that the others are known.
  if [ "$status" -ge 128 ] && [ "$status" != 137 ] && [ "${#extra[@]}" = 0 ]; then
    local crashed tests=() t
    crashed="$(grep -o '^test_[a-z_]*' "$log" | tail -n 1)"
    cp "$log" "$ANARI_LOGS/smoke-$name-crash.txt"
    [ -n "$crashed" ] || return 0
    # In alphabetical order, the order of a full run.
    for t in $(grep -o 'def test_[a-z_]*' "$TESTS" | sed 's/def //' | sort); do
      [ "$t" = "$crashed" ] || tests+=("AnariSmokeTest.$t")
    done
    start=$(date +%s)
    timeout --signal=KILL "$TIMEOUT" bash "$ANARI_WS/scripts/blender.sh" \
      --background --factory-startup -noaudio \
      --python "$TESTS" -- "${args[@]}" "${tests[@]}" >"$log" 2>&1
    status=$?
    echo "$name: crashed in $crashed (smoke-$name-crash.txt); without it: exit=$status time=$(($(date +%s) - start))s $(grep -E '^(OK|FAILED|Ran )' "$log" | tr '\n' ' ')"
  fi
}

# The configurations of the Windows README (section 7), plus cycles on CUDA.
run cycles cycles "computeDevice=cpu" "$LIB"
run cycles_optix cycles "computeDevice=optix" "$LIB"
run cycles_cuda cycles "computeDevice=cuda" "$LIB"
run helide helide "" "$LIB"
run barney barney "" "$LIB"
run visrtx visrtx "" "$LIB"
run visrtx_quality visrtx "renderer=quality" "$LIB"
run visionaray visionaray "" "$LIB"
run visionaray_cuda visionaray_cuda "" "$LIB"
run ospray ospray "" "$ANARI_INSTALL/ospray/lib" "$LIB"
run rpr rpr "" "$ANARI_INSTALL/rpr/lib" "$LIB"
run rpr_cpu rpr "computeDevice=cpu" "$ANARI_INSTALL/rpr/lib" "$LIB"
run photon photon "" "$ANARI_INSTALL/photon/lib" "$LIB"
run photon_cpu photon "" "$ANARI_INSTALL/photon_cpu/lib" "$LIB"
run mitsuba mitsuba "mitsuba.variant=cuda_ad_rgb" "$ANARI_INSTALL/mitsuba/lib" "$LIB"
run mitsuba_llvm mitsuba "mitsuba.variant=llvm_ad_rgb" "$ANARI_INSTALL/mitsuba/lib" "$LIB"
run moonray moonray "" "$ANARI_INSTALL/moonray/lib" "$LIB"
# MoonRay with its GPU path, the second build of scripts/build_moonray.sh (MOONRAY_XPU=ON).
run moonray_xpu moonray "moonray.executionMode=xpu" "$ANARI_INSTALL/moonray_xpu/lib" "$LIB"
echo "all done"
