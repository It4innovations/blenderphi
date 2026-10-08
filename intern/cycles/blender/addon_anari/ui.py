# SPDX-FileCopyrightText: 2011-2026 Blender Foundation
#
# SPDX-License-Identifier: Apache-2.0

from __future__ import annotations

import bpy
from bpy.types import (
    Operator,
    Panel,
)
from bpy.props import IntProperty

from .properties import available_devices

# Cycles panels which don't apply to ANARI rendering.
_excluded_panels = {
    'CYCLES_RENDER_PT_bake',
    'CYCLES_RENDER_PT_bake_influence',
    'CYCLES_RENDER_PT_bake_selected_to_active',
    'CYCLES_RENDER_PT_bake_output',
    'CYCLES_RENDER_PT_bake_output_margin',
    'CYCLES_RENDER_PT_sampling_path_guiding',
    'CYCLES_RENDER_PT_sampling_path_guiding_debug',
    'CYCLES_RENDER_PT_performance_texture_cache',
    'CYCLES_RENDER_PT_performance_acceleration_structure',
}


def _all_subclasses(cls):
    for subclass in cls.__subclasses__():
        yield subclass
        yield from _all_subclasses(subclass)


def _compatible_panels():
    """Panels shown for Cycles, which are shown for ANARI as well since all Cycles settings
    apply to ANARI rendering."""
    panels = []
    for panel in _all_subclasses(bpy.types.Panel):
        compat = getattr(panel, "COMPAT_ENGINES", None)
        if compat is None or not isinstance(compat, set):
            continue
        if 'CYCLES' in compat and panel.__name__ not in _excluded_panels:
            panels.append(panel)
    return panels


def draw_device(self, context):
    if context.engine != 'ANARI':
        return

    layout = self.layout
    layout.use_property_split = True
    layout.use_property_decorate = False

    settings = context.scene.anari
    col = layout.column()
    col.prop(settings, "device")
    col.prop(settings, "device_parameters", text="Parameters")


class VIEW3D_OT_anari_local_render_device(Operator):
    """Use a specific ANARI device for rendering in this 3D viewport"""
    bl_idname = "view3d.anari_local_render_device"
    bl_label = "Set Local ANARI Render Device"
    bl_options = {'INTERNAL'}

    index: IntProperty(default=-1)

    @classmethod
    def poll(cls, context):
        return context.space_data and context.space_data.type == 'VIEW_3D'

    def execute(self, context):
        view = context.space_data
        if self.index < 0 or (view.use_local_render_device and view.local_render_device == self.index):
            view.use_local_render_device = False
        else:
            view.use_local_render_device = True
            view.local_render_device = self.index

        context.scene.update_render_engine()
        return {'FINISHED'}


class VIEW3D_PT_anari_render_device(Panel):
    bl_space_type = 'VIEW_3D'
    bl_region_type = 'UI'
    bl_category = "View"
    bl_label = "ANARI Render Device"
    bl_parent_id = "VIEW3D_PT_view3d_properties"
    bl_options = {'DEFAULT_CLOSED'}

    @classmethod
    def poll(cls, context):
        return context.engine == 'ANARI'

    def draw(self, context):
        layout = self.layout
        view = context.space_data

        col = layout.column(align=True)
        op = col.operator(VIEW3D_OT_anari_local_render_device.bl_idname,
                          text="Scene Device", depress=not view.use_local_render_device)
        op.index = -1
        for i, device in enumerate(available_devices()):
            is_active = view.use_local_render_device and view.local_render_device == i
            op = col.operator(VIEW3D_OT_anari_local_render_device.bl_idname,
                              text=device[1], depress=is_active)
            op.index = i


classes = (
    VIEW3D_OT_anari_local_render_device,
    VIEW3D_PT_anari_render_device,
)


def register():
    from bpy.utils import register_class

    bpy.types.RENDER_PT_context.append(draw_device)

    # Many Cycles panels share the same COMPAT_ENGINES set, give the excluded panels their own
    # copy before adding ANARI to the shared one.
    for panel in _all_subclasses(bpy.types.Panel):
        if panel.__name__ in _excluded_panels and isinstance(getattr(panel, "COMPAT_ENGINES", None), set):
            panel.COMPAT_ENGINES = set(panel.COMPAT_ENGINES) - {'ANARI'}

    for panel in _compatible_panels():
        panel.COMPAT_ENGINES.add('ANARI')

    for cls in classes:
        register_class(cls)


def unregister():
    from bpy.utils import unregister_class

    bpy.types.RENDER_PT_context.remove(draw_device)

    for panel in _compatible_panels():
        panel.COMPAT_ENGINES.discard('ANARI')

    for cls in classes:
        unregister_class(cls)
