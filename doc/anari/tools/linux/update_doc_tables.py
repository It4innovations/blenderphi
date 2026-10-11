"""Replace the generated tables of doc/anari/LINUX.md with the current ones.

  python update_doc_tables.py

Reads smoke-tables.md, pynari-table.md and compare-windows.md from build/logs-linux of the
workspace (fetched from the Linux workspace, or written by compare_all.py) and puts them in place
of the tables of sections 8 and 9. The text around the tables is not touched.
"""
import os

here = os.path.dirname(os.path.abspath(__file__))
doc = os.path.join(here, "..", "..", "LINUX.md")
# tools/linux -> tools -> anari -> doc -> blenderphi -> workspace
logs = os.path.normpath(os.path.join(here, "..", "..", "..", "..", "..", "build", "logs-linux"))


def read(name):
    with open(os.path.join(logs, name), encoding="utf-8") as f:
        return f.read().replace("\r\n", "\n").strip("\n")


def replace_between(text, start, end, new, keep_start=True):
    """Replace what lies between the markers `start` and `end` (both stay)."""
    i = text.index(start) + (len(start) if keep_start else 0)
    j = text.index(end, i)
    return text[:i] + new + text[j:]


with open(doc, encoding="utf-8", newline="") as f:
    s = f.read().replace("\r\n", "\n")

# Section 8: the comparison table, from its header line to the first bullet after it.
start = "| Linux | Windows log |"
i = s.index(start)
j = s.index("\n\n* ", i)
s = s[:i] + read("compare-windows.md") + s[j:]

# Section 8: the three result tables.
i = s.index("### Test status")
j = s.index("## 9. pynari results")
s = s[:i] + read("smoke-tables.md") + "\n\n" + s[j:]

# Section 9: the sample matrix, from its header line to the line with the total.
i = s.index("| sample | ")
j = s.index("runs passed.", i) + len("runs passed.")
s = s[:i] + read("pynari-table.md") + s[j:]

with open(doc, "w", encoding="utf-8", newline="\n") as f:
    f.write(s)
print("updated", os.path.normpath(doc))
