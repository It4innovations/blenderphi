/* SPDX-FileCopyrightText: 2011-2026 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include "util/string.h"
#include "util/unique_ptr.h"
#include "util/vector.h"

CCL_NAMESPACE_BEGIN

class Device;
class DeviceInfo;
class Profiler;
class Stats;

/* ANARI device.
 *
 * Renders the Cycles scene with an ANARI back-end (Barney, Cycles-ANARI, Mitsuba-ANARI, ...).
 * The Cycles scene is synchronized from Blender as usual and then mirrored into ANARI objects,
 * the rendered frame is written into the Cycles render buffers so that passes, display, denoising
 * and output work the same as for the other devices. */

unique_ptr<Device> device_anari_create(const DeviceInfo &info,
                                       Stats &stats,
                                       Profiler &profiler,
                                       bool headless);

/* Probe the known ANARI libraries and append a device for every available device subtype. */
void device_anari_info(vector<DeviceInfo> &devices);

string device_anari_capabilities();

/* Additional directories in which ANARI libraries are searched, besides the directory of the
 * ANARI front-end library and the default system search path. Resets the device list. */
void device_anari_set_library_search_paths(const vector<string> &paths);

/* Additional library names to probe, besides the built-in list of known back-ends. */
void device_anari_set_extra_library_names(const vector<string> &names);

CCL_NAMESPACE_END
