"""Markdown tables of the metrics printed by anari_smoke_tests.py.

  python smoke_tables.py <logs dir>   (reads smoke-final-<config>.txt)
"""
import os
import re
import sys

logs = sys.argv[1]
CONFIGS = ("cycles", "cycles_optix", "barney", "visrtx", "visrtx_quality", "mitsuba", "moonray",
           "helide", "visionaray", "visionaray_cuda", "ospray", "rpr", "photon", "photon_cpu")
TITLES = {"cycles": "cycles (CPU)", "cycles_optix": "cycles (OptiX)", "visrtx": "visrtx (default)",
          "visrtx_quality": "visrtx (quality)", "mitsuba": "mitsuba (cuda)",
          "visionaray": "visionaray (CPU)", "visionaray_cuda": "visionaray (CUDA)",
          "photon": "photon (CUDA)", "photon_cpu": "photon (CPU)"}
# Configurations without a log are left out of the tables.
CONFIGS = tuple(c for c in CONFIGS
                if os.path.exists(os.path.join(logs, "smoke-final-{:s}.txt".format(c))))
TESTS = ("test_build_options", "test_engine_registered", "test_basic_render", "test_texture",
         "test_texture_mapping", "test_lights", "test_environment_map", "test_background",
         "test_object_types", "test_instancing")

number = r"([0-9.]+)"
patterns = [
    ("basic render", re.compile(r"Cycles mean .*brightness ratio " + number + ", relative difference " + number)),
    ("texture mapping", re.compile(r"texture mapping pattern agreement " + number)),
    ("point light", re.compile(r"POINT light brightness ratio " + number + ", relative difference " + number)),
    ("spot light", re.compile(r"SPOT light brightness ratio " + number + ", relative difference " + number)),
    ("area light", re.compile(r"AREA light brightness ratio " + number + ", relative difference " + number)),
    ("sun light", re.compile(r"SUN light brightness ratio " + number + ", relative difference " + number)),
    ("environment 0 deg", re.compile(r"environment rotated 0: brightness ratio " + number + ", relative difference " + number)),
    ("environment 90 deg", re.compile(r"environment rotated 90: brightness ratio " + number + ", relative difference " + number)),
    ("instancing", re.compile(r"instancing relative difference " + number)),
]
objects = ("gn_instances", "gn_realized", "gn_points", "gn_curves", "gn_volume",
           "particle_instances", "particle_hair", "hair_curves", "text", "bevel_curve",
           "metaball", "subdivision")
for name in objects:
    patterns.append(("object " + name, re.compile(
        r"object " + name + ": coverage " + number + ", brightness ratio " + number +
        ", relative difference " + number)))

values = {}
status = {}
for config in CONFIGS:
    path = os.path.join(logs, "smoke-final-{:s}.txt".format(config))
    if not os.path.exists(path):
        continue
    text = open(path, encoding="utf-8", errors="replace").read()
    failed = set(re.findall(r"^FAIL: (test_\w+)", text, re.M)) | set(
        re.findall(r"^ERROR: (test_\w+)", text, re.M))
    status[config] = failed
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


out = ["### Test status\n\n", header()]
for test in TESTS:
    cells = []
    for config in CONFIGS:
        if config not in status:
            cells.append("–")
        else:
            cells.append("**fail**" if test in status[config] else "pass")
    out.append("| `{:s}` | {:s} |\n".format(test, " | ".join(cells)))

out.append("\n### Brightness ratio and relative difference against Blender Cycles\n\n")
out.append("Cells are *brightness ratio / relative difference*; for texture mapping the pattern "
           "agreement, for instancing the relative difference.\n\n")
out.append(header())
for key, _ in patterns:
    if key.startswith("object "):
        continue
    cells = []
    for config in CONFIGS:
        v = values.get((key, config))
        cells.append(" / ".join(v) if v else "–")
    out.append("| {:s} | {:s} |\n".format(key, " | ".join(cells)))

out.append("\n### Object types\n\n")
out.append("Cells are *mask coverage / brightness ratio / relative difference*.\n\n")
out.append(header())
for name in objects:
    cells = []
    for config in CONFIGS:
        v = values.get(("object " + name, config))
        cells.append(" / ".join(v) if v else "–")
    out.append("| {:s} | {:s} |\n".format(name, " | ".join(cells)))
print("".join(out))
