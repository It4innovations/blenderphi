# SPDX-FileCopyrightText: 2011-2026 Blender Foundation
#
# SPDX-License-Identifier: Apache-2.0

"""
ANARI render engine.

The scene is synchronized by Cycles exactly as for the Cycles render engine, and then rendered
with an ANARI device (Barney, Cycles-ANARI, Mitsuba-ANARI, ...). All Cycles settings (objects,
materials, lights, world, sampling, passes, ...) are shared, the ANARI specific settings are the
choice of the ANARI device and its parameters.
"""

from __future__ import annotations

bl_info = {
    "name": "ANARI Render Engine",
    "author": "",
    "blender": (5, 0, 0),
    "description": "Render with ANARI devices, using the Cycles scene synchronization",
    "warning": "",
    "doc_url": "",
    "tracker_url": "",
    "support": 'OFFICIAL',
    "category": "Render",
}

# Support 'reload' case.
if "bpy" in locals():
    import importlib
    if "properties" in locals():
        importlib.reload(properties)
    if "ui" in locals():
        importlib.reload(ui)

import bpy


def _cycles_engine():
    """The engine module of the Cycles add-on, which implements the render session."""
    from cycles import engine
    return engine


class ANARIRender(bpy.types.RenderEngine):
    bl_idname = 'ANARI'
    bl_label = "ANARI"
    bl_use_eevee_viewport = True
    bl_use_preview = True
    bl_use_exclude_layers = True
    bl_use_spherical_stereo = True
    bl_use_custom_freestyle = True

    def __init__(self, *args, **kwargs):
        super().__init__(*args, **kwargs)
        self.session = None

    def __del__(self):
        _cycles_engine().free(self)

    # Final render.
    def update(self, data, depsgraph):
        engine = _cycles_engine()
        if not self.session:
            engine.create(self, data)
        engine.reset(self, data, depsgraph)

    def render(self, depsgraph):
        _cycles_engine().render(self, depsgraph)

    def render_frame_finish(self):
        _cycles_engine().render_frame_finish(self)

    def draw(self, context, depsgraph):
        _cycles_engine().draw(self, depsgraph, context.space_data)

    # Viewport render.
    def view_update(self, context, depsgraph):
        engine = _cycles_engine()
        if not self.session:
            engine.create(self, context.blend_data,
                          context.region, context.space_data, context.region_data)

        engine.reset(self, context.blend_data, depsgraph)
        engine.sync(self, depsgraph, context.blend_data)

    def view_draw(self, context, depsgraph):
        _cycles_engine().view_draw(self, depsgraph, context.region, context.space_data, context.region_data)

    def view_pause(self, context):
        _cycles_engine().view_pause(self, True)

    def view_resume(self, context):
        _cycles_engine().view_pause(self, False)

    def update_render_passes(self, scene, srl):
        _cycles_engine().register_passes(self, scene, srl)


classes = (
    ANARIRender,
)


def _ensure_cycles_enabled():
    """The ANARI engine uses the Cycles add-on for the scene settings and the render session."""
    import addon_utils
    loaded_default, loaded_state = addon_utils.check("cycles")
    if not loaded_state:
        addon_utils.enable("cycles", default_set=True, persistent=True)


def register():
    from bpy.utils import register_class
    from . import properties
    from . import ui

    _ensure_cycles_enabled()

    properties.register()
    ui.register()

    for cls in classes:
        register_class(cls)


def unregister():
    from bpy.utils import unregister_class
    from . import properties
    from . import ui

    for cls in classes:
        unregister_class(cls)

    ui.unregister()
    properties.unregister()
