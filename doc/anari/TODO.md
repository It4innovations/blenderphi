# Shader graph export: plan and status

Today every Blender material is flattened into one ANARI `physicallyBased` or `matte`
material (see [MATERIALS.md](MATERIALS.md)). This plan adds three ways to send the whole
shader graph to the devices that can evaluate one, and keeps the flattening as the default
and as the fallback.

Status marks: `[ ]` open, `[~]` in progress, `[x]` done and verified.

## 1. Export branches

| Branch | Format | Read from | ANARI material subtype | Devices |
|---|---|---|---|---|
| 1 | Cycles XML | Cycles `ShaderGraph` | `cyclesXml` | cyclesphi-anari |
| 2 | MaterialX | Blender node tree (`export_to_materialx`) | `materialx` | VisRTX, RadeonProRenderANARI |
| 3 | rdla | Cycles `ShaderGraph` | `rdla` | openmoonray-anari |
| 4 | `physicallyBased` / `matte` | Cycles `ShaderGraph` (flattened) | standard | all others, and the fallback |

Rules:

* **Selection by capability.** The exporter asks the device which material subtypes it has
  (`anariGetObjectSubtypes`) and uses the first graph branch the device offers. Nothing is
  keyed on the library name.
* **Fallback per material.** When a graph cannot be exported (an unsupported node, a failed
  conversion) that one material falls back to branch 4. The other materials are not
  affected.
* **User switch.** A render setting selects `Auto` (graph when available) or
  `Physically Based` (always branch 4), for debugging and for comparing both paths.
* **Surfaces only, at first.** World, light and volume shaders keep their current export.

## 2. Protocol

A graph material is a text plus the images it uses. Nothing is written to disk.

### 2.1 Parameters common to all graph subtypes

| Parameter | Type | Meaning |
|---|---|---|
| `source` | `STRING` | The graph, in the format of the subtype |
| *image parameters* | `SAMPLER` (`image2D`) | One per image used by the graph, see 2.2 |

### 2.2 Images

* The pixels travel as ANARI arrays, inside standard `image2D` samplers. The device takes
  the array from the sampler; the wrap mode and filter of the sampler are ignored, because
  the graph has its own.
* **Binding rule:** a file name in the graph text that is equal to the name of a parameter
  of the material refers to the image of that parameter. The exporter uses the names
  `image.<n>` for Cycles XML and rdla, and `<node name>/file` for MaterialX (the name VisRTX
  already expects).
* The exporter shares one array between all materials that use an image, and replaces it
  only when the image changes.

### 2.3 Geometry attributes

A graph cannot refer to Blender attribute names, because ANARI geometry only has `color`
and `attribute0`..`attribute3`. The exporter rewrites the references:

| In Blender | In the graph text |
|---|---|
| Default UV map | `attribute0` |
| Named UV map, named attribute | the slot the exporter assigned (`attribute1`..`3`) |
| Default color attribute | `color` |
| Generated coordinates | a slot holding Cycles' `generated` attribute |

How a slot is named inside the text depends on the format: `vertex.attribute0` (the Cycles
attribute name in cyclesphi-anari), texture coordinate index 0 (MaterialX), `anari_attribute0`
or the mesh `st` (MoonRay).

### 2.4 Subtype `cyclesXml` (cyclesphi-anari)

* `source`: one `<shader>` element in the dialect of cyclesphi's `xml_read_shader_graph`:
  node elements named after the Cycles node type with a unique `name` attribute and
  `<socket name="..." value="..."/>` children, then `<connect from_node from_socket to_node
  to_socket/>` elements. The output node is addressed as `output`.
* Colors and vectors are written as 4 floats, transforms as 12 floats, arrays inline.
* An image node has `<socket name="filename" value="image.<n>"/>`.
* The graph is written before Cycles finalizes it; the device finalizes it itself.

### 2.5 Subtype `materialx` (VisRTX, RadeonProRenderANARI)

* `source`: a MaterialX document as XML text; `sourceType` = `"documentInline"`.
* `materialName` (optional): the `surfacematerial` to use.
* Image parameters are named `<node name>/file`, and the `file` input of that node has the
  same string as its value.
* Texture coordinates are `texcoord` nodes with an index, never `geompropvalue` by name.

### 2.6 Subtype `rdla` (openmoonray-anari)

* `source`: an rdla fragment (Lua) that creates the material and its maps.
* `materialName`: the name of the root `Material` object in the fragment.
* All object names in a fragment start with a prefix that is unique per material and never
  `/anari/`.
* An `ImageMap` has `["texture"] = "image.<n>"`.
* **Emitters sampled by lights.** A path tracer without light sampling of emitting surfaces
  finds a lamp only by chance. A device which can sample one with lights of its own declares
  `emissionSampling` on the material subtype (`AnariDevice::has_parameter`), and Blender then
  sends, from `Shader::emission_sampling` of Cycles:

  | Parameter | Type | Meaning |
  |---|---|---|
  | `emissionSampling` | `STRING` | `none`, `front`, `back` or `both`: the sides to sample. On `rdla` and on `physicallyBased` (flattened materials; the device takes `emissive` there) |
  | `emissionColor` | `FLOAT32_VEC3` | `rdla`: the radiance the lights emit |
  | `emissionMap` | `STRING` | `rdla`, optional: name of a `Map` of the fragment the radiance is multiplied with |
  | `sampledMaterialName` | `STRING` | `rdla`: name of a second root in the fragment, which emits for camera and mirror rays only. The surface is shaded with it while the lights deliver the emission to diffuse and glossy rays |

  The writer adds the second root to every shader which emits for diffuse rays; the device
  uses it or not. Renderer parameters of openmoonray-anari bound the cost:
  `moonray.maxMeshLights` (64) and `moonray.maxMeshLightTriangles` (500 000).
* **Falloff of lights.** A device which declares `falloff.distance` on the `point` light
  takes, on `point`, `spot` and `quad` lights, what the shader of the light does to the
  emission over the distance: `falloff.distance` (`ARRAY1D` of `FLOAT32`, increasing) and
  `falloff.color` (`ARRAY1D` of `FLOAT32_VEC3`), the factor on the emission at these
  distances from the point of the light which emits; linear in between, constant outside.

## 3. Viewport performance

The viewport must stay as responsive as with the flattened materials.

* **Serialize only what changed.** A graph is serialized only for shaders Cycles reports as
  modified.
* **Skip unchanged text.** The exporter keeps a hash of the last text sent per material.
  An identical text is not sent again, so the device does not rebuild or recompile it.
* **No DOM.** The text is built by appending to a string; no XML library is involved on the
  Blender side.
* **Images are shared and not copied.** One ANARI array per image, shared between materials.
  Where the pixel format matches an ANARI type, the array shares Cycles' memory instead of
  copying it.
* **No geometry export on material edits.** Geometry is exported again only when a material
  needs a new attribute slot or changes its subtype.
* **Constant edits (dragging a slider).** A changed constant is a new text. This is cheap
  for Cycles and MoonRay. For MaterialX on VisRTX it means a new MDL compilation: the
  exporter should send changed constants as parameter overrides instead (open, see 5.2).
* **Measure.** `CYCLES_ANARI_TRACE=1` logs the time of every material export.

## 4. Work in blenderphi

* [x] Material back-end selection, per-material fallback, change tracking with text hashes
  (`scene.cpp`: `export_graph_sources`, `set_graph_material`).
* [x] Image binding: `image2D` samplers shared through `AnariImageCache`.
* [ ] Shared (zero-copy) arrays for images whose format matches an ANARI type. Cycles frees
  its pixels as soon as an image is removed or reloaded, so the ANARI array has to be
  released first: needs a hook in `AnariDevice::image_free`.
* [x] Branch 1: Cycles XML writer (`graph_xml.cpp`). Verified against cyclesphi-anari with
  `test_shader_graph`: all four graphs within 0.6 % of Blender's Cycles.
* [x] Branch 2: `export_to_materialx_inline` in Blender (`NOD_shader_materialx.hh`), called
  from the Cycles sync, text handed to the device (`graph_materialx.cpp`). Verified against
  RadeonProRenderANARI with `test_shader_graph`: checker 3.5 %, math on an image 5.8 %, Mix
  Shader with a procedural mask 1.5 % (the basic render is at 8.4 %).
* [x] Branch 2: a node tree with a linked node Blender's MaterialX export does not implement
  (Color Ramp, Voronoi, ...) is not exported, the material falls back to branch 4. Nodes
  which the export implements by returning the default of their socket (Attribute, Color
  Attribute) are not detected yet.
* [x] Fix in Blender's MaterialX export of the Mix Shader node: a shader without an opacity
  of its own made the share of the other shader transparent
  (`node_shader_mix_shader.cc`; this also changes the USD and Hydra export).
* [ ] Branch 2: named UV maps (`geompropvalue` by name) rewritten to attribute slots.
* [ ] Branch 2: Color Ramp and curves in Blender's MaterialX export.
* [x] Branch 3: rdla writer (`graph_rdla.cpp`): Principled, Diffuse, Glossy, Glass,
  Emission and Transparent to DWA materials, Mix Shader to `DwaLayerMaterial`; image
  textures on the default UV map, checker with a whole number of squares, attributes and
  vertex colors, math, mix, invert, separate/combine, color ramp and clamp to MoonRay maps.
  Verified against openmoonray-anari with `test_shader_graph`: checker 1.3 %, Mix Shader
  with a procedural mask 0.4 %, math on an image 6.6 % (the basic render is at 6.5 %).
* [x] Branch 3: Mapping node, noise, bump, subsurface, hair and Add Shader of two BSDFs, with
  the rewrite of the writer to moonshine networks (see 5.4): nothing falls back any more,
  and every node has a test against Cycles.
* [x] Render setting *Materials*: `Auto` / `Physically Based` (also the device parameter
  `materials=physicallyBased`).
* [x] Smoke test `test_shader_graph`: procedural checker, noise with a color ramp, non-linear
  math on an image, and a Mix Shader with a procedural mask, compared with Blender's Cycles.
* [x] [MATERIALS.md](MATERIALS.md) section 5 and the README describe the graph export.
* [x] Full smoke matrix (`tools/run_smoke_all.sh`) on 2026-10-09 and the result tables of
  the result tables (now in WINDOWS.md) regenerated: all 14 configurations pass the 14 tests, except the environment
  and clip start tests on helide. The images of the result tables are still the ones of
  2026-10-06 (`tools/exr_to_png.py`, `tools/smoke_montage.py`).
* [ ] The demo scene comparison leaves open: MoonRay and VisRTX in direct sunlight on Lone
  Monk (2 times too bright with the materials, right with one gray material), MoonRay on
  Classroom (5 times), Radeon ProRender on Monster (0.15).

Debugging aids: `CYCLES_ANARI_GRAPH_FORMAT=cyclesXml|materialx|rdla` forces a format (also
on a device which does not take it) and `CYCLES_ANARI_GRAPH_DUMP=<directory>` writes the
text of every exported graph to a file.

Measured export cost (`CYCLES_ANARI_TRACE=1`, Cycles XML): 0.02–0.3 ms per material, 1.6 ms
for a material with a color ramp (256 table entries).

## 5. Work in the devices

### 5.1 cyclesphi-anari

* [x] Material subtype `cyclesXml`: parses `source` from a string, binds images from
  parameters, keeps the id channels, rebuilds only when the text or a used image changed,
  reports errors through the ANARI status callback and falls back to gray. Standalone test:
  `build/graph-scratch/cycles` (58 checks, CPU and OptiX).
* [x] Cost of a material edit, measured on a mesh with 1 million triangles
  (`build/graph-scratch/cycles/bench_material_edit.cpp`): a new `source` text costs about
  1 ms more than an unchanged frame (13 ms against 12 ms on the CPU, 5.3 ms against 4.6 ms
  with OptiX). The mesh is not synchronized again and the BVH is not rebuilt.
* [ ] Giving a surface another material object costs a full mesh sync and BVH build (about
  430 ms per million triangles on the CPU, 100 ms with OptiX), although only the shader
  assignment changes. The exporter does this when a material changes its subtype, for
  example when it falls back from the graph to `physicallyBased`.
* [ ] A material which starts to read a mesh attribute costs 62 ms (CPU) or 34 ms (OptiX)
  per million triangles once.
* [ ] The device's Cycles copy lacks some nodes and sockets of Blender's Cycles
  (`boolean_math`, `integer_math`, `get_vector_component`; `missing` on image and attribute
  nodes, the tangent inputs of `glass_bsdf`, dispersion on `principled_bsdf`): unknown
  sockets warn and are dropped, unknown nodes give the gray fallback.
* [ ] True displacement: the exporter always sends `displacement_method = bump`.
* [x] A material with only a volume output on a mesh (homogeneous fog in a box) is sent
  as `cyclesXml` with the mesh.
* [ ] Volume shaders of volume objects as XML with several named spatial fields (later).
* [x] `attribute4`..`attribute7` on geometry (`CYCLES_GEOMETRY_ATTRIBUTES`), `maxBounce`
  and `castShadow` on lights, ray visibility of lights and of the world, `objectRandom` on
  instances.

### 5.2 VisRTX

* [x] Built with `VISRTX_ENABLE_MDL_SUPPORT` and `VISRTX_ENABLE_MATERIALX_SUPPORT` into
  `build/visrtx-mdl` and `install/visrtx-mdl` (the plain VisRTX in `install/bin` is not
  touched), without source changes: MDL SDK 2025.0.5 from the prebuilt release in
  `external/mdl-sdk`, MaterialX 1.39.4 copied from Blender's libraries to
  `install/materialx`. Probe and timing harness: `build/graph-scratch/visrtx`.
* [x] Blender's documents render: `open_pbr_surface` with constants, images bound as
  samplers and pattern nodes; Mix and Add Shader of the simple BSDFs.
* [x] Renders from Blender (2026-10-11), with `install/visrtx-mdl/bin` first in the library
  path and on `PATH`: the device loads `libmdl_sdk.dll` by its bare name, so the MDL DLLs
  have to be on `PATH` or next to `blender.exe`; without them the device still lists
  `materialx` and then renders nothing. Two fixes in VisRTX: the transcoder looks the
  material up by the name the generated MDL exports (Blender names its material
  `material`, a reserved word of MDL, which the generator renames to `material1`), and a
  MaterialX material whose document was not transcoded or compiled says so in its `valid`
  property, so Blender flattens it. Smoke suite 15 of 16: basic render 1.06 / 0.06, checker
  0.08, math on an image 0.12; `test_texture_mapping` fails (the Mapping node is not
  honored), and a rejected graph is flattened without baking (Mix Shader with a mask
  0.55). The suite takes 17 minutes, each material compiles for 14 to 22 seconds.
* [ ] A new graph costs 3 to 30 seconds until the first frame (MDL code generation and the
  OptiX module), and a changed constant sent as a new document 3 to 8 seconds. A constant
  sent as a parameter override costs about 6 ms. The exporter has to send constant edits as
  overrides (named `<node>_<input>`; they stay until unset) before this is usable in the
  viewport.
* [ ] A Principled BSDF inside a Mix or Add Shader fails: Blender writes a `thin_film_bsdf`
  node, which MaterialX 1.39.4 has no definition for.
* [ ] OpenPBR through MDL ignores the IOR and the specular level of the base layer (a
  limitation of MaterialX's MDL generator).
* [ ] The `file` value of an image node must be the parameter name or empty, never an
  absolute path (the exporter writes the parameter name).

### 5.3 RadeonProRenderANARI

* [x] Material subtype `materialx` on `rprMaterialXSetFileAsBuffer`, images through
  `rprMaterialXAddPreloadedImage`; `texcoord` index 1..3 becomes `attribute1..3`. Errors are
  reported and give a gray fallback; the property `valid` of the material tells whether the
  fallback is in use. Standalone test: `build/graph-scratch/rpr/test` (44 checks, GPU and
  CPU).
* [x] Surface models: Northstar needs a definition for every surface shader node. The
  device carries its own for `open_pbr_surface` (adapted: the stock one renders black),
  `standard_surface` and `UsdPreviewSurface`. pbrlib BSDF graphs work without one.
* [x] The device rewrites what Northstar gets wrong without a message: document-level
  `<output>` elements (they made every Blender document black), `separate2/3/4`, a `mix` or
  `add` of two emission shaders, `opacity` on a `surface` node, BSDF roughness.
* [x] Fixed in the device: vertex attributes went black after the second commit of any
  material, also for `matte` and `physicallyBased`.
* [ ] Cost of a material edit: a changed constant needs a reload of the whole document into
  a new node, 5–17 ms, and the next frame costs about 10–20 ms more.
* [ ] Not correct on Northstar: emission under a coat is about 10 times too dark,
  transmission depth and thin film have no effect, the frame alpha of transparent surfaces
  stays 1. Unknown nodes (error and gray fallback): `randomfloat`, `blackbody`,
  `ambientocclusion`.
* [ ] Texture mapping agreement is 0.990 instead of 1.000 (a quarter pixel shift at the
  borders of the checker squares).

### 5.4 openmoonray-anari

* [x] Material subtype `rdla`: the fragment is checked in a scratch scene first, then loaded
  into the device's scene; the objects of a replaced fragment are reset; any error gives a
  magenta error material and an ANARI error. Standalone test: `build/graph-scratch/moonray`
  (60 checks).
* [x] Images from memory for `rdla` images and for the `image1D`, `image2D` and `hdri`
  paths: a custom OpenImageIO input serves the pixels, nothing is written to disk
  (`OPENMOONRAY_ANARI_TEXTURE_FILES=1` goes back to scratch files). The `transferFunction1D`
  volume still writes a scratch `.vdb`.
* [x] moonshine built into the runtime: all 106 DSOs (`build-moonray-windows.ps1 -Target
  moonshine`), 174 classes in `install/moonray/rdl2dso`.
* [x] Geometry attributes as primitive attributes: `st` is `attribute0`, slots are
  `anari_attribute0..3` (slots 1..3 on triangle geometry only), the vertex color is `Cd`.
* Withdrawn (2026-10-10): a `cyclesXml` material subtype in the device, which ran the value
  nodes as byte code of Cycles' SVM inside two MoonRay shaders built from the headers of
  the Cycles kernel. It matched Cycles at the noise floor (0.001 on 284 of 366 node tests)
  and brought Barcelona from 1.08 / 0.31 to 1.04 / 0.08, but it put Cycles code and a build
  dependency on a Blender tree into another renderer. It was removed again the same day:
  the device has nothing of Cycles in it.
* [x] Instead the RDLA writer of Blender (`graph_rdla.cpp`) translates the whole graph to
  networks of existing MoonRay and moonshine classes
  ([MATERIALS.md](MATERIALS.md#55-rdla-graph_rdlacpp) has the table per node): the closures
  to one `DwaBaseMaterial`, every value node to one or several maps, a shader with a Light
  Path node to a `RaySwitchMaterial`. No flattening of whole materials: a node without a
  counterpart becomes its best approximation or a constant, with a note.
  * Node tests against Cycles: 289 compared, 241 within 0.02 and 268 within 0.05. Noise
    textures have another pattern with the same statistics, which is accepted.
  * All 605 surface shaders of the seven demo scenes are exported, none is flattened.
  * The noise maps were in the runtime all along, under versioned names (`NoiseMap_v2`,
    `NoiseWorleyMap_v3`).
* [x] Bump: `AnariBumpNormalMap`, one small shader of the device repository
  (`openmoonray-anari/dso/`, `build-moonray-windows.ps1 -Target anari_dso`), written against
  the MoonRay shading API. It evaluates the bound height three times on copies of the
  shading state moved by the derivatives; that relies on the state layout header MoonRay
  installs, as the public shading API has no call to evaluate a map at another point.
  21 checks against the analytic normal of a sine height field, largest error 0.0026.
* [x] Device: per instance primitive attributes `anari_object_random`, `anari_object_color`,
  `anari_object_index` for Object Info; a mesh placed by a mirroring transform rendered
  black (the black papers of Classroom), it gets a left-handed `orientation` now.
* [ ] Open in the translation: anisotropy, the reflection color of glass, sheen, rough
  diffuse (Lambertian), ray depth, reflective caustics, a color read together
  with its alpha, Vector Transform from camera to world (0.26, not explained), a bright
  rim on glass with transparent shadows (0.31, not explained). No counterpart in moonshine:
  Brick, Magic, Particle Info, Wireframe, Ambient Occlusion, UDIM images.
* [ ] Hair, what stays approximate: a Hair BSDF whose lobes add up to more than one is
  0.5 to 0.9 as bright (the hair of Cosmos Laundromat), it has no highlight of its own, a
  dense curtain of Principled Hair lit from behind is 2.6 to 5 times too bright (cause not
  found), a diffuse BSDF on curves is 1.2 times brighter.
* [ ] Subsurface, what stays approximate: light through thin parts is 0.3 to 1.9 of Cycles
  depending on the albedo, and more saturated; a Principled subsurface weight below one is
  a shorter radius, not a mix with diffuse.
* [ ] The notes of the writer are only printed with `CYCLES_ANARI_TRACE=1`; they could be
  shown as info messages.
* [x] Hair and subsurface in the RDLA writer (2026-10-11,
  [MATERIALS.md](MATERIALS.md#55-rdla-graph_rdlacpp)): Hair BSDF as `HairDiffuseMaterial`
  with the hue of Cycles, Principled Hair as `HairMaterial_v3`, Hair Info from attributes
  the device computes for curves, subsurface radius converted per method.
* [x] Monster Under The Bed on MoonRay: the nine area lights with a node tree (an emission
  mixed out by Light Path *Ray Length*) contributed nothing. Two causes, both in the exporter:
  a Math node of two values which are constant only in the evaluation of the light shader
  (the ray length divided by two) returned its first input, so the lights faded out over one
  meter instead of two; and the average over 16 distances to the scene is nearly zero for a
  light which only reaches two meters. Fixed in `material.cpp`, and the device takes the
  falloff as it is now (`falloff.distance`, `falloff.color`, a `ColorRampLightFilter` whose
  distance is the one to the sampled point of the light, as the ray length of Cycles):
  Monster 0.75 / 0.55 to 0.92 / 0.22; a single such light over a floor is at 1.05, its
  profile within 6 % (`test_light_falloff`: 1.02 / 0.03).
  * A ramp of MoonRay has ten control points: the device keeps the ten of a longer table
    which lose least.
  * A disk light is still a rectangle of the same area; the textured emission of a light
    and the world as a graph are not done.
* [x] `MeshLight` for emitting surfaces (2026-10-11, protocol in 2.6,
  [MATERIALS.md](MATERIALS.md#55-rdla-graph_rdlacpp)): one light per emitting surface on a
  mesh of its own with all instances and both sides, the surface emits for camera and
  mirror rays only. Closed room, 16 samples, relative RMS against Cycles with 1024 samples:
  0.67 as a surface, 0.12 with the light, Cycles itself 0.14; a lamp of 0.2 m 3.4 / 0.13 /
  0.16. What remains:
  * [ ] Cost. MoonRay samples every light at every hit: Classroom takes 192 s instead of
    76 s with its four lights, for a noise of 0.167 instead of 0.202 (one light, the
    daylight: 103 s and 0.173). Emitters without a map could share one light with the
    radiance as a vertex attribute, emitters of one material one light across objects; a
    threshold on the share of the power would leave the small ones to the surface.
  * [ ] An edit of an emitter rebuilds the sampling structure of its light in MoonRay: about
    a second per 250 000 triangles, nothing for a lamp of a few triangles. Emitters above
    `moonray.maxMeshLightTriangles` stay surfaces.
  * [ ] One more bounce of the emission than before at the same diffuse depth, and the
    clamp of indirect samples applies to the light: +2 % at depth 8, +26 % at depth 2 in a
    closed room.
  * [ ] Geometry other than triangles; an emission whose map reads the space of the object
    takes a light per instance; one which reads the direction of view or the side which is
    hit (Fresnel, Layer Weight, Backfacing) gets no light and stays a surface; the unsampled
    side of a one-sided emitter only reaches the scene through camera and mirror rays.
  * Two fixes in the MoonRay fork, `MeshLight.cc` / `.ispc`: the map of a mesh light was
    evaluated at the position of the receiver, and the energy of the faces from the map in
    the wrong space.
* [ ] Barbershop on MoonRay: 263 graphs load without an error, but a sample takes about
  28 s and 18 GB; 64 samples were not finished.
* [ ] Cost of a material edit: MoonRay reloads every geometry assigned to a changed root
  shader, so an edit costs about 40 ms at 180k triangles and about 1 s at 4.5 million, the
  same as for `physicallyBased`. It would need a change in `rdl2::Layer`.
* [ ] Rules the exporter has to follow: an object name keeps its class for the life of the
  device (the writer puts the class into the name); only maps may be bound; an unknown
  attribute is an error for the whole material.
* [ ] Build `materialx_shaders` (optional, the `ND_*` maps).

## 6. Real scenes

The Cycles demo scenes of blender.org (Classroom, Barcelona Pavillion, Lone Monk, Monster
Under The Bed) are compared with `tests/python/anari_scene_compare.py`: it renders a .blend
with Cycles and with an ANARI device, once with shader graphs and once flattened, and prints
the brightness ratio and the relative difference of the block averaged luminance. It can
also replace all materials by one gray material (to separate lights and geometry from the
materials) and unlink given node types (to find the nodes behind a difference).

### 6.1 cyclesphi-anari (480 pixels wide, 64 samples)

| Scene | At first | Now | Noise of the metric |
|---|---|---|---|
| Monster Under The Bed | 6.27 / 5.94 | 1.002 / 0.012 | 0.037 |
| Barcelona Pavillion | 0.28 / 0.79 | 0.999 / 0.011 | 0.008 |
| Lone Monk | 0.0004 / 1.00 | 1.016 / 0.051 | 0.019 |
| Classroom | 0.73 / 0.47 | 1.020 / 0.173 | not measured |
| Italian Flat | 1.02 / 0.10 | 1.019 / 0.098 | not measured |
| Cosmos Laundromat (benchmark) | 1.00 / 0.09 | 1.007 / 0.057 | not measured |
| Barbershop | 1.54 / 0.85 | 1.007 / 0.062 | not measured |

(brightness ratio / relative difference; the noise is Cycles against Cycles with another
seed. The last three scenes were added on 2026-10-10, "at first" is their first render
then.) All materials of the seven scenes are exported as graphs (279 in Barbershop). What
the scenes needed beyond the material graphs:

* [x] Shader graphs of lights and of the world as XML: device extension
  `CYCLES_SHADER_XML` (`source` on lights, `backgroundSource` on the renderer). Sky textures,
  mixed backgrounds and lights which fade their emission with a node graph depend on it.
* [x] Ray visibility of objects: device extension `CYCLES_INSTANCE_VISIBILITY`
  (`visibility.camera`, `.diffuse`, `.glossy`, `.transmission`, `.volumeScatter`, `.shadow`
  on instances). Classroom lights the room through an emissive plane which is invisible to
  camera and shadow rays.
* [x] Cycles no longer applies object transforms to the vertices for the ANARI device
  (`scene/geometry.cpp`). With them applied, object space texture coordinates were world
  coordinates on the device, and mirrored objects had an inverted winding and rendered
  black.
* [x] Camera `near` / `far` and `imageRegion` in the device (they were not read), and
  viewplane changes of the orthographic camera now reach the next frame.
* [x] Images are marked as alpha associated in the XML, the device must not associate the
  alpha of the pixels Cycles loaded a second time.
* [x] The rotation of the sun of a sky texture is written as in the Blender node: every
  simplification of the graph mirrors it.
* [x] Without light graphs: the strength of a light linked to a Light Falloff node.
* [x] Barcelona: the trees were green instead of orange and the image 4 % darker. The
  materials color them with the random number of the Object Info node, which the device did
  not have: Blender sends `objectRandom` on the instance now (and the object color as the
  uniform `color`), 0.999 / 0.011.
* [x] Classroom: the remaining difference of 0.18 is in the brightest pixels (the lamps).
  The medians of image regions agree within 1.00 to 1.06.
* [x] Barbershop, 1.54 / 0.85 at first:
  * The mirrors showed the window lights. Lights have ray visibility too
    (`visibility.diffuse`, `.glossy`, `.transmission`, `.volumeScatter` on the light), and
    so has the world (`backgroundVisibility.*` on the renderer): 1.25 / 0.42.
  * The walls and the ceiling were 1.8 times too bright: their color comes from a ramp over
    the *Pointiness* of the Geometry node, which Cycles computes for the mesh. It is
    exported in an attribute slot now, like *Random Per Island*.
  * The wall material reads five attributes. cyclesphi-anari has `attribute4`..`attribute7`
    now (`CYCLES_GEOMETRY_ATTRIBUTES`), and the exporter assigns the slots per shader
    instead of for the whole scene: 1.007 / 0.062, no attribute is dropped any more.
  * `maxBounce` and `castShadow` on lights (*Max Bounces* is 0 to 3 on most lights of the
    scene). With one gray material for all objects the scene is at 0.999 / 0.041.
  * A material with only a volume output (the fog box) is exported with its mesh.
* Barbershop: the device warns about three image nodes without an image ("not the name of
  an image2D sampler parameter"). Their files are not in the scene's download (skin maps of
  the agent, `generic_scratches.png`, `guilder_ornament.png`); both renderers show such a
  node in the color of a missing image.
* [ ] True displacement, world volumes, and light shaders with `Is Camera Ray` style
  outputs (always 0 for lights, as in Cycles) were not tested.

### 6.2 The other devices

At first Radeon ProRender and MoonRay were far from Cycles on these scenes, with graphs and
flattened alike (Classroom: rpr 2.39 / 2.20, moonray 4.81 / 5.16; Barcelona: 0.29 / 0.79 and
0.27 / 0.80; Monster on rpr: 4.55 / 5.60). The material graphs were not what decided, the
scenes need the following from every device, with or without graphs. Results are in
[MATERIALS.md](MATERIALS.md#56-what-the-smoke-test-shows).

Done in the exporter:

* [x] The ANARI device of Cycles is derived from the CPU device, so the Cycles CPU kernels
  can evaluate shaders (`ShaderEval`) for the export.
* [x] World shaders which are not a constant or an environment image are baked into an
  equirectangular image for the `hdri` light (0.3 s for 2048x1024), the sun of a sky texture
  becomes a `directional` light. Barcelona then matches on every device tested (0.96 to 1.04).
* [x] Ray visibility is sent to every device which declares `visibility.camera` on the
  `transform` instance (parameter info instead of an extension name). Devices without it:
  objects hidden from camera or shadow rays are left out, unless they emit.
* [x] Color nodes which are not linear are fitted with the output transform of the sampler
  (`apply_function`). Lone Monk has a Hue/Saturation node in 31 of 34 materials, which was
  ignored: the scene was 3.6 times too bright on every device without graphs.
* [x] Light shaders with their own falloff (Light Falloff with *Smooth*, *Linear*,
  *Constant*; Light Path *Ray Length*) are averaged over the distances to the objects.
  Monster was 16 times too bright with all nine lights at full strength.
* [x] A Mix Shader with an empty input scales the other closure instead of returning it.
* [x] A graph the device can't evaluate (material property `valid` is false) is replaced by
  the flattened material. Radeon ProRender does not have the MaterialX nodes `hsvadjust`
  and `randomfloat` and rendered five Classroom materials as white diffuse.
* [x] Path length limits and clamps of the integrator are sent to every device whose
  renderer declares `maxBounce`, `maxDiffuseBounce`, `maxGlossyBounce`,
  `maxTransmissionBounce`, `maxVolumeBounce`, `maxTransparencyBounce`, `clampDirect`,
  `clampIndirect`. Classroom renders with 2 diffuse bounces and clamps of 5.5 and 3.0: a
  device without them is 1.4 times brighter from the clamping alone.

* [x] Small emissive meshes which are hidden from camera or shadow rays become `quad`
  lights on devices without ray visibility (the plane in the windows of Classroom, the sky
  cards of Lone Monk); a textured emission is averaged. Objects which cast no shadow are
  left out there.
* [x] The normal map sampler returns the encoded color, the devices decode it. The
  exporter decoded it as well, which made every normal mapped surface wrong on five
  devices.
* [x] VisRTX: `fireflyFilterMode` = `none`, its default darkens emissive lighting.
* [x] The frustum sent with `imageRegion` has the aspect ratio of the window.
* [x] Smoke tests `test_camera_shift`, `test_camera_clip`, `test_normal_map`.
* [x] Baking for flattened materials ([MATERIALS.md](MATERIALS.md#18-baked-base-color)): the
  Cycles kernel evaluates what a sampler can't express on the meshes, through the
  displacement output of the shader. Baked are the factor of a Mix Shader between two
  shaders of constant parameters (as a mask, all parameters exact), a procedural base
  color, or the emission of a shader which only emits; into an image with a cell per
  triangle, or into vertex colors for dense meshes.
* [x] *Generated* texture coordinates of image textures in flattened materials are an
  attribute slot, not `objectPosition`.
* [x] Diffuse and glossy shaders mixed by Fresnel or Layer Weight become a dielectric; a
  Mix or Math with a node which can't be evaluated uses the other input.
* [x] `objectRandom` on instances for the Object Info node.
* [x] A world which camera rays see differently than the rays which light the scene (Mix
  Shader by Light Path *Is Camera Ray*, or a world hidden from diffuse rays) is evaluated a
  second time for camera rays and shown as the background image of the renderer
  (`KHR_RENDERER_BACKGROUND_IMAGE`), resampled for every view; the `hdri` light is not
  visible then. Cosmos Laundromat has a darker sky for the camera than for the light:
  barney 3.44 / 2.39 to 1.09 / 0.18. Smoke test `test_world_camera_ray` (barney 0.98 / 0.05,
  VisRTX 0.98 / 0.05).
* [x] Averages over the surface for what a sampler has to leave out
  ([MATERIALS.md](MATERIALS.md#14-value-nodes)): the factor of a Mix Shader or Mix node, the
  second of two textures, and the average the base color is calibrated to are taken at
  points of the meshes which have the shader, not over the whole image. Of two textures the
  one which varies more on the surface is kept. Cosmos Laundromat on barney: 1.07 / 0.18 to
  1.00 / 0.11 (jacket no longer half metallic, hands no longer red, hair orange).
* [x] Colors with Layer Weight or Fresnel nodes are baked, with these nodes at a typical
  direction of view (the dead tree of Cosmos Laundromat).
* [x] A Mix Shader by *Is Camera Ray* is the shader of the camera; a reflection mixed over
  a transparent shader by a Fresnel factor gets that factor as opacity. Small objects which
  cast no shadows and have no light inside stay (the lamp globes of Barbershop).
* [ ] An image with *Box* projection on generated coordinates is one planar sampler: it is
  stretched on the sides of an object (the doors of Barbershop, visible when the shader
  leaves it at full contrast).
* [ ] Roughness and metallic which are procedural stay constants; only one value per shader
  is baked.
* [ ] A glass which is clear for shadow rays (*Is Shadow Ray*) is still a 50 % mix.
* [x] Curves are exported with the attribute slots of their shader, and the Hair Info
  outputs *Intercept*, *Length* and *Random* go to cyclesphi-anari as attributes
  ([MATERIALS.md](MATERIALS.md#15-texture-coordinates-and-attributes)).
* [x] Lamp globes: a small object which casts no shadow around a light stays half
  transparent and the light is twice as strong (Classroom).
* [ ] VisRTX with MaterialX on the demo scenes (2026-10-11, graphs / flattened materials):
  Barcelona 1.01 / 0.12 (13 / 10), Cosmos 0.99 / 0.19 (24 / 58), Lone Monk 1.02 / 0.41
  (18 / 18), Monster 0.76 / 0.59 (3 / 16), Classroom 2.55 / 3.31 (59 / 20), Italian Flat
  blown out by a factor of 1646 (73 / 32), Barbershop 4.14 / 11.4 (105 / 159). Better than
  the flattened materials on Barcelona only; which materials break Classroom and Italian
  Flat was not looked into. Lamp globes (see above) are only handled for flattened
  materials.
* [ ] Blender 5.2 splash scene (a lion with fur from geometry nodes, sky with a sun of 30
  degrees, two area lights), `external/demo/splash52`: barney renders it 25 times too
  bright with white speckles. Not the materials (the same with a gray override), not the
  fur, not the sun; with the world off it is ten times too dark instead. VisRTX renders
  the same export at 0.83 / 0.42. Cause not found.
* [x] Fast GI: the devices without it get `maxDiffuseBounce` and `maxGlossyBounce` limited
  to its number of bounces. Italian Flat (6 diffuse bounces, Fast GI after 2) on barney:
  2.57 / 3.01 to 1.09 / 0.55.
* [x] The rule for diffuse and glossy shaders mixed by Fresnel also holds when the factor
  passes through math and mix nodes (the *DiffuseGlossy* node group of Barbershop made
  every material a white metal).
* [x] A base color which is an image changed by nodes a sampler can't express keeps the
  sampler, scaled to the average color the kernel evaluates on the meshes
  ([MATERIALS.md](MATERIALS.md#18-baked-base-color)). 64 materials of Barbershop: the
  inside of the door went from 0.94 to 0.08, the floor from 0.50 to 0.15. Barbershop on
  barney 2.62 / 1.87 to 1.81 / 1.16.
* [x] An emissive surface whose shader fades the emission with the distance (Light Falloff)
  or with the direction of view (Layer Weight, Fresnel) emits the average over the
  distances to the scene and over the hemisphere, like a light. The lamp shades of
  Barbershop are brightest seen face on: their emission is scaled by 0.66.
* [ ] RadeonProRenderANARI: the trees of Barcelona have no leaves since 2026-10-10
  (1.25 / 0.45 instead of 0.93 / 0.13: without their shadows the walls are in full sun).
  On every other device the leaves are there. Checked: the camera, the lights and the
  integrator are right (one gray diffuse material for everything: 1.001 / 0.005, a gray
  Principled BSDF: 0.98 / 0.02), the smoke tests for normal maps, textures, lens shift
  and the backdrop pass, and the flattened leaf material is the same as the one barney
  gets: base color from an image with a scaled output transform (alpha row 0 0 0 1),
  opacity from the alpha of the same image. Not the uniform `color` of the instances
  either (it is no longer sent to devices without Object Info). What the device makes of
  an opacity sampler next to a base color sampler with an output transform is the next
  thing to test, in `build/graph-scratch/rpr/test`.
* [ ] RadeonProRenderANARI is slow on the larger scenes: Italian Flat and Cosmos did not
  finish 64 samples in 450 s. Photon returned no image for Cosmos after 443 s.
* [ ] Barbershop is still too bright without graphs: 1.43 / 0.63 with flattened materials
  and lights on cyclesphi-anari (1.07 / 0.32 with one gray material, so the lights are not
  the reason), 1.79 / 1.16 on barney. The whole room is brighter, the lamps themselves
  1.8 times. Ruled out: *Max Bounces* of the lights (1 to 3 % of the image in Cycles), the
  disabled caustics (2 to 7 %). Not expressed: the lamp glass is a Mix Shader by *Is Camera
  Ray*, the lamp color goes through a Color Burn with a noise texture, and several
  materials read more attributes than barney has slots.
* [x] The distances for lights with their own falloff are taken to vertices of the meshes.

Done in the devices (all uncommitted working tree changes):

* [x] Ray visibility per instance in openmoonray-anari and RadeonProRenderANARI.
* [x] Camera `imageRegion` in openmoonray-anari, RadeonProRenderANARI and mitsuba-anari:
  lens shift, render border and the camera view of the viewport depend on it (Lone Monk
  has a lens shift, its framing was off). mitsuba-anari: also the orthographic camera for
  frames which are not square.
* [x] Camera `near` / `far` in barney, VisRTX, anari-visionaray, mitsuba-anari and Photon,
  as planes which clip camera rays only. Lone Monk has an emissive card nearer than the
  clip start: these devices rendered the card instead of the scene.
* [x] VisRTX: `matte` bounces light (it was lit directly only).
* [x] openmoonray-anari: output transform of samplers, textures for roughness, metallic,
  clearcoat, emission, normal and opacity (cutout), `attribute1..3`, the `transform`
  sampler; presence shadows for cutouts.
* [x] Bounce limits and clamps in openmoonray-anari (all eight parameters) and
  RadeonProRenderANARI (`maxBounce`, `maxDiffuseBounce`, `clampIndirect`; Northstar ignores
  the other limits).
* [x] MoonRay core: `ProjectiveCamera` kept the attribute keys of the first camera class in
  statics, so switching between a perspective and an orthographic camera lost the depth of
  field or rendered black. Fixed in `rendering/pbr/camera/ProjectiveCamera.cc`.

Open:

* [x] Bounce limits and clamps in VisRTX (all eight parameters) and barney (total,
  diffuse, glossy, transmission and both clamps). VisRTX: Classroom 2.86 / 3.92 to
  0.83 / 0.63. With the clamps of the scene both devices are darker than Cycles, without
  them VisRTX is at 1.8 and barney at 0.84: the clamps act per light sample and depend on
  how each device picks lights.
* [x] VisRTX: normal mapped surfaces gained energy (view directions below the horizon of
  the mapped normal, bounce directions into the surface, shadow origins of flipped faces):
  Lone Monk 2.19 / 1.34 to 1.04 / 0.37. barney: normal maps brighten the lit side now, and
  a ray without a scatter direction is not traced a second time (emitters were counted
  twice).
* [x] Bounce limits and clamps in the remaining devices. Classroom went from 2 to 3 times
  too bright to 1.09 (OSPRay), 1.52 (Photon) and 1.69 (Visionaray CUDA).
  * anari-ospray: `maxBounce`, `maxDiffuseBounce` (OSPRay's `maxScatteringEvents`), both
    clamps as `maxContribution` (OSPRay has one); a diffuse-only `physicallyBased` is
    rendered as a Lambertian.
  * anari-visionaray: the `default` renderer takes parameters at all now, `maxBounce`,
    `maxDiffuseBounce` and the clamps.
  * Photon: all limits and both clamps.
* [x] Normal maps in Visionaray (without tangents on the mesh too) and Photon.
* [ ] openmoonray-anari: a textured opacity in the `blend` mode is a hard cutout at 0.5.
  Fractional presence would need `DwaBaseMaterial` for the flattened path.
* [ ] Normal maps: OSPRay only darkens (`test_normal_map`).
* [ ] Visionaray picks one light per hit uniformly; its CPU library has almost no indirect
  light on the demo scenes (as before the changes).
* [x] anari-visionaray after the merge of upstream on 2026-10-10: it reports
  `KHR_FRAME_ACCUMULATION`, so Blender lets it accumulate and fetches the image once per
  batch of samples, as with barney. A regression of the merge is fixed in the fork: the new
  rule "a hit without a normal is lit as if it faced the light" also applied to volumes
  without gradient shading, which came out 1.7 times too bright (`gn_volume` 1.71 / 2.17,
  back to 0.98 / 0.06 with the rule limited to surfaces, `renderer/Pathtrace_impl.cpp`).
* [ ] Visionaray (CUDA) is slow on large scenes for a reason inside the device. Barbershop
  at 1920x804, per sample after the first frame: 1.87 s with one opaque gray material
  (barney: 0.084 s) and about 3.6 s with the materials of the scene; the first frame takes
  17 s (barney 3 s). Fetching the image costs milliseconds, the GPU is at 100 % with 3 of
  11 GB used, and the versions before and after the merge take the same time. The device
  traces rays with its own BVH traversal in CUDA where barney uses OptiX, and it traces a
  ray again from every transparent layer it passes (`intersectSurfaces` in
  `renderer/common.h`). Not profiled further. `CYCLES_ANARI_TRACE=1` prints these times at
  the end of a render ("timing ...").
* [ ] Lights: *Max Bounces*, the ray visibility of a light other than diffuse, and the ray
  visibility of the world other than diffuse exist on cyclesphi-anari only.
* [ ] Materials read at most four attributes of a mesh (eight on cyclesphi-anari).
* [ ] helide (ANARI SDK) has no camera `near`.
* [ ] mitsuba-anari needs minutes per frame on the demo scenes (Dr.Jit compiles a kernel
  of 100 000 operations), it was left out of the comparison. It emits from the front face
  of a mesh only.
* [ ] `build\visrtx-mdl` does not have the VisRTX changes.
* [ ] Lights which cast no shadow (Classroom's fill light outside of the windows) have no
  equivalent in ANARI.
* [ ] Lights with their own falloff are one average strength: Monster is at 0.35 to 0.5 of
  Cycles on most devices, with a different distribution of the light.
* [ ] Baking covers one value per shader. Open: roughness and metallic from procedural
  textures, a base color which is a texture changed by a procedural one (most Lone Monk
  materials), nested mixes.
* [x] openmoonray-anari: the graphs are translated to MoonRay and moonshine classes by
  Blender's RDLA writer, see 5.4. Against the flattened materials of the day before:
  Classroom 5.17 / 5.60 to 1.44 / 2.38, Barcelona 1.08 / 0.31 to 1.04 / 0.07, Lone Monk
  1.98 / 1.09 to 1.37 / 0.39, Monster 0.57 / 0.97 to 0.76 / 0.56; Italian Flat 1.30 / 0.56,
  Cosmos Laundromat 1.00 / 0.07.
* [ ] openmoonray-anari, what remains is lighting: Classroom is dominated by fireflies from
  emissive meshes (see 5.4), the sunlit courtyard of Lone Monk is overexposed (a sun of
  irradiance 149 from the world next to an emissive sky dome mesh; which of the two is
  not checked). Lights and the world are not sent as graphs to MoonRay
  (`DecayLightFilter` and `ColorRampLightFilter` could carry a Light Falloff).
* [ ] Fallbacks of the MaterialX export: Color Ramp, curves, Object Info.

## 7. Open questions

* Does one MaterialX document serve both VisRTX and Radeon ProRender? Radeon ProRender
  takes Blender's documents as they are (its device adapts them). VisRTX is not built with
  MaterialX yet, see 5.2.
* Radeon ProRender ignores the `colorspace` attribute of image inputs, the type of the ANARI
  array carries the encoding. If VisRTX applies the attribute as well, sRGB images would be
  linearized twice there.
* Cycles nodes without an equivalent in MoonRay (Add Shader of two BSDFs, noise, most Light
  Path outputs) make the material fall back today. Which of them should be approximated
  instead?
* A material that falls back changes its subtype, which costs a full geometry update on
  the device (5.1). Should a fallback inside the graph subtype be possible instead, for
  example the flattened material written as a small graph?

## Development

AI agent harnesses were used for selected development, testing, and optimization tasks
