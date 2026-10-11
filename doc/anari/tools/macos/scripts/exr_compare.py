"""Compare two EXR images: mean of each and the relative RMS difference.

  bash scripts/blender.sh --background --factory-startup -noaudio \
      --python scripts/exr_compare.py -- <a.exr> <b.exr> [<a.exr> <b.exr> ...]

Used to compare renders of the same scene on two compute devices (CPU and Metal).
"""
import sys

import bpy
import numpy as np


def load(path):
    image = bpy.data.images.load(path)
    width, height = image.size
    pixels = np.empty(width * height * 4, dtype=np.float32)
    image.pixels.foreach_get(pixels)
    bpy.data.images.remove(image)
    return pixels.reshape(height, width, 4)[:, :, :3]


args = sys.argv[sys.argv.index("--") + 1:]
for a_path, b_path in zip(args[0::2], args[1::2]):
    a, b = load(a_path), load(b_path)
    difference = np.sqrt(np.mean((a - b) ** 2)) / max(float(np.sqrt(np.mean(b ** 2))), 1e-12)
    print("COMPARE {:s} | {:s}: mean {:.5f} / {:.5f}, relative RMS difference {:.4f}".format(
        a_path.split("/test_out/")[-1], b_path.split("/test_out/")[-1],
        float(a.mean()), float(b.mean()), float(difference)))
