/* SPDX-FileCopyrightText: 2011-2026 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

#pragma once

#ifdef WITH_ANARI

#  include "integrator/path_trace_work.h"

CCL_NAMESPACE_BEGIN

class AnariDevice;
struct AnariMappedFrame;

/* Path trace work which renders with an ANARI device and writes the rendered frame into the
 * render buffers, so the rest of the pipeline (passes, display, denoising, output) is shared
 * with the other devices. */
class PathTraceWorkANARI : public PathTraceWork {
 public:
  PathTraceWorkANARI(Device *device,
                     Film *film,
                     DeviceScene *device_scene,
                     const bool *cancel_requested_flag);

  void init_execution() override {}

  void render_samples(RenderStatistics &statistics,
                      const int start_sample,
                      const int samples_num,
                      const int sample_offset) override;

  void copy_to_display(PathTraceDisplay *display,
                       PassMode pass_mode,
                       const int num_samples) override;
  void destroy_gpu_resources(PathTraceDisplay *display) override;

  bool copy_render_buffers_from_device() override;
  bool copy_render_buffers_to_device() override;
  bool zero_render_buffers() override;

  int adaptive_sampling_converge_filter_count_active(const float threshold, bool reset) override;
  void cryptomatte_postproces() override {}
  void denoise_volume_guiding_buffers() override {}

 protected:
  /* Write the frame into the render buffers. When `accumulate` is false the passes are
   * overwritten with the frame scaled by the number of samples, otherwise added. */
  void write_render_buffers(const AnariMappedFrame &frame, bool accumulate, int num_samples);

  AnariDevice *anari_device_;

  /* Number of samples in the render buffers. */
  int num_samples_ = 0;
  bool need_reset_ = true;
};

CCL_NAMESPACE_END

#endif /* WITH_ANARI */
