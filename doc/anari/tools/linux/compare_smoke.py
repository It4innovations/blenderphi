"""Compare the metrics of two smoke test logs (anari_smoke_tests.py), number by number.

  python compare_smoke.py <reference log> <log> [--tolerance 0.0005] [--all]

Every line `ANARI <library>: ...` is split into its text and its numbers. Lines are
matched by their text; the largest absolute difference of the numbers of each line is
printed for the lines which differ by more than the tolerance (or for all with --all),
and the tests which passed or failed differently are listed.

Used to compare the Linux runs (logs fetched from build/logs of the Linux workspace)
with the Windows runs in F:/work/anari/build/logs.
"""
import argparse
import re

NUMBER = re.compile(r"-?\d+\.\d+(?:e-?\d+)?|-?\d+")


def metrics(path):
    values = {}
    status = {}
    summary = ""
    text = open(path, encoding="utf-8", errors="replace").read()
    for line in text.splitlines():
        m = re.match(r"^(test_\w+) \(.*\) \.\.\. (.*)$", line)
        if m:
            status.setdefault(m.group(1), m.group(2).strip())
        m = re.match(r"^(FAIL|ERROR): (test_\w+)", line)
        if m:
            status[m.group(2)] = m.group(1)
        if re.match(r"^(Ran \d+ tests?|OK|FAILED)", line):
            summary += re.sub(r" in [0-9.]+s", "", line.strip()) + " "
        if not line.startswith("ANARI "):
            continue
        body = line.split(": ", 1)[1] if ": " in line else line
        key = NUMBER.sub("#", body)
        values[key] = [float(n) for n in NUMBER.findall(body)]
    return values, status, summary.strip()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("reference")
    ap.add_argument("log")
    ap.add_argument("--tolerance", type=float, default=0.0005)
    ap.add_argument("--all", action="store_true")
    a = ap.parse_args()
    ref, ref_status, ref_summary = metrics(a.reference)
    new, new_status, new_summary = metrics(a.log)
    print("reference: {:s}\n      log: {:s}".format(ref_summary, new_summary))
    failed_ref = sorted(t for t, s in ref_status.items() if s in ("FAIL", "ERROR"))
    failed_new = sorted(t for t, s in new_status.items() if s in ("FAIL", "ERROR"))
    print("failed in the reference: {:s}".format(", ".join(failed_ref) or "-"))
    print("failed in the log:       {:s}".format(", ".join(failed_new) or "-"))
    worst = 0.0
    count = 0
    for key in ref:
        if key not in new:
            print("only in the reference: {:s}".format(key))
            continue
        a_values, b_values = ref[key], new[key]
        if len(a_values) != len(b_values):
            print("different shape: {:s}".format(key))
            continue
        diff = max((abs(x - y) for x, y in zip(a_values, b_values)), default=0.0)
        worst = max(worst, diff)
        count += 1
        if diff > a.tolerance or a.all:
            print("{:8.4f}  {:s}\n          ref {}\n          new {}".format(
                diff, key, a_values, b_values))
    for key in new:
        if key not in ref:
            print("only in the log: {:s} {}".format(key, new[key]))
    print("{:d} lines compared, largest difference {:.4f}".format(count, worst))


if __name__ == "__main__":
    main()
