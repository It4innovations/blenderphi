/* SPDX-FileCopyrightText: 2011-2022 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include <atomic>

//#include "app/opengl/shader.h"

#include "session/display_driver.h"

//#include "util/function.h"
#include "util/unique_ptr.h"
#include "util/vector.h"
#include "util/thread.h"

CCL_NAMESPACE_BEGIN

class BRaaSHPCDisplayDriver : public DisplayDriver {
 public:
  /* Callbacks for enabling and disabling the OpenGL context. Must be provided to support enabling
   * the context on the Cycles render thread independent of the main thread. */
  BRaaSHPCDisplayDriver();
  ~BRaaSHPCDisplayDriver();

  //virtual void graphics_interop_activate() override;
  //virtual void graphics_interop_deactivate() override;

  

  //void set_zoom(float zoom_x, float zoom_y);

 //protected:
  virtual void next_tile_begin() override;

  virtual bool update_begin(const Params &params, int texture_width, int texture_height) override;
  virtual void update_end() override;

  virtual half4 *map_texture_buffer() override;
  virtual void copy_texture_buffer(const half4* rgba_pixels, int texture_x, int texture_y, int pixels_width, int pixels_height) override;
  virtual void unmap_texture_buffer() override;

  //virtual GraphicsInterop graphics_interop_get() override;

  virtual void zero() override;
  virtual void draw(const Params &params) override;

  virtual bool only_device_buffer() override 
  { 
	  return use_gpujpeg;
  };

  virtual bool buffer_uchar_srgba() override 
  { 
    return false;
  };

  void renderBegin();
  void renderEnd();

  void wait();
  //bool ready() const;

  bool is_gpujpeg() { return use_gpujpeg; };

public:
	vector<half4> pixels;
  //vector<uchar4> pixels;
	void* d_pixels;
	int render_finished;
	//std::atomic<bool> render_finished;

  thread_mutex mutex;
	thread_condition_variable cv;

  std::chrono::time_point<std::chrono::steady_clock> start;
	float duration;

  bool use_gpujpeg;
	int width;
	int height;
};

CCL_NAMESPACE_END
