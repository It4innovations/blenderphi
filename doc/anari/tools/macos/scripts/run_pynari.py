"""Run the pynari samples with the ANARI devices of this workspace on macOS.

This is pynari/doc/windows/tools/run_all.py with the paths, the library names and the
configurations of the macOS workspace: the module is imported and its settings are
replaced, so the samples, the checks and the result files are the same as on Windows.
Use scripts/run_pynari.sh, which starts it with the interpreter of build/pynari-venv:

  bash scripts/run_pynari.sh                          every installed configuration
  bash scripts/run_pynari.sh -r helide,barney -s sample0
  bash scripts/run_pynari.sh -t 1200 -j 2             timeout per run [s], parallel jobs
  bash scripts/run_pynari.sh -r ospray --results build/pynari-out/results-ospray.json

Results: build/pynari-out/<configuration>/<sample>.png and .log, results.json, results.md.
Runs which execute at the same time need different --results files.
"""
import importlib.util
import os
import shutil
import sys

WS = os.path.realpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
TOOLS = f"{WS}/pynari/doc/windows/tools"

spec = importlib.util.spec_from_file_location("run_all", f"{TOOLS}/run_all.py")
run_all = importlib.util.module_from_spec(spec)
spec.loader.exec_module(run_all)

LIB = f"{WS}/install/lib"
BLENDER_LIB = f"{WS}/build/blender/bin/Blender.app/Contents/Resources/lib"
OSPRAY_LIB = f"{WS}/install/ospray/lib"
RPR_LIB = f"{WS}/install/rpr/lib"
PHOTON_CPU_LIB = f"{WS}/install/photon_cpu/lib"
MITSUBA_LIB = f"{WS}/install/mitsuba/lib"
MOONRAY_LIB = f"{WS}/install/moonray/lib"

# A home directory of its own, like scripts/blender.sh: nothing a device or matplotlib
# writes may land in the real one.
HOME = f"{WS}/cache/home"


def config(library, directories, **env):
    """(ANARI library, library directories, environment) of a configuration.

    macOS has no counterpart of the DLL search path of Windows. The front-end library
    finds device libraries next to itself (install/lib); devices in other directories and
    the shared libraries a device depends on are found through DYLD_LIBRARY_PATH.
    """
    env = dict(env)
    env["DYLD_LIBRARY_PATH"] = os.pathsep.join(directories)
    env["HOME"] = HOME
    env["CFFIXED_USER_HOME"] = HOME
    # The kernel cache of Dr.Jit (Mitsuba), by default ~/.drjit.
    env["DRJIT_CACHE_DIR"] = f"{WS}/cache/drjit"
    # viewer_headless.py looks for the viewer examples in the Windows workspace.
    env["PYTHONPATH"] = f"{WS}/pynari/viewer"
    return (library, directories, env)


run_all.ROOT = WS
run_all.SRC = f"{WS}/pynari"
run_all.OUT = f"{WS}/build/pynari-out"
run_all.PYTHON = f"{WS}/build/pynari-venv/bin/python"
run_all.MODULE_DIR = f"{WS}/build/pynari"   # pynari.cpython-313-darwin.so
run_all.LAUNCH = f"{run_all.OUT}/_launch.py"
run_all.RENDERERS = {
    "helide": config("helide", [LIB]),
    "barney": config("barney", [LIB]),
    # cyclesphi-anari depends on the shared libraries of Blender (OpenImageIO, OpenVDB, ...).
    "cycles_cpu": config("cycles", [LIB, BLENDER_LIB], ANARI_CYCLES_FORCE_CPU="1"),
    "cycles_metal": config("cycles", [LIB, BLENDER_LIB], CYCLES_ANARI_USE_GPU="METAL"),
    "visionaray": config("visionaray", [LIB]),
    "ospray": config("ospray", [OSPRAY_LIB, LIB]),
    "rpr": config("rpr", [RPR_LIB, LIB], ANARI_RPR_COMPUTE_DEVICE="gpu"),
    "rpr_cpu": config("rpr", [RPR_LIB, LIB], ANARI_RPR_COMPUTE_DEVICE="cpu"),
    # Photon links the Embree (and through it the TBB) of Blender.
    "photon_cpu": config("photon", [PHOTON_CPU_LIB, LIB, BLENDER_LIB]),
    "mitsuba_llvm": config("mitsuba", [MITSUBA_LIB, LIB], ANARI_MITSUBA_VARIANT="llvm_ad_rgb"),
    "mitsuba_metal": config("mitsuba", [MITSUBA_LIB, LIB],
                            ANARI_MITSUBA_VARIANT="metal_ad_rgb"),
    "mitsuba_scalar": config("mitsuba", [MITSUBA_LIB, LIB],
                             ANARI_MITSUBA_VARIANT="scalar_rgb"),
    # openmoonray-anari with the MoonRay runtime next to it (install/moonray).
    "moonray": config("moonray", [MOONRAY_LIB, LIB]),
}

# Not in the default matrix (select them with -r): the slow scalar variant of Mitsuba, and
# Radeon ProRender on the GPU, which does not finish a frame on this machine (every sample
# runs into the timeout), see doc/anari/MACOS.md.
NOT_DEFAULT = {"mitsuba_scalar", "rpr"}


def installed(name):
    library, directories, _ = run_all.RENDERERS[name]
    return any(os.path.exists(f"{d}/libanari_library_{library}.dylib") for d in directories)


run_all.installed = installed
run_all.DEFAULT_RENDERERS = [r for r in run_all.RENDERERS
                             if r not in NOT_DEFAULT and installed(r)]


def prepare():
    os.makedirs(run_all.OUT, exist_ok=True)
    os.makedirs(HOME, exist_ok=True)
    # The headless drivers of the viewer and of the interactive sample are used as they are.
    for name in ("viewer_headless.py", "interactive_headless.py"):
        shutil.copyfile(f"{TOOLS}/{name}", f"{run_all.OUT}/{name}")
    # The launcher of Windows registers DLL directories, which only exists there.
    with open(run_all.LAUNCH, "w") as f:
        f.write('''"""Launcher of run_pynari.py: runs a pynari sample as __main__.
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
''')


if __name__ == "__main__":
    prepare()
    if "-r" not in sys.argv and "--renderers" not in sys.argv and "--md-only" not in sys.argv:
        sys.argv[1:1] = ["-r", ",".join(run_all.DEFAULT_RENDERERS)]
    if sys.argv[1:] == ["--md-only"]:
        sys.exit("--md-only is not supported by this wrapper")
    run_all.main()
