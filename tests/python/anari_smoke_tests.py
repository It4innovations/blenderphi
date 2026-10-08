# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""
Smoke tests for the ANARI render engine.

The ANARI engine synchronizes the scene with Cycles and renders it with an ANARI library
(Barney, Cycles-ANARI, Mitsuba-ANARI, ...). These tests build small scenes procedurally,
render them with the given ANARI library and check that the result is sane. Renders are
compared against the internal Cycles render of the same scene: strictly for the Cycles-ANARI
library (same renderer), loosely for other libraries.

Usage:
  blender --background --factory-startup --python tests/python/anari_smoke_tests.py -- \\
      --library barney --library-path /path/to/anari/bin --outdir /tmp/anari_tests
"""

__all__ = (
    "main",
)

import argparse
import math
import os
import sys
import unittest

import bpy

args = None

# Image size and samples are kept small, these are smoke tests.
RESOLUTION = (160, 120)


def parse_arguments(argv):
    parser = argparse.ArgumentParser(description="ANARI render engine smoke tests")
    parser.add_argument("--library", required=True,
                        help="ANARI library to test (barney, cycles, mitsuba, helide, ...)")
    parser.add_argument("--library-path", action="append", default=[],
                        help="Directory with ANARI libraries, can be given multiple times")
    parser.add_argument("--device-parameters", default="",
                        help="ANARI device parameters, name=value pairs separated by ';'")
    parser.add_argument("--samples", type=int, default=32)
    parser.add_argument("--outdir", default="",
                        help="Directory to write the rendered images to")
    parser.add_argument("--strict", action="store_true",
                        help="Compare against Cycles with a strict threshold (implied for 'cycles')")
    parser.add_argument("tests", nargs="*",
                        help="Tests to run, e.g. AnariSmokeTest.test_lights (default: all)")
    return parser.parse_args(argv)


# --------------------------------------------------------------------
# Utilities.

def anari_devices():
    import _cycles
    if not getattr(_cycles, "with_anari", False):
        return ()
    return _cycles.anari_devices()


def find_device(library):
    """Device of the given library: (id, description, library, subtype)."""
    for device in anari_devices():
        name = device[2].split(",")[0]
        if name == library:
            return device
    return None


def setup_addon():
    import addon_utils
    addon_utils.enable("cycles", default_set=True)
    addon_utils.enable("anari", default_set=True)

    preferences = bpy.context.preferences.addons["anari"].preferences
    preferences.library_search_paths = ";".join(args.library_path)


def clear_scene():
    for obj in list(bpy.data.objects):
        bpy.data.objects.remove(obj, do_unlink=True)
    for collection in (bpy.data.meshes, bpy.data.materials, bpy.data.lights, bpy.data.cameras,
                       bpy.data.images, bpy.data.curves, bpy.data.pointclouds, bpy.data.hair_curves):
        for datablock in list(collection):
            collection.remove(datablock)


def setup_render(scene, engine):
    scene.render.engine = engine
    scene.render.resolution_x, scene.render.resolution_y = RESOLUTION
    scene.render.resolution_percentage = 100
    scene.render.film_transparent = False
    scene.render.image_settings.file_format = 'OPEN_EXR'
    scene.render.image_settings.color_depth = '32'
    scene.view_settings.view_transform = 'Standard'

    cscene = scene.cycles
    cscene.samples = args.samples
    cscene.use_adaptive_sampling = False
    cscene.use_denoising = False
    cscene.device = 'CPU'
    cscene.max_bounces = 4

    if engine == 'ANARI':
        device = find_device(args.library)
        scene.anari.device_id = device[0] if device else ""
        scene.anari.device_parameters = args.device_parameters


def new_material(name, color=(0.8, 0.8, 0.8), metallic=0.0, roughness=0.5,
                 emission=None, transmission=0.0, image=None):
    material = bpy.data.materials.new(name)
    tree = material.node_tree
    bsdf = tree.nodes.get("Principled BSDF")
    bsdf.inputs["Base Color"].default_value = (*color, 1.0)
    bsdf.inputs["Metallic"].default_value = metallic
    bsdf.inputs["Roughness"].default_value = roughness
    bsdf.inputs["Transmission Weight"].default_value = transmission
    if emission:
        bsdf.inputs["Emission Color"].default_value = (*emission[0], 1.0)
        bsdf.inputs["Emission Strength"].default_value = emission[1]
    if image:
        texture = tree.nodes.new("ShaderNodeTexImage")
        texture.image = image
        tree.links.new(texture.outputs["Color"], bsdf.inputs["Base Color"])
    return material


def checker_image(name, size=64, squares=8):
    image = bpy.data.images.new(name, size, size, alpha=False)
    pixels = []
    for y in range(size):
        for x in range(size):
            odd = ((x * squares // size) + (y * squares // size)) % 2
            pixels.extend((0.9, 0.1, 0.1, 1.0) if odd else (0.1, 0.1, 0.9, 1.0))
    image.pixels.foreach_set(pixels)
    image.pack()
    return image


def add_mesh(name, primitive, location, material, **kwargs):
    getattr(bpy.ops.mesh, primitive)(location=location, **kwargs)
    obj = bpy.context.active_object
    obj.name = name
    obj.data.materials.append(material)
    return obj


def add_camera(scene, location=(0.0, -7.0, 3.0), target=(0.0, 0.0, 0.6), lens=35.0):
    camera_data = bpy.data.cameras.new("Camera")
    camera_data.lens = lens
    camera = bpy.data.objects.new("Camera", camera_data)
    scene.collection.objects.link(camera)
    camera.location = location
    direction = (target[0] - location[0], target[1] - location[1], target[2] - location[2])
    from mathutils import Vector
    camera.rotation_euler = Vector(direction).to_track_quat('-Z', 'Y').to_euler()
    scene.camera = camera
    return camera


def add_light(scene, light_type, location, energy, size=1.0, rotation=(0.0, 0.0, 0.0)):
    light_data = bpy.data.lights.new(light_type.lower(), light_type)
    light_data.energy = energy
    if light_type == 'AREA':
        light_data.size = size
    elif light_type in {'POINT', 'SPOT'}:
        light_data.shadow_soft_size = size
    light = bpy.data.objects.new(light_type.lower(), light_data)
    light.location = location
    light.rotation_euler = rotation
    scene.collection.objects.link(light)
    return light


def set_world_color(scene, color, strength=1.0):
    world = scene.world or bpy.data.worlds.new("World")
    scene.world = world
    tree = world.node_tree
    background = tree.nodes.get("Background")
    for link in list(background.inputs["Color"].links):
        tree.links.remove(link)
    background.inputs["Color"].default_value = (*color, 1.0)
    background.inputs["Strength"].default_value = strength


def environment_image(name, width=64, height=32):
    """Equirectangular HDR image: blue sky, brown ground and a bright red patch around the
    horizontal center of the image (the -X direction in Blender)."""
    image = bpy.data.images.new(name, width, height, alpha=False, float_buffer=True)
    pixels = []
    for y in range(height):
        for x in range(width):
            if abs(x - width // 2) < width // 8 and abs(y - height // 2) < height // 4:
                pixels.extend((4.0, 0.2, 0.1, 1.0))
            elif y >= height // 2:
                pixels.extend((0.2, 0.4, 1.0, 1.0))
            else:
                pixels.extend((0.4, 0.3, 0.2, 1.0))
    image.pixels.foreach_set(pixels)
    image.pack()
    return image


def set_world_environment(scene, image, strength=1.0, rotation_z=0.0):
    """World lit by an environment texture, optionally rotated by a mapping node."""
    world = scene.world or bpy.data.worlds.new("World")
    scene.world = world
    tree = world.node_tree
    background = tree.nodes.get("Background")
    background.inputs["Strength"].default_value = strength
    texture = tree.nodes.new("ShaderNodeTexEnvironment")
    texture.image = image
    tree.links.new(texture.outputs["Color"], background.inputs["Color"])
    if rotation_z != 0.0:
        coordinates = tree.nodes.new("ShaderNodeTexCoord")
        mapping = tree.nodes.new("ShaderNodeMapping")
        mapping.inputs["Rotation"].default_value = (0.0, 0.0, rotation_z)
        tree.links.new(coordinates.outputs["Generated"], mapping.inputs["Vector"])
        tree.links.new(mapping.outputs["Vector"], texture.inputs["Vector"])
    return texture


def build_basic_scene(scene):
    """Ground plane with a checker texture, diffuse, metallic, glass and emissive objects lit by
    an area light, a sun and the world."""
    clear_scene()
    add_mesh("Ground", "primitive_plane_add", (0, 0, 0),
             new_material("Ground", image=checker_image("Checker")), size=8)
    add_mesh("Cube", "primitive_cube_add", (-1.6, 0.5, 0.6),
             new_material("Red", color=(0.8, 0.1, 0.1), roughness=0.6), size=1.2)
    sphere = add_mesh("Sphere", "primitive_uv_sphere_add", (0.2, 0.5, 0.7),
                      new_material("Metal", color=(0.9, 0.8, 0.5), metallic=1.0, roughness=0.2),
                      radius=0.7)
    bpy.ops.object.shade_smooth()
    add_mesh("Glass", "primitive_ico_sphere_add", (1.7, 0.2, 0.5),
             new_material("Glass", color=(1, 1, 1), roughness=0.0, transmission=1.0),
             radius=0.5, subdivisions=3)
    add_mesh("Emitter", "primitive_cube_add", (1.7, 1.8, 0.3),
             new_material("Emitter", color=(0, 0, 0), emission=((0.2, 1.0, 0.2), 5.0)), size=0.4)
    add_camera(scene)
    add_light(scene, 'AREA', (0.0, -2.0, 4.0), 400.0, size=2.0)
    add_light(scene, 'SUN', (0, 0, 5), 2.0, rotation=(math.radians(40), 0, math.radians(30)))
    set_world_color(scene, (0.05, 0.07, 0.1))
    del sphere


def render(scene, name):
    """Render the scene and return its linear RGBA pixels as a flat list."""
    outdir = args.outdir or bpy.app.tempdir
    os.makedirs(outdir, exist_ok=True)
    filepath = os.path.join(outdir, "{:s}.exr".format(name))
    scene.render.filepath = filepath
    bpy.ops.render.render(write_still=True)

    image = bpy.data.images.load(filepath, check_existing=False)
    pixels = [0.0] * (image.size[0] * image.size[1] * 4)
    image.pixels.foreach_get(pixels)
    size = tuple(image.size)
    bpy.data.images.remove(image)
    return pixels, size


def image_stats(pixels):
    n = len(pixels) // 4
    rgb_sum = [0.0, 0.0, 0.0]
    non_finite = 0
    for i in range(n):
        for c in range(3):
            v = pixels[i * 4 + c]
            if not math.isfinite(v):
                non_finite += 1
                continue
            rgb_sum[c] += v
    mean = [s / n for s in rgb_sum]
    return mean, non_finite


def block_means(pixels, size, block=8):
    """Downsampled luminance, used for a noise tolerant comparison."""
    width, height = size
    bw, bh = width // block, height // block
    result = []
    for by in range(bh):
        for bx in range(bw):
            total = 0.0
            for y in range(by * block, (by + 1) * block):
                for x in range(bx * block, (bx + 1) * block):
                    i = (y * width + x) * 4
                    total += 0.2126 * pixels[i] + 0.7152 * pixels[i + 1] + 0.0722 * pixels[i + 2]
            result.append(total / (block * block))
    return result


def pattern_agreement(pixels, reference, size, block=4):
    """Fraction of blocks in which the dominant color (red or blue) of both images agrees."""
    width, height = size
    agree = total = 0
    for by in range(height // block):
        for bx in range(width // block):
            sums = [[0.0, 0.0], [0.0, 0.0]]
            for y in range(by * block, (by + 1) * block):
                for x in range(bx * block, (bx + 1) * block):
                    i = (y * width + x) * 4
                    for k, image in enumerate((pixels, reference)):
                        sums[k][0] += image[i]
                        sums[k][1] += image[i + 2]
            if abs(sums[1][0] - sums[1][1]) < 0.1 * (sums[1][0] + sums[1][1]):
                continue  # Mixed block in the reference.
            total += 1
            agree += (sums[0][0] > sums[0][1]) == (sums[1][0] > sums[1][1])
    return agree / max(total, 1)


def corner_mean(pixels, size, extent=12):
    """Mean RGBA of the four image corners."""
    width, height = size
    result = [0.0, 0.0, 0.0, 0.0]
    count = 0
    for y0 in (0, height - extent):
        for x0 in (0, width - extent):
            for y in range(y0, y0 + extent):
                for x in range(x0, x0 + extent):
                    i = (y * width + x) * 4
                    for c in range(4):
                        result[c] += pixels[i + c]
                    count += 1
    return [v / count for v in result]


def relative_difference(a, b):
    """Relative RMS difference of the downsampled luminance of two images."""
    numerator = sum((x - y) ** 2 for x, y in zip(a, b))
    denominator = sum(y * y for y in b) or 1e-8
    return math.sqrt(numerator / denominator)


# --------------------------------------------------------------------
# Object types.

def geometry_nodes_modifier(obj, name, build):
    """Add a geometry nodes modifier, build(tree, input_socket) returns the output socket."""
    tree = bpy.data.node_groups.new(name, 'GeometryNodeTree')
    tree.interface.new_socket("Geometry", in_out='INPUT', socket_type='NodeSocketGeometry')
    tree.interface.new_socket("Geometry", in_out='OUTPUT', socket_type='NodeSocketGeometry')
    group_input = tree.nodes.new("NodeGroupInput")
    group_output = tree.nodes.new("NodeGroupOutput")
    output = build(tree, group_input.outputs[0])
    tree.links.new(output, group_output.inputs[0])
    modifier = obj.modifiers.new(name, 'NODES')
    modifier.node_group = tree
    return modifier


def set_material_node(tree, geometry, material):
    node = tree.nodes.new("GeometryNodeSetMaterial")
    node.inputs["Material"].default_value = material
    tree.links.new(geometry, node.inputs["Geometry"])
    return node.outputs["Geometry"]


def build_gn_instances(material, realize):
    bpy.ops.mesh.primitive_grid_add(x_subdivisions=4, y_subdivisions=4, size=2.5)
    obj = bpy.context.active_object

    def build(tree, geometry):
        points = tree.nodes.new("GeometryNodeMeshToPoints")
        tree.links.new(geometry, points.inputs["Mesh"])
        cube = tree.nodes.new("GeometryNodeMeshCube")
        cube.inputs["Size"].default_value = (0.35, 0.35, 0.35)
        instances = tree.nodes.new("GeometryNodeInstanceOnPoints")
        tree.links.new(points.outputs["Points"], instances.inputs["Points"])
        tree.links.new(set_material_node(tree, cube.outputs["Mesh"], material),
                       instances.inputs["Instance"])
        rotate = tree.nodes.new("GeometryNodeRotateInstances")
        rotate.inputs["Rotation"].default_value = (0.3, 0.2, 0.5)
        tree.links.new(instances.outputs["Instances"], rotate.inputs["Instances"])
        result = rotate.outputs["Instances"]
        if realize:
            realize_node = tree.nodes.new("GeometryNodeRealizeInstances")
            tree.links.new(result, realize_node.inputs["Geometry"])
            result = realize_node.outputs["Geometry"]
        return result

    geometry_nodes_modifier(obj, "Instances", build)
    return obj


def build_gn_points(material):
    bpy.ops.mesh.primitive_ico_sphere_add(subdivisions=2, radius=1.0)
    obj = bpy.context.active_object

    def build(tree, geometry):
        points = tree.nodes.new("GeometryNodeMeshToPoints")
        points.inputs["Radius"].default_value = 0.15
        tree.links.new(geometry, points.inputs["Mesh"])
        return set_material_node(tree, points.outputs["Points"], material)

    geometry_nodes_modifier(obj, "Points", build)
    return obj


def build_gn_curves(material):
    # Curves generated on a curves object render as hair (on a mesh object they become legacy
    # curve instances, which render engines skip).
    obj = bpy.data.objects.new("Curves", bpy.data.hair_curves.new("Curves"))
    bpy.context.scene.collection.objects.link(obj)

    def build(tree, geometry):
        grid = tree.nodes.new("GeometryNodeMeshGrid")
        grid.inputs["Size X"].default_value = 2.0
        grid.inputs["Size Y"].default_value = 2.0
        grid.inputs["Vertices X"].default_value = 6
        grid.inputs["Vertices Y"].default_value = 6
        points = tree.nodes.new("GeometryNodeMeshToPoints")
        tree.links.new(grid.outputs["Mesh"], points.inputs["Mesh"])
        line = tree.nodes.new("GeometryNodeCurvePrimitiveLine")
        line.inputs["End"].default_value = (0.0, 0.0, 1.2)
        instances = tree.nodes.new("GeometryNodeInstanceOnPoints")
        tree.links.new(points.outputs["Points"], instances.inputs["Points"])
        tree.links.new(line.outputs["Curve"], instances.inputs["Instance"])
        realize = tree.nodes.new("GeometryNodeRealizeInstances")
        tree.links.new(instances.outputs["Instances"], realize.inputs["Geometry"])
        radius = tree.nodes.new("GeometryNodeSetCurveRadius")
        radius.inputs["Radius"].default_value = 0.05
        tree.links.new(realize.outputs["Geometry"], radius.inputs["Curve"])
        return set_material_node(tree, radius.outputs["Curve"], material)

    geometry_nodes_modifier(obj, "Curves", build)
    obj.location = (0.0, 0.0, -0.6)
    return obj


def build_gn_volume(material):
    bpy.ops.mesh.primitive_cube_add(size=1.0)
    obj = bpy.context.active_object

    def build(tree, geometry):
        volume = tree.nodes.new("GeometryNodeVolumeCube")
        volume.inputs["Resolution X"].default_value = 32
        volume.inputs["Resolution Y"].default_value = 32
        volume.inputs["Resolution Z"].default_value = 32
        volume.inputs["Min"].default_value = (-1.0, -1.0, -1.0)
        volume.inputs["Max"].default_value = (1.0, 1.0, 1.0)
        # Density falling off from the center: 1 - |position| / 1.2.
        position = tree.nodes.new("GeometryNodeInputPosition")
        length = tree.nodes.new("ShaderNodeVectorMath")
        length.operation = 'LENGTH'
        tree.links.new(position.outputs["Position"], length.inputs[0])
        density = tree.nodes.new("ShaderNodeMath")
        density.operation = 'MULTIPLY_ADD'
        density.inputs[1].default_value = -2.0
        density.inputs[2].default_value = 2.0
        tree.links.new(length.outputs["Value"], density.inputs[0])
        tree.links.new(density.outputs["Value"], volume.inputs["Density"])
        return set_material_node(tree, volume.outputs["Volume"], material)

    geometry_nodes_modifier(obj, "Volume", build)
    return obj


def volume_material(name):
    material = bpy.data.materials.new(name)
    tree = material.node_tree
    for node in list(tree.nodes):
        if node.type == 'BSDF_PRINCIPLED':
            tree.nodes.remove(node)
    volume = tree.nodes.new("ShaderNodeVolumePrincipled")
    volume.inputs["Color"].default_value = (0.9, 0.4, 0.2, 1.0)
    volume.inputs["Density"].default_value = 2.0
    output = tree.nodes.get("Material Output")
    tree.links.new(volume.outputs["Volume"], output.inputs["Volume"])
    return material


def build_particles(material, hair):
    bpy.ops.mesh.primitive_grid_add(x_subdivisions=8, y_subdivisions=8, size=2.5)
    emitter = bpy.context.active_object
    emitter.data.materials.append(material)
    modifier = emitter.modifiers.new("Particles", 'PARTICLE_SYSTEM')
    settings = modifier.particle_system.settings
    settings.count = 60
    settings.frame_start = settings.frame_end = 1
    settings.emit_from = 'FACE'
    settings.use_emit_random = False
    if hair:
        settings.type = 'HAIR'
        settings.hair_length = 0.6
        settings.root_radius = 2.0
        settings.radius_scale = 0.02
        settings.render_type = 'PATH'
    else:
        bpy.ops.mesh.primitive_uv_sphere_add(radius=1.0, location=(0.0, 0.0, -10.0))
        instance = bpy.context.active_object
        instance.data.materials.append(material)
        settings.type = 'EMITTER'
        settings.physics_type = 'NO'
        settings.normal_factor = 0.0
        settings.render_type = 'OBJECT'
        settings.instance_object = instance
        settings.particle_size = 0.12
        instance.hide_render = False
    emitter.show_instancer_for_render = True
    return emitter


def build_hair_curves(material):
    curves = bpy.data.hair_curves.new("Hair")
    count, segments = 40, 4
    curves.add_curves([segments + 1] * count)
    positions = []
    import random
    rng = random.Random(1)
    for _ in range(count):
        x, y = rng.uniform(-1.0, 1.0), rng.uniform(-1.0, 1.0)
        for k in range(segments + 1):
            positions.append((x + 0.05 * k, y, -0.8 + 0.4 * k))
    for point, position in zip(curves.points, positions):
        point.position = position
        point.radius = 0.04
    curves.materials.append(material)
    obj = bpy.data.objects.new("Hair", curves)
    bpy.context.scene.collection.objects.link(obj)
    return obj


def build_text(material):
    bpy.ops.object.text_add(location=(-1.2, 0.0, -0.3), rotation=(math.radians(90.0), 0.0, 0.0))
    obj = bpy.context.active_object
    obj.data.body = "ANARI"
    obj.data.extrude = 0.1
    obj.data.size = 0.9
    obj.data.materials.append(material)
    return obj


def build_bevel_curve(material):
    bpy.ops.curve.primitive_bezier_circle_add(radius=1.0, rotation=(math.radians(90.0), 0.0, 0.0))
    obj = bpy.context.active_object
    obj.data.bevel_depth = 0.15
    obj.data.materials.append(material)
    return obj


def build_metaball(material):
    bpy.ops.object.metaball_add(type='BALL', radius=0.8, location=(-0.4, 0.0, 0.0))
    obj = bpy.context.active_object
    element = obj.data.elements.new()
    element.co = (1.0, 0.0, 0.0)
    element.radius = 0.8
    obj.data.materials.append(material)
    return obj


def build_subdivision(material):
    bpy.ops.mesh.primitive_cube_add(size=1.5)
    obj = bpy.context.active_object
    obj.modifiers.new("Subdivision", 'SUBSURF').levels = 2
    obj.data.materials.append(material)
    return obj


OBJECT_TYPES = (
    ("gn_instances", lambda m, v: build_gn_instances(m, realize=False)),
    ("gn_realized", lambda m, v: build_gn_instances(m, realize=True)),
    ("gn_points", lambda m, v: build_gn_points(m)),
    ("gn_curves", lambda m, v: build_gn_curves(m)),
    ("gn_volume", lambda m, v: build_gn_volume(v)),
    ("particle_instances", lambda m, v: build_particles(m, hair=False)),
    ("particle_hair", lambda m, v: build_particles(m, hair=True)),
    ("hair_curves", lambda m, v: build_hair_curves(m)),
    ("text", lambda m, v: build_text(m)),
    ("bevel_curve", lambda m, v: build_bevel_curve(m)),
    ("metaball", lambda m, v: build_metaball(m)),
    ("subdivision", lambda m, v: build_subdivision(m)),
)


def object_mask(pixels, size, background):
    """Pixels which differ from the background color."""
    width, height = size
    mask = []
    for i in range(width * height):
        d = sum(abs(pixels[i * 4 + c] - background[c]) for c in range(3))
        mask.append(d > 0.05)
    return mask


def dilate_mask(mask, size):
    width, height = size
    result = list(mask)
    for y in range(height):
        for x in range(width):
            if mask[y * width + x]:
                for dy in (-1, 0, 1):
                    for dx in (-1, 0, 1):
                        xx, yy = x + dx, y + dy
                        if 0 <= xx < width and 0 <= yy < height:
                            result[yy * width + xx] = True
    return result


def mask_agreement(a, b, size):
    """Coverage agreement of two object masks, tolerating one pixel differences at the edges
    (pixel filters differ between renderers): the smaller of the fractions of each mask covered
    by the other one, dilated by a pixel."""
    dilated_a = dilate_mask(a, size)
    dilated_b = dilate_mask(b, size)
    count_a = sum(a)
    count_b = sum(b)
    if count_a == 0 or count_b == 0:
        return 1.0 if count_a == count_b else 0.0
    precision = sum(1 for x, y in zip(a, dilated_b) if x and y) / count_a
    recall = sum(1 for x, y in zip(b, dilated_a) if x and y) / count_b
    return min(precision, recall)


# --------------------------------------------------------------------
# Tests.

class AnariSmokeTest(unittest.TestCase):

    @classmethod
    def setUpClass(cls):
        setup_addon()
        cls.device = find_device(args.library)
        cls.strict = args.strict or args.library == "cycles"

    def setUp(self):
        if self.device is None:
            self.fail("ANARI library \"{:s}\" not found, available devices: {!r}".format(
                args.library, [d[1] for d in anari_devices()]))
        self.scene = bpy.context.scene

    def render_engine(self, engine, name):
        setup_render(self.scene, engine)
        suffix = args.library if engine == 'ANARI' else "cycles_reference"
        return render(self.scene, "{:s}_{:s}".format(name, suffix))

    def test_build_options(self):
        self.assertTrue(bpy.app.build_options.anari)
        import _cycles
        self.assertTrue(_cycles.with_anari)

    def test_engine_registered(self):
        engines = {getattr(cls, "bl_idname", None) for cls in bpy.types.RenderEngine.__subclasses__()}
        self.assertIn('ANARI', engines)

    def test_basic_render(self):
        build_basic_scene(self.scene)
        pixels, size = self.render_engine('ANARI', "basic")
        self.assertEqual(size, RESOLUTION)

        mean, non_finite = image_stats(pixels)
        self.assertEqual(non_finite, 0, "render contains NaN or infinite values")
        self.assertGreater(sum(mean), 1e-3, "render is black")

        # Compare against the internal Cycles render of the same scene.
        reference, reference_size = self.render_engine('CYCLES', "basic")
        self.assertEqual(size, reference_size)
        difference = relative_difference(block_means(pixels, size), block_means(reference, size))
        reference_mean, _ = image_stats(reference)
        ratio = sum(mean) / max(sum(reference_mean), 1e-8)
        print("ANARI {:s}: mean {!r}, Cycles mean {!r}, brightness ratio {:.3f}, "
              "relative difference {:.3f}".format(args.library, mean, reference_mean, ratio, difference))

        if self.strict:
            self.assertLess(difference, 0.2, "render differs too much from Cycles")
        else:
            self.assertGreater(ratio, 0.2, "render is much darker than Cycles")
            self.assertLess(ratio, 5.0, "render is much brighter than Cycles")

    def test_texture(self):
        """A textured plane seen from above, the checker pattern has to show up."""
        clear_scene()
        add_mesh("Ground", "primitive_plane_add", (0, 0, 0),
                 new_material("Ground", roughness=1.0, image=checker_image("Checker")), size=4)
        add_camera(self.scene, location=(0.0, 0.0, 5.0), target=(0.0, 0.0, 0.0), lens=50.0)
        set_world_color(self.scene, (1.0, 1.0, 1.0), 1.0)

        pixels, size = self.render_engine('ANARI', "texture")
        width, height = size
        # Red and blue squares: the image must contain both red and blue dominated pixels.
        red = blue = 0
        for i in range(width * height):
            r, b = pixels[i * 4], pixels[i * 4 + 2]
            if r > 2.0 * b:
                red += 1
            elif b > 2.0 * r:
                blue += 1
        print("ANARI {:s}: red pixels {:d}, blue pixels {:d}".format(args.library, red, blue))
        self.assertGreater(red, width * height // 10, "texture red squares are missing")
        self.assertGreater(blue, width * height // 10, "texture blue squares are missing")

    def test_lights(self):
        """Point, spot, area and sun lights each light the scene on their own."""
        for light_type, energy in (('POINT', 500.0), ('SPOT', 1000.0), ('AREA', 500.0), ('SUN', 3.0)):
            with self.subTest(light=light_type):
                clear_scene()
                add_mesh("Ground", "primitive_plane_add", (0, 0, 0), new_material("Ground"), size=8)
                add_camera(self.scene)
                rotation = (math.radians(30), 0, 0) if light_type == 'SUN' else (0, 0, 0)
                add_light(self.scene, light_type, (0.0, 0.0, 3.0), energy, rotation=rotation)
                set_world_color(self.scene, (0.0, 0.0, 0.0), 0.0)

                pixels, size = self.render_engine('ANARI', "light_" + light_type.lower())
                mean, non_finite = image_stats(pixels)
                self.assertEqual(non_finite, 0)
                self.assertGreater(sum(mean), 1e-3, "{:s} light does not light the scene".format(light_type))

                reference, _ = self.render_engine('CYCLES', "light_" + light_type.lower())
                reference_mean, _ = image_stats(reference)
                ratio = sum(mean) / max(sum(reference_mean), 1e-8)
                difference = relative_difference(block_means(pixels, size),
                                                 block_means(reference, size))
                print("ANARI {:s}: {:s} light brightness ratio {:.3f}, relative difference {:.3f}".format(
                    args.library, light_type, ratio, difference))
                if self.strict:
                    self.assertLess(difference, 0.25)
                else:
                    self.assertGreater(ratio, 0.2, "light is much darker than in Cycles")
                    self.assertLess(ratio, 5.0, "light is much brighter than in Cycles")

    def test_texture_mapping(self):
        """A texture scaled and rotated by a mapping node matches the Cycles render."""
        clear_scene()
        material = new_material("Ground", roughness=1.0, image=checker_image("Checker"))
        tree = material.node_tree
        texture = next(node for node in tree.nodes if node.type == 'TEX_IMAGE')
        coordinates = tree.nodes.new("ShaderNodeTexCoord")
        mapping = tree.nodes.new("ShaderNodeMapping")
        mapping.inputs["Location"].default_value = (0.1, 0.2, 0.0)
        mapping.inputs["Scale"].default_value = (2.5, 2.5, 1.0)
        mapping.inputs["Rotation"].default_value = (0.0, 0.0, math.radians(30.0))
        tree.links.new(coordinates.outputs["UV"], mapping.inputs["Vector"])
        tree.links.new(mapping.outputs["Vector"], texture.inputs["Vector"])
        add_mesh("Ground", "primitive_plane_add", (0, 0, 0), material, size=4)
        add_camera(self.scene, location=(0.0, 0.0, 5.0), target=(0.0, 0.0, 0.0), lens=50.0)
        set_world_color(self.scene, (1.0, 1.0, 1.0), 1.0)

        pixels, size = self.render_engine('ANARI', "texture_mapping")
        reference, _ = self.render_engine('CYCLES', "texture_mapping")
        agreement = pattern_agreement(pixels, reference, size)
        print("ANARI {:s}: texture mapping pattern agreement {:.3f}".format(
            args.library, agreement))
        self.assertGreater(agreement, 0.9 if self.strict else 0.75)

    def test_environment_map(self):
        """An environment image lights the scene and shows up in the background, also when
        rotated by a mapping node."""
        image = environment_image("Environment")
        for rotation in (0.0, math.radians(90.0)):
            with self.subTest(rotation=math.degrees(rotation)):
                clear_scene()
                image = environment_image("Environment")
                add_mesh("Sphere", "primitive_uv_sphere_add", (-3.0, 0.0, 0.0),
                         new_material("White", color=(0.8, 0.8, 0.8), roughness=1.0), radius=0.7)
                bpy.ops.object.shade_smooth()
                add_camera(self.scene, location=(0.0, 0.0, 0.0), target=(-1.0, 0.0, 0.0), lens=20.0)
                self.scene.world = bpy.data.worlds.new("World")
                self.scene.world.use_nodes = True
                set_world_environment(self.scene, image, 1.0, rotation)

                name = "environment_{:d}".format(round(math.degrees(rotation)))
                pixels, size = self.render_engine('ANARI', name)
                reference, _ = self.render_engine('CYCLES', name)
                mean, non_finite = image_stats(pixels)
                reference_mean, _ = image_stats(reference)
                self.assertEqual(non_finite, 0)
                ratio = sum(mean) / max(sum(reference_mean), 1e-8)
                difference = relative_difference(block_means(pixels, size),
                                                 block_means(reference, size))
                agreement = pattern_agreement(pixels, reference, size)
                print("ANARI {:s}: environment rotated {:.0f}: brightness ratio {:.3f}, "
                      "relative difference {:.3f}, pattern agreement {:.3f}".format(
                          args.library, math.degrees(rotation), ratio, difference, agreement))
                if self.strict:
                    self.assertLess(difference, 0.1)
                else:
                    self.assertGreater(ratio, 0.5)
                    self.assertLess(ratio, 2.0)
                    self.assertGreater(agreement, 0.8, "environment is oriented differently")

    def test_background(self):
        """The world color is the visible background, and changing it, its strength or making
        the film transparent updates an existing render session."""
        clear_scene()
        add_mesh("Cube", "primitive_cube_add", (0.0, 0.0, 0.0),
                 new_material("Gray", roughness=1.0), size=0.5)
        add_camera(self.scene, location=(0.0, -8.0, 0.0), target=(0.0, 0.0, 0.0), lens=35.0)
        self.scene.render.use_persistent_data = True
        try:
            steps = (
                ("background_red", (0.8, 0.2, 0.1), 1.0, False),
                ("background_blue", (0.1, 0.3, 0.9), 1.0, False),
                ("background_strength", (0.1, 0.3, 0.9), 2.0, False),
                ("background_transparent", (0.1, 0.3, 0.9), 2.0, True),
            )
            for name, color, strength, transparent in steps:
                with self.subTest(step=name):
                    set_world_color(self.scene, color, strength)
                    setup_render(self.scene, 'ANARI')
                    self.scene.render.film_transparent = transparent
                    pixels, size = render(self.scene, "{:s}_{:s}".format(name, args.library))
                    # The corners show the background only.
                    corner = corner_mean(pixels, size)
                    print("ANARI {:s}: {:s}: corner {:s}".format(
                        args.library, name, ", ".join("{:.3f}".format(c) for c in corner)))
                    if transparent:
                        self.assertLess(corner[3], 0.05, "background is not transparent")
                    else:
                        expected = [c * strength for c in color]
                        for channel in range(3):
                            self.assertAlmostEqual(corner[channel], expected[channel],
                                                   delta=0.05 + 0.05 * expected[channel])
                        self.assertGreater(corner[3], 0.95)
        finally:
            self.scene.render.use_persistent_data = False
            self.scene.render.film_transparent = False

    def test_object_types(self):
        """Mesh, curve, text, metaball, subdivision, geometry nodes (instances, points, curves,
        volumes), particle instances and hair, and hair curves objects render like in Cycles."""
        background = (0.2, 0.2, 0.2)
        for name, build in OBJECT_TYPES:
            with self.subTest(object_type=name):
                clear_scene()
                for group in list(bpy.data.node_groups):
                    bpy.data.node_groups.remove(group)
                material = new_material("Orange", color=(0.9, 0.4, 0.1), roughness=0.6)
                build(material, volume_material("Smoke"))
                add_camera(self.scene, location=(0.0, -7.0, 0.0), target=(0.0, 0.0, 0.0), lens=40.0)
                add_light(self.scene, 'SUN', (0, 0, 5), 3.0,
                          rotation=(math.radians(50), 0, math.radians(30)))
                set_world_color(self.scene, background, 1.0)

                pixels, size = self.render_engine('ANARI', "object_" + name)
                reference, _ = self.render_engine('CYCLES', "object_" + name)
                mean, non_finite = image_stats(pixels)
                reference_mean, _ = image_stats(reference)
                self.assertEqual(non_finite, 0)
                reference_mask = object_mask(reference, size, background)
                self.assertGreater(sum(reference_mask), 100, "test scene shows no object")
                coverage = mask_agreement(object_mask(pixels, size, background), reference_mask,
                                          size)
                difference = relative_difference(block_means(pixels, size),
                                                 block_means(reference, size))
                ratio = sum(mean) / max(sum(reference_mean), 1e-8)
                print("ANARI {:s}: object {:s}: coverage {:.3f}, brightness ratio {:.3f}, "
                      "relative difference {:.3f}".format(args.library, name, coverage, ratio,
                                                          difference))
                self.assertGreater(coverage, 0.95 if self.strict else 0.85, "object shape differs")
                if self.strict:
                    self.assertLess(difference, 0.1)

    def test_instancing(self):
        """Linked duplicates share their geometry and get their own transforms."""
        clear_scene()
        material = new_material("Gray")
        original = add_mesh("Cube", "primitive_cube_add", (-2.0, 0.0, 0.5), material, size=1.0)
        for i in range(1, 5):
            duplicate = original.copy()
            duplicate.location.x = -2.0 + i
            self.scene.collection.objects.link(duplicate)
        add_camera(self.scene, location=(0.0, -6.0, 2.0), target=(0.0, 0.0, 0.5))
        set_world_color(self.scene, (1.0, 1.0, 1.0), 1.0)

        pixels, size = self.render_engine('ANARI', "instancing")
        reference, _ = self.render_engine('CYCLES', "instancing")
        difference = relative_difference(block_means(pixels, size), block_means(reference, size))
        print("ANARI {:s}: instancing relative difference {:.3f}".format(args.library, difference))
        self.assertLess(difference, 0.25 if self.strict else 0.6)


def main():
    global args
    argv = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
    args = parse_arguments(argv)
    program = unittest.main(argv=[sys.argv[0], *args.tests], exit=False, verbosity=2)
    # Report failures through the exit code.
    if not program.result.wasSuccessful():
        sys.exit(1)


if __name__ == "__main__":
    main()
