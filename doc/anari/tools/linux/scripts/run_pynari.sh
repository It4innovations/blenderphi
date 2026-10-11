#!/usr/bin/env bash
# run_pynari.sh [run_all.py options] - the pynari sample matrix on Linux.
#
# Runs scripts/run_pynari.py with the interpreter of build/pynari-venv, see there for
# the options. Without -r, every configuration whose device library is installed is run:
#   helide visrtx barney cycles_optix cycles_cpu mitsuba_cuda mitsuba_llvm moonray
#   visionaray visionaray_cuda ospray rpr photon photon_cpu
# Results: build/pynari-out/<configuration>/, results.json and results.md.
# Runs which execute at the same time need different --results files.
#
# Needs: scripts/build_pynari.sh.
set -euo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/env.sh"
exec "$ANARI_BUILD/pynari-venv/bin/python" "$ANARI_WS/scripts/run_pynari.py" "$@"
