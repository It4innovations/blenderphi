"""Run the pynari samples with the ANARI devices of this workspace on Linux.

This is pynari/doc/windows/tools/run_all.py with the paths, the library names and the
configurations of the Linux workspace: the module is imported and its settings are
replaced, so the samples, the checks and the result files are the same as on Windows.
Use scripts/run_pynari.sh, which starts it with the interpreter of build/pynari-venv:

  bash scripts/run_pynari.sh                          every installed configuration
  bash scripts/run_pynari.sh -r helide,barney -s sample0
  bash scripts/run_pynari.sh -t 1200 -j 2             timeout per run [s], parallel jobs
  bash scripts/run_pynari.sh -r ospray --results build/pynari-out/results-ospray.json

Results: build/pynari-out/<configuration>/<sample>.png and .log, results.json, results.md.
Runs which execute at the same time need different --results files.

Every run sees one GPU only (ANARI_GPU, default 0), so that a test loop never occupies
both GPUs of the node.
"""
import importlib.util
import os
import sys

WS = os.path.realpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
TOOLS = f"{WS}/pynari/doc/windows/tools"

spec = importlib.util.spec_from_file_location("run_all", f"{TOOLS}/run_all.py")
run_all = importlib.util.module_from_spec(spec)
spec.loader.exec_module(run_all)

LIB = f"{WS}/install/lib"
BLENDER_LIB = f"{WS}/build/blender/bin/lib"
OSPRAY_LIB = f"{WS}/install/ospray/lib"
RPR_LIB = f"{WS}/install/rpr/lib"
PHOTON_LIB = f"{WS}/install/photon/lib"
PHOTON_CPU_LIB = f"{WS}/install/photon_cpu/lib"
MITSUBA_LIB = f"{WS}/install/mitsuba/lib"
MOONRAY_LIB = f"{WS}/install/moonray/lib"
MOONRAY_DEPS_LIB = f"{WS}/install/moonray-deps/lib"

# A home directory of its own, like scripts/blender.sh: nothing a device or matplotlib
# writes may land in the real one.
HOME = f"{WS}/cache/home"
GPU = os.environ.get("ANARI_GPU", "0")


def config(library, directories, **env):
    """(ANARI library, library directories, environment) of a configuration.

    Linux has no counterpart of the DLL search path of Windows. The front-end library
    finds device libraries through the dynamic loader: LD_LIBRARY_PATH lists the
    directories of the device and of the shared libraries it depends on.
    """
    env = dict(env)
    env["LD_LIBRARY_PATH"] = os.pathsep.join(
        directories + [d for d in os.environ.get("LD_LIBRARY_PATH", "").split(os.pathsep) if d])
    env["HOME"] = HOME
    if GPU != "all":
        env["CUDA_VISIBLE_DEVICES"] = GPU
    # viewer_headless.py looks for the viewer examples in the Windows workspace.
    env["PYTHONPATH"] = f"{WS}/pynari/viewer"
    # sample06 initializes MPI (mpi4py with the MPICH of the virtual environment).
    # MPICH's UCX would open the InfiniBand ports of the node and fails there
    # ("uct_iface_open(ud_verbs/mlx5_1:1) failed"); the samples run on one node.
    env["UCX_TLS"] = os.environ.get("UCX_TLS", "self,sm,tcp")
    return (library, directories, env)


run_all.ROOT = WS
run_all.SRC = f"{WS}/pynari"
run_all.OUT = f"{WS}/build/pynari-out"
run_all.PYTHON = f"{WS}/build/pynari-venv/bin/python"
run_all.MODULE_DIR = f"{WS}/build/pynari"   # pynari.cpython-312-x86_64-linux-gnu.so
run_all.LAUNCH = f"{run_all.OUT}/_launch.py"
run_all.RENDERERS = {
    "helide": config("helide", [LIB]),
    "visrtx": config("visrtx", [LIB]),
    "barney": config("barney", [LIB]),
    # cyclesphi-anari depends on the shared libraries of Blender (OpenImageIO, OpenVDB, ...).
    "cycles_optix": config("cycles", [LIB, BLENDER_LIB], CYCLES_ANARI_USE_GPU="OPTIX"),
    "cycles_cuda": config("cycles", [LIB, BLENDER_LIB], CYCLES_ANARI_USE_GPU="CUDA"),
    "cycles_cpu": config("cycles", [LIB, BLENDER_LIB], ANARI_CYCLES_FORCE_CPU="1"),
    "mitsuba_cuda": config("mitsuba", [MITSUBA_LIB, LIB], ANARI_MITSUBA_VARIANT="cuda_ad_rgb"),
    "mitsuba_llvm": config("mitsuba", [MITSUBA_LIB, LIB], ANARI_MITSUBA_VARIANT="llvm_ad_rgb"),
    "mitsuba_scalar": config("mitsuba", [MITSUBA_LIB, LIB],
                             ANARI_MITSUBA_VARIANT="scalar_rgb"),
    # MoonRay is built against the libraries of Blender (TBB, Embree, OpenVDB, OpenEXR,
    # OpenImageIO, OpenSubdiv) and its own third party libraries in install/moonray-deps.
    "moonray": config("moonray", [MOONRAY_LIB, MOONRAY_DEPS_LIB, LIB, BLENDER_LIB]),
    "visionaray": config("visionaray", [LIB]),
    "visionaray_cuda": config("visionaray_cuda", [LIB]),
    "ospray": config("ospray", [OSPRAY_LIB, LIB]),
    "rpr": config("rpr", [RPR_LIB, LIB], ANARI_RPR_COMPUTE_DEVICE="gpu"),
    "rpr_cpu": config("rpr", [RPR_LIB, LIB], ANARI_RPR_COMPUTE_DEVICE="cpu"),
    "photon": config("photon", [PHOTON_LIB, LIB]),
    # The CPU build of Photon uses the Embree and TBB of Blender.
    "photon_cpu": config("photon", [PHOTON_CPU_LIB, LIB, BLENDER_LIB]),
}

# Not in the default matrix (select them with -r): the slow scalar variant of Mitsuba,
# and configurations which the Windows matrix does not have.
NOT_DEFAULT = {"mitsuba_scalar", "cycles_cuda", "rpr_cpu"}


def installed(name):
    library, directories, _ = run_all.RENDERERS[name]
    return any(os.path.exists(f"{d}/libanari_library_{library}.so") for d in directories)


run_all.installed = installed
run_all.DEFAULT_RENDERERS = [r for r in run_all.RENDERERS
                             if r not in NOT_DEFAULT and installed(r)]


LAUNCHER = '''"""Launcher of run_pynari.py: runs a pynari sample as __main__.
Usage: python _launch.py <sample.py> [sample arguments...]
PYNARI_MODULE_DIR is the directory with the pynari extension module."""
import os, runpy, sys

module_dir = os.environ.get("PYNARI_MODULE_DIR")
if module_dir:
    sys.path.insert(0, module_dir)

# Never open interactive windows.
import matplotlib
matplotlib.use("Agg")

sample = sys.argv[1]
sys.argv = sys.argv[1:]
sys.path.insert(0, os.path.dirname(os.path.abspath(sample)))
runpy.run_path(sample, run_name="__main__")
'''


def install_file(path, text):
    """Write a helper script. Several matrix runs may start at the same time and share
    these files, so one which is already there is left alone and a new one appears
    complete (a rename), never half written."""
    try:
        with open(path, encoding="utf-8") as f:
            if f.read() == text:
                return
    except OSError:
        pass
    temporary = f"{path}.{os.getpid()}.tmp"
    with open(temporary, "w", encoding="utf-8") as f:
        f.write(text)
    os.replace(temporary, path)


def prepare():
    os.makedirs(run_all.OUT, exist_ok=True)
    os.makedirs(HOME, exist_ok=True)
    # The headless drivers of the viewer and of the interactive sample are used as they are.
    for name in ("viewer_headless.py", "interactive_headless.py"):
        with open(f"{TOOLS}/{name}", encoding="utf-8") as f:
            install_file(f"{run_all.OUT}/{name}", f.read())
    # The launcher of Windows registers DLL directories, which only exists there.
    install_file(run_all.LAUNCH, LAUNCHER)


if __name__ == "__main__":
    prepare()
    if "-r" not in sys.argv and "--renderers" not in sys.argv and "--md-only" not in sys.argv:
        sys.argv[1:1] = ["-r", ",".join(run_all.DEFAULT_RENDERERS)]
    if sys.argv[1:] == ["--md-only"]:
        sys.exit("--md-only is not supported by this wrapper")
    run_all.main()
