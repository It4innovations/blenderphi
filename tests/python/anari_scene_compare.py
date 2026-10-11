# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""
Render a .blend file with Cycles and with an ANARI device and compare the images.

The ANARI device renders the scene twice: with the shader graphs sent to the device ("auto")
and with all materials converted to one physically based material ("flat"). Usage:

    blender --background --factory-startup --disable-autoexec -noaudio \
        --python tests/python/anari_scene_compare.py -- \
        --blend scene.blend --library cycles --library-path <dir with the ANARI libraries> \
        --outdir <dir> [--device-parameters "computeDevice=optix"] [--samples 64] [--width 480]

The Cycles render is kept in the output directory and used again by later runs with the same
size and sample count. For every mode a line starting with "RESULT" is printed with the metrics
as JSON, and a PNG with the reference, the ANARI render and their difference side by side is
written.
"""

import argparse
import json
import os
import sys
import time

import bpy
import numpy as np


def parse_arguments(argv):
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[1])
    parser.add_argument("--blend", required=True, help="File to render")
    parser.add_argument("--library", required=True, help="ANARI library (cycles, rpr, moonray, ...)")
    parser.add_argument("--library-path", action="append", default=[],
                        help="Directory with ANARI libraries, can be given multiple times")
    parser.add_argument("--device-parameters", default="",
                        help="ANARI device parameters, name=value pairs separated by ';'")
    parser.add_argument("--modes", default="auto,flat",
                        help="What to render and compare with the Cycles reference: auto (ANARI with "
                             "shader graphs), flat (ANARI with flattened materials), noise (Cycles "
                             "with another seed, the noise level of the metrics)")
    parser.add_argument("--samples", type=int, default=64)
    parser.add_argument("--width", type=int, default=480, help="Width of the render in pixels")
    parser.add_argument("--outdir", required=True)
    parser.add_argument("--name", default="", help="Name of the scene in the output, default: file name")
    parser.add_argument("--unlink-nodes", default="",
                        help="Node types (bl_idname, comma separated) whose output links are removed "
                             "in all materials before rendering, to find the nodes behind a difference")
    parser.add_argument("--sheet-exposure", type=float, default=0.0,
                        help="Exposure of the comparison PNG in stops, the metrics are not affected")
    parser.add_argument("--override-material", action="store_true",
                        help="Render all objects with one gray diffuse material, to compare lights, "
                             "geometry and the integrator without the materials of the scene")
    parser.add_argument("--setup", default="",
                        help="Python statements run after the file is loaded, with `scene` and `bpy` "
                             "defined: changes to the scene for an experiment. Needs --tag")
    parser.add_argument("--tag", default="",
                        help="Suffix of the output file names, to keep the results of an experiment "
                             "apart from the ones of the unchanged scene")
    return parser.parse_args(argv)


def find_device(library):
    import _cycles
    for device in _cycles.anari_devices():
        if device[2].split(",")[0] == library:
            return device
    return None


def setup_scene(scene, args):
    render = scene.render
    aspect = (render.resolution_y * render.pixel_aspect_y) / (render.resolution_x * render.pixel_aspect_x)
    render.resolution_x = args.width
    render.resolution_y = max(int(round(args.width * aspect)), 1)
    render.resolution_percentage = 100
    render.pixel_aspect_x = render.pixel_aspect_y = 1.0
    render.use_border = False
    # Compare what the renderers deliver, not what the scene does with it afterwards.
    render.use_compositing = False
    render.use_sequencer = False
    render.use_motion_blur = False
    render.image_settings.file_format = 'OPEN_EXR'
    render.image_settings.color_depth = '32'

    cscene = scene.cycles
    cscene.samples = args.samples
    cscene.use_adaptive_sampling = False
    cscene.use_denoising = False
    cscene.device = 'CPU'


def render_scene(scene, filepath):
    """Render the scene to an EXR file, returns the linear RGBA pixels and the render time."""
    scene.render.filepath = filepath
    start = time.time()
    bpy.ops.render.render(write_still=True)
    seconds = time.time() - start
    return load_pixels(filepath), seconds


def load_pixels(filepath):
    image = bpy.data.images.load(filepath, check_existing=False)
    width, height = image.size
    pixels = np.empty(width * height * 4, dtype=np.float32)
    image.pixels.foreach_get(pixels)
    bpy.data.images.remove(image)
    return pixels.reshape(height, width, 4)


def luminance(pixels):
    return 0.2126 * pixels[..., 0] + 0.7152 * pixels[..., 1] + 0.0722 * pixels[..., 2]


def block_means(values, block=8):
    height, width = values.shape
    height -= height % block
    width -= width % block
    cropped = values[:height, :width]
    return cropped.reshape(height // block, block, width // block, block).mean(axis=(1, 3))


def compare(pixels, reference):
    """Brightness ratio and relative RMS difference of the block averaged luminance: the
    averaging makes the comparison tolerant to noise."""
    finite = np.isfinite(pixels[..., :3]).all(axis=2)
    clean = np.where(finite[..., None], pixels, 0.0)
    a = block_means(luminance(clean))
    b = block_means(luminance(reference))
    difference = float(np.sqrt(((a - b) ** 2).sum() / max((b * b).sum(), 1e-12)))
    ratio = float(a.mean() / max(b.mean(), 1e-12))
    # The same on the colors, to see wrong hues which keep the luminance.
    color = float(np.sqrt(sum(((block_means(clean[..., c]) - block_means(reference[..., c])) ** 2).sum()
                              for c in range(3)) /
                          max(sum((block_means(reference[..., c]) ** 2).sum() for c in range(3)), 1e-12)))
    return {
        "brightness_ratio": round(ratio, 4),
        "relative_difference": round(difference, 4),
        "color_difference": round(color, 4),
        "non_finite_pixels": int((~finite).sum()),
    }


def to_display(pixels):
    """Linear to sRGB, clipped, for the comparison sheet."""
    rgb = np.clip(pixels[..., :3], 0.0, 1.0)
    return np.where(rgb <= 0.0031308, rgb * 12.92, 1.055 * np.power(rgb, 1.0 / 2.4) - 0.055)


def save_sheet(filepath, images):
    """Save the images side by side as a PNG."""
    sheet = np.concatenate(images, axis=1)
    height, width = sheet.shape[:2]
    rgba = np.ones((height, width, 4), dtype=np.float32)
    rgba[..., :3] = sheet
    image = bpy.data.images.new("sheet", width, height, alpha=False)
    image.pixels.foreach_set(rgba.ravel())
    image.filepath_raw = filepath
    image.file_format = 'PNG'
    image.save()
    bpy.data.images.remove(image)


def main():
    argv = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
    args = parse_arguments(argv)
    name = args.name or os.path.splitext(os.path.basename(args.blend))[0]
    outdir = os.path.join(args.outdir, name)
    os.makedirs(outdir, exist_ok=True)

    # Scripts of the file are not run: it may come from anywhere.
    bpy.ops.wm.open_mainfile(filepath=args.blend, load_ui=False, use_scripts=False)

    import addon_utils
    addon_utils.enable("cycles", default_set=True)
    addon_utils.enable("anari", default_set=True)
    bpy.context.preferences.addons["anari"].preferences.library_search_paths = ";".join(args.library_path)

    device = find_device(args.library)
    if device is None:
        print("ANARI library \"{:s}\" not found".format(args.library))
        sys.exit(1)

    scene = bpy.context.scene
    setup_scene(scene, args)
    size = "{:d}x{:d}_{:d}spp".format(scene.render.resolution_x, scene.render.resolution_y, args.samples)
    if args.unlink_nodes:
        types = set(args.unlink_nodes.split(","))
        trees = [m.node_tree for m in bpy.data.materials if m.node_tree]
        trees += [g for g in bpy.data.node_groups if g.bl_idname == 'ShaderNodeTree']
        for tree in trees:
            for node in tree.nodes:
                if node.bl_idname in types:
                    for output in node.outputs:
                        for link in list(output.links):
                            tree.links.remove(link)
        size += "_without_" + "_".join(sorted(t.replace("ShaderNode", "") for t in types))
    if args.override_material:
        material = bpy.data.materials.new("Override")
        tree = material.node_tree
        bsdf = tree.nodes.get("Principled BSDF")
        diffuse = tree.nodes.new("ShaderNodeBsdfDiffuse")
        diffuse.inputs["Color"].default_value = (0.5, 0.5, 0.5, 1.0)
        tree.links.new(diffuse.outputs["BSDF"], bsdf.outputs["BSDF"].links[0].to_socket)
        for view_layer in scene.view_layers:
            view_layer.material_override = material
        size += "_override"
    if args.setup:
        if not args.tag:
            print("--setup needs --tag")
            sys.exit(1)
        exec(args.setup, {"bpy": bpy, "scene": scene})
    if args.tag:
        size += "_" + args.tag

    reference_path = os.path.join(outdir, "cycles_reference_{:s}.exr".format(size))
    if os.path.exists(reference_path):
        reference = load_pixels(reference_path)
        reference_seconds = None
    else:
        scene.render.engine = 'CYCLES'
        reference, reference_seconds = render_scene(scene, reference_path)

    for mode in args.modes.split(","):
        parameters = args.device_parameters
        if mode == "flat":
            parameters = "materials=physicallyBased;" + parameters
        if mode == "noise":
            # Cycles again with another seed: the difference is the noise level of the metrics.
            scene.render.engine = 'CYCLES'
            scene.cycles.seed += 1
        else:
            scene.render.engine = 'ANARI'
            scene.anari.device_id = device[0]
            scene.anari.device_parameters = parameters

        filepath = os.path.join(outdir, "{:s}_{:s}_{:s}.exr".format(args.library, mode, size))
        pixels, seconds = render_scene(scene, filepath)

        result = compare(pixels, reference)
        result.update({
            "scene": name,
            "library": args.library,
            "mode": mode,
            "size": size,
            "seconds": round(seconds, 1),
            "reference_seconds": round(reference_seconds, 1) if reference_seconds else None,
        })
        print("RESULT " + json.dumps(result))

        gain = 2.0 ** args.sheet_exposure
        finite = np.where(np.isfinite(pixels), pixels, 0.0)
        difference = np.clip(np.abs(finite[..., :3] - reference[..., :3]) * gain * 2.0, 0.0, 1.0)
        save_sheet(os.path.join(outdir, "{:s}_{:s}_{:s}.png".format(args.library, mode, size)),
                   [to_display(reference * gain), to_display(finite * gain), difference])


if __name__ == "__main__":
    main()
