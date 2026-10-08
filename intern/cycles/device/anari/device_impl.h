/* SPDX-FileCopyrightText: 2011-2026 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

#pragma once

#ifdef WITH_ANARI

#  include <anari/anari.h>

#  include "device/device.h"

#  include "util/map.h"
#  include "util/set.h"
#  include "util/thread.h"
#  include "util/types.h"
#  include "util/unique_ptr.h"

CCL_NAMESPACE_BEGIN

class AnariScene;

/* Tracing of the ANARI calls for debugging, enabled with the CYCLES_ANARI_TRACE environment
 * variable. */
bool anari_trace_enabled();
#  define ANARI_TRACE(...) \
    if (anari_trace_enabled()) { \
      fprintf(stderr, "[cycles anari] " __VA_ARGS__); \
      fputc('\n', stderr); \
      fflush(stderr); \
    } \
    ((void)0)

/* Get the library loaded with the given full name, loading it if needed. Libraries are kept
 * loaded for the lifetime of the process. */
ANARILibrary anari_library_get(const string &full_name);

/* Frame size and the region of the full camera image it covers, in normalized [0, 1] raster
 * coordinates with the origin in the lower left corner. */
struct AnariFrameWindow {
  int width = 0;
  int height = 0;
  float x0 = 0.0f, y0 = 0.0f, x1 = 1.0f, y1 = 1.0f;

  bool operator==(const AnariFrameWindow &other) const
  {
    return width == other.width && height == other.height && x0 == other.x0 && y0 == other.y0 &&
           x1 == other.x1 && y1 == other.y1;
  }
  bool operator!=(const AnariFrameWindow &other) const
  {
    return !(*this == other);
  }
};

/* Frame channels which are requested from the ANARI device. */
struct AnariFrameChannels {
  bool depth = false;
  bool normal = false;
  bool albedo = false;
  bool object_id = false;
  bool instance_id = false;

  bool operator==(const AnariFrameChannels &other) const
  {
    return depth == other.depth && normal == other.normal && albedo == other.albedo &&
           object_id == other.object_id && instance_id == other.instance_id;
  }
  bool operator!=(const AnariFrameChannels &other) const
  {
    return !(*this == other);
  }
};

/* Mapped frame buffer channels, pointers are null for channels which are not available. */
struct AnariMappedFrame {
  int width = 0;
  int height = 0;
  const float4 *color = nullptr;
  const float *depth = nullptr;
  /* 12 byte vectors as in ANARI, not the 16 byte aligned Cycles float3. */
  const packed_float3 *normal = nullptr;
  const packed_float3 *albedo = nullptr;
  const uint *object_id = nullptr;
  const uint *instance_id = nullptr;
};

/* Image texture loaded by the image manager, accessible by the scene export to create samplers. */
struct AnariImage {
  const void *pixels = nullptr;
  ImageDataType data_type = IMAGE_DATA_NUM_TYPES;
  int width = 0;
  int height = 0;
  /* Incremented whenever the pixels change, so cached ANARI arrays can be updated. */
  uint64_t version = 0;
};

class AnariDevice : public Device {
 public:
  AnariDevice(const DeviceInfo &info_, Stats &stats_, Profiler &profiler_, bool headless_);
  ~AnariDevice() override;

  /* Device. */
  BVHLayoutMask get_bvh_layout_mask(uint64_t kernel_features) const override;
  void build_bvh(BVH *bvh, Progress &progress, bool refit) override;
  void const_copy_to(const char *name, void *host, const size_t size) override;
  bool load_kernels(uint64_t kernel_features) override;

  void prepare_scene_update(Scene *scene) override;
  void optimize_for_scene(Scene *scene) override;

  bool has_unified_memory_any() const override
  {
    return true;
  }
  bool has_unified_image_memory_all() const override
  {
    return true;
  }

  /* ANARI handles. */
  ANARIDevice anari_device() const
  {
    return device_;
  }
  const string &library_name() const
  {
    return library_short_name_;
  }
  /* Subtype of the renderer, "default" unless chosen with the "renderer" parameter. */
  const string &renderer_subtype() const
  {
    return renderer_subtype_;
  }

  /* Whether the device has an extension or supports an object subtype. */
  bool has_extension(const char *name) const;
  bool has_subtype(ANARIDataType type, const char *subtype) const;

  /* Rendering, used by PathTraceWorkANARI. */

  /* Whether the device accumulates samples across frames by itself, in which case the frame
   * contains the average of all samples since the last reset. Otherwise every frame is an
   * independent estimate which is accumulated in the Cycles render buffers. */
  bool accumulates_samples() const
  {
    return accumulates_samples_;
  }

  /* Set up the frame for the given window and channels, restarting accumulation if needed. */
  void frame_setup(const AnariFrameWindow &window,
                   const AnariFrameChannels &channels,
                   bool reset_accumulation);
  /* Render one frame and wait for it. Returns false on error. */
  bool frame_render();
  /* Number of samples accumulated in the frame since the last reset. */
  int frame_num_samples() const
  {
    return frame_num_samples_;
  }
  AnariMappedFrame frame_map();
  void frame_unmap();
  /* Convert the ANARI depth (distance along the ray) to Cycles depth for a frame pixel. */
  float frame_depth_to_camera_z(float t, int x, int y) const;

  /* Images, used by the scene export. */
  const AnariImage *find_image(uint image_info_id) const;

 protected:
  void mem_alloc(device_memory &mem) override;
  void mem_copy_to(device_memory &mem) override;
  void mem_move_to_host(device_memory &mem) override;
  void mem_copy_from(
      device_memory &mem, const size_t y, size_t w, const size_t h, size_t elem) override;
  void mem_zero(device_memory &mem) override;
  void mem_free(device_memory &mem) override;
  device_ptr mem_alloc_sub_ptr(device_memory &mem, const size_t offset, size_t size) override;

  void image_alloc(device_image &mem);
  void image_free(device_image &mem);

  static void status_callback(const void *user_data,
                              ANARIDevice device,
                              ANARIObject source,
                              ANARIDataType source_type,
                              ANARIStatusSeverity severity,
                              ANARIStatusCode code,
                              const char *message);

  ANARILibrary library_ = nullptr;
  ANARIDevice device_ = nullptr;
  string library_short_name_;
  string renderer_subtype_ = "default";
  set<string> extensions_;
  bool accumulates_samples_ = false;

  ANARIFrame frame_ = nullptr;
  AnariFrameWindow frame_window_;
  AnariFrameChannels frame_channels_;
  bool frame_mapped_ = false;
  int frame_num_samples_ = 0;

  unique_ptr<AnariScene> scene_;

  mutable thread_mutex images_mutex_;
  map<uint, AnariImage> images_;
  uint64_t image_version_ = 0;
};

CCL_NAMESPACE_END

#endif /* WITH_ANARI */
