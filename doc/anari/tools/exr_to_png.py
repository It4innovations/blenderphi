"""Convert the EXR images written by anari_smoke_tests.py to sRGB PNGs.

Run with Blender (EXR support):
  blender --background --factory-startup -noaudio --python exr_to_png.py -- <test_out dir> <png dir>

Every <test_out>/<renderer>/<scene>_<renderer>.exr becomes <png dir>/<renderer>/<scene>.png and
the Cycles reference (<scene>_<renderer>_reference.exr, identical for all renderers) becomes
<png dir>/reference/<scene>.png. Pixels are composited over a mid gray checkerboard (so
transparency is visible), clamped and sRGB encoded.
"""
import glob
import os
import sys

import bpy
import numpy as np

args = sys.argv[sys.argv.index("--") + 1:]
src_root, dst_root = os.path.abspath(args[0]), os.path.abspath(args[1])
# test_out sub-directory -> ANARI library name used in the file names
renderers = {"cycles": "cycles", "cycles_optix": "cycles", "barney": "barney", "visrtx": "visrtx",
             "visrtx_quality": "visrtx", "mitsuba": "mitsuba", "moonray": "moonray",
             "helide": "helide", "visionaray": "visionaray", "visionaray_cuda": "visionaray_cuda",
             "ospray": "ospray", "rpr": "rpr", "photon": "photon",
             "photon_cpu": "photon"}


def srgb(linear):
    linear = np.clip(linear, 0.0, 1.0)
    return np.where(linear <= 0.0031308, linear * 12.92, 1.055 * np.power(linear, 1.0 / 2.4) - 0.055)


def convert(src, dst):
    image = bpy.data.images.load(src)
    width, height = image.size
    pixels = np.empty(width * height * 4, dtype=np.float32)
    image.pixels.foreach_get(pixels)
    bpy.data.images.remove(image)
    pixels = pixels.reshape(height, width, 4)

    yy, xx = np.mgrid[0:height, 0:width]
    checker = np.where(((xx // 8) + (yy // 8)) % 2 == 0, 0.35, 0.25)[..., None]
    alpha = np.clip(pixels[..., 3:4], 0.0, 1.0)
    rgb = srgb(pixels[..., :3]) * alpha + checker * (1.0 - alpha)

    out = bpy.data.images.new("png", width, height, alpha=False, float_buffer=False)
    rgba = np.concatenate([rgb, np.ones((height, width, 1))], axis=2).astype(np.float32)
    out.pixels.foreach_set(rgba.ravel())
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    out.filepath_raw = dst
    out.file_format = 'PNG'
    out.save()
    bpy.data.images.remove(out)


count = 0
for renderer, library in renderers.items():
    for src in sorted(glob.glob(os.path.join(src_root, renderer, "*_{:s}.exr".format(library)))):
        scene = os.path.basename(src)[:-len("_{:s}.exr".format(library))]
        convert(src, os.path.join(dst_root, renderer, scene + ".png"))
        count += 1
        reference = src[:-len(".exr")] + "_reference.exr"
        reference_png = os.path.join(dst_root, "reference", scene + ".png")
        if os.path.exists(reference) and not os.path.exists(reference_png):
            convert(reference, reference_png)
            count += 1
print("converted {:d} images".format(count))
