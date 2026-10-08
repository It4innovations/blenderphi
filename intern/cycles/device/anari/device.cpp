/* SPDX-FileCopyrightText: 2011-2026 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

#include "device/anari/device.h"

#include "device/device.h"

#ifdef WITH_ANARI
#  include "device/anari/device_impl.h"

#  include <anari/anari.h>

#  include <algorithm>

#  include "util/log.h"
#  include "util/map.h"
#  include "util/string.h"
#  include "util/thread.h"
#endif

CCL_NAMESPACE_BEGIN

#ifdef WITH_ANARI

namespace {

/* Back-ends which are probed by default. VisGL is not listed: it renders with its own OpenGL
 * context which conflicts with the one of Blender. */
const char *const known_library_names[] = {
    "barney",
    "cycles",
    "mitsuba",
    "moonray",
    "visrtx",
    "helide",
    "ospray",
    "visionaray",
    "visionaray_cuda",
    "rpr",
    "photon",
};

thread_mutex search_paths_mutex;

vector<string> &library_search_paths()
{
  static vector<string> paths;
  return paths;
}

vector<string> &extra_library_names()
{
  static vector<string> names;
  return names;
}

void library_status_callback(const void * /*user_data*/,
                             ANARIDevice /*device*/,
                             ANARIObject /*source*/,
                             ANARIDataType /*source_type*/,
                             ANARIStatusSeverity severity,
                             ANARIStatusCode /*code*/,
                             const char *message)
{
  /* Library level messages are mostly about libraries which are not installed while probing
   * for available back-ends, which is expected. */
  (void)severity;
  LOG_DEBUG << "ANARI: " << message;
}

/* Library name with an optional directory, in the form understood by `anariLoadLibrary`. */
string library_name_with_path(const string &name, const string &directory)
{
  if (directory.empty()) {
    return name;
  }
  string dir = directory;
  if (!dir.empty() && dir.back() != '/' && dir.back() != '\\') {
    dir += "/";
  }
  return name + "," + dir;
}

/* Libraries are never unloaded: some back-ends register process exit handlers or keep
 * global GPU state which does not survive unloading the library (Mitsuba, Barney). */
map<string, ANARILibrary> &loaded_libraries()
{
  static map<string, ANARILibrary> libraries;
  return libraries;
}

/* Load the library from the first location it is found in. Returns the full library name used
 * to load it, so the device can be created from the same location later on. */
ANARILibrary load_library(const string &name, string &r_full_name)
{
  vector<string> locations;
  {
    const thread_scoped_lock lock(search_paths_mutex);
    locations = library_search_paths();
  }
  /* Default search: next to the ANARI front-end library and in the system search path. */
  locations.insert(locations.begin(), string());

  for (const string &location : locations) {
    const string full_name = library_name_with_path(name, location);
    ANARILibrary library = anari_library_get(full_name);
    if (library) {
      r_full_name = full_name;
      return library;
    }
  }
  return nullptr;
}

}  // namespace

ANARILibrary anari_library_get(const string &full_name)
{
  const thread_scoped_lock lock(search_paths_mutex);
  auto it = loaded_libraries().find(full_name);
  if (it != loaded_libraries().end()) {
    return it->second;
  }
  ANARILibrary library = anariLoadLibrary(full_name.c_str(), library_status_callback, nullptr);
  if (library) {
    loaded_libraries()[full_name] = library;
  }
  return library;
}

unique_ptr<Device> device_anari_create(const DeviceInfo &info,
                                       Stats &stats,
                                       Profiler &profiler,
                                       bool headless)
{
  return make_unique<AnariDevice>(info, stats, profiler, headless);
}

void device_anari_info(vector<DeviceInfo> &devices)
{
  vector<string> names(std::begin(known_library_names), std::end(known_library_names));
  {
    const thread_scoped_lock lock(search_paths_mutex);
    for (const string &name : extra_library_names()) {
      if (std::find(names.begin(), names.end(), name) == names.end()) {
        names.push_back(name);
      }
    }
  }
  if (getenv("ANARI_LIBRARY")) {
    names.push_back("environment");
  }

  for (const string &name : names) {
    string full_name;
    ANARILibrary library = load_library(name, full_name);
    if (!library) {
      continue;
    }

    const char **subtypes = anariGetDeviceSubtypes(library);
    for (int i = 0; subtypes && subtypes[i]; i++) {
      const string subtype = subtypes[i];
      /* MPI devices need an MPI launcher, they can't be used from within Blender. */
      if (subtype == "mpi") {
        continue;
      }

      DeviceInfo info;
      info.type = DEVICE_ANARI;
      info.id = "ANARI_" + name + "_" + subtype;
      info.description = "ANARI " + name;
      if (subtype != "default") {
        info.description += " (" + subtype + ")";
      }
      info.num = int(devices.size());
      info.display_device = false;
      info.has_nanovdb = false;
      info.has_osl = false;
      info.has_guiding = false;
      info.has_profiling = false;
      info.has_peer_memory = false;
      info.has_gpu_queue = false;
      info.denoisers = DENOISER_NONE;
      info.anari_library = full_name;
      info.anari_device_subtype = subtype;

      devices.push_back(info);
    }
  }
}

string device_anari_capabilities()
{
  string capabilities;
  vector<DeviceInfo> devices;
  device_anari_info(devices);
  for (const DeviceInfo &info : devices) {
    capabilities += info.description + " [" + info.anari_library + ", " +
                    info.anari_device_subtype + "]\n";
  }
  return capabilities;
}

void device_anari_set_library_search_paths(const vector<string> &paths)
{
  {
    const thread_scoped_lock lock(search_paths_mutex);
    library_search_paths() = paths;
  }
  Device::tag_update();
}

void device_anari_set_extra_library_names(const vector<string> &names)
{
  {
    const thread_scoped_lock lock(search_paths_mutex);
    extra_library_names() = names;
  }
  Device::tag_update();
}

#else

unique_ptr<Device> device_anari_create(const DeviceInfo & /*info*/,
                                       Stats & /*stats*/,
                                       Profiler & /*profiler*/,
                                       bool /*headless*/)
{
  return nullptr;
}

void device_anari_info(vector<DeviceInfo> & /*devices*/) {}

string device_anari_capabilities()
{
  return "";
}

void device_anari_set_library_search_paths(const vector<string> & /*paths*/) {}

void device_anari_set_extra_library_names(const vector<string> & /*names*/) {}

#endif

CCL_NAMESPACE_END
