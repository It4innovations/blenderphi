/* SPDX-FileCopyrightText: 2011-2022 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

#include "braas_hpc_display_driver.h"

#include "util/log.h"
#include "util/string.h"

CCL_NAMESPACE_BEGIN

/* --------------------------------------------------------------------
 * BRaaSHPCDisplayDriver.
 */

BRaaSHPCDisplayDriver::BRaaSHPCDisplayDriver()
{
	d_pixels = nullptr;
	duration = 0.0f;	
	width = 0;
	height = 0;
	use_gpujpeg = false;
	render_finished = 1;

	const char* env_use_gpujpeg = getenv("CYCLES_BRAAS_HPC_USE_GPUJPEG");
	if (env_use_gpujpeg) {
		int enabled = atoi(env_use_gpujpeg);
		if (enabled) {
#ifdef WITH_CLIENT_GPUJPEG
			use_gpujpeg = true;
#else
			printf("BRaaSHPCDisplayDriver (enable_gpujpeg): Not compiled with GPUJPEG support\n");
#endif	
		}
	}
}

BRaaSHPCDisplayDriver::~BRaaSHPCDisplayDriver() {}

/* --------------------------------------------------------------------
 * Update procedure.
 */

void BRaaSHPCDisplayDriver::next_tile_begin()
{
  /* Assuming no tiles used in interactive display. */
}

bool BRaaSHPCDisplayDriver::update_begin(const Params& /*params*/, int texture_width, int texture_height)
{
	start = std::chrono::steady_clock::now();

	width = texture_width;
	height = texture_height;

	//if (pixels.size() != width * height) {
	//	pixels.resize(width * height);
	//}

	return true;
}

void BRaaSHPCDisplayDriver::update_end()
{
	renderEnd();
}

void BRaaSHPCDisplayDriver::renderBegin()
{
	/* Signal session thread to start. */
	{
		const thread_scoped_lock session_thread_lock(mutex);
		render_finished = 0;
	}
	cv.notify_all();
}

void BRaaSHPCDisplayDriver::renderEnd()
{
	auto end = std::chrono::steady_clock::now();
	duration = std::chrono::duration<float>(end - start).count();

	/* Signal session thread to end. */
	{
		const thread_scoped_lock session_thread_lock(mutex);
		render_finished = 1;
	}
	cv.notify_all();
}

void BRaaSHPCDisplayDriver::wait()
{
	while (true) {
		thread_scoped_lock session_thread_lock(mutex);
		if (render_finished == 1) {
			break;
		}
		cv.wait(session_thread_lock);
	}
}

//bool BRaaSHPCDisplayDriver::ready() const
//{
//	return render_finished;
//}

void BRaaSHPCDisplayDriver::copy_texture_buffer(const half4* rgba_pixels, int /*texture_x*/, int /*texture_y*/, int /*pixels_width*/, int /*pixels_height*/)
{
	d_pixels = (void*)rgba_pixels;
}

/* --------------------------------------------------------------------
 * Texture buffer mapping.
 */

half4* BRaaSHPCDisplayDriver::map_texture_buffer()
{
	if (pixels.size() != width * height) {
		pixels.resize(width * height);
	}

	half4* mapped_rgba_pixels = (half4*)pixels.data();

	return mapped_rgba_pixels;
}

void BRaaSHPCDisplayDriver::unmap_texture_buffer()
{
	renderEnd();
}

/* --------------------------------------------------------------------
 * Drawing.
 */

void BRaaSHPCDisplayDriver::zero()
{
}

void BRaaSHPCDisplayDriver::draw(const Params& /*params*/)
{
}

CCL_NAMESPACE_END
