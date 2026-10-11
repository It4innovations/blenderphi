"""Measure how responsive the rendered 3D viewport is with an ANARI library.

Needs a window, so Blender is started without --background:

  blender --factory-startup --disable-autoexec -noaudio --no-window-focus \
      --python anari_viewport_bench.py -- --blend scene.blend --library barney \
      --library-path <dir> [--device-parameters ...] [--object NAME] [--materials A,B]

The script switches the viewport to rendered mode and then, like a user would, orbits the view,
moves an object and drags a value of a material. In every phase it changes the scene at 20 Hz
for a few seconds and records how the main loop keeps up:

  ticks   how many of the changes were applied (the main loop is blocked while Blender syncs)
  gap     the longest and the mean time between two changes, in ms: the hitch a user feels
  draws   how often the viewport was redrawn, per second

One line per phase is printed with the prefix BENCH. With CYCLES_ANARI_TRACE=1 the exporter adds
the time of every scene update ("update: done in ... ms") to stderr.
"""
import argparse
import math
import sys
import time

import bpy
from mathutils import Quaternion

parser = argparse.ArgumentParser()
parser.add_argument("--blend", required=True)
parser.add_argument("--library", required=True)
parser.add_argument("--library-path", action="append", default=[])
parser.add_argument("--device-parameters", default="")
parser.add_argument("--object", default="")
parser.add_argument("--materials", default="")
parser.add_argument("--samples", type=int, default=32)
parser.add_argument("--seconds", type=float, default=4.0)
parser.add_argument("--settle", type=float, default=60.0,
                    help="Longest time to wait for the first image")
parser.add_argument("--min-settle", type=float, default=0.0,
                    help="Shortest time to wait before the phases start. The viewport is redrawn "
                         "while the scene still loads, so large scenes need a fixed wait")
args = parser.parse_args(sys.argv[sys.argv.index("--") + 1:])

INTERVAL = 0.05


def log(text):
    print("BENCH " + text, flush=True)


def find_device(library):
    import _cycles
    for device in _cycles.anari_devices():
        if device[2].split(",")[0] == library:
            return device
    return None


bpy.ops.wm.open_mainfile(filepath=args.blend, load_ui=False, use_scripts=False)
import addon_utils
addon_utils.enable("cycles", default_set=True)
addon_utils.enable("anari", default_set=True)
bpy.context.preferences.addons["anari"].preferences.library_search_paths = ";".join(args.library_path)
device = find_device(args.library)
if device is None:
    log("library {} not found".format(args.library))
    bpy.ops.wm.quit_blender()

scene = bpy.context.scene
scene.render.engine = 'ANARI'
scene.anari.device_id = device[0]
scene.anari.device_parameters = args.device_parameters
scene.cycles.preview_samples = args.samples
scene.cycles.use_preview_adaptive_sampling = False

state = {"draws": 0, "last_draw": 0.0, "first_draw": 0.0}


def on_draw():
    now = time.perf_counter()
    state["draws"] += 1
    state["last_draw"] = now
    if state["first_draw"] == 0.0:
        state["first_draw"] = now


bpy.types.SpaceView3D.draw_handler_add(on_draw, (), 'WINDOW', 'POST_PIXEL')


def view3d():
    for window in bpy.context.window_manager.windows:
        for area in window.screen.areas:
            if area.type == 'VIEW_3D':
                return window, area, area.spaces.active
    return None, None, None


class Phase:
    """Apply a change at a fixed rate and record how the main loop keeps up."""

    def __init__(self, name, change):
        self.name = name
        self.change = change
        self.start = None
        self.times = []
        self.draws = 0

    def step(self):
        now = time.perf_counter()
        if self.start is None:
            self.start = now
            self.draws = state["draws"]
        self.times.append(now)
        if now - self.start >= args.seconds:
            gaps = [(b - a) * 1000.0 for a, b in zip(self.times, self.times[1:])]
            elapsed = now - self.start
            log("{:28s} ticks {:3d} of {:3d}  gap max {:7.1f} ms  mean {:6.1f} ms  draws {:5.1f}/s".format(
                self.name, len(self.times), int(elapsed / INTERVAL) + 1, max(gaps) if gaps else 0.0,
                sum(gaps) / max(len(gaps), 1), (state["draws"] - self.draws) / elapsed))
            return False
        self.change(len(self.times))
        return True


def make_phases(area, space):
    region_3d = space.region_3d
    phases = []

    def orbit(i):
        region_3d.view_rotation = Quaternion((0.0, 0.0, 1.0), math.radians(1.0)) @ region_3d.view_rotation
        area.tag_redraw()

    def start_orbit(i):
        if i == 1:
            region_3d.view_perspective = 'PERSP'
        orbit(i)

    phases.append(Phase("orbit the view", start_orbit))

    obj = bpy.data.objects.get(args.object) if args.object else None
    if obj is None:
        meshes = [o for o in scene.objects if o.type == 'MESH' and not o.hide_render and o.visible_get()]
        meshes.sort(key=lambda o: -len(o.data.polygons))
        obj = meshes[0] if meshes else None
    if obj is not None:
        origin = obj.location.copy()

        def move(i):
            obj.location = origin + obj.matrix_world.to_3x3().col[0].normalized() * 0.002 * math.sin(i * 0.5)

        phases.append(Phase("move {} ({} faces)".format(obj.name[:14], len(obj.data.polygons)), move))

    for name in [n for n in args.materials.split(",") if n]:
        material = bpy.data.materials.get(name)
        socket = None
        if material and material.node_tree:
            for node in material.node_tree.nodes:
                for node_input in node.inputs:
                    if (node_input.name == "Roughness" and not node_input.is_linked and
                            node_input.enabled and hasattr(node_input, "default_value")):
                        socket = node_input
                        break
                if socket:
                    break
        if socket is None:
            log("material {}: no Roughness value to drag".format(name))
            continue
        value = socket.default_value

        def drag(i, socket=socket, value=value):
            socket.default_value = min(max(value + 0.05 * math.sin(i * 0.7), 0.0), 1.0)

        phases.append(Phase("edit material {}".format(name[:14]), drag))
    return phases


run = {"phase": -1, "phases": None, "t0": time.perf_counter(), "idle_until": 0.0, "rendered": False}


def tick():
    now = time.perf_counter()
    window, area, space = view3d()
    if area is None:
        log("no 3D viewport")
        bpy.ops.wm.quit_blender()
        return None

    if not run["rendered"]:
        # Rendered mode, through the camera: the same view for every run.
        space.shading.type = 'RENDERED'
        space.region_3d.view_perspective = 'CAMERA'
        run["rendered"] = True
        run["t0"] = now
        state["draws"] = 0
        state["first_draw"] = 0.0
        log("viewport {}x{} switched to rendered".format(area.width, area.height))
        return 0.25

    if run["phase"] < 0:
        # Wait until the image converged: no redraw for a while.
        waited = now - run["t0"]
        quiet = now - state["last_draw"] if state["last_draw"] else 0.0
        if waited >= args.min_settle and ((state["draws"] > 2 and quiet > 2.0) or waited > args.settle):
            log("{:28s} first draw {:6.2f} s  converged after {:6.2f} s  draws {:d}".format(
                "start", state["first_draw"] - run["t0"], state["last_draw"] - run["t0"], state["draws"]))
            run["phases"] = make_phases(area, space)
            run["phase"] = 0
            return INTERVAL
        return 0.25

    if now < run["idle_until"]:
        return 0.25
    if run["phase"] >= len(run["phases"]):
        log("done")
        bpy.ops.wm.quit_blender()
        return None
    if not run["phases"][run["phase"]].step():
        run["phase"] += 1
        # Let the image converge again before the next phase.
        run["idle_until"] = time.perf_counter() + 3.0
        return 0.25
    return INTERVAL


bpy.app.timers.register(tick, first_interval=1.0)
