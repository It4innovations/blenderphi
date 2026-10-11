#!/usr/bin/env bash
# build_pynari.sh - pynari (Python bindings of ANARI) against the ANARI SDK in install/.
#
# Creates the Python environment build/pynari-venv from the system Python (3.12, the
# node has its development package) with numpy, pybind11, Pillow, matplotlib and
# mpi4py from PyPI, and builds the extension module in build/pynari. The module is
# used from there, no wheel is installed. The pip cache is cache/pip (scripts/env.sh).
#
# mpi4py is only used by sample06. Its PyPI wheel needs an MPI library at run time;
# the `mpich` package of PyPI puts one into the virtual environment, so no environment
# module (and none of its run time paths) is needed.
#
# Needs: scripts/build_anari_sdk.sh. Run the samples with scripts/run_pynari.sh.
set -euo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/env.sh"

PYTHON="${PYNARI_PYTHON:-/usr/bin/python3}"
VENV="$ANARI_BUILD/pynari-venv"
B="$ANARI_BUILD/pynari"

if [ ! -x "$VENV/bin/python" ]; then
  log_run pynari-venv "$PYTHON" -m venv "$VENV"
fi
log_run pynari-pip "$VENV/bin/python" -m pip install numpy pybind11 pillow matplotlib
log_run pynari-pip-mpi4py "$VENV/bin/python" -m pip install --only-binary=:all: mpi4py mpich ||
  echo "mpi4py could not be installed: sample06 will fail"

log_run pynari-configure cmake -S "$ANARI_WS/pynari" -B "$B" "${ANARI_CMAKE_COMMON[@]}" \
  -Danari_DIR="$ANARI_SDK_CMAKE_DIR" \
  -DPYNARI_USE_INSTALLED_PYBIND=ON \
  -Dpybind11_DIR="$("$VENV/bin/python" -m pybind11 --cmakedir)" \
  -DPython_EXECUTABLE="$VENV/bin/python" \
  -DPYBIND11_FINDPYTHON=ON \
  -DCMAKE_CUDA_ARCHITECTURES="$ANARI_CUDA_ARCH"
log_run pynari-build cmake --build "$B" --parallel "$JOBS"
ls -l "$B"/pynari*.so
