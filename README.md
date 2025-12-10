# BRaaS-HPC / BlenderPhi — Release Notes
## Releases
**BlenderPhi v4.5.5**
- Karolina cluster: https://code.it4i.cz/raas/blenderphi/-/blob/main/releases/blenderphi-v4.5.5/blenderphi-v4.5.5-karolina-linux-x64-gcc13.tar.xz

## Highlights (BlenderPhi v4.5.5)

- New BRaaS-HPC display driver for interactive remote rendering.
- Optional GPUJPEG compression path for remote display.
- Binary XML scene loader (`cycles_xml_bin`) with NanoVDB and raw volume support.
- New volume meshing controls and NanoVDB-driven metadata handling.
- Unified byte-RGBA display pipeline on CPU, CUDA, HIP, and oneAPI.
- New build options for GPU/CPU image handling on all GPU backends.

## New Features (BlenderPhi v4.5.5)

### BRaaS-HPC display driver integration
- Added a new `BRaaSHPCDisplayDriver` implementation in the Cycles Blender integration.
- The driver plugs into the generic `DisplayDriver` interface and provides:
  - Custom mapping/unmapping of the display texture buffer.
  - A path to copy data from Cycles’ render buffers into a BRaaS-HPC-managed buffer.
  - Support for both half-float and byte RGBA output (via `buffer_uchar_srgba()`).
- The driver is linked against `braas_hpc_renderengine::braas_hpc_renderengine` [BRaaS-HPC RenderEngine](https://github.com/It4innovations/braas-hpc-renderengine), enabling interaction with the external [BRaaS-HPC](https://github.com/It4innovations/braas-hpc) render engine.

### GPUJPEG support for remote display
- Link: https://github.com/It4innovations/braas-hpc-gpujpeg
- New build-time option: `WITH_CLIENT_GPUJPEG` in the top-level `CMakeLists.txt`.
- When enabled:
  - Blender/Cycles locates GPUJPEG via `find_package(GPUJPEG REQUIRED)` in `intern/cycles/blender/CMakeLists.txt`.
  - The BRaaS-HPC display driver is compiled with GPUJPEG support.
  - At runtime, the driver checks the `CYCLES_BRAAS_HPC_USE_GPUJPEG` environment variable to enable/disable GPUJPEG compression dynamically, and prints a diagnostic message if GPUJPEG was not compiled in.

### Byte-RGBA display buffer
- Introduced a new `uchar4` display buffer (`display_rgba_byte_`) alongside existing half-float buffers.
- Added `film_convert_byte_rgba_*` kernels on CPU, CUDA, HIP, and oneAPI backends:
  - Convert float render buffers to sRGBA `uchar4` buffers.
  - Provide unified handling of byte outputs for fast network transport or GPUJPEG encoding.
- The display destination now exposes `pixels_uchar_srgba` / `d_pixels_uchar_srgba` when a byte buffer is used, and the display driver can advertise `buffer_uchar_srgba()` accordingly.

### Binary XML scene loader (`cycles_xml_bin`)
- Replaced the old XML loader in standalone Cycles:
  - `cycles_xml.cpp` / `.h` are commented out from the build.
  - New `cycles_xml_bin.cpp` / `.h` are added, and standalone now includes `app/cycles_xml_bin.h`.
- The new loader supports:
  - Volume attributes with `volume_type` handling.
  - NanoVDB grids (when enabled).
  - Raw volume data as a fallback.
  - Direct creation of `NanoVDBImageLoader` instances from in-memory nanogrid data, avoiding filesystem round-trips.

### Volume meshing control
- Added a new property to `Volume`:
  - `NODE_SOCKET_API(bool, volume_mesh)` in `intern/cycles/scene/volume.h`.
- In `GeometryManager::device_update_preprocess`:
  - Automatic volume mesh generation (`create_volume_mesh`) is now conditional on `!volume->get_volume_mesh()`.
- This allows explicit control over whether a volume should be meshed or not — important when backends/pipelines handle volumes differently (e.g., NanoVDB vs mesh).

## Improvements and Changes (BlenderPhi v4.5.5)

### NanoVDB and VDB loaders
- Extended `VDBImageLoader`:
  - Stores raw NanoVDB grid data (`nanogrid_data`) and derives metadata:
    - Number of channels based on grid type (scalar vs vector grid).
    - `byte_size`, resolution, and world-space bounding box.
    - World transform from `grid->map().mMatD` and `mVecD`.
- Added alternative `OIIOImageLoader` constructor taking a name and vector data plus custom metadata — makes it easier to feed in-memory images into Cycles.

### GPU/CPU image handling toggle
- Introduced build-time option `WITH_GPU_CPUIMAGE` in device/kernel CMake:
  - `intern/cycles/device/CMakeLists.txt` defines `-DWITH_GPU_CPUIMAGE` when enabled.
  - `intern/cycles/kernel/CMakeLists.txt` propagates `WITH_GPU_CPUIMAGE` to CUDA and HIP compile flags.
  - In oneAPI context (`context_begin.h`), includes `kernel/device/cpu/image.h` when `WITH_GPU_CPUIMAGE` is defined; otherwise falls back to `kernel/device/gpu/image.h`.
- This enables GPU kernels to optionally reuse CPU image handling code for specific HPC/cluster configurations.

### Display driver API extensions
- Extended `DisplayDriver` interface (`display_driver.h`) with optional BRaaS-related hooks:
  - `virtual bool only_device_buffer()` — hint that only device buffers (no CPU copy) are used.
  - `virtual bool buffer_uchar_srgba()` — indicates byte-RGBA buffer usage.
  - `virtual void copy_texture_buffer(const half4* rgba_pixels, int texture_x, int texture_y, int pixels_width, int pixels_height)` — optional callback to copy from a host buffer into the driver’s storage.
- Existing drivers can ignore these new methods by relying on defaults; the BRaaS-HPC driver overrides them.

## Build & Configuration

### New CMake options
- `WITH_CLIENT_GPUJPEG` (OFF by default)
  - Controls GPUJPEG support for BRaaS-HPC client compression.
- `WITH_GPU_CPUIMAGE`
  - Enables compilation paths that use CPU image handling on GPU backends (CUDA, HIP, oneAPI).

### New external dependencies
- `GPUJPEG` (required when `WITH_CLIENT_GPUJPEG` is ON).
- `braas_hpc_renderengine` [BRaaS-HPC RenderEngine](https://github.com/It4innovations/braas-hpc-renderengine) (required by `intern/cycles/blender` when [BRaaS-HPC](https://github.com/It4innovations/braas-hpc) integration is enabled).

## Stability and Compatibility
- The new `volume_mesh` flag provides better control over geometry generation for volume data, important when mixing OpenVDB/NanoVDB volumes with mesh-based acceleration structures.

# Development
- [Build Instructions](https://developer.blender.org/docs/handbook/building_blender/)
- [Code Review & Bug Tracker](https://projects.blender.org)
- [Developer Forum](https://devtalk.blender.org)
- [Developer Documentation](https://developer.blender.org/docs/)


# License
Blender as a whole is licensed under the GNU General Public License, Version 3.
Individual files may have a different but compatible license.

See [blender.org/about/license](https://www.blender.org/about/license) for details.
