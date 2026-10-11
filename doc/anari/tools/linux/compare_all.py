"""Summary of compare_smoke.py for every Linux configuration against the Windows logs.

  python compare_all.py [<Linux logs dir>] [<Windows logs dir>]

For each configuration: the date of the Windows log, the largest difference of the
metrics which the README tables show (basic render, texture mapping, lights,
environment, instancing, object types, lens shift, clip start, emissive room, light
falloff), and the other printed
metrics which differ by more than 0.02 (shader graph, normal map, denoiser, ...). The
Windows logs are from different days, so the second group also shows changes of the
exporter between the runs.
"""
import os
import sys
import time

import compare_smoke

here = os.path.dirname(os.path.abspath(__file__))
# tools/linux -> tools -> anari -> doc -> blenderphi -> workspace
workspace = os.path.normpath(os.path.join(here, "..", "..", "..", "..", ".."))
linux = sys.argv[1] if len(sys.argv) > 1 else os.path.join(workspace, "build", "logs-linux")
windows = sys.argv[2] if len(sys.argv) > 2 else os.path.join(workspace, "build", "logs")

PAIRS = [("cycles", "cycles"), ("cycles_optix", "cycles_optix"), ("cycles_cuda", "cycles_optix"),
         ("helide", "helide"), ("barney", "barney"), ("visrtx", "visrtx"),
         ("visrtx_quality", "visrtx_quality"), ("visionaray", "visionaray"),
         ("visionaray_cuda", "visionaray_cuda"), ("ospray", "ospray"), ("rpr", "rpr"),
         ("rpr_cpu", "rpr"), ("photon", "photon"), ("photon_cpu", "photon_cpu"),
         ("mitsuba", "mitsuba"), ("mitsuba_llvm", "mitsuba"),
         ("moonray", "moonray"), ("moonray_xpu", "moonray")]
TABLE = ("mean [", "texture mapping", "light brightness", "environment rotated", "instancing",
         "object ", "lens shift", "clip start", "emissive room", "light falloff")

print("| Linux | Windows log | table metrics: lines, largest difference | other metrics which differ |")
print("|---|---|---|---|")
for name, ref_name in PAIRS:
    a = os.path.join(windows, "smoke-final-{:s}.txt".format(ref_name))
    b = os.path.join(linux, "smoke-{:s}.txt".format(name))
    if not (os.path.exists(a) and os.path.exists(b)):
        continue
    ref, _, _ = compare_smoke.metrics(a)
    new, _, _ = compare_smoke.metrics(b)
    worst, where, count, others = 0.0, "", 0, []
    for key in ref:
        if key not in new or len(ref[key]) != len(new[key]):
            continue
        # Pixel counts are not ratios.
        if key.startswith("red pixels"):
            continue
        diff = max((abs(x - y) for x, y in zip(ref[key], new[key])), default=0.0)
        if any(key.startswith(t) or (t == "light brightness" and t in key) for t in TABLE):
            count += 1
            if diff > worst:
                worst, where = diff, key.split(":")[0].replace(" #", "").strip()
        elif diff > 0.02:
            others.append("{:s} {:.3f}".format(key.split(" relative")[0].split(":")[0].replace(" #", "").strip(), diff))
    date = time.strftime("%m-%d %H:%M", time.localtime(os.path.getmtime(a)))
    print("| {:s} | {:s}, {:s} | {:d}, {:.3f}{:s} | {:s} |".format(
        name, ref_name, date, count, worst, " ({:s})".format(where) if worst > 0.02 else "",
        "; ".join(others) or "-"))
