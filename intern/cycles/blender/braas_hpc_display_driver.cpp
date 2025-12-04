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
	: d_pixels(nullptr), duration(0.0f) //render_finished(true),
{
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

	return true;
}

void BRaaSHPCDisplayDriver::update_end()
{
	auto end = std::chrono::steady_clock::now();
	duration = std::chrono::duration<float>(end - start).count();
	//renderEnd();
}

//void BRaaSHPCDisplayDriver::renderBegin()
//{
//	//std::lock_guard<std::mutex> lock(mutex);
//	thread_scoped_lock lock(mutex);
//	start = std::chrono::steady_clock::now();
//	render_finished = false;
//	//render_finished.store(false, std::memory_order_release);
//	//print("renderBegin()\n"); fflush(0);
//}

//void BRaaSHPCDisplayDriver::renderEnd()
//{
//	//std::lock_guard<std::mutex> lock(mutex);
//	thread_scoped_lock lock(mutex);
//
//	auto end = std::chrono::steady_clock::now();
//	duration = std::chrono::duration<float>(end - start).count();
//
//	render_finished = true;
//	//render_finished.store(true, std::memory_order_release);
//
//	// Notify the wait thread
//	cv.notify_all();
//	//print("renderEnd()\n"); fflush(0);
//}

//void BRaaSHPCDisplayDriver::wait()
//{
//	//thread_scoped_lock lock(mutex);
//	//cv.wait(lock, [this] { return render_finished; });
//	while (true) {
//		thread_scoped_lock session_thread_lock(mutex);
//		if (render_finished) {
//			break;
//		}
//		cv.wait(session_thread_lock);
//	}
//}
//
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

	half4* mapped_rgba_pixels = pixels.data();

	return mapped_rgba_pixels;
}

void BRaaSHPCDisplayDriver::unmap_texture_buffer()
{
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
