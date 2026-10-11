# ANARI rendering in Blender (blenderphi)

This branch of Blender (`main-anari`, Blender 5.3 alpha) can render through
[ANARI](https://www.khronos.org/anari/) devices. ANARI is implemented as a Cycles device
(`DEVICE_ANARI`): Blender syncs the scene into Cycles as usual, and the ANARI device mirrors
the Cycles scene (`ccl::Scene`) into ANARI objects and renders it with the selected ANARI
back-end. Any ANARI library can be used. The ones built and tested here are:

| ANARI library | Renderer | Hardware |
|---|---|---|
| `cycles` | [cyclesphi-anari](https://github.com/It4innovations/cyclesphi-anari): Cycles itself as an ANARI device | CPU, CUDA, OptiX |
| `barney` | [barney](https://github.com/jar091/barney) | CUDA/OptiX |
| `visrtx` | [VisRTX](https://github.com/jar091/VisRTX), RTX device | OptiX |
| `mitsuba` | [mitsuba-anari](https://github.com/jar091/mitsuba3-anari): Mitsuba 3 | CUDA (`cuda_ad_rgb`), CPU (`llvm_ad_rgb`, `scalar_rgb`) |
| `moonray` | [openmoonray-anari](https://github.com/jar091/openmoonray-anari): DreamWorks MoonRay | CPU (XPU) |
| `helide` | [ANARI-SDK](https://github.com/jar091/ANARI-SDK) reference device | CPU |
| `visionaray`, `visionaray_cuda` | [anari-visionaray](https://github.com/jar091/anari-visionaray): Visionaray | CPU, CUDA (one library each) |
| `ospray` | [anari-ospray](https://github.com/jar091/anari-ospray): Intel OSPRay | CPU |
| `rpr` | [RadeonProRenderANARI](https://github.com/jar091/RadeonProRenderANARI): AMD Radeon ProRender (Northstar) | GPU (CUDA on NVIDIA, HIP on AMD), CPU fallback |
| `photon` | [Photon](https://github.com/jar091/photon): Kokkos path tracer | CUDA with OptiX, or CPU with Embree (two builds of the same library) |

This document describes what is the same on every platform: how the device works, how it
is used in Blender, what the smoke tests check and what the known limits are. Where the
sources are, how each component is built and what the tests give is described per platform:
[WINDOWS.md](WINDOWS.md), [LINUX.md](LINUX.md) and [MACOS.md](MACOS.md), see
[section 2](#2-platforms). How materials, lights and geometry are translated for each
device is in [MATERIALS.md](MATERIALS.md), the open items in [TODO.md](TODO.md).

Contents:

1. [How it works](#1-how-it-works)
2. [Platforms](#2-platforms)
3. [Using ANARI in Blender](#3-using-anari-in-blender)
4. [Smoke tests](#4-smoke-tests)
5. [Known differences and limitations](#5-known-differences-and-limitations)
6. [Changes made to the ANARI devices](#6-changes-made-to-the-anari-devices)

## 1. How it works

* `intern/cycles/device/anari/` holds the ANARI Cycles device:
  * `device.cpp` loads ANARI libraries and lists their devices.
  * `device_impl.cpp` owns the ANARI device and frame.
  * `scene.cpp` (`AnariScene`) mirrors the Cycles scene: meshes, curves, point clouds,
    volumes, instances, lights, camera, world/background and integrator settings.
  * `material.cpp` (`AnariMaterialBuilder`) turns Cycles shader graphs into ANARI materials
    and samplers (`physicallyBased` or `matte`, with image textures, mapping, vertex colors
    and constants) and volume shaders into volume parameters.
  * `graph_xml.cpp`, `graph_materialx.cpp` and `graph_rdla.cpp` send the whole shader graph
    as text to the devices which evaluate graphs (cyclesphi-anari, RadeonProRenderANARI,
    openmoonray-anari), see [MATERIALS.md](MATERIALS.md#5-shader-graphs-as-text) and
    [TODO.md](TODO.md).
* `intern/cycles/integrator/path_trace_work_anari.cpp` replaces the Cycles path tracing work
  for the ANARI device. It renders ANARI frames and copies the color, depth, normal, albedo
  and id channels into the Cycles render buffers. Cycles film conversion, passes and the
  denoiser then work as usual.
* `intern/cycles/blender/addon_anari/` is the `ANARI` render engine add-on. It provides
  preferences (library search paths), the device selection and the device parameters. The
  ANARI engine reuses the Cycles panels that apply.
* `tests/python/anari_smoke_tests.py` holds the smoke tests, see [section 4](#4-smoke-tests).

What is exported:

* **Geometry.** Triangle meshes with normals, UVs and vertex colors; subdivision surfaces
  (after Blender's subdivision); hair and curves as `curve` geometry; point clouds as
  `sphere` geometry. Geometry nodes instances become ANARI instances.
* **Volumes.** The density grid of `Volume` geometry (from geometry nodes or `.vdb` files) is
  read from the NanoVDB grid Cycles builds and resampled into a `structuredRegular` field.
  It is rendered as a `principled` volume on cyclesphi-anari (Cycles' own Principled Volume)
  and as a `transferFunction1D` volume on the other devices: constant albedo and an
  extinction proportional to the density.
* **Materials.**
  * The Principled BSDF and diffuse-only shaders become `physicallyBased` or `matte`
    materials.
  * Image textures, with their mapping node, become `image2D` samplers.
  * Emission becomes the material's emissive color.
* **Lights.** Point (with radius), spot (with blend), area (rectangle) and sun lights, with
  visibility to the camera.
* **World.**
  * An environment texture or constant world color becomes an `hdri` light, with its
    rotation from the mapping node.
  * The world color is also the renderer background.
  * Transparent film is supported.
* **Camera.** Perspective (with depth of field), orthographic and equirectangular panorama
  cameras, render borders, and clipping.
* **Integrator.** Bounces, clamping, filter width and light tree settings are passed to
  cyclesphi-anari as renderer parameters. The other devices use the generic
  `pixelSamples`/`maxRayDepth`.

## 2. Platforms

The stack is developed on Windows and was brought up on Linux and macOS from the same
sources. Each platform has its own document with the workspace, the build of every
component, the test results and what does not work there:

| Platform | Document | Machine | What runs |
|---|---|---|---|
| Windows 11 | [WINDOWS.md](WINDOWS.md) | Visual Studio 2022, NVIDIA RTX 2080 Ti | all libraries of the table above, on the GPU where they have a GPU back end |
| Linux (Rocky Linux 10) | [LINUX.md](LINUX.md) | GCC 14, two NVIDIA RTX PRO 6000 Blackwell, no display | all libraries; Blender without a window; Radeon ProRender on the CPU only |
| macOS 26 (Apple silicon) | [MACOS.md](MACOS.md) | Apple M4, Xcode | cyclesphi-anari and Mitsuba on Metal; barney, Visionaray, OSPRay, Photon, MoonRay and Radeon ProRender on the CPU; no VisRTX |

The Windows document is the reference: the other two describe their platform as
differences from it. pynari, the Python binding the devices are also tested with, has its
own documents per platform in the pynari repository (`pynari/doc/windows`, `doc/linux`,
`doc/macos`).

## 3. Using ANARI in Blender

1. **Point Blender at the ANARI libraries.** Start `build/blender/bin/Release/blender.exe`.
   In *Preferences → Add-ons → ANARI*, set **Library Search Paths** to the directories with
   the `anari_library_<name>.dll` files, separated by `;` (the example is the Windows
   workspace of [WINDOWS.md](WINDOWS.md); on Linux and macOS the libraries are
   `libanari_library_<name>.so` and `.dylib`, and the separator is `:`):

   ```
   F:\work\anari\install\bin;F:\work\anari\install\mitsuba\bin;F:\work\anari\install\moonray\bin;F:\work\anari\install\ospray\bin;F:\work\anari\install\rpr\bin
   ```

   Photon's two builds have the same library name: add either
   `F:\work\anari\install\photon\bin` (GPU) or `F:\work\anari\install\photon_cpu\bin`
   (CPU), not both.

   The preferences list the ANARI devices that were found. **Additional Libraries** adds
   library names to probe beyond the known back-ends (barney, cycles, mitsuba, moonray,
   visrtx, helide, ospray, visionaray, visionaray_cuda, rpr, photon).

2. **Select the engine and device.** In *Render Properties*, set **Render Engine** to
   **ANARI** and pick the **Device**. Viewport rendering and final renders both work. The
   3D viewport sidebar (*View* tab → *View* → **ANARI Render Device**) can override the
   device for one viewport.

3. **Device Parameters** (Render Properties) are `name=value` pairs separated by `;`,
   passed to `anariSetParameter` on the device:

   | Device | Useful parameters |
   |---|---|
   | cycles | `computeDevice=cpu\|cuda\|optix` |
   | mitsuba | `mitsuba.variant=cuda_ad_rgb\|llvm_ad_rgb\|scalar_rgb` |
| moonray | `moonray.executionMode=auto\|scalar\|vectorized\|xpu` |
   | rpr | `computeDevice=gpu\|cpu\|gpu+cpu`, `gpuIndex=<n>` |
   | all | `renderer=<subtype>`: a pseudo parameter that selects the ANARI renderer subtype instead of `default`; `denoise=true`: the denoiser of the device, see below |

4. **Materials** (Render Properties, below the device) switches between the two ways
   materials are sent. *Shader Graph* (the default) sends the shader node graph to devices
   which evaluate it (cycles, rpr, moonray) and the flattened material to the others.
   *Flattened* always converts the nodes to one physically based material, also on devices
   which take graphs; the device parameter `materials=physicallyBased` does the same.

5. **Device Denoiser** (Render Properties) turns on the denoiser built into the renderer of
   the device, on devices which have one (the `denoise` parameter of the renderer: barney,
   VisRTX, OSPRay, cyclesphi-anari). It is off by default; the device parameter
   `denoise=true` does the same. It is independent of the *Denoise* settings of the render,
   which Blender applies itself to the image the device returns.

6. **Samples, bounces, filter, film transparency and the world** are taken from the usual
   Cycles settings.

Environment variables:

| Variable | Effect |
|---|---|
| `CYCLES_ANARI_TRACE=1` | Trace the ANARI scene sync (Blender side); at the end of a render also the time of the first and of the later frames inside the device, what the device reports of it (`duration` of the frame) and the time spent fetching the image |
| `CYCLES_ANARI_GRAPH_DUMP=<directory>` | Write the text of every exported shader graph to a file |
| `CYCLES_ANARI_GRAPH_FORMAT=cyclesXml\|materialx\|rdla` | Force a shader graph format, also on a device which does not take it (debugging) |
| `OPENMOONRAY_ANARI_TEXTURE_FILES=1` | MoonRay: write images to scratch texture files instead of serving them from memory |
| `ANARI_CYCLES_LOG_LEVEL=info\|debug\|trace` | Cycles log level inside cyclesphi-anari |
| `ANARI_CYCLES_FORCE_CPU=1`, `CYCLES_ANARI_USE_GPU=OPTIX` | Override cyclesphi-anari's compute device |
| `ANARI_MITSUBA_VARIANT` | Default Mitsuba variant |
| `ANARI_RPR_COMPUTE_DEVICE=gpu\|cpu\|gpu+cpu` | Override the RadeonProRender compute device |
| `PHOTON_BACKEND=optix\|embree\|kokkos` | Ray tracing back-end of Photon |
| `PHOTON_NUM_THREADS=<n>` | Number of threads of Photon's CPU build |
| `PHOTON_VERBOSE=1` | Photon prints the back-end it selected |

## 4. Smoke tests

`tests/python/anari_smoke_tests.py` renders small scenes (160×120) with the ANARI engine
and with Blender's own Cycles, and compares the two:

```bat
build\blender\bin\Release\blender.exe --background --factory-startup -noaudio ^
  --python blenderphi\tests\python\anari_smoke_tests.py -- ^
  --library <cycles|barney|visrtx|mitsuba|moonray|helide|visionaray|visionaray_cuda|ospray|rpr|photon> ^
  --library-path F:\work\anari\install\bin [--library-path ...] ^
  --outdir F:\work\anari\build\test_out\<library> ^
  [--device-parameters "computeDevice=optix"] [AnariSmokeTest.test_lights ...]
```

| Test | Checks |
|---|---|
| `test_build_options`, `test_engine_registered` | Blender is built with ANARI and the engine is registered |
| `test_basic_render` | A lit Principled BSDF scene is not black, has no NaNs, and is close to Cycles |
| `test_texture` | An image texture shows its red and blue checker squares |
| `test_texture_mapping` | A texture scaled and rotated by a mapping node matches Cycles |
| `test_shader_graph` | Materials which need the node graph (procedural textures, math, a Mix Shader with a mask) match Cycles on cyclesphi-anari; for the other devices the difference is printed. Not part of the result tables |
| `test_lights` | Point, spot, area and sun light each light the scene like in Cycles |
| `test_normal_map` | A tangent space normal map of stripes, lit from the side, shades like in Cycles; for devices other than cyclesphi-anari the difference is printed. Not part of the result tables |
| `test_camera_shift` | A lens shift moves the image as in Cycles (camera `imageRegion`). Not part of the result tables |
| `test_camera_clip` | An emissive plane nearer than the clip start lights the scene but is not seen, because only camera rays are clipped (camera `near`). Not part of the result tables |
| `test_environment_map` | An HDRI lights the scene and shows in the background, also rotated by 90° |
| `test_world_camera_ray` | A world which camera rays see five times darker than the rays which light the scene (Mix Shader by Light Path *Is Camera Ray*): the backdrop is the background image of the renderer, not the environment light. Not part of the result tables |
| `test_device_denoiser` | The *Device Denoiser* setting is off by default; with it on, the noise of a render with four samples is printed against the one without (devices without a denoiser render the same). Not part of the result tables |
| `test_emissive_room` | A closed room lit by nothing but an emitting plane is lit like in Cycles. A device which samples emitting surfaces as lights (it declares the material parameter `emissionSampling`) is asked to; the others only find the lamp by chance, which shows as noise |
| `test_light_falloff` | An area light whose node tree fades its emission out over the first two meters of the ray lights the floor like in Cycles. A device takes the falloff as a ramp over the distance when it declares `falloff.distance` on lights; the others get the light with its average emission |
| `test_background` | World color, strength and transparent film, including changes during an existing render session |
| `test_object_types` | Geometry nodes (instances, realized, points, curves, volume), particle instances and hair, hair curves, text, bevel curve, metaball and subdivision |
| `test_instancing` | Linked duplicates share their geometry |

Each comparison prints a line such as
`ANARI barney: light point: brightness ratio 1.04, relative difference 0.05`:

* **brightness ratio:** mean of the ANARI render divided by the mean of the Cycles render.
* **relative difference:** the relative RMS difference of 8×8 block averaged luminance, so
  it tolerates noise.
* **coverage:** the agreement of the object masks, with one pixel of tolerance for
  different pixel filters.

The thresholds are strict for cyclesphi-anari (the same renderer) and loose for the other
devices.

`tests/python/anari_scene_compare.py` compares any .blend file with Cycles: it renders the
file with Cycles and with an ANARI device (with shader graphs and with flattened materials),
prints the brightness ratio and the relative difference, and writes the images side by side.
`--override-material` and `--unlink-nodes` help to find what a difference comes from. The
results for the Cycles demo scenes are in
[MATERIALS.md](MATERIALS.md#56-what-the-smoke-test-shows).

```bat
build\blender\bin\Release\blender.exe --background --factory-startup --disable-autoexec -noaudio ^
  --python blenderphi\tests\python\anari_scene_compare.py -- ^
  --blend <file.blend> --library cycles --library-path F:\work\anari\install\bin ^
  --device-parameters "computeDevice=optix" --outdir F:\work\anari\build\test_out\demo
```

The commands above are the ones of the Windows workspace. How the suite is run for all
configurations at once, and what it gives, is in the document of each platform:
[WINDOWS.md](WINDOWS.md#5-running-the-smoke-tests), [LINUX.md](LINUX.md) and
[MACOS.md](MACOS.md).

## 5. Known differences and limitations

* **Spot lights.** Blender spot lights have a radius, which gives a soft edge in Cycles,
  cyclesphi-anari, MoonRay and OSPRay. barney, VisRTX, Mitsuba, Visionaray, Radeon
  ProRender and Photon treat spots as point sources: their cone has a sharp edge (relative
  difference about 0.5 in the spot test), while the brightness matches.
* **VisRTX `default` renderer.**
  * It is the `interactive` renderer.
  * Volumes are rendered as emission/absorption as described in the ANARI specification:
    they are not lit, so `gn_volume` is too bright.
  * Metallic surfaces have no environment reflections (the black sphere in the basic test).
  * Use `renderer=quality` to get results comparable with Cycles.
* **helide** is the minimal reference device of the ANARI SDK.
  * It has no `hdri` light, so the environment test fails.
  * Its lighting is not physically based, so its lights and materials differ from Cycles.
  * Its volumes are unlit, like VisRTX's default renderer.
* **Mitsuba.**
  * Lights with a radius are slightly brighter (point 1.11, spot 1.12), because Mitsuba's
    sphere emitters integrate the radius differently.
  * The rough dielectric in the basic test is noisier at the same sample count.
* **MoonRay** renders on the CPU with all cores (vectorized mode). The device parameter
  `moonray.executionMode=xpu` moves the ray intersections to the GPU, while shading stays
  on the CPU; on the test machine this was slower than the CPU mode (18 s instead of 15 s
  for the pynari `sample02`). MoonRay is one of the slowest back-ends, and at 32 samples
  its light-sampling noise is higher.
* **Visionaray** (CPU and CUDA).
  * Spot lights are point sources with a sharp edge, like barney, VisRTX and Mitsuba.
  * The glass sphere of the basic test (`transmission = 1`, `roughness = 0`) renders
    bright white instead of refracting the floor. Together with the noise, this is the
    relative difference of that test.
  * The path tracer samples one light per hit uniformly and has no frame accumulation
    (`ANARI_KHR_FRAME_ACCUMULATION`), so Blender averages its frames. At 32 samples the
    images are noisier than those of the other devices.
  * `curve` geometry is rendered as cone segments with flat ends: hair is not rounded at
    its joints and tips.
* **OSPRay** renders on the CPU with its `pathtracer` renderer.
  * `transmission` without a `thickness` is thin-walled glass, as the ANARI specification
    defines it: light passes straight through, without refraction. The other devices
    render solid glass either way. The exporter therefore sets `thickness` on transmissive
    materials.
  * Rough textured surfaces show a pale specular haze (the texture tests).
  * OSPRay 3.2.0 is the latest release. anari-ospray also targets the unreleased OSPRay
    3.3 (`devel`); its `specularMetallic` material parameter is emulated for 3.2, see
    [section 6](#6-changes-made-to-the-anari-devices).
  * The light tests are within 0.87–1.11 of Cycles, a little further off than the other
    path tracers. The cause was not investigated.
* **Radeon ProRender** renders on the GPU with the Northstar plugin.
  * Spot lights have a sharp edge, like barney, VisRTX and Mitsuba (relative difference
    about 0.5), while the brightness matches.
  * The first frame of a scene builds RPR's acceleration structures, about 4–5 s per
    million triangles, so scene changes are slower than with the other GPU devices.
  * Only the color and depth channels are provided. The normal, albedo and id channels
    are not, and `unstructured` fields and the `primitive` sampler are not supported: they
    warn.
  * The OpenCL back-end (`computeDevice=opencl`) fails to compile its kernels on this
    GPU. It is never selected automatically.
* **Photon** gives the same metrics on the GPU and on the CPU (the same kernels and
  random seeds).
  * Spot lights have a sharp edge, like barney, VisRTX and Mitsuba.
  * Photon only traces triangles. Spheres, cylinders, cones and curves are tessellated,
    and instances are flattened into one mesh: memory grows with the number of instances.
  * Shadow rays treat transparent (`opacity`) surfaces and glass as opaque.
  * The hdri light is looked up without filtering: small environment maps look blocky.
  * The CPU build uses Kokkos' Threads back-end, whose workers spin while a frame renders.
    It slows down by a factor of 10 or more when other jobs use the CPU cores.
* **Volumes.**
  * Only the density grid is exported. Color, temperature, emission and blackbody grids
    are not, except on cyclesphi-anari, which uses Cycles' Principled Volume with constant
    parameters.
  * The transfer-function devices get a constant albedo and an extinction proportional to
    the density.
  * The exporter keeps the opacity range small (≤ 0.01), so both the
    specification's `-ln(1 - opacity) / unitDistance` and the linear
    `opacity / unitDistance` used by some devices give that extinction.
* **Materials.** On cyclesphi-anari, RadeonProRenderANARI and openmoonray-anari the shader
  node graph is sent as text, as far as the device and the export can express it; see
  [MATERIALS.md](MATERIALS.md#5-shader-graphs-as-text). On the other devices, and as the
  fallback, shaders are converted to a single `physicallyBased` (or `matte`) material.
  Image textures (with mapping), vertex colors and constants are supported, and mixed
  closures are blended into one material. Color nodes which are not linear (Hue/Saturation,
  curves, gamma, color ramps) are approximated by the linear transform of the sampler which
  fits the colors of the texture best. What a sampler can't express (procedural colors,
  the mask of a Mix Shader, emission) is evaluated by the Cycles kernel in Blender and
  baked into a texture or vertex colors.
* **MoonRay materials.** The node graph is translated by Blender to networks of the classes
  MoonRay and moonshine have; the device contains nothing of Cycles. Most nodes come out
  the same within noise. Noise and Voronoi textures have the same character but another
  pattern, and some shading models differ (anisotropy, sheen, rough diffuse, subsurface).
  MoonRay does not sample emissive meshes as lights: scenes lit by emissive planes or lamp
  meshes are noisy there. Details in [MATERIALS.md](MATERIALS.md#55-rdla-graph_rdlacpp).
* **World.** An Environment Texture and a constant color are exported as they are. Any other
  world shader (sky texture, procedural, mixed backgrounds) is evaluated by the Cycles
  kernel into an environment image, and the sun of a sky texture becomes a directional
  light. A world which the camera sees differently than the rays which light the scene
  (Light Path *Is Camera Ray*, or hidden from diffuse rays) is shown as the background image
  of the renderer; with a panoramic camera the camera sees the lighting. On cyclesphi-anari
  the world shader is sent as a graph instead.
* **Fast GI** exists on cyclesphi-anari only. The other devices end the paths at its number
  of bounces, without the ambient occlusion lookup of the world.
* **Lights.** ANARI lights fall off with the inverse square of the distance. A light whose
  shader changes the falloff (Light Falloff node, Light Path *Ray Length*) gets one average
  strength; the light is distributed differently than in Cycles. Lights which cast no
  shadow do cast one, and *Max Bounces* of a light is ignored. cyclesphi-anari gets the
  light shaders as graphs and has both settings (`castShadow`, `maxBounce`).
* **Ray visibility** of objects (camera, shadow, diffuse, ...) is sent to cyclesphi-anari,
  openmoonray-anari and RadeonProRenderANARI. On the other devices an object hidden from
  camera or shadow rays is left out, unless it emits light. The ray visibility of lights
  and of the world is only sent to cyclesphi-anari; elsewhere a light or a world which
  diffuse rays don't see is left out, and the others light everything (a light hidden
  from glossy rays shows up in mirrors).
* **Mesh attributes.** A material can read four attributes of a mesh (UV maps, color and
  generic attributes, generated coordinates, pointiness), eight on cyclesphi-anari. Further
  ones are dropped with a warning.
* **Camera clipping** (`near`, `far`) only clips camera rays in Cycles: a scene may have a
  light card right in front of the camera. A device which ignores the clip planes renders
  that card instead of the scene.
* **Viewport.** Every ANARI device renders the viewport progressively. A scene change
  re-syncs only what changed (objects, lights, world, camera).

## 6. Changes made to the ANARI devices

All changes are committed on the branches listed in [WINDOWS.md](WINDOWS.md#2-workspace-and-sources).

**blenderphi (this branch)**

* The ANARI Cycles device, the `ANARI` render engine add-on and the smoke tests
  (sections [1](#1-how-it-works) and [4](#4-smoke-tests)).
* **Volume export.** The NanoVDB density grid is resampled to a `structuredRegular` field.
  Its bounding box comes from the leaf nodes, because Cycles builds grids without
  statistics. The field becomes a `principled` volume on cyclesphi-anari and a
  `transferFunction1D` volume elsewhere.
* **Materials.** Diffuse-only shaders become `matte`; the texture `inTransform` is set only
  when it is not the identity. Transmissive materials get a `thickness`, which makes them
  solid (refractive) glass instead of thin-walled glass.
* **Libraries.** `ospray`, `visionaray`, `visionaray_cuda`, `rpr` and `photon` are probed
  by default.
* **Lights.** Light visibility, spot radius and the Cycles-specific soft spot falloff are
  exported.
* **Renderer settings.** The integrator settings are mapped to cyclesphi-anari renderer
  parameters, and the pseudo device parameter `renderer=<subtype>` selects the renderer.
* **Shader evaluation.** It is skipped for the ANARI device. This fixes a crash with
  environment textures.

**ANARI-SDK (helium)**

* `Array::valueAtLinear`/`valueAtClosest` read one element past the end of the array at
  coordinate 1.0. In helide this produced NaN pixels in volumes whose maximum value equals
  the end of `valueRange`.

**cyclesphi-anari**

* **Merge and build.**
  * Merged with jeffamstutz/anari-cycles and ported to Cycles 5.3.
  * Builds on Windows with CUDA, OptiX and OpenImageDenoise.
  * Finds its kernels next to the DLL.
* **Lights.**
  * Fixed the HDRI rotation (the basis was a reflection matrix).
  * Fixed the Rodrigues rotation formula.
  * Spot smoothing now works in cosine space, like Cycles, with a `softFalloff`
    extension.
* **Volumes.**
  * The volume bounding mesh had one face wound inward, so volumes were invisible when
    seen through that face.
  * Volumes added after the first frame crashed or vanished, because Cycles' volume octree
    was not rebuilt.
  * The RAW3D voxel kernels applied stochastic interpolation in object space instead of
    voxel space, which blurred volumes and made them grow.
  * `transferFunction1D` opacity now follows the specification (`-ln(1 - opacity)`).
* **Meshes and objects.**
  * Meshes without normals are now flat shaded. Before, averaged normals made some
    triangles black.
  * Objects committed without parameters (e.g. a default `matte`) are now committed.
* **New features.**
  * `image3D` sampler.
  * `unstructured` spatial field (resampled to a grid).
  * Smoother isosurfaces on coarse grids.

**barney**

* `matte` albedo and the Lambertian BRDF normalization.
* The environment map's `visible` flag.
* Spot lights added to the build.
* The ambient light was counted several times: its radiance included the 1/pdf factor,
  which was then applied again. Its MIS pdf also did not match the hemisphere sampling.
  A white-furnace test went from 1.29 to 0.50 (expected 0.5).
* Asking for the device subtype `mpi` in a build without MPI threw an exception through
  the ANARI C API, which terminated the application. It now warns and creates the default
  device.

**VisRTX**

* **Lights.** Quad, spot and point light units now follow the specification, spot angles
  are half angles, and sphere lights use solid-angle sampling.
* **Invalid volumes** (e.g. on unsupported fields) no longer abort the process.
* **Interactive renderer volumes.** The ray-marching step is bounded, so small volumes are
  not skipped.
* **Quality renderer.** It now shows ambient light in reflections, with correct MIS.

**mitsuba-anari**

* **Materials.**
  * The material mapping: Principled-like dielectric, metal and glass, two-sided surfaces,
    and unbounded colors.
  * Bitmap textures with UV transforms, and colors from vertex attributes.
  * `specular = 0` produced NaNs in `scalar_rgb`.
  * PBR `opacity` was applied in the `opaque` alpha mode.
  * `image1D` samplers, and `image3D` samplers driven by the surface position
    (`worldPosition`, `objectPosition`). An unsupported sampler warns instead of failing
    the frame.
* **Lights and environment.**
  * Area emitters for emissive surfaces.
  * The hdri is flipped vertically and respects `visible`; an hdri replaces the ambient
    light instead of failing.
  * The renderer `background` can be an ARRAY2D image.
* **Geometry.**
  * Cylinders and cones.
  * Large sphere sets as one `ellipsoids` shape.
  * Isosurfaces.
* **Volumes.** Volumes as heterogeneous media, and `unstructured` fields.
* **Camera.** Depth of field.
* **Runtime.** The `llvm_ad_rgb` variant now works (LLVM-C.dll) and no longer crashes at
  exit.
* **Scene updates on CUDA.** Moving an instance in the viewport crashed Blender with
  `cuda_ad_rgb`. The device puts the cached shapegroup into a new Mitsuba scene, and the
  OptiX backend only builds the acceleration structure of groups flagged as changed. The
  group is now flagged again.

**openmoonray-anari**

* **Runtime.** The DSOs now load with Blender's DLLs, by setting `AddDllDirectory`.
  MoonRay core is built against Blender's OpenVDB. The device also sets
  `REZ_MOONRAY_ROOT` to its package: without it, the XPU mode did not find its GPU
  programs and fell back to the CPU.
* **Lights and environment.**
  * Spot lights.
  * The HDRI `visible` flag; the HDRI is filtered bilinearly and replaces the ambient light.
  * Image backgrounds.
* **Materials and textures.**
  * Textured `baseColor`, the `image2D` `inTransform`, nearest filtering, and `image1D`
    samplers.
  * Clear glass: physicallyBased `transmission` was ignored.
* **Geometry.**
  * Cylinders and cones.
  * Flat shading without normals.
  * Round curves.
* **Volumes.** The `transferFunction1D` opacity from color alpha, with resampling of coarse
  fields.
* **Camera.** Depth of field.
* **Frame.** Frame accumulation (`KHR_FRAME_ACCUMULATION`).
* **Diagnostics and speed.**
  * Unsupported subtypes now warn.
  * About 2× faster default light and BSDF sampling. The new `moonray.lightSamples` and
    `moonray.bsdfSamples` parameters restore MoonRay's defaults.

**anari-visionaray**

* **Lights.**
  * Spot lights had no distance falloff and treated `openingAngle` as the half angle: the
    spot test was 63× too bright. `power` is now supported.
  * Quad lights only read `intensity`: with `radiance` the area light test was 150× too
    dark. `radiance`, `intensity` and `power` now follow the specification, and `side` is
    honored (it was always `both`).
  * Point lights accept `power` and, with a radius, `radiance`.
* **Geometry.**
  * The device advertised `ANARI_KHR_GEOMETRY_CURVE` without a `curve` geometry, so hair
    and curves were missing. `curve` is now implemented with cone segments.
  * `isosurface` only accepted an array of isovalues, not a single `FLOAT32`.
  * Cones and cylinders with an index array read it from host memory on the CUDA device.
* **Objects without parameters.** An object committed without any parameter (a default
  `matte` material) was never finalized, and rendering it crashed with an access
  violation.
* **Volumes.**
  * The `transferFunction1D` lookup and the `structuredRegular` field were both sampled
    half a texel off: the first and last transfer function entries were not at the ends of
    `valueRange`, and the field was stretched beyond its bounds. Volumes rendered smaller
    and denser (`gn_volume` coverage 0.64).
  * CPU device: the value ranges of the acceleration grid ignored the neighboring samples
    of a cell, so parts of coarse volumes were skipped.
  * CPU device: `unstructured` fields rendered empty. The point query of the wide BVH
    rejects hits at distance 0.
* **Frame.** CPU device: rendering an incomplete frame left the rendering semaphore
  locked, and the next call blocked forever.
* **Ambient light in reflections.** The path tracer only gave the ambient light to
  diffuse surfaces, so a bounce ray that left the scene saw nothing: mirrors and glossy
  metals lit by the ambient light alone reflected a black sky. Such rays now see the
  ambient light, weighted by the non-diffuse share of the bounce.
* **Shadows on the CPU device.** A ray that starts on the cap of a cone or cylinder
  (shadow, ambient occlusion and bounce rays) could stop at its own primitive: the
  intersectors report that hit behind the ray origin, and the BVH traversal of the
  Visionaray library took it without checking `tmin`. A geometry made of several cones
  or cylinders cast no shadows on itself. The traversal
  (`visionaray/detail/bvh/intersect_ray1_bvhN*.inl`) now skips hits behind the origin.

**anari-ospray**

* **Runtime.** OSPRay loads its modules with a plain `LoadLibrary`, which searches their
  dependencies next to `blender.exe` and in `PATH`. The device now loads
  `ospray_module_cpu.dll` from its own directory first.
* **Background.** OSPRay adds the renderer background on top of a visible hdri light: the
  world was twice as bright. With a visible hdri, the background is now black and keeps
  its alpha.
* **Volumes.** `unitDistance` of `transferFunction1D` was ignored: Blender's volumes were
  invisible.
* **Materials.**
  * anari-ospray relies on the Principled parameter `specularMetallic` of the unreleased
    OSPRay 3.3. In OSPRay 3.2, `specular` also weights the metallic and the transmissive
    lobe, so metals and glass with `specular = 0` rendered black. For OSPRay ≤ 3.2 the
    device now keeps these two lobes at full weight.
  * The default infinite `attenuationDistance` turned thin-walled glass black.
* **Samplers.** OSPRay 3.2 picks the coarsest MIP level for texture lookups on spheres,
  whose texture coordinate is constant per primitive: an `image1D` color map gave every
  sphere the average color of the map. The device now disables MIP map generation
  (`disableMipMapGeneration`).

**RadeonProRenderANARI**

The device dated from 2022 and was written for a pre-1.0 ANARI SDK and an older RPR core.
It was ported to ANARI SDK 0.17 and RadeonProRender SDK 3.1.7.1:

* **Architecture.** The device is now built on helium, like helide: deferred commits,
  object lifetime, and generated queries for subtypes, parameters and extensions
  (`rpr_device.json`). glm is no longer needed.
* **Runtime.** `RadeonProRender64.dll`, Northstar, the CUDA 13 runtime and the precompiled
  kernels are loaded from the directory of the device. The RPR context is created on first
  use, and a failed GPU context falls back to the CPU with a warning. All RPR calls are
  serialized, and RPR objects are deleted on the render thread.
* **Frame.** `channel.color` (`UFIXED8_RGBA_SRGB`, `UFIXED8_VEC4`, `FLOAT32_VEC4`) and
  `channel.depth`, alpha from RPR's opacity AOV, and `ANARI_KHR_FRAME_ACCUMULATION`.
* **Geometry.** `triangle` and `quad` as RPR meshes; `sphere` as instances of one sphere
  mesh; `cylinder`, `cone` and `curve` tessellated; `isosurface` with marching tetrahedra.
* **Materials.** `matte` and `physicallyBased` on the RPR uber material, with constants,
  attributes or samplers (`image1D`, `image2D`, `image3D` through a slice atlas,
  `transform`) for every parameter.
* **Lights.** Directional, point, spot, quad and hdri lights with the units of the
  specification. A visible hdri replaces the background.
* **Scene.** Groups, transform instances and world-level arrays.
* **Volumes.** `structuredRegular` fields with `transferFunction1D` on RPR grid volumes.

**Photon**

Photon's ANARI layer was a stub without volumes, samplers, spot and hdri lights or alpha.
It was replaced, and the path tracer needed fixes to render with the right brightness:

* **Build.** Photon builds on Windows with MSVC, with Kokkos Cuda (OptiX) or Kokkos Threads
  (Embree), against an installed ANARI SDK. The Kokkos code is compiled by nvcc and the
  device by MSVC; the two sides only share standard C++ types (`RenderBridge.h`).
* **ANARI device** (new, on helium): frames with color, depth, normal and albedo channels
  and `ANARI_KHR_FRAME_ACCUMULATION`; perspective and orthographic cameras; `triangle`,
  `quad`, `sphere`, `cylinder`, `cone`, `curve` and `isosurface` geometry; `matte` and
  `physicallyBased` materials with `image1D`, `image2D`, `image3D` and `transform`
  samplers; directional, point, spot, quad and hdri lights; `structuredRegular` fields
  with `transferFunction1D` volumes.
* **Kokkos lifetime.** Kokkos is initialized once per process, on the first frame, and all
  Kokkos work runs on one thread of the device. An exit handler finalizes Kokkos; without
  it the process hung (Threads) or aborted (Cuda) at exit. When a process ends without
  running its exit handlers, the device terminates it from `DllMain` with the exit code of
  the process, as a last resort.
* **Path tracer.** These changes also affect Photon's pbrt renders:
  * Emissive and environment hits were counted twice, and the environment sample
    overwrote the shadow ray of the light sample. There is now one light strategy per
    bounce with multiple importance sampling.
  * The specular and clearcoat lobes had a stray cosine factor, `specular` was unused, and
    the spot falloff was inverted. The environment map pdf missed a factor.
  * The throughput clamp and adaptive sampling are parameters now. They are off for ANARI
    and keep their defaults for pbrt scenes.
  * New: alpha, ambient light, volumes, refraction, and barycentric coordinates from all
    back-ends.

**Since 2026-10-09 (working tree changes, not committed)**

These came out of rendering the Cycles demo scenes, see [TODO.md](TODO.md#6-real-scenes):

* **blenderphi.** Shader graphs as text ([MATERIALS.md](MATERIALS.md#5-shader-graphs-as-text));
  the ANARI device is derived from the CPU device; world shaders are baked; non-linear
  color nodes are fitted; ray visibility, bounce limits and clamps are sent to devices
  which declare them; light cards; the normal map sampler is not decoded any more.
* **cyclesphi-anari.** Material subtype `cyclesXml`, light and world graphs
  (`CYCLES_SHADER_XML`), instance visibility, camera `near` / `far` / `imageRegion`.
* **RadeonProRenderANARI.** Material subtype `materialx`; instance visibility; camera
  `imageRegion`; renderer `maxBounce`, `maxDiffuseBounce`, `clampIndirect`; RPR
  instances are kept across scene updates.
* **openmoonray-anari.** Material subtype `rdla` (with moonshine); textures from memory;
  instance visibility; camera `imageRegion`; sampler output transforms and textured
  roughness, metallic, clearcoat, emission, normal and opacity; `attribute1..3` and the
  `transform` sampler; the bounce limits and clamps. MoonRay core: perspective and
  orthographic cameras in one process (`ProjectiveCamera.cc`).
* **barney, anari-visionaray, Photon.** Camera `near` / `far` (barney: also for the
  orthographic camera, where they were read but not used).
* **VisRTX.** Camera `near` / `far`; `matte` bounces light.
* **mitsuba-anari.** Camera `near` / `far` and `imageRegion`; the orthographic camera
  for frames which are not square.
