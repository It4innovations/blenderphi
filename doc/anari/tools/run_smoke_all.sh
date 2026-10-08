#!/bin/bash
# Run the Blender ANARI smoke tests for every ANARI library; logs go to build/logs/smoke-final-<name>.txt
# Usage: run_smoke_all.sh [configuration ...]   (default: all configurations)
cd /f/work/anari
BLENDER=build/blender/bin/Release/blender.exe
TESTS=blenderphi/tests/python/anari_smoke_tests.py
BIN=F:/work/anari/install/bin
SELECTED=" $* "

run() {
  local name=$1 library=$2 params=$3
  shift 3
  if [ "$SELECTED" != "  " ] && [[ "$SELECTED" != *" $name "* ]]; then return; fi
  local args=(--library "$library" --outdir "F:/work/anari/build/test_out/$name")
  for path in "$@"; do args+=(--library-path "$path"); done
  [ -n "$params" ] && args+=(--device-parameters "$params")
  local start=$(date +%s)
  timeout 3600 "$BLENDER" --background --factory-startup -noaudio --python "$TESTS" -- "${args[@]}" \
    > "build/logs/smoke-final-$name.txt" 2>&1
  local status=$?
  echo "$name exit=$status time=$(( $(date +%s) - start ))s $(grep -E '^(OK|FAILED)' build/logs/smoke-final-$name.txt)"
}

run cycles cycles "computeDevice=cpu" "$BIN"
run cycles_optix cycles "computeDevice=optix" "$BIN"
run barney barney "" "$BIN"
run visrtx visrtx "" "$BIN"
run visrtx_quality visrtx "renderer=quality" "$BIN"
run mitsuba mitsuba "mitsuba.variant=cuda_ad_rgb" F:/work/anari/install/mitsuba/bin "$BIN"
run moonray moonray "" F:/work/anari/install/moonray/bin "$BIN"
run helide helide "" "$BIN"
run visionaray visionaray "" "$BIN"
run visionaray_cuda visionaray_cuda "" "$BIN"
run ospray ospray "" F:/work/anari/install/ospray/bin "$BIN"
run rpr rpr "" F:/work/anari/install/rpr/bin "$BIN"
run photon photon "" F:/work/anari/install/photon/bin "$BIN"
run photon_cpu photon "" F:/work/anari/install/photon_cpu/bin "$BIN"
echo all done
