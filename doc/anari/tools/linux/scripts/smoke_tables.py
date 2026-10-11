"""Markdown tables of the metrics printed by anari_smoke_tests.py, for the Linux runs.

  python3 scripts/smoke_tables.py [<logs dir>] [<configuration> ...]
                                  (reads smoke-<configuration>.txt, default: build/logs
                                   of this workspace and every configuration with a log)

The Linux counterpart of blenderphi/doc/anari/tools/smoke_tables.py: the same metrics,
the configurations of scripts/run_smoke.sh, and every test of the suite in the status table.
"""
import os
import re
import sys

here = os.path.dirname(os.path.abspath(__file__))
logs = sys.argv[1] if len(sys.argv) > 1 else os.path.join(here, "..", "build", "logs")

CONFIGS = ("cycles", "cycles_optix", "cycles_cuda", "helide", "barney", "visrtx",
           "visrtx_quality", "visionaray", "visionaray_cuda", "ospray", "rpr", "rpr_cpu",
           "photon", "photon_cpu", "mitsuba", "mitsuba_llvm", "moonray", "moonray_xpu")
TITLES = {"cycles": "cycles (CPU)", "cycles_optix": "cycles (OptiX)", "cycles_cuda": "cycles (CUDA)",
          "visrtx": "visrtx (default)", "visrtx_quality": "visrtx (quality)",
          "visionaray": "visionaray (CPU)", "visionaray_cuda": "visionaray (CUDA)",
          "rpr": "rpr (default)", "rpr_cpu": "rpr (CPU)", "photon": "photon (CUDA)",
          "photon_cpu": "photon (CPU)", "mitsuba": "mitsuba (cuda)",
          "mitsuba_llvm": "mitsuba (llvm)", "moonray": "moonray (CPU)",
          "moonray_xpu": "moonray (XPU)"}
if len(sys.argv) > 2:
    CONFIGS = tuple(sys.argv[2:])


def log_path(config):
    return os.path.join(logs, "smoke-{:s}.txt".format(config))


# Configurations without a log are left out of the tables.
CONFIGS = tuple(c for c in CONFIGS if os.path.exists(log_path(c)))

number = r"([0-9.]+)"
ratio_difference = "brightness ratio " + number + ", relative difference " + number
patterns = [
    ("basic render", re.compile(r"Cycles mean .*" + ratio_difference)),
    ("texture mapping", re.compile(r"texture mapping pattern agreement " + number)),
    ("point light", re.compile(r"POINT light " + ratio_difference)),
    ("spot light", re.compile(r"SPOT light " + ratio_difference)),
    ("area light", re.compile(r"AREA light " + ratio_difference)),
    ("sun light", re.compile(r"SUN light " + ratio_difference)),
    ("environment 0 deg", re.compile(r"environment rotated 0: " + ratio_difference)),
    ("environment 90 deg", re.compile(r"environment rotated 90: " + ratio_difference)),
    ("instancing", re.compile(r"instancing relative difference " + number)),
    ("lens shift", re.compile(r"lens shift relative difference " + number)),
    ("clip start", re.compile(r"clip start " + ratio_difference)),
    ("normal map", re.compile(r"normal map " + ratio_difference)),
    ("world for camera rays", re.compile(r"world for camera rays: " + ratio_difference)),
]
objects = ("gn_instances", "gn_realized", "gn_points", "gn_curves", "gn_volume",
           "particle_instances", "particle_hair", "hair_curves", "text", "bevel_curve",
           "metaball", "subdivision")
for name in objects:
    patterns.append(("object " + name, re.compile(
        r"object " + name + ": coverage " + number + ", " + ratio_difference)))

values = {}
failed = {}
ran = {}
summary = {}
tests = []
for config in CONFIGS:
    text = open(log_path(config), encoding="utf-8", errors="replace").read()
    failed[config] = set(re.findall(r"^(?:FAIL|ERROR): (test_\w+)", text, re.M))
    ran[config] = set(re.findall(r"^(test_\w+) \(", text, re.M))
    for test in sorted(ran[config]):
        if test not in tests:
            tests.append(test)
    m = re.search(r"^Ran (\d+) tests? in ([0-9.]+)s\s+^(OK|FAILED.*)$", text, re.M)
    if m:
        summary[config] = "{:s} tests, {:s} s, {:s}".format(m.group(1), m.group(2), m.group(3))
    elif "TIMEOUT" in text:
        summary[config] = "timeout"
    else:
        summary[config] = "did not finish (crash)"
    for line in text.splitlines():
        if not line.startswith("ANARI "):
            continue
        for key, pattern in patterns:
            m = pattern.search(line)
            if m:
                values[(key, config)] = m.groups()


def header():
    names = [TITLES.get(c, c) for c in CONFIGS]
    return "| | " + " | ".join(names) + " |\n|---|" + "---|" * len(CONFIGS) + "\n"


def rows(keys, label=lambda key: key):
    lines = []
    for key in keys:
        cells = []
        for config in CONFIGS:
            v = values.get((key, config))
            cells.append(" / ".join(v) if v else "–")
        lines.append("| {:s} | {:s} |\n".format(label(key), " | ".join(cells)))
    return lines


out = ["### Test status\n\n", header()]
for test in sorted(tests):
    cells = []
    for config in CONFIGS:
        if test not in ran[config]:
            cells.append("–")
        else:
            cells.append("**fail**" if test in failed[config] else "pass")
    out.append("| `{:s}` | {:s} |\n".format(test, " | ".join(cells)))
out.append("| result | {:s} |\n".format(" | ".join(summary[c] for c in CONFIGS)))

out.append("\n### Brightness ratio and relative difference against Blender Cycles\n\n")
out.append("Cells are *brightness ratio / relative difference*; for texture mapping the pattern "
           "agreement, for instancing and the lens shift the relative difference.\n\n")
out.append(header())
out.extend(rows([key for key, _ in patterns if not key.startswith("object ")]))

out.append("\n### Object types\n\n")
out.append("Cells are *mask coverage / brightness ratio / relative difference*.\n\n")
out.append(header())
out.extend(rows(["object " + name for name in objects], lambda key: key[len("object "):]))
sys.stdout.write("".join(out))
