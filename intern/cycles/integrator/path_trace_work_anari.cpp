/* SPDX-FileCopyrightText: 2011-2026 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

#ifdef WITH_ANARI

#  include "integrator/path_trace_work_anari.h"

#  include "device/anari/device_impl.h"

#  include "integrator/pass_accessor_cpu.h"
#  include "integrator/path_trace_display.h"

#  include "scene/film.h"
#  include "scene/pass.h"
#  include "scene/scene.h"
#  include "session/buffers.h"

#  include "util/log.h"
#  include "util/tbb.h"

CCL_NAMESPACE_BEGIN

PathTraceWorkANARI::PathTraceWorkANARI(Device *device,
                                       Film *film,
                                       DeviceScene *device_scene,
                                       const bool *cancel_requested_flag)
    : PathTraceWork(device, film, device_scene, cancel_requested_flag),
      anari_device_(static_cast<AnariDevice *>(device))
{
  DCHECK_EQ(device->info.type, DEVICE_ANARI);
}

void PathTraceWorkANARI::render_samples(RenderStatistics &statistics,
                                        const int /*start_sample*/,
                                        const int samples_num,
                                        const int /*sample_offset*/)
{
  const BufferParams &params = effective_buffer_params_;
  if (params.width <= 0 || params.height <= 0 || params.full_width <= 0 ||
      params.full_height <= 0)
  {
    return;
  }

  /* Region of the full image covered by the render buffer. */
  AnariFrameWindow window;
  window.width = params.width;
  window.height = params.height;
  window.x0 = float(params.full_x) / float(params.full_width);
  window.y0 = float(params.full_y) / float(params.full_height);
  window.x1 = float(params.full_x + params.width) / float(params.full_width);
  window.y1 = float(params.full_y + params.height) / float(params.full_height);

  /* Frame channels for the passes in the render buffers. */
  AnariFrameChannels channels;
  channels.depth = params.get_pass_offset(PASS_DEPTH) != PASS_UNUSED;
  channels.normal = anari_device_->has_extension("ANARI_KHR_FRAME_CHANNEL_NORMAL") &&
                    (params.get_pass_offset(PASS_NORMAL) != PASS_UNUSED ||
                     params.get_pass_offset(PASS_DENOISING_NORMAL) != PASS_UNUSED);
  channels.albedo = anari_device_->has_extension("ANARI_KHR_FRAME_CHANNEL_ALBEDO") &&
                    params.get_pass_offset(PASS_DENOISING_ALBEDO) != PASS_UNUSED;
  channels.object_id = anari_device_->has_extension("ANARI_KHR_FRAME_CHANNEL_OBJECT_ID") &&
                       params.get_pass_offset(PASS_MATERIAL_ID) != PASS_UNUSED;
  channels.instance_id = anari_device_->has_extension("ANARI_KHR_FRAME_CHANNEL_INSTANCE_ID") &&
                         params.get_pass_offset(PASS_OBJECT_ID) != PASS_UNUSED;

  anari_device_->frame_setup(window, channels, need_reset_);
  need_reset_ = false;

  const bool device_accumulates = anari_device_->accumulates_samples();

  int num_rendered = 0;
  for (int sample = 0; sample < samples_num; ++sample) {
    if (is_cancel_requested()) {
      break;
    }
    if (!anari_device_->frame_render()) {
      break;
    }
    num_rendered++;

    if (!device_accumulates) {
      /* Every frame is an independent estimate, accumulate in the render buffers. */
      const AnariMappedFrame frame = anari_device_->frame_map();
      write_render_buffers(frame, true, num_samples_ + num_rendered);
      anari_device_->frame_unmap();
    }
  }

  num_samples_ += num_rendered;

  if (device_accumulates && num_rendered > 0) {
    /* The frame holds the average of all samples since the last reset. */
    const AnariMappedFrame frame = anari_device_->frame_map();
    write_render_buffers(frame, false, num_samples_);
    anari_device_->frame_unmap();
  }

  statistics.occupancy = 1.0f;
}

void PathTraceWorkANARI::write_render_buffers(const AnariMappedFrame &frame,
                                              const bool accumulate,
                                              const int num_samples)
{
  const BufferParams &params = effective_buffer_params_;
  if (frame.color == nullptr || frame.width != params.width || frame.height != params.height) {
    return;
  }

  const int pass_stride = params.pass_stride;
  const int pass_combined = params.get_pass_offset(PASS_COMBINED);
  const int pass_depth = params.get_pass_offset(PASS_DEPTH);
  const int pass_normal = params.get_pass_offset(PASS_NORMAL);
  const int pass_denoising_normal = params.get_pass_offset(PASS_DENOISING_NORMAL);
  const int pass_denoising_albedo = params.get_pass_offset(PASS_DENOISING_ALBEDO);
  const int pass_object_id = params.get_pass_offset(PASS_OBJECT_ID);
  const int pass_material_id = params.get_pass_offset(PASS_MATERIAL_ID);
  const int pass_sample_count = params.get_pass_offset(PASS_SAMPLE_COUNT);

  /* When overwriting, the averaged frame is scaled to the sum the film conversion expects. */
  const float scale = accumulate ? 1.0f : float(num_samples);

  float *render_buffer = buffers_->buffer.data();

  parallel_for(0, params.height, [&](const int y) {
    for (int x = 0; x < params.width; x++) {
      const int64_t pixel = int64_t(params.offset) + (params.full_x + x) +
                            int64_t(params.full_y + y) * params.stride;
      float *buffer = render_buffer + pixel * pass_stride;
      const int64_t index = int64_t(y) * frame.width + x;

      auto write = [&](const int offset, const float *value, const int num_components) {
        if (offset == PASS_UNUSED) {
          return;
        }
        float *out = buffer + offset;
        if (accumulate) {
          for (int i = 0; i < num_components; i++) {
            out[i] += value[i];
          }
        }
        else {
          for (int i = 0; i < num_components; i++) {
            out[i] = value[i] * scale;
          }
        }
      };

      if (pass_combined != PASS_UNUSED) {
        const float4 c = frame.color[index];
        /* The fourth channel of the combined pass holds the transparency. */
        const float value[4] = {c.x, c.y, c.z, 1.0f - c.w};
        write(pass_combined, value, 4);
      }

      if (frame.normal) {
        const float3 n = frame.normal[index];
        const float value[3] = {n.x, n.y, n.z};
        write(pass_normal, value, 3);
        write(pass_denoising_normal, value, 3);
      }
      if (frame.albedo) {
        const float3 a = frame.albedo[index];
        const float value[3] = {a.x, a.y, a.z};
        write(pass_denoising_albedo, value, 3);
      }

      /* Data passes which are written once and not averaged. */
      if (pass_depth != PASS_UNUSED && frame.depth) {
        const float t = frame.depth[index];
        buffer[pass_depth] = (std::isfinite(t) && t > 0.0f) ?
                                 anari_device_->frame_depth_to_camera_z(t, x, y) :
                                 0.0f;
      }
      if (pass_object_id != PASS_UNUSED && frame.instance_id) {
        const uint id = frame.instance_id[index];
        buffer[pass_object_id] = (id == ~0u) ? 0.0f : float(id);
      }
      if (pass_material_id != PASS_UNUSED && frame.object_id) {
        const uint id = frame.object_id[index];
        buffer[pass_material_id] = (id == ~0u) ? 0.0f : float(id);
      }
      if (pass_sample_count != PASS_UNUSED) {
        *reinterpret_cast<uint *>(buffer + pass_sample_count) = uint(num_samples);
      }
    }
  });
}

void PathTraceWorkANARI::copy_to_display(PathTraceDisplay *display,
                                         PassMode pass_mode,
                                         const int num_samples)
{
  half4 *rgba_half = display->map_texture_buffer();
  if (!rgba_half) {
    return;
  }

  const KernelFilm &kfilm = device_scene_->data.film;

  const PassAccessor::PassAccessInfo pass_access_info = get_display_pass_access_info(pass_mode);
  if (pass_access_info.type == PASS_NONE) {
    display->unmap_texture_buffer();
    return;
  }

  const BufferParams &effective_buffer_params = (pass_mode == PassMode::DENOISED) ?
                                                    effective_denoised_buffer_params_ :
                                                    effective_buffer_params_;

  const PassAccessorCPU pass_accessor(pass_access_info, kfilm.exposure, num_samples);

  PassAccessor::Destination destination = get_display_destination_template(display, pass_mode);
  destination.pixels_half_rgba = rgba_half;

  pass_accessor.get_render_tile_pixels(buffers_.get(), effective_buffer_params, destination);

  display->unmap_texture_buffer();
}

void PathTraceWorkANARI::destroy_gpu_resources(PathTraceDisplay * /*display*/) {}

bool PathTraceWorkANARI::copy_render_buffers_from_device()
{
  return buffers_->copy_from_device();
}

bool PathTraceWorkANARI::copy_render_buffers_to_device()
{
  buffers_->buffer.copy_to_device();
  return true;
}

bool PathTraceWorkANARI::zero_render_buffers()
{
  buffers_->zero();
  num_samples_ = 0;
  need_reset_ = true;
  return true;
}

int PathTraceWorkANARI::adaptive_sampling_converge_filter_count_active(const float /*threshold*/,
                                                                       bool /*reset*/)
{
  /* Adaptive sampling is not supported, all pixels stay active until the sample limit. */
  return effective_buffer_params_.width * effective_buffer_params_.height;
}

CCL_NAMESPACE_END

#endif /* WITH_ANARI */
