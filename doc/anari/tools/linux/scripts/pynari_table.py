"""Markdown matrix of the pynari results of scripts/run_pynari.sh.

  python3 scripts/pynari_table.py [results-a.json results-b.json ...]
                                  (default: build/pynari-out/results*.json)

The Linux counterpart of pynari/doc/windows/tools/pynari_table.py, with the
configurations of scripts/run_pynari.py. A cell is the status and the time of the run;
an image which looks empty is marked (BLACK, FLAT).
"""
import glob
import json
import os
import sys

CONFIGS = ("helide", "visrtx", "barney", "cycles_optix", "cycles_cuda", "cycles_cpu",
           "mitsuba_cuda", "mitsuba_llvm", "mitsuba_scalar", "moonray", "visionaray",
           "visionaray_cuda", "ospray", "rpr", "rpr_cpu", "photon", "photon_cpu")

here = os.path.dirname(os.path.abspath(__file__))
paths = sys.argv[1:] or sorted(glob.glob(os.path.join(here, "..", "build", "pynari-out",
                                                      "results*.json")))
results = {}
for path in paths:
    with open(path) as f:
        for r in json.load(f):
            if r["renderer"] in CONFIGS:
                results[(r["sample"], r["renderer"])] = r
# Configurations without results are left out.
CONFIGS = tuple(c for c in CONFIGS if any(config == c for _, config in results))

samples = sorted({s for s, _ in results})
lines = ["| sample | " + " | ".join(CONFIGS) + " |",
         "|---|" + "---|" * len(CONFIGS)]
passed = total = 0
for sample in samples:
    cells = []
    for config in CONFIGS:
        r = results.get((sample, config))
        if r is None:
            cells.append("–")
            continue
        total += 1
        ok = r["status"] == "PASS"
        passed += ok
        cell = "{:s} {:.0f}s".format("ok" if ok else "**" + r["status"] + "**", r["time"])
        if ok and r.get("verdict") not in (None, "ok"):
            cell += " ({:s})".format(r["verdict"])
        cells.append(cell)
    lines.append("| {:s} | {:s} |".format(sample, " | ".join(cells)))
print("\n".join(lines))
print("\n{:d} of {:d} runs passed.".format(passed, total))
