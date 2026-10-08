# SPDX-FileCopyrightText: 2011-2026 Blender Foundation
#
# SPDX-License-Identifier: Apache-2.0

from __future__ import annotations

import bpy
from bpy.props import (
    EnumProperty,
    PointerProperty,
    StringProperty,
)

# Keep references to the strings of the dynamic enum items, Blender does not copy them.
_device_items_cache = []


def _split_list(value):
    import os
    items = []
    for item in value.replace("\n", ";").split(";"):
        item = item.strip()
        if item:
            items.append(os.path.expandvars(os.path.expanduser(item)))
    return items


def apply_library_search(preferences=None):
    """Pass the library search settings of the add-on preferences to Cycles."""
    import _cycles
    if not getattr(_cycles, "with_anari", False):
        return
    if preferences is None:
        addon = bpy.context.preferences.addons.get(__package__)
        preferences = addon.preferences if addon else None
    if preferences is None:
        return
    _cycles.anari_set_library_search(_split_list(preferences.library_search_paths),
                                     _split_list(preferences.extra_library_names))


_library_search_applied = False


def available_devices():
    """List of (id, description, library, subtype) of the available ANARI devices."""
    global _library_search_applied
    import _cycles
    if not getattr(_cycles, "with_anari", False):
        return ()
    if not _library_search_applied:
        apply_library_search()
        _library_search_applied = True
    return _cycles.anari_devices()


def _restart_render(context):
    # Recreate the render sessions so the new device is used.
    for window in context.window_manager.windows:
        scene = window.scene
        if scene and scene.render.engine == 'ANARI':
            scene.update_render_engine()


def _library_search_update(self, context):
    apply_library_search(self)
    _restart_render(context)


class ANARIPreferences(bpy.types.AddonPreferences):
    bl_idname = __package__

    library_search_paths: StringProperty(
        name="Library Search Paths",
        description=(
            "Directories containing ANARI libraries (anari_library_<name>), separated by ';'. "
            "Libraries are also searched next to the ANARI library and in the system search path"
        ),
        default="",
        update=_library_search_update,
    )
    extra_library_names: StringProperty(
        name="Additional Libraries",
        description=(
            "Names of additional ANARI libraries to probe, separated by ';', besides the known "
            "back-ends (barney, cycles, mitsuba, moonray, visrtx, helide, ...)"
        ),
        default="",
        update=_library_search_update,
    )

    def draw(self, context):
        self.draw_impl(self.layout, context)

    def draw_impl(self, layout, context):
        layout.use_property_split = True
        layout.use_property_decorate = False

        col = layout.column()
        col.prop(self, "library_search_paths")
        col.prop(self, "extra_library_names")

        devices = available_devices()
        box = layout.box()
        if not devices:
            box.label(text="No ANARI devices found", icon='INFO')
        for device in devices:
            box.label(text="{:s}  [{:s}]".format(device[1], device[2]), icon='CHECKMARK')


def _device_items(self, context):
    global _device_items_cache
    items = [(device[0], device[1], "ANARI library \"{:s}\", device \"{:s}\"".format(device[2], device[3]), i)
             for i, device in enumerate(available_devices())]
    if not items:
        items = [('NONE', "No ANARI Devices", "No ANARI device was found", 0)]
    _device_items_cache = items
    return items


def _device_get(self):
    for item in _device_items(self, None):
        if item[0] == self.device_id:
            return item[3]
    return 0


def _device_set(self, value):
    for item in _device_items(self, None):
        if item[3] == value and item[0] != 'NONE':
            self.device_id = item[0]
            return


def _device_update(self, context):
    _restart_render(context)


class ANARISceneSettings(bpy.types.PropertyGroup):
    device_id: StringProperty(
        name="Device ID",
        description="Identifier of the ANARI device used for rendering",
        default="",
    )
    device: EnumProperty(
        name="Device",
        description="ANARI device to render with",
        items=_device_items,
        get=_device_get,
        set=_device_set,
        update=_device_update,
    )
    device_parameters: StringProperty(
        name="Device Parameters",
        description=(
            "Parameters passed to the ANARI device as name=value pairs separated by ';', "
            "for example \"mitsuba.variant=cuda_ad_rgb\" or \"cudaDevice=0\""
        ),
        default="",
        update=_device_update,
    )

    @classmethod
    def register(cls):
        bpy.types.Scene.anari = PointerProperty(
            name="ANARI Scene Settings",
            description="ANARI scene settings",
            type=cls,
        )

    @classmethod
    def unregister(cls):
        del bpy.types.Scene.anari


classes = (
    ANARIPreferences,
    ANARISceneSettings,
)


def register():
    from bpy.utils import register_class
    for cls in classes:
        register_class(cls)
    apply_library_search()


def unregister():
    from bpy.utils import unregister_class
    for cls in reversed(classes):
        unregister_class(cls)
