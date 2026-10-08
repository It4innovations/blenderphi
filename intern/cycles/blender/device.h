/* SPDX-FileCopyrightText: 2011-2022 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

#pragma once

namespace blender {
struct Scene;
struct UserDef;
struct View3D;
}  // namespace blender

#include "device/device.h"

CCL_NAMESPACE_BEGIN

/* Get number of threads to use for rendering. */
int blender_device_threads(blender::Scene &b_scene);

/* Convert Blender settings to device specification. In addition, preferences_device contains the
 * device chosen in Cycles global preferences, which is useful for the denoiser device selection.
 */
DeviceInfo blender_device_info(blender::UserDef &b_preferences,
                               blender::Scene &b_scene,
                               bool background,
                               bool preview,
                               DeviceInfo &preferences_device);

/* Whether the scene is rendered with the ANARI render engine, which uses Cycles to synchronize
 * the scene and an ANARI device to render it. */
bool blender_use_anari_engine(const blender::Scene &b_scene);

/* ANARI device chosen in the scene settings of the ANARI add-on, or the local render device of
 * the 3D viewport. */
DeviceInfo blender_anari_device_info(blender::Scene &b_scene, const blender::View3D *b_v3d);

CCL_NAMESPACE_END
