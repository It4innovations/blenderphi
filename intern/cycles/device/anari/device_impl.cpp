/* SPDX-FileCopyrightText: 2011-2026 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

#ifdef WITH_ANARI

#  include "device/anari/device_impl.h"
#  include "device/anari/scene.h"

#  include <cstdlib>
#  include <cstring>

#  include "util/aligned_malloc.h"
#  include "util/log.h"
#  include "util/string.h"
#  include "util/time.h"

CCL_NAMESPACE_BEGIN

namespace {

/* Parse a list of "name=value" device parameters separated by ';' or newlines. The type of the
 * value is guessed: booleans, integers, floats and otherwise strings. */
/* Set the parameters of the device, given as name=value pairs. The pseudo parameter "renderer"
 * selects the subtype of the renderer instead. */
void set_device_parameters(ANARIDevice device, const string &parameters, string &renderer_subtype)
{
  vector<string> items;
  string_split(items, parameters, ";\n", true);
  for (string item : items) {
    const size_t eq = item.find('=');
    if (eq == string::npos) {
      continue;
    }
    const string name = string_strip(item.substr(0, eq));
    const string value = string_strip(item.substr(eq + 1));
    if (name.empty()) {
      continue;
    }
    if (name == "renderer") {
      renderer_subtype = value;
      continue;
    }

    if (value == "true" || value == "false") {
      const bool b = value == "true";
      anariSetParameter(device, device, name.c_str(), ANARI_BOOL, &b);
      continue;
    }

    char *end = nullptr;
    const long long ivalue = strtoll(value.c_str(), &end, 10);
    if (end && *end == '\0' && !value.empty()) {
      const int32_t i = int32_t(ivalue);
      anariSetParameter(device, device, name.c_str(), ANARI_INT32, &i);
      continue;
    }

    const double fvalue = strtod(value.c_str(), &end);
    if (end && *end == '\0' && !value.empty()) {
      const float f = float(fvalue);
      anariSetParameter(device, device, name.c_str(), ANARI_FLOAT32, &f);
      continue;
    }

    anariSetParameter(device, device, name.c_str(), ANARI_STRING, value.c_str());
  }
}

}  // namespace

bool anari_trace_enabled()
{
  static const bool enabled = getenv("CYCLES_ANARI_TRACE") != nullptr;
  return enabled;
}

AnariDevice::AnariDevice(const DeviceInfo &info_,
                         Stats &stats_,
                         Profiler &profiler_,
                         bool headless_)
    : Device(info_, stats_, profiler_, headless_)
{
  library_short_name_ = info.anari_library.substr(0, info.anari_library.find(','));

  ANARI_TRACE("load library %s", info.anari_library.c_str());
  library_ = anari_library_get(info.anari_library);
  if (!library_) {
    set_error(string_printf("Failed to load ANARI library \"%s\"", info.anari_library.c_str()));
    return;
  }

  const string subtype = info.anari_device_subtype.empty() ? "default" :
                                                              info.anari_device_subtype;

  const char **extensions = anariGetDeviceExtensions(library_, subtype.c_str());
  for (int i = 0; extensions && extensions[i]; i++) {
    extensions_.insert(extensions[i]);
  }

  ANARI_TRACE("new device %s", subtype.c_str());
  device_ = anariNewDevice(library_, subtype.c_str());
  if (!device_) {
    set_error(string_printf("Failed to create ANARI device \"%s\" of library \"%s\"",
                            subtype.c_str(),
                            library_short_name_.c_str()));
    return;
  }

  ANARIStatusCallback callback = status_callback;
  anariSetParameter(device_, device_, "statusCallback", ANARI_STATUS_CALLBACK, &callback);
  /* The value of pointer parameters is the pointer itself. */
  anariSetParameter(device_, device_, "statusCallbackUserData", ANARI_VOID_POINTER, this);

  /* Parameters for specific back-ends, configured by the user. */
  if (const char *env_parameters = getenv("CYCLES_ANARI_DEVICE_PARAMETERS")) {
    set_device_parameters(device_, env_parameters, renderer_subtype_);
  }
  set_device_parameters(device_, info.anari_device_parameters, renderer_subtype_);

  ANARI_TRACE("commit device");
  anariCommitParameters(device_, device_);

  /* Devices with the frame accumulation extension accumulate samples by themselves as long as
   * nothing changes. Others render an independent image with every frame. */
  accumulates_samples_ = has_extension("ANARI_KHR_FRAME_ACCUMULATION") ||
                         library_short_name_ == "barney" || library_short_name_ == "visrtx" ||
                         library_short_name_ == "cycles" || library_short_name_ == "mitsuba";
  if (const char *env_accumulation = getenv("CYCLES_ANARI_DEVICE_ACCUMULATION")) {
    accumulates_samples_ = atoi(env_accumulation) != 0;
  }

  ANARI_TRACE("new frame");
  frame_ = anariNewFrame(device_);
  if (accumulates_samples_) {
    /* KHR_FRAME_ACCUMULATION: accumulation is off unless enabled on the frame. */
    const bool accumulation = true;
    anariSetParameter(device_, frame_, "accumulation", ANARI_BOOL, &accumulation);
  }

  ANARI_TRACE("new scene");
  scene_ = make_unique<AnariScene>(*this);

  LOG_INFO << "Created ANARI device " << info.description << " (" << info.anari_library << ", "
           << subtype << "), accumulation by device: " << accumulates_samples_;
}

AnariDevice::~AnariDevice()
{
  if (device_) {
    if (frame_mapped_) {
      frame_unmap();
    }
    scene_.reset();
    if (frame_) {
      anariRelease(device_, frame_);
    }
    anariRelease(device_, device_);
  }
}

void AnariDevice::status_callback(const void *user_data,
                                  ANARIDevice /*device*/,
                                  ANARIObject /*source*/,
                                  ANARIDataType /*source_type*/,
                                  ANARIStatusSeverity severity,
                                  ANARIStatusCode /*code*/,
                                  const char *message)
{
  const AnariDevice *self = static_cast<const AnariDevice *>(user_data);
  const string &name = self ? self->library_short_name_ : string();

  switch (severity) {
    case ANARI_SEVERITY_FATAL_ERROR:
      LOG_ERROR << "ANARI " << name << " fatal error: " << message;
      break;
    case ANARI_SEVERITY_ERROR:
      LOG_ERROR << "ANARI " << name << " error: " << message;
      break;
    case ANARI_SEVERITY_WARNING:
      LOG_WARNING << "ANARI " << name << " warning: " << message;
      break;
    case ANARI_SEVERITY_PERFORMANCE_WARNING:
      LOG_INFO << "ANARI " << name << " performance warning: " << message;
      break;
    case ANARI_SEVERITY_INFO:
      LOG_DEBUG << "ANARI " << name << ": " << message;
      break;
    case ANARI_SEVERITY_DEBUG:
    default:
      LOG_TRACE << "ANARI " << name << ": " << message;
      break;
  }
}

bool AnariDevice::has_extension(const char *name) const
{
  return extensions_.find(name) != extensions_.end();
}

bool AnariDevice::has_subtype(ANARIDataType type, const char *subtype) const
{
  if (!device_) {
    return false;
  }
  const char **subtypes = anariGetObjectSubtypes(device_, type);
  for (int i = 0; subtypes && subtypes[i]; i++) {
    if (strcmp(subtypes[i], subtype) == 0) {
      return true;
    }
  }
  return false;
}

/* --------------------------------------------------------------------
 * Device.
 */

BVHLayoutMask AnariDevice::get_bvh_layout_mask(uint64_t /*kernel_features*/) const
{
  /* The ANARI back-end builds its own acceleration structures, request the simplest layout and
   * skip building it, see #build_bvh. */
  return BVH_LAYOUT_BVH2;
}

void AnariDevice::build_bvh(BVH * /*bvh*/, Progress & /*progress*/, bool /*refit*/) {}

void AnariDevice::const_copy_to(const char * /*name*/, void * /*host*/, const size_t /*size*/) {}

bool AnariDevice::load_kernels(const uint64_t /*kernel_features*/)
{
  return device_ != nullptr && !have_error();
}

void AnariDevice::prepare_scene_update(Scene *scene)
{
  if (scene_) {
    scene_->prepare_update(scene);
  }
}

void AnariDevice::optimize_for_scene(Scene *scene)
{
  if (scene_) {
    const double start_time = time_dt();
    scene_->update(scene);
    LOG_DEBUG << "ANARI scene update in " << time_dt() - start_time << " seconds";
  }
}

/* --------------------------------------------------------------------
 * Memory.
 *
 * All data lives in host memory, the scene export reads it from there.
 */

void AnariDevice::mem_alloc(device_memory &mem)
{
  if (mem.type == MEM_IMAGE_TEXTURE) {
    assert(!"mem_alloc not supported for images.");
    return;
  }
  if (mem.type == MEM_GLOBAL) {
    assert(!"mem_alloc not supported for global memory.");
    return;
  }

  if (mem.type == MEM_DEVICE_ONLY) {
    void *data = util_aligned_malloc(mem.memory_size(), MIN_ALIGNMENT_DEVICE_MEMORY);
    mem.device_pointer = (device_ptr)data;
  }
  else {
    mem.device_pointer = (device_ptr)mem.host_pointer;
  }

  mem.device_size = mem.memory_size();
  stats.mem_alloc(mem.device_size);
}

void AnariDevice::mem_copy_to(device_memory &mem)
{
  if (mem.type == MEM_GLOBAL) {
    if (mem.device_pointer) {
      stats.mem_free(mem.device_size);
    }
    mem.device_pointer = (device_ptr)mem.host_pointer;
    mem.device_size = mem.memory_size();
    stats.mem_alloc(mem.device_size);
  }
  else if (mem.type == MEM_IMAGE_TEXTURE) {
    image_free((device_image &)mem);
    image_alloc((device_image &)mem);
  }
  else if (!mem.device_pointer) {
    mem_alloc(mem);
  }
}

void AnariDevice::mem_move_to_host(device_memory & /*mem*/) {}

void AnariDevice::mem_copy_from(
    device_memory & /*mem*/, size_t /*y*/, size_t /*w*/, size_t /*h*/, size_t /*elem*/)
{
}

void AnariDevice::mem_zero(device_memory &mem)
{
  if (!mem.device_pointer) {
    mem_alloc(mem);
  }
  if (mem.device_pointer) {
    memset((void *)mem.device_pointer, 0, mem.memory_size());
  }
}

void AnariDevice::mem_free(device_memory &mem)
{
  if (mem.type == MEM_IMAGE_TEXTURE) {
    image_free((device_image &)mem);
  }
  else if (mem.device_pointer) {
    if (mem.type == MEM_DEVICE_ONLY) {
      util_aligned_free((void *)mem.device_pointer, mem.memory_size());
    }
    mem.device_pointer = 0;
    stats.mem_free(mem.device_size);
    mem.device_size = 0;
  }
}

device_ptr AnariDevice::mem_alloc_sub_ptr(device_memory &mem,
                                          const size_t offset,
                                          size_t /*size*/)
{
  return (device_ptr)(((char *)mem.device_pointer) + mem.memory_elements_size(offset));
}

void AnariDevice::image_alloc(device_image &mem)
{
  mem.device_pointer = (device_ptr)mem.host_pointer;
  mem.device_size = mem.memory_size();
  stats.mem_alloc(mem.device_size);

  const thread_scoped_lock lock(images_mutex_);
  AnariImage &image = images_[mem.image_info_id];
  image.pixels = mem.host_pointer;
  image.data_type = ImageDataType(mem.info.data_type);
  image.width = int(mem.data_width);
  image.height = int(max(mem.data_height, size_t(1)));
  image.version = ++image_version_;
}

void AnariDevice::image_free(device_image &mem)
{
  if (mem.device_pointer) {
    mem.device_pointer = 0;
    stats.mem_free(mem.device_size);
    mem.device_size = 0;

    const thread_scoped_lock lock(images_mutex_);
    images_.erase(mem.image_info_id);
  }
}

const AnariImage *AnariDevice::find_image(uint image_info_id) const
{
  const thread_scoped_lock lock(images_mutex_);
  auto it = images_.find(image_info_id);
  return (it != images_.end()) ? &it->second : nullptr;
}

/* --------------------------------------------------------------------
 * Frame.
 */

void AnariDevice::frame_setup(const AnariFrameWindow &window,
                              const AnariFrameChannels &channels,
                              bool reset_accumulation)
{
  if (!device_ || !frame_) {
    return;
  }

  if (frame_mapped_) {
    frame_unmap();
  }

  ANARI_TRACE("frame setup %dx%d", window.width, window.height);
  bool changed = scene_ ? scene_->update_frame(frame_, window) : false;

  if (window.width != frame_window_.width || window.height != frame_window_.height) {
    const uint32_t size[2] = {uint32_t(window.width), uint32_t(window.height)};
    anariSetParameter(device_, frame_, "size", ANARI_UINT32_VEC2, size);
    changed = true;
  }

  if (channels != frame_channels_ || frame_num_samples_ == 0 && frame_window_.width == 0) {
    const ANARIDataType color_type = ANARI_FLOAT32_VEC4;
    anariSetParameter(device_, frame_, "channel.color", ANARI_DATA_TYPE, &color_type);

    auto set_channel = [&](const char *name, bool use, ANARIDataType type) {
      if (use) {
        anariSetParameter(device_, frame_, name, ANARI_DATA_TYPE, &type);
      }
      else {
        anariUnsetParameter(device_, frame_, name);
      }
    };
    set_channel("channel.depth", channels.depth, ANARI_FLOAT32);
    set_channel("channel.normal", channels.normal, ANARI_FLOAT32_VEC3);
    set_channel("channel.albedo", channels.albedo, ANARI_FLOAT32_VEC3);
    set_channel("channel.objectId", channels.object_id, ANARI_UINT32);
    set_channel("channel.instanceId", channels.instance_id, ANARI_UINT32);
    changed = true;
  }

  frame_window_ = window;
  frame_channels_ = channels;

  if (changed || reset_accumulation) {
    /* Committing the frame restarts accumulation in the back-ends. */
    anariCommitParameters(device_, frame_);
    frame_num_samples_ = 0;
  }
}

bool AnariDevice::frame_render()
{
  if (!device_ || !frame_ || frame_window_.width <= 0 || frame_window_.height <= 0) {
    return false;
  }
  if (frame_mapped_) {
    frame_unmap();
  }

  ANARI_TRACE("render frame");
  anariRenderFrame(device_, frame_);
  anariFrameReady(device_, frame_, ANARI_WAIT);
  frame_num_samples_++;
  return !have_error();
}

AnariMappedFrame AnariDevice::frame_map()
{
  AnariMappedFrame mapped;
  if (!device_ || !frame_) {
    return mapped;
  }
  if (frame_mapped_) {
    frame_unmap();
  }

  auto map_channel = [&](const char *name, ANARIDataType expected_type) -> const void * {
    ANARI_TRACE("map %s", name);
    uint32_t width = 0;
    uint32_t height = 0;
    ANARIDataType type = ANARI_UNKNOWN;
    const void *data = anariMapFrame(device_, frame_, name, &width, &height, &type);
    if (data && (int(width) != frame_window_.width || int(height) != frame_window_.height ||
                 type != expected_type))
    {
      LOG_WARNING << "ANARI frame channel " << name << " has unexpected size or type";
      anariUnmapFrame(device_, frame_, name);
      return nullptr;
    }
    if (data) {
      mapped.width = int(width);
      mapped.height = int(height);
    }
    return data;
  };

  mapped.color = static_cast<const float4 *>(map_channel("channel.color", ANARI_FLOAT32_VEC4));
  if (frame_channels_.depth) {
    mapped.depth = static_cast<const float *>(map_channel("channel.depth", ANARI_FLOAT32));
  }
  if (frame_channels_.normal) {
    mapped.normal = static_cast<const packed_float3 *>(
        map_channel("channel.normal", ANARI_FLOAT32_VEC3));
  }
  if (frame_channels_.albedo) {
    mapped.albedo = static_cast<const packed_float3 *>(
        map_channel("channel.albedo", ANARI_FLOAT32_VEC3));
  }
  if (frame_channels_.object_id) {
    mapped.object_id = static_cast<const uint *>(map_channel("channel.objectId", ANARI_UINT32));
  }
  if (frame_channels_.instance_id) {
    mapped.instance_id = static_cast<const uint *>(
        map_channel("channel.instanceId", ANARI_UINT32));
  }

  frame_mapped_ = true;
  return mapped;
}

float AnariDevice::frame_depth_to_camera_z(const float t, const int x, const int y) const
{
  return scene_ ? scene_->depth_to_camera_z(t, x, y) : t;
}

void AnariDevice::frame_unmap()
{
  if (!frame_mapped_) {
    return;
  }
  anariUnmapFrame(device_, frame_, "channel.color");
  if (frame_channels_.depth) {
    anariUnmapFrame(device_, frame_, "channel.depth");
  }
  if (frame_channels_.normal) {
    anariUnmapFrame(device_, frame_, "channel.normal");
  }
  if (frame_channels_.albedo) {
    anariUnmapFrame(device_, frame_, "channel.albedo");
  }
  if (frame_channels_.object_id) {
    anariUnmapFrame(device_, frame_, "channel.objectId");
  }
  if (frame_channels_.instance_id) {
    anariUnmapFrame(device_, frame_, "channel.instanceId");
  }
  frame_mapped_ = false;
}

CCL_NAMESPACE_END

#endif /* WITH_ANARI */
