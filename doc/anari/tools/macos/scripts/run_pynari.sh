#!/usr/bin/env bash
# run_pynari.sh [run_all.py options] - the pynari sample matrix on macOS.
#
# Runs scripts/run_pynari.py with the interpreter of build/pynari-venv, see there for
# the options. Without -r, every configuration whose device library is installed is run:
#   helide barney cycles_cpu cycles_metal visionaray ospray rpr_cpu photon_cpu mitsuba_llvm
#   mitsuba_metal
# Results: build/pynari-out/<configuration>/, results.json and results.md.
#
# Needs: scripts/build_pynari.sh.
set -euo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/env.sh"
exec "$ANARI_BUILD/pynari-venv/bin/python" "$ANARI_WS/scripts/run_pynari.py" "$@"
