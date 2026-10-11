# Shaders and materials in the Blender ANARI device

This document describes how Blender shaders reach the ANARI renderers: what the exporter in
blenderphi does with a Cycles shader graph, and how each ANARI device turns the result into
its own shading model. It complements the [README](README.md) and the documents of the platforms
([WINDOWS.md](WINDOWS.md), [LINUX.md](LINUX.md), [MACOS.md](MACOS.md)), which cover building, usage
and test results.

ANARI has no shader graphs. A material is a fixed parameter set (`matte` or
`physicallyBased`), and each parameter is a constant, a geometry attribute, or a sampler
(an image lookup with a linear transform before and after). So the pipeline has two lossy
steps:

1. **Blender side.** The Cycles shader graph is collapsed into one `physicallyBased` (or
   `matte`) material. Only what can be written as "constant, attribute or image, followed by
   a linear transform" survives.
2. **Device side.** Each device maps `physicallyBased` onto the closest thing its renderer
   has, and honors a different subset of the parameters and sampler features.

The Blender side below was read directly from the code. The device side was compiled by
reading each device's source (state of the `mjar/devel` branches, October 2026); it was not
checked parameter by parameter with renders. Where the smoke tests cover something, that is
said.

Three devices no longer need the flattening: cyclesphi-anari, RadeonProRenderANARI and
openmoonray-anari get the shader graph itself, see
[section 5](#5-shader-graphs-as-text). Sections 1 to 4 describe the flattened export, which
all other devices use and which is the fallback for these three.

Contents:

1. [Blender side: the exporter](#1-blender-side-the-exporter)
2. [Device side: summary tables](#2-device-side-summary-tables)
3. [Device side: notes per device](#3-device-side-notes-per-device)
4. [What this means for Blender scenes](#4-what-this-means-for-blender-scenes)
5. [Shader graphs as text](#5-shader-graphs-as-text)

## 1. Blender side: the exporter

The code is in `intern/cycles/device/anari/`:
[material.h](../../intern/cycles/device/anari/material.h),
[material.cpp](../../intern/cycles/device/anari/material.cpp) (`AnariMaterialBuilder`) and
[scene.cpp](../../intern/cycles/device/anari/scene.cpp) (`AnariScene::sync_materials`).

Blender syncs its node trees into Cycles as usual, so the exporter works on the Cycles
`ShaderGraph` (after Cycles' own simplification and constant folding), not on Blender nodes.
No Cycles kernel runs: `ShaderEval` returns early for `DEVICE_ANARI`
(`intern/cycles/integrator/shader_eval.cpp`), so nothing that needs shader evaluation on the
device is computed.

### 1.1 When materials are created

* One ANARI material is created per Cycles `Shader` that is used by a surface geometry.
  Light and world shaders are handled with the lights and the world
  ([section 1.7](#17-lights-world-and-volume-shaders)).
* A material is re-evaluated when its shader is modified or its graph was replaced. Its
  parameters are unset and set again on the same ANARI object.
* The subtype of an ANARI object cannot change. When a shader switches between `matte` and
  `physicallyBased`, the material is released and recreated, and the surfaces that use it
  are rebuilt.
* A mesh, curve or point cloud with several material slots is split into one ANARI surface
  per shader. The shader's pass index is set as the surface `id`.
* Geometry without a shader gets the default material: a diffuse `matte`, like Cycles'
  default surface.

### 1.2 Intermediate representation

The graph is reduced to two small structures:

* **`AnariValue`** drives one material parameter. It is one of:
  * `CONSTANT`: a `float4`.
  * `IMAGE`: an image texture node, the attribute used as texture coordinate, and an input
    transform (`in_transform`).
  * `ATTRIBUTE`: the name of an ANARI attribute.

  Image and attribute values also carry a 4×4 output matrix and an offset
  (`out = M * value + offset`). Every supported node operation is folded into this matrix:
  `apply_linear` scales and offsets the channels, `apply_dot` replaces the value by a
  weighted sum of its channels (a channel pick, luminance or average).
* **`AnariPrincipled`** is the parameter set of the ANARI `physicallyBased` material, each
  entry an `AnariValue`: base color, metallic, roughness, opacity, transmission, specular,
  specular color, clearcoat and its roughness, sheen color and roughness, emissive and the
  normal map. IOR and the iridescence values are plain floats. Flags record whether a BSDF
  or emission was found, and whether the shader is only a Lambertian diffuse BSDF.

### 1.3 Closure nodes

`evaluate_closure` walks from the Surface output and returns an `AnariPrincipled`:

| Cycles node | Result |
|---|---|
| Principled BSDF | Base Color, Metallic, Roughness, IOR, Alpha → `opacity`, Transmission Weight, Specular Tint → `specularColor`, Coat Weight and Coat Roughness → `clearcoat*`, Sheen Tint × Sheen Weight → `sheenColor`, Sheen Roughness, Emission Color × Emission Strength → `emissive`, Thin Film Thickness and IOR → `iridescence*`, tangent-space Normal Map |
| Diffuse, Subsurface Scattering | Color as base color, roughness 1, specular 0; flagged as diffuse-only |
| Translucent, Toon | The same, but not flagged as diffuse-only |
| Glossy, Metallic BSDF | Color, metallic 1, Roughness |
| Glass, Refraction | Color, Roughness, IOR, transmission 1 |
| Sheen | Black base color, Color → `sheenColor`, Roughness |
| Transparent, Ray Portal | Opacity 0 |
| Holdout | Opacity 0, flagged as holdout |
| Emission | Color × Strength → `emissive`, black base color, specular 0. The strength may be computed by other nodes |
| Mix Shader | See below |
| Add Shader | BSDF + emission: the BSDF with the emission added. Otherwise a 50 % mix |
| Any other closure with a Color input | Diffuse material of that color |
| Anything else, or no closure found | Default gray material |

Details:

* **Specular.** Cycles' Specular IOR Level 0.5 is the unmodified Fresnel of the IOR, which
  is a specular weight of 1 in ANARI. The exporter sends `specular = 2 × Specular IOR Level`.
* **Constant-only inputs.** IOR, Sheen Weight, Emission Strength, the thin film values and
  the factor of mix nodes are read as constants. When such an input is linked, its default
  is used.
* **Mix Shader:**
  * One side fully transparent: the other side is kept and the factor becomes its opacity.
    A textured factor becomes an opacity sampler (an alpha mask).
  * Constant factor: every parameter is blended linearly. Two constants give a constant; a
    constant and a texture fold into the texture's output transform. Two textures cannot be
    combined by an ANARI sampler, so the one with the larger weight is used.
  * Textured factor between two opaque shaders: mixed by the average of the factor over the
    surface (see [1.4](#14-value-nodes)). The jacket of Cosmos Laundromat is a fabric with a
    golden decal, mixed by the alpha of the decal image, which is 0 on nearly all of the
    jacket: a 50 % mix made it a pale, half metallic coat.
  * *Is Camera Ray* of a Light Path node as the factor: the shader for the camera is used.
    The other one is a cheaper or clearer stand-in for the rest of the light transport (the
    glass of the lamps in Barbershop is 80 % transparent for it); half of each is neither.
  * A reflection mixed over a transparent shader by a Fresnel or Layer Weight factor (clear
    glass): the factor for a typical direction of view becomes the opacity of the reflection.
  * A factor which evaluates to a constant through other nodes is used like a constant
    one.
  * An input without a shader: the other one is scaled by its weight.
  * A diffuse and a glossy shader with a factor computed from a Fresnel or Layer Weight node
    (directly, or through the math and mix nodes a node group shapes the reflectivity with),
    the way dielectrics were built before the Principled BSDF: the diffuse shader with a specular
    weight of 1 and the roughness of the glossy one. Blending the two would give a half
    metallic material with a washed out color.
* **Normal maps.** Only a Normal Map node in tangent space with a textured Color input is
  exported. The sampler returns the color of the image, with the normal encoded in [0, 1]:
  the devices decode it (`2 * color - 1`), as they do for the normal textures of glTF, which
  the loader of the ANARI SDK passes on without an output transform. (The exporter used to
  decode it with the output transform, which VisRTX, barney, Visionaray, Radeon ProRender and
  cyclesphi-anari then decoded a second time: Lone Monk was five times too bright on
  VisRTX.) Bump nodes and displacement are not exported.
* **No Surface output.** A volume-only shader gets no ANARI surface at all.

### 1.4 Value nodes

`evaluate_output` follows the link of a parameter input and returns an `AnariValue`:

| Cycles node | Result |
|---|---|
| Image Texture, Environment Texture | `IMAGE`. The Alpha output picks the alpha channel |
| Color Attribute | `ATTRIBUTE`: `color` for the default layer, otherwise an attribute slot |
| Attribute | `ATTRIBUTE` in a slot. Fac is the channel average, Alpha picks the alpha channel |
| RGB, Value | Constant |
| Implicit conversions, RGB to BW | Luminance (from colors) or channel average (from vectors) |
| Separate Color | RGB mode: channel pick. Other modes: luminance |
| Invert | Linear, with a constant factor |
| Mix or Math with a node which can't be evaluated on one side | The other side alone, instead of a mix with a default color |
| Mix (color, float, vector, legacy MixRGB) | *Mix*, *Multiply*, *Add* and *Subtract* are folded when one side is constant. The other blend modes are fitted (see below) when one side is constant. Two textures, or a textured factor: see *Averages over the surface* below |
| Math | *Multiply*, *Add*, *Subtract* and *Divide* by a constant are folded. The other operations, and all of them with *Clamp*, are fitted when one operand is constant. Of two textures the second one is its average |
| Color Ramp, RGB Curves, Float Curve, Hue/Saturation/Value, Gamma, Bright/Contrast, Clamp, Map Range | Fitted. Linear cases (Map Range *Linear*, Hue/Saturation with only the value changed) are exact. A Color Ramp of a procedural texture is the average of the ramp over the values of the texture |
| Light Path, *Ray Length* output | Only in the shader of a light, see 1.7 |
| Light Falloff | The strength, see 1.7 |
| Everything else (noise, Voronoi, gradient, sky, ...) | The default value of the parameter |

So a procedural texture does not fail the export: the parameter it drives silently gets
its default, with a debug log line. For the base color the default is only used when the
color can't be baked, see [1.8](#18-baked-base-color).

**Fitted functions.** An ANARI sampler can only apply a linear transform to the color it
reads (`outTransform`, `outOffset`). A node which is not linear in its color is evaluated
with the functions of the Cycles kernel:

* On a constant the result is exact.
* On a texture, `apply_function` reads up to 4096 texels of the image on a regular grid,
  evaluates the node chain for them, and sets the output transform to the least squares fit
  (a 3x4 matrix per sampler). Most textures cover a narrow range of colors, where a curve, a
  gamma or a saturation change is close to linear.
* On an attribute, where the values are not known, the fit is over the unit cube of colors.

The fit replaces the output transform accumulated so far, so chains of such nodes work, in
any order with the linear ones. All other inputs of the node have to be constant. Before
this, these nodes were passed through unchanged: a Hue/Saturation node with *Value* 0.25
left the texture four times too bright.

**Averages over the surface.** A sampler keeps one texture. Where a node combines two, or
mixes by a textured factor, the one which is not kept is replaced by its average, which is
taken where the shader is used and not over the image:

* `AnariScene::surface_attribute` returns the attribute a sampler reads (a UV map, a vertex
  color) at up to 256 triangles of up to 8 meshes which have the shader, four points per
  triangle for meshes of few triangles. The builder looks the image up at these points
  (`AnariMaterialBuilder::surface_values`), with the transform of the texture coordinates
  and the extension of the image. Without the attribute (generated coordinates, box
  projection) it falls back to 4096 texels of the whole image.
* An image is often an atlas of which one shader uses a part (hands, shirt and trousers of
  the man in Cosmos Laundromat share one), and a mask may cover next to nothing of a mesh:
  the average over the image is not the average on the surface.
* **Which texture is kept.** The one whose color varies more over the surface, weighted by
  the factor of the node (`value_statistics`): the red and white stripes painted into the
  vertex colors of the barber pole say more than the grain of the wood they are mixed with.
  Where the variances are not known, the first input, or for *Mix* the one with more weight.
* **Factors.** A textured factor of a Mix node is its average. A factor from a Layer Weight
  or Fresnel node, which tints the rim of an object, is evaluated for the mean cosine of a
  surface seen from all sides (2/3) instead of the default 0.5 of the socket, which mixed
  half of the rim color in everywhere. A factor behind nodes which can't be evaluated is
  what these nodes return for their defaults (the color of a ramp), not 0.5.
* **Color Ramp of a procedural texture.** The color in the middle of a ramp says little
  about a ramp of many stops. The ramp is averaged over the values of the texture: a bell
  curve around 0.5 with a standard deviation of 0.13 for a noise and 0.29 for other
  textures, for Hair Info and for Object Info. The hair of Cosmos Laundromat is a noise
  through a ramp of oranges with one light stop in the middle, on curves, where nothing can
  be baked: it went from beige to orange. Pointiness, which is nearly the same everywhere,
  is not spread.

### 1.5 Texture coordinates and attributes

ANARI geometries have four generic attributes, `attribute0`..`attribute3`.
`AnariAttributeSlots` assigns them:

* `attribute0` is always the default UV map.
* A UV Map node or Attribute node with a name takes the next free slot. A newly assigned
  slot re-exports the geometry.
* The slots are assigned per shader (`AnariShaderSlots`), not for the whole scene: a mesh is
  exported as one ANARI geometry per shader anyway, and a scene has many more attribute
  names than slots. A shader which changes starts with empty slots; its geometry is only
  exported again when the slots differ afterwards.
* With all four in use, a warning is logged. A texture coordinate then falls back to the
  default UV map, and an attribute used as a value falls back to the parameter's default.
* A device can declare more slots as `vertex.attribute4` and following on the `triangle`
  geometry (up to `attribute7`, found with `anariGetParameterInfo`). cyclesphi-anari does,
  for the materials it gets as graphs: the wall of the Barbershop scene reads five
  attributes.
* The default color attribute is exported as ANARI vertex `color`.

Meshes are exported with `vertex.normal`, `vertex.tangent` (for normal maps), `vertex.color`
and `vertex.attribute0..3`. Each triangle gets its own vertices, so per-corner UVs and
split normals work without the face-varying extension.

Curves (hair) are exported with `vertex.radius` and the attribute slots of their shader as
`vertex.attribute0..`, one value per key: an attribute of the curve is repeated for its
keys. That carries the texture coordinates of the surface a hair grows on (`attribute0` for
the UV map of particle hair, a named slot for `surface_uv_coordinate` of the curves of
geometry nodes) and, for devices which take the graph as Cycles XML, the *Intercept*,
*Length* and *Random* outputs of the Hair Info node: Cycles keeps them as attributes of the
curves (`curve_intercept`, `curve_length`, `curve_random`), the writer links their slots in
place of the node. The fur of the Blender 5.2 splash scene takes its color from images
looked up with `surface_uv_coordinate`, varied by *Random* and *Intercept*; without the
attributes it was white on cyclesphi-anari (1.94 / 1.06, with them 0.92 / 0.16).

For an image texture, `evaluate_texture_coordinate` walks up the Vector input:

* **Mapping nodes** are accumulated into `in_transform` (Point, Texture, Vector and Normal
  types). The image node's own texture mapping is applied as well.
* **UV Map / Attribute node:** the attribute's slot.
* **Texture Coordinate node:** *UV* is `attribute0`. *Generated* takes an attribute slot with
  the coordinates Cycles has for the mesh, normalized to its texture space (when no slot is
  free: `objectPosition`). *Object* is `objectPosition`, and *Normal* is `objectNormal`.
* Anything else falls back to the default UV map.

### 1.6 Creating the ANARI objects

**Subtype.** `physicallyBased` is used when the device has it, otherwise `matte`. A shader
that is only a Lambertian diffuse BSDF, without emission and without a normal map, is
exported as `matte` when the device has that subtype. All back-ends evaluate `matte`
exactly, while their `physicallyBased` models differ (several ignore `specular = 0`).

**Parameters.** `set_material` sets, for `physicallyBased`:

| ANARI parameter | Source | Notes |
|---|---|---|
| `baseColor` | value | |
| `metallic`, `roughness` | value | |
| `opacity` | value | `alphaMode = "blend"` is set when the opacity is textured or below 1 |
| `ior` | float | |
| `transmission` | value | |
| `thickness` | 1.0 | Only when the material transmits. ANARI's default thickness of 0 means thin-walled glass without refraction; Cycles' transmission is refractive |
| `specular`, `specularColor` | value | |
| `clearcoat`, `clearcoatRoughness` | value | |
| `sheenColor`, `sheenRoughness` | value | |
| `emissive` | value | Only when the shader emits |
| `iridescence`, `iridescenceThickness`, `iridescenceIor` | float | Only with a thin film |
| `normal` | sampler | Only with a normal map |

For `matte` only `color`, `opacity` and `alphaMode` are set.

**Values.** `set_value` writes one `AnariValue`:

* A constant is set as `FLOAT32`, `FLOAT32_VEC3` or `FLOAT32_VEC4`.
* An attribute without an output transform is set as its name string (`"color"`,
  `"attribute1"`, ...).
* An attribute with an output transform becomes a `transform` sampler.
* An image becomes an `image2D` sampler with:
  * `inAttribute`: the texture coordinate attribute.
  * `inTransform`: set only when it is not the identity, because not all devices support
    it.
  * `outTransform` and `outOffset`: set only when not the identity.
  * `wrapMode1/2` from the node's extension (`repeat`, `clampToEdge`, `mirrorRepeat`,
    `clampToBorder`) and `filter` (`nearest` for *Closest*, otherwise `linear`).
* When the sampler cannot be created, the parameter is left unset and the device uses its
  default.

**Images.** `AnariImageCache` creates the ANARI arrays from the pixels the Cycles image
manager has loaded for the device. They are converted to one of three formats all back-ends
support: 8-bit RGBA, 8-bit sRGB RGBA (byte images in the sRGB color space) or 32-bit float
RGBA. Arrays are shared between samplers, recreated when the image changes, and released
when no material uses them any more. UDIM tiles and images that failed to load are not
available.

### 1.7 Lights, world and volume shaders

* **Lights.** The light's node tree is evaluated with the same code. A constant emission
  multiplies the light's strength; a textured emission is ignored. On a device without the
  `quad` light, an area light becomes a quad with a black emissive material.
  * **Own falloff.** ANARI lights fall off with the inverse square of the distance. A light
    shader can change that: a Light Falloff node (*Linear*, *Constant*, *Smooth*), or a mix
    factor or strength computed from Light Path *Ray Length*. Such a shader is evaluated for
    up to 16 distances from the light to the objects of the scene (for area and spot lights
    the objects in front of them), and the results are averaged with the weight
    1 / distance². The scene then receives about the same light in total, but distributed
    with the inverse square falloff. A lamp with *Smooth* set to avoid a hot spot on the
    ceiling above it still makes that hot spot.
  * **The falloff as a ramp.** A device which declares `falloff.distance` on the `point`
    light (openmoonray-anari) gets the emission of such a shader over the distance instead
    of the average: the shader is evaluated from the light to the far end of the scene, at
    17 distances and up to 31 more where a line between two of them misses the emission by
    more than half a percent, and the result is sent as `falloff.distance` and
    `falloff.color`, relative to the largest emission, which multiplies the strength.
    MoonRay applies it as a `ColorRampLightFilter` over the distance to the sampled point of
    the light, which is the ray length of Cycles. Approximate: MoonRay's ramp has ten
    control points (the device keeps the ten which lose least), and a disk light is a
    rectangle of the same area.
  * A Mix Shader of an emission with an empty input scales the emission by the factor.
  * Settings of a light which ANARI does not have are sent to a device which declares the
    parameter on the `point` light (all light subtypes then take it): `castShadow` (BOOL),
    `maxBounce` (INT32, *Max Bounces* of the light: with 0 it only lights what the camera
    sees directly) and the ray visibility of the light object as `visibility.diffuse`,
    `.glossy`, `.transmission` and `.volumeScatter` (a light hidden from glossy rays lights
    the room without showing up in the mirrors). cyclesphi-anari has all of them. Elsewhere
    a light which casts no shadow is shadowed, a light which diffuse rays don't see is left
    out, and the others light everything.
* **World.** `evaluate_background` looks for the Background node, through mix and add
  shaders.
  * An Environment Texture becomes the image of the `hdri` light, with its mapping as the
    rotation.
  * A constant color is used as is.
  * Anything else (sky texture, procedurals, a mix of backgrounds, a linked strength) is
    **baked**: the Cycles CPU kernel evaluates the world shader for every direction of an
    equirectangular image (`SHADER_EVAL_BACKGROUND`), 2048x1024 for final renders and
    1024x512 in the viewport, with 2x2 samples per pixel. That takes about 0.3 s for the
    larger size and is repeated only when the world shader changes. The image becomes the
    `hdri` light.
  * **Backdrop.** What the camera sees of the world is not always what lights the scene: the
    shader can tell camera rays apart with the Light Path node (a darker or blurred sky
    behind the subject), or the world is hidden from diffuse rays and only a backdrop. An
    `hdri` light can't be both. In these cases the world is evaluated a second time as
    camera rays see it, into an equirectangular map which stays in Blender. For every view
    it is resampled into an image of the size of the frame (at most 960 pixels wide in the
    viewport), which is the `background` of the renderer (`KHR_RENDERER_BACKGROUND_IMAGE`);
    the `hdri` light is then not `visible`. The second evaluation needed a switch in the
    Cycles kernel (`kernel/bake/bake.h`: a negative width in the input selects camera
    rays). Panoramic cameras and devices without the extension show the light as before.
  * **Sun of a sky texture.** The sun disc is too small and too bright for an environment
    image; Cycles samples it on its own and the kernel leaves it out of the baked image.
    The exporter integrates the world shader over the sun disc, with the sun minus without
    it, and adds a `directional` light with that irradiance and the angular diameter of the
    sun. Everything the shader does to the sky color applies to the sun as well.
* **Shader evaluation.** For the two points above, and for true displacement, the ANARI
  device of Cycles is derived from the CPU device: the scene data is in host memory and the
  CPU kernels can evaluate shaders. Only rendering is done by the ANARI device.
* **Volumes.** `evaluate_volume` reads Principled Volume, Volume Scatter, Volume Absorption
  and Emission nodes, combined through mix and add shaders, as constants only. The result
  is a `principled` volume on a device that has that subtype (cyclesphi-anari) and a
  `transferFunction1D` volume with a constant color elsewhere. See the
  [README](README.md#5-known-differences-and-limitations).

### 1.8 Baked base color

A sampler can't evaluate a procedural texture, a mix of two textures or a mix with a
textured factor. The flattened material then has a base color which is a constant standing
in for something else (the flag `base_color_lossy`). For such shaders the base color is
evaluated by the Cycles kernel instead, on the surface of every mesh which uses the shader:

* **Which shaders.** Flattened materials only, whose base color came out as a constant
  although nodes had to be left out. A base color which kept its image texture is not
  baked: the texture has more detail than a baked image. The shader has to be used by
  meshes only and to have bump displacement.
* **How the kernel evaluates it.** `ShaderEval` of Cycles can evaluate the displacement of a
  shader at a point (primitive and barycentric coordinates) of a mesh. Before the scene
  update compiles the shaders, `anari_link_base_color_bake` links the base color of the
  closures to the displacement output: the Base Color of a Principled BSDF, the Color of
  the other BSDFs, mixed like the Mix Shaders mix them (with their factors, also textured
  ones). With bump displacement the output does not move the mesh. Cycles adds bump from
  it to its own copy of the shader, which is not exported.
* **Where the color goes.** For a mesh of few, large triangles into an image with one
  square cell of texels per triangle (`bake_base_color_atlas`): the corners of the triangle
  are one texel inside of its cell, the texels outside of the triangle repeat its edge, and
  the texture coordinates are in an attribute slot of their own. The surface gets its own
  material, the flattened one with this image as base color. For meshes of small triangles
  the color is evaluated at the corners and sent as `vertex.color`, with the base color of
  the material set to the `color` attribute.
* **Resolution.** The diagonal of the scene is 4096 texels. A cell is as large as the
  longest edge of the triangles needs, between 6 and 256 texels, and an image has at most
  one million texels; below 6 texels vertex colors are used.
* **Cost.** About 0.1 s per million texels on the CPU. A change of the shader bakes again
  on all meshes which use it.
* **Emission.** Of a shader which only emits, the emission (color times strength) is baked
  the same way, into a float image or float vertex colors. The softboxes of Monster emit
  a gradient times ten, which flattened to a constant of a quarter of that.
* **Mix factor.** When the surface is a Mix Shader of two shaders with constant parameters
  and a factor which is not constant (a procedural mask, a texture), the factor is baked
  as a mask into an 8-bit linear image. Every parameter of the material which differs
  between the two shaders is a sampler of that image with its own output transform,
  `a + (b - a) * mask` (`AnariPrincipled::mix_masked`): base color, emission, roughness,
  metallic, opacity and the others are all exact, for a diffuse shader mixed with an
  emission as well as for two different BSDFs. Not for a cutout by the alpha of an image,
  where the image itself stays the mask at its full resolution.
* **View dependence.** The kernel evaluates the displacement without a direction of view,
  where Layer Weight and Fresnel nodes return what they are at grazing angles: the rim color
  everywhere. Before the color is linked, the inputs these nodes feed behind it are set to
  their value for a typical direction of view, a cosine of 2/3 (`bake_fix_view`). The dead
  tree of Cosmos Laundromat, whose color is noise, pointiness and a Fresnel tint, is baked
  that way; it used to be a light gray constant. Nothing is baked which depends on the path
  or the camera (Light Path, the incoming direction, camera coordinates). Of a diffuse and a
  glossy shader mixed by a view dependent factor the color of the diffuse one is baked. The
  baked color is the same from all sides: nothing is baked again when the camera moves.
* **Average of a textured base color.** A base color which kept its image texture, but
  with nodes left out (the texture multiplied by a ramp over the pointiness, added to a
  noise, ...), keeps the sampler: its colors can be far off, even above 1. The kernel
  evaluates the base color at some points of the meshes which have the shader, and the
  output transform of the sampler is scaled so that its average at the same points is that
  color (`calibrate_base_color`, the points of *Averages over the surface* in
  [1.4](#14-value-nodes)). The detail of the texture stays, the brightness and the tint of
  the surface are those of Cycles. The scale keeps black as it is; a sampler whose average
  is next to nothing, negative, or more than four times too dark gets the difference added
  instead, as a ratio of such numbers would blow the texture up. The average used to be
  taken over the texels of the whole image: the hands of the man in Cosmos Laundromat came
  out 3.5 times too bright and red, because most of their atlas is the dark trousers.
* **Limits.** One value per shader: the mix factor, the base color, or the emission of a
  shader without a BSDF. Roughness or metallic from procedural textures stay constants.

`test_shader_graph` on devices without graphs (relative difference to Cycles, before and
with baking):

| Graph | barney | VisRTX | OSPRay |
|---|---|---|---|
| Procedural checker | 2.50 → 0.04 | 0.04 | 0.09 |
| Noise through a color ramp | 0.82 → 0.04 | 0.04 | 0.21 |
| Mix Shader with a procedural mask | 0.55 → 0.03 | 0.03 | 0.03 |

Monster on barney and VisRTX went from 0.32 / 0.81 to 0.76 / 0.59 with the wall and the
softboxes baked.

## 2. Device side: summary tables

All ten devices implement `matte` and `physicallyBased`. The Blender exporter only uses
these two subtypes.

### 2.1 What `physicallyBased` becomes

| Device | Native model | `matte` |
|---|---|---|
| cyclesphi-anari | A Cycles `ShaderGraph` with one Principled BSDF node, built per material | Principled BSDF with roughness 1 |
| barney | A fixed BSDF struct (`packedBSDF::PhysicallyBased`), a port of VisRTX's | Lambertian |
| VisRTX | Hand-written OptiX callable shader: diffuse, GGX, clearcoat, sheen, iridescence, refraction | Lambertian |
| mitsuba-anari | A stack of Mitsuba BSDFs: `roughplastic` + `diffuse`, blended with `roughdielectric` and `principled`, in `mask` and `twosided` | `diffuse` in `twosided` |
| openmoonray-anari | MoonRay `UsdPreviewSurface` | `UsdPreviewSurface` with roughness 1 |
| helide | No BSDF: base color shaded by the view angle and ambient light | The same |
| anari-visionaray | Burley diffuse, GGX, clearcoat, Charlie sheen, GGX refraction | Lambertian |
| anari-ospray | OSPRay `principled` | OSPRay `obj` (`kd`, `d`) |
| RadeonProRenderANARI | RPR Uber material (`UBERV2`) | Uber material, diffuse layer only |
| Photon | Disney-style BSDF: diffuse, GGX, sheen, clearcoat, delta refraction | The same with only the diffuse lobe |

### 2.2 Parameters the exporter sends

**M**: honored, and can be driven by a sampler. **C**: honored as a constant only.
**–**: ignored.

| Parameter | cycles | barney | visrtx | mitsuba | moonray | helide | visionaray | ospray | rpr | photon |
|---|---|---|---|---|---|---|---|---|---|---|
| `baseColor` | M | M | M | M | M | M | M | M | M | M |
| `metallic` | M | M | M | C | M | – | M | M | M | M |
| `roughness` | M | M | M | C | M | – | M | M | M | M |
| `opacity` | M | M | M | C | M ⁶ | – ¹ | M | M | M | M |
| `ior` | M | C | C | C | C | – | C | M | C | C |
| `transmission` | M | M | M | C | C ² | – | M | M | M | M |
| `thickness` | C ³ | M | M | – | – | – | – | C ³ | – | C ³ |
| `specular` | M | M | M | C | – | – | – | M | M | C |
| `specularColor` | M | M | M | C | – | – | – | – | M | – |
| `clearcoat` | M | M | M | – | M | – | M | M | M | C |
| `clearcoatRoughness` | M | M | M | – | M | – | M | M | M | C |
| `sheenColor` | M ⁴ | M | M | – | – | – | M | M ⁴ | M | – |
| `sheenRoughness` | M | M | M | – | – | – | M | M | – | – |
| `emissive` | M | M | M | C ⁵ | M | – | M | M | M | M |
| `iridescence*` | C | M | M | – | – | – | – | – | – | – |
| `normal` (sampler) | M | M | M | – | M | – | M | M | M | – |

1. helide uses the opacity only in its `opacityHeatmap` render mode.
2. MoonRay folds the transmission into the opacity: untinted, without refraction of its own.
3. Used only as a switch between thin-walled and solid.
4. The sheen weight is 0 or 1, from whether the color is black.
5. Mitsuba creates an area emitter for the shape.
6. MoonRay renders a textured opacity as a cutout: at `alphaCutoff` in the `mask` mode, at
   0.5 in the `blend` mode.

### 2.3 Attributes and samplers

| Device | Attribute names | Sampler subtypes | `inTransform` | `outTransform` / `outOffset` |
|---|---|---|---|---|
| cyclesphi-anari | `color`, `attribute0..3`, `primitiveId`; samplers also take world/object position and normal | `image1D/2D/3D`, `transform`, `primitive` | yes (shader nodes) | yes |
| barney | `color`, `attribute0..3`, `worldPosition`, `objectPosition` | `image1D/2D/3D`, `transform`, `primitive` | yes | yes (not for `primitive`) |
| VisRTX | `color`, `attribute0..3`, world/object position and normal (object = world) | `image1D/2D/3D`, `compressedImage2D`, `transform`, `primitive` | yes | yes |
| mitsuba-anari | `color`, `attribute0..3`, for the base color only | `image1D/2D/3D` | yes (applied to the mesh UVs) | yes (baked into the texels) |
| openmoonray-anari | `color`, `attribute0..3` | `image1D/2D`, `transform` (per-channel scale of an attribute, colors only) | scale, offset, rotation | yes (ImageMap gain and offset; a copy of the texels for matrices which mix channels) |
| helide | `color`, `attribute0..3`, world/object position and normal (object = world) | `image1D/2D/3D`, `transform`, `primitive` | yes | yes |
| anari-visionaray | `color`, `attribute0..3`, world/object position and normal | `image1D/2D/3D`, `transform`, `primitive`, `volume` | yes | yes |
| anari-ospray | `color`, `attribute0..3`, for the base color only | `image1D/2D` | yes (2D part) | yes (baked into the texels) |
| RadeonProRenderANARI | `color`, `attribute0..3`, world/object position and normal | `image1D/2D/3D`, `transform` | yes (node graph) | yes (node graph) |
| Photon | `color`, `attribute0..3`, world position and normal (object = world) | `image1D/2D/3D`, `transform` | yes | yes |

## 3. Device side: notes per device

Paths are relative to the workspace (`F:\work\anari`).

### cyclesphi-anari

`cyclesphi-anari/device/material/`, `device/sampler/`

* Every ANARI material builds a Cycles `ShaderGraph`: attribute nodes for the vertex color
  and `attribute0..3`, a Principled BSDF node, and for each sampler an Image Texture node
  with math nodes for the transforms and wrap modes.
* Extensions Blender uses, each found through the parameter it declares: ray visibility
  and `objectRandom` on instances; `visibility.*`, `maxBounce` and `castShadow` on lights;
  `backgroundVisibility.*`, the path length limits and the clamps on the renderer;
  `attribute4..7` on geometry (`CYCLES_GEOMETRY_ATTRIBUTES`, read by `cyclesXml` graphs
  only).
* `specular` is multiplied by 0.5 into Specular IOR Level, the inverse of what the Blender
  exporter does, so the value round-trips.
* `opacity`, `alphaMode` and `alphaCutoff` are combined with the color alpha by math nodes.
* `thickness` with `attenuationColor` / `attenuationDistance` adds an Absorption Volume to
  the Volume output.
* Extensions on `physicallyBased`: `subsurface*` and `emissiveStrength`.
* Further material subtypes, which Blender does not use:
  * `osl`: OSL source text (`source`) or compiled bytecode (`bytecode`); ANARI parameters
    are forwarded to the shader inputs of the same name. Needs OSL and a CPU or OptiX
    device.
  * `hair`: the Principled Hair BSDF.
  * `toon`: the Toon BSDF.
* With the device parameter `cyclesphi.xmlTemplates`, a `physicallyBased.xml` or
  `matte.xml` Cycles XML shader replaces the built graph.

### barney

`barney/barney/anari/Material.cpp`, `barney/barney/native/material/`,
`barney/barney/native/packedBSDF/`

* `physicallyBased` evaluates the same lobes as VisRTX and forwards all parameters.
* Coverage is `baseColor.a × opacity`; `blend` is stochastic.
* `thickness` and the attenuation parameters are a Beer-Lambert tint, not a traced volume.
* A `normal` sampler is a tangent-space map; a `normal` attribute is a world-space normal.
* An attribute name outside the table above (`worldNormal`, `objectNormal`) throws an
  exception in `parseAttribute` (`barney/native/render/Sampler.cpp`).
* Further subtypes, not used by Blender: `nvisii` (a Disney-style parameter set) and
  `glass` (`ior`, `attenuationColor`).

### VisRTX

`VisRTX/devices/rtx/device/material/`, `devices/rtx/device/gpu/evalMaterialParameters.h`

* The native `PBR` material honors every parameter of the specification.
* `matte` is a Lambertian with a cosine sampled bounce (it had no continuation ray and was
* Further subtypes, not used by Blender and only in builds with MDL:
  * `mdl`: an MDL module, MDLE file or inline MDL code (`source`, `sourceType`,
    `materialName`). Other ANARI parameters are matched by name to the MDL arguments;
    texture arguments take a sampler or a file path.
  * `materialx`: a `.mtlx` file or inline document, transcoded to MDL.
  * `physicallyBasedMDL`: `physicallyBased` implemented in MDL. It replaces the native one
    only with `VISRTX_USE_MDL_FOR_PHYSICALLY_BASED` (off by default).
* The differences of the `default` (interactive) renderer are a renderer matter, see the
  [README](README.md#5-known-differences-and-limitations).

### mitsuba-anari

`mitsuba-anari/src/material/Material.cpp`, `src/sampler/Sampler.cpp`,
`src/scene/MitsubaSceneBuilder.cpp`

* `physicallyBased` is assembled from Mitsuba plugins:
  * Dielectric: `plastic` / `roughplastic` plus a `diffuse` scaled by 1 - F0. With
    `specular = 0` it is `diffuse` alone.
  * `transmission > 0`: blended with `dielectric` / `roughdielectric`.
  * `metallic > 0`: blended with `principled` at metallic 1.
  * `opacity < 1`: wrapped in `mask`.
  * Non-transmissive lobes are `twosided`.
* Only the base color can be textured. A sampler or attribute on `metallic`, `roughness`,
  `opacity` or `transmission` warns and the constant is used.
* `normal`, `clearcoat`, `sheenColor` and `iridescence` warn and are ignored.
* Bitmap textures are Mitsuba `bitmap` textures; vertex colors are `mesh_attribute`
  textures. The input transform is applied to the mesh UVs on the CPU.
* `transform` and `primitive` samplers are unsupported and fall back to the constant.

### openmoonray-anari

`openmoonray-anari/src/anari_library_moonray/material/`, `sampler/`, `backend/`

* From Blender the device normally gets `rdla` materials: networks of MoonRay and moonshine
  classes which Blender writes ([section 5.5](#55-rdla-graph_rdlacpp)). What follows is
  the flattened path, used with *Materials: Flattened*.
* For those networks the device exports per primitive `anari_attribute0..3`, the tangent
  (`anari_tangent`, `anari_tangent_sign`), the alpha of a four component color
  (`anari_color_alpha`) and per instance `anari_object_random`, `anari_object_color` and
  `anari_object_index`. A mesh placed by a mirroring transform gets a left-handed
  `orientation` (it rendered black before).
* Both standard subtypes are a MoonRay `UsdPreviewSurface`. It has no specular weight or
  tint, no sheen and no refraction parameter.
* A sampler is an `ImageMap` bound to the attribute: base color, emission, roughness,
  metallic, clearcoat and its roughness, and the normal. The `color` attribute is an
  `AttributeMap` on the primitive attribute `Cd`.
* A textured opacity is a cutout (presence): exact in the `mask` mode, cut at 0.5 in the
  `blend` mode. Presence shadows are switched on while a cutout material is in the scene.
* Image pixels are served from memory. `OPENMOONRAY_ANARI_TEXTURE_FILES=1` writes scratch
  texture files with OpenImageIO's `make_texture` instead.
* The input transform is decomposed into the `ImageMap` scale, offset and rotation; shear
  is approximated.
* The output transform is the `gain` and `offset_adjust` of the `ImageMap` when it scales
  the channels on their own, which costs nothing. A matrix which mixes channels is baked
  into a copy of the texels, shared by the samplers with the same image and matrix (85 ms
  and 32 MB for 2048², 300 ms and 128 MB for 4096²).
* The renderer takes the path length limits and clamps of section 5.1. MoonRay has one
  clamp value: `clampIndirect` sets it, and `clampDirect` only decides whether it also
  applies at the first hit.
* Unsupported parameters warn.

### helide

`ANARI-SDK/src/devices/helide/material/`, `sampler/`

* `physicallyBased` exists but reads only `baseColor` and `opacity`. There is no BSDF.
* Its samplers are complete, which makes helide useful for checking textures and
  attributes, not shading.

### anari-visionaray

`anari-visionaray/scene/surface/material/`, `anari-visionaray/renderer/common.h`

* `specular` and `specularColor` are not read: the specular lobe always has its full
  weight.
* `ior` is a constant; `normal` must be a sampler.
* It also reads the anisotropy parameters (`anisotropyStrength`, `anisotropyDirection`,
  `anisotropyRotation`), which the exporter does not send.

### anari-ospray

`anari-ospray/scene/surface/material/`, `scene/surface/material/sampler/`

* Each ANARI parameter maps to an OSPRay `principled` parameter and its `map_*` texture
  slot.
* Attributes are accepted only for the base color, as OSPRay vertex or primitive colors.
* All texture maps of a material share one texture coordinate stream. Different
  `inAttribute`s on the samplers of one material warn.
* `alphaMode = "opaque"` forces the opacity to 1; `alphaCutoff` is not read.
* `thickness <= 0` sets OSPRay's `thin`, which is why the exporter sets a thickness.
* `specularColor` and iridescence are not read.
* For OSPRay 3.2 the device remaps `specular`, see the
  [README](README.md#6-changes-made-to-the-anari-devices).

### RadeonProRenderANARI

`RadeonProRenderANARI/material/`, `sampler/`

* The Uber material's reflection layer is in IOR mode when `metallic` is a constant 0, and
  in metalness mode otherwise.
* Refraction uses the base color; coating has a fixed IOR of 1.5.
* Every parameter accepts a constant, an attribute or a sampler. Samplers and their
  transforms are RPR material node graphs (`IMAGE_TEXTURE` and arithmetic nodes).
* `image3D` is a 2D slice atlas. `nearest` filtering is emulated by snapping the UVs.
* `sheenRoughness`, `thickness`, the attenuation parameters and iridescence are not used.
* The renderer has `maxBounce`, `maxDiffuseBounce` and `clampIndirect` of section 5.1.
  Northstar ignores RPR's limits for glossy, refraction and shadow depth, so those are not
  offered. A transparent surface counts as a bounce of the total limit, unlike in Cycles.
  The clamp applies per color channel to light gathered after the first hit, never to
  direct light.

### Photon

`photon/src/photon/anari/SceneObjects.cpp`, `RenderBridge.cpp`,
`photon/include/photon/pt/disney_bsdf.h`

* One material class serves both subtypes. An unknown subtype warns and becomes a default
  matte.
* The ANARI device hands plain structs (`bridge::MaterialData`) to the Kokkos path tracer,
  which resolves attributes and samplers per hit.
* `roughness` is squared to the GGX alpha; `specular` is scaled by 0.5.
* `normal` warns and is ignored. `specularColor` and the sheen parameters are not read.

## 4. What this means for Blender scenes

These follow from combining sections 1 and 2. Only the first point is covered by the smoke
tests.

* **Base color textures work everywhere.** An image texture on the base color, with a
  mapping node, is honored by all ten devices (`test_texture`, `test_texture_mapping`).
* **Other textured inputs depend on the device.** Roughness, metallic, alpha and
  transmission maps fall back to a constant on Mitsuba. MoonRay takes textures for the base
  color, emission, roughness, metallic, clearcoat, the normal and the opacity (as a cutout),
  and constants for the IOR and the transmission.
* **Node math needs the output transform.** Invert, Multiply or Add on a texture, a Color
  Ramp, the fitted color nodes and the alpha channel of an image are all expressed through
  `outTransform` / `outOffset`. The same operations on an attribute (a vertex color) need
  the `transform` sampler, which Mitsuba and OSPRay do not have; MoonRay's only scales an
  attribute per channel.
* **Only the default UV map is portable.** Additional UV maps and named attributes use
  `attribute1..3`; MoonRay only accepts `attribute0` for textures.
* **Generated and Object coordinates are an approximation.** They are sent as
  `objectPosition` on a 2D image sampler, without the bounding box normalization of
  Cycles. Several devices return the world-space position for it, and MoonRay, OSPRay and
  Mitsuba do not accept it for a 2D image. The *Normal* output is sent as `objectNormal`,
  which barney rejects with an exception.
* **Normal maps.** `test_normal_map` (stripes leaning to both sides, lit from one side;
  relative difference to Cycles): cyclesphi-anari 0.001, MoonRay 0.05, Radeon ProRender
  0.05, VisRTX 0.14. barney and OSPRay darken the stripes which face away but do not
  brighten the others (0.43 and 0.39). Visionaray and Photon render the plane flat (0.70),
  Mitsuba ignores the parameter.
* **Coat, sheen and thin film** are rendered where table 2.2 says so; elsewhere the
  material renders without that lobe.
* **Glass color on MoonRay is lost**, because the transmission becomes plain opacity.
* **Procedural textures are lost in the flattened material.** They only render on the
  devices which get the graph as text, see the next section.

## 5. Shader graphs as text

Devices which can evaluate a shader graph get it as a text in the `source` parameter of a
material subtype made for it. The plan, the protocol and the open work are in
[TODO.md](TODO.md); this section describes what works.

| Device | Material subtype | Text | Written from |
|---|---|---|---|
| cyclesphi-anari | `cyclesXml` | Cycles shader graph as XML | the Cycles graph |
| RadeonProRenderANARI | `materialx` | MaterialX 1.39 document | the Blender node tree |
| openmoonray-anari | `rdla` | MoonRay scene objects (RDLA) | the Cycles graph |

### 5.1 Selection and fallback

* The exporter asks the device for its material subtypes and uses the graph subtype the
  device has. The render setting *Materials* (or the device parameter
  `materials=physicallyBased`) switches the graphs off.
* A material whose graph cannot be written falls back to the flattened material of
  sections 1 to 4. The other materials of the scene are not affected.
* On cyclesphi-anari the shaders of lights and of the world are sent as graphs too (device
  extension `CYCLES_SHADER_XML`: `source` on a light, `backgroundSource` on the renderer).
  The light keeps its strength from the Blender light, the graph multiplies it. A sky
  texture, a mixed background or a light which fades its emission with nodes only render
  this way. On the other devices world and light shaders are exported as before
  ([section 1.7](#17-lights-world-and-volume-shaders)), and so are the shaders of volume
  objects everywhere.
* With the world graph the ray visibility of the world is sent as `backgroundVisibility.diffuse`,
  `.glossy`, `.transmission` and `.volumeScatter` on the renderer: a world which only the
  camera sees does not light the scene. Without graphs such a world gives no environment
  light.
* The ray visibility of objects (camera, diffuse, glossy, transmission, volume scatter,
  shadow) is sent as six BOOL parameters `visibility.camera`, `.diffuse`, `.glossy`,
  `.transmission`, `.volumeScatter` and `.shadow` on the `transform` instance. Standard ANARI
  has no such flags: the exporter sends them to a device which declares the parameter
  (`anariGetParameterInfo` for `visibility.camera`), which cyclesphi-anari,
  openmoonray-anari and RadeonProRenderANARI do. This has nothing to do with the graph
  export, it also applies with flattened materials.
* The Object Info node needs the data of the object: its *Random* number is sent as the
  instance parameter `objectRandom` (FLOAT32) to devices which declare it, its *Color* and
  *Alpha* are the uniform `color` of the instance, its *Object Index* the `id`.
  cyclesphi-anari sets them on its objects: the trees of Barcelona, colored by the random
  number, went from green to the orange of Cycles (0.96 / 0.04 to 1.00 / 0.01).
* On the other devices an object is in all rays or in none. An object hidden from camera
  rays or from shadow rays is left out, unless one of its materials emits light: a window
  pane or a blind which casts no shadow would otherwise keep the sun out of the room, and a
  hidden object would show up in the image. An object which only casts no shadow stays when
  it is small (under 4 % of the size of the scene, 12 % when it emits) and no light is
  inside its bounds: the shadow it casts is the one of any small object. The globes of the
  wall lamps of Barbershop are like that, they glow by themselves; without them the lamps
  had neither their globe nor their light. Emissive ones stay because the scene is usually
  lit by them:
  * A mesh of up to 64 triangles with one constant emission becomes `quad` lights (two
    triangles of a parallelogram are one quad, a single triangle is the parallelogram of
    twice its area with half the radiance). A light shadows nothing and is hidden from the
    camera with its `visible` parameter. This is the emissive plane in a window or next to
    the camera which many interior scenes are lit with.
  * Other emissive objects (a textured backdrop) stay geometry.
  * A small object which casts no shadow and has a light inside (the globes of the ceiling
    lamps of Classroom, each around a point light) stays with half of its opacity and twice
    its emission, and the lights inside are made twice as strong
    (`update_light_shells`). Every path from the light crosses the globe once, so the scene
    gets the light it gets in Cycles, and the camera sees the globe as bright as it is
    there. Only when no other object uses the shader. Classroom on barney: 0.65 / 0.63 to
    0.84 / 0.48.
* The path length limits and the clamps of the Cycles integrator are sent to a device whose
  renderer declares them: `maxBounce`, `maxDiffuseBounce`, `maxGlossyBounce`,
  `maxTransmissionBounce`, `maxVolumeBounce`, `maxTransparencyBounce` (INT32) and
  `clampDirect`, `clampIndirect` (FLOAT32, 0 for none). They are the names of the
  cyclesphi-anari renderer. Every device also gets the standard `maxRayDepth` with the total
  limit. A device without the limits renders interiors brighter than Cycles: Classroom is
  set up with two diffuse bounces and clamps of 5.5 and 3.0.
* *Fast GI* (method *Replace*) ends the paths after its number of bounces and looks up
  ambient occlusion of the world instead, which is no light at all inside a room. Only
  cyclesphi-anari has it. The other devices get the nearest limits: `maxDiffuseBounce` at
  most the Fast GI bounces minus one, `maxGlossyBounce` at most the Fast GI bounces. Italian
  Flat renders with 6 diffuse bounces but Fast GI after 2.

### 5.2 Images and attributes

* Images are never written to disk. A graph refers to an image by the name of a material
  parameter, which holds an `image2D` sampler with the pixels Cycles loaded. One ANARI array
  is shared by all materials that use an image.
* Blender attribute names do not exist on ANARI geometry. The exporter rewrites them to the
  attribute slots (`attribute0` is the default UV map, named UV maps and attributes take
  `attribute1`..`3`, the default color attribute is `color`). Generated texture coordinates
  take a slot too, so procedural textures get the same coordinates as in Blender.
* What Cycles computes for a mesh is not on the device either and takes a slot as well:
  *Pointiness* and *Random Per Island* of the Geometry node. Without the pointiness the
  walls and the ceiling of Barbershop were 1.8 times too bright (it selects their color in
  a ramp).
* cyclesphi-anari has eight slots instead of four
  ([section 1.5](#15-texture-coordinates-and-attributes)).

### 5.3 Cycles XML (`graph_xml.cpp`)

* The whole surface graph is written: every node, socket value and link, also the volume
  and displacement outputs of the material. A material with only a volume output (fog in a
  box) is exported with its mesh, the device renders the volume inside of it.
* The graph is written before Cycles finalizes it for its own kernels, and the copies Cycles
  makes for bump mapping are left out: the device's Cycles finalizes the graph itself.
* Not exported as graph: materials with OSL script nodes or tiled (UDIM) images.
* The device always uses bump mapping for the displacement output.
* The device has its own copy of Cycles. A node or socket it does not know is reported: an
  unknown socket is dropped, an unknown node gives a gray material.

### 5.4 MaterialX (`graph_materialx.cpp`, `NOD_shader_materialx.hh`)

* The document comes from Blender's own MaterialX export, the one USD and Hydra use. A
  Principled BSDF is an `open_pbr_surface`; shaders behind a Mix or Add Shader become a
  graph of BSDF nodes.
* Blender's export does not implement every node (Color Ramp, curves, Voronoi, Brick,
  Magic and Sky textures are missing). A node tree with such a node linked falls back.
* The active UV map is a `texcoord` node. Other UV maps are not supported yet.
* Noise textures are exported, but MaterialX noise is a different function: the pattern
  differs from Cycles.

### 5.5 RDLA (`graph_rdla.cpp`)

The MoonRay device is a generic device: it loads the scene objects it is given and has
nothing of Cycles in it. The translation of the Cycles graph is done here, in the writer,
to networks of the classes MoonRay and moonshine already have. One Cycles node may become
several maps.

* **Closures.** One `DwaBaseMaterial` per material. A Mix Shader mixes the attributes of
  its two sides, weighted by the lobe each belongs to (presence, conductor, refraction,
  diffuse). An Add Shader is a half-and-half mix with doubled colors; emission is added.
* **Ray type.** A shader which uses the Light Path node is written once per ray type and
  the variants are combined in a `RaySwitchMaterial`. Shadow rays take the presence of the
  default material, so "transparent for shadow rays" casts no shadow.
* **Bump.** moonshine has normal maps and true displacement, but nothing which turns a
  height into a normal. The device repository has one small shader of its own for that,
  `AnariBumpNormalMap` (`openmoonray-anari/dso/`, built with `-Target anari_dso`; written
  against the MoonRay shading API, no code of Cycles): three evaluations of the height at
  the shading point and two points a fraction of the pixel footprint away, then the surface
  gradient, as Cycles does. It serves the Bump node and a displacement output used as bump.
  A MoonRay runtime without it shows the error material for every shader with a bump.
* **No flattening.** The writer only gives up on a shader without a surface. A node
  without a counterpart becomes its best approximation or a constant, and a note says so
  (`CYCLES_ANARI_TRACE=1` prints the notes).
* **Emitters.** MoonRay does not sample emitting geometry: a lamp is found by a path which
  happens to hit it. The device can put a `MeshLight` on an emitting surface, and declares
  the material parameter `emissionSampling` for it. Which surfaces is decided by Cycles
  (`Shader::emission_sampling`, which also gives the sides). The writer adds to every shader
  which emits for diffuse rays a second root: a `RaySwitchMaterial` whose variants emit for
  camera and mirror rays only, so that nothing is counted twice, and it passes the radiance
  (`emissionColor`) and the name of the map of the emission (`emissionMap`), if any, with
  the text. Flattened materials (`materials=physicallyBased`) take the same path with
  `emissive`. One light serves all instances and both sides of a surface; an object hidden
  from diffuse or glossy rays keeps that on its light. An emission which depends on the
  direction of view (Fresnel, Layer Weight, Backfacing) is left to the surface by the
  device: a light has no direction of view. `CYCLES_ANARI_NO_EMISSION_SAMPLING=1`
  leaves the emission to the surfaces (for measuring).
* `CYCLES_ANARI_RDLA_NO_BUMP=1` leaves the bump shader out (debugging).

Node tests against Cycles (310 tests, 289 compared: 241 within 0.02, 268 within 0.05; the
number is the relative difference of 8x8 block means, the noise floor is about 0.004; the
median of a group, the worst case in brackets):

| Cycles node | MoonRay classes | Difference |
|---|---|---|
| Texture Coordinate (UV, Generated, Object, Normal, Camera, Window, Reflection) | `AttributeMap`, `TransformSpaceMap` | 0.004 (0.006) |
| Geometry (Position, Normal, Incoming, Backfacing, Pointiness, Random Per Island) | `AttributeMap`, `TransformSpaceMap`, `TwoSidedMap`; pointiness and island random from attribute slots | 0.004 (0.005) |
| Object Info | `AttributeMap` on the instance attributes `anari_object_random`, `_color`, `_index` | 0.004 |
| Camera Data | `TransformSpaceMap`, `OpMap` | 0.004 |
| Fresnel, Layer Weight | `OpMap` dot of normal and view, then a `RampMap` with the Fresnel curve | 0.005 (0.018) |
| Image Texture | `ImageMap`; clip and extend with an `OpMap` mask | 0.043 (filtering of linear instead of encoded texels) |
| Image Texture, box projection | `ProjectTriplanarMap_v2` | 0.019 (0.043 with blend) |
| Math (41 operations), Vector Math (25) | `OpMap`, `ClampMap`, `OpSqrtMap`, compositions | 0.001 (0.007) |
| Mix, all 19 blend types | `BlendMap`, `OpMap`, `RgbToHsvMap` / `HsvToRgbMap` | 0.006 (0.011) |
| Color Ramp, RGB and Float Curves | `RampMap` (10 control points; longer tables are split) | 0.005 (0.010) |
| Hue/Saturation, Brightness/Contrast, Gamma, Invert, RGB to BW, Separate/Combine | `ColorCorrectHsvMap`, `OpMap`, `RgbToFloatMap`, `FloatToRgbMap` | 0.005 (0.009) |
| Clamp, Map Range | `ClampMap`, `OpMap`, `RampMap`, `RemapMap` | 0.004 |
| Mapping, Vector Transform | `OpMap`, `TransformSpaceMap` | 0.003 (camera to world: 0.26) |
| Checker, Gradient, Wave | `OpMap` compositions, `RampMap` | 0.004 to 0.012 |
| Noise, Voronoi, White Noise | `NoiseMap_v2`, `NoiseWorleyMap_v3`, `RandomMap` | another pattern with the same statistics (noise: mean 0.504 / 0.503, feature size 15 / 14 pixels) |
| Sky Texture on a surface | `RampMap`, `OpMap` | 0.005 (Preetham 0.034) |
| Bump, displacement as bump | `AnariBumpNormalMap`, `CombineNormalMap` | 0.025 (0.052) |
| Normal Map | `ImageNormalMap`, or `TransformNormalMap` and `RgbToNormalMap` | 0.023 (0.038) |
| Diffuse, Transparent, Translucent, Emission, Refraction | `DwaBaseMaterial` | 0.005 (rough diffuse 0.055) |
| Glossy | a white conductor weighted by the color | 0.027 (roughness 0.05: 0.49, anisotropic: 0.51) |
| Glass, Principled | `DwaBaseMaterial` | 0.09, 0.029 (anisotropic 0.35) |
| Mix Shader, Add Shader | attribute mixing | 0.012 (0.078; transparent plus glossy 0.33) |
| Light Path | `RaySwitchMaterial` | camera switch 0.004, diffuse 0.16 |
| Hair BSDF (Reflection, Transmission) | one `HairDiffuseMaterial` with a front and a back color | mean color 0.92 to 1.08 of Cycles; with a color above one 0.5 to 0.9, see below |
| Principled Hair BSDF | `HairMaterial_v3`; with another BSDF a `HairLayerMaterial` | 0.62 to 1.01 front-lit; a dense back-lit curtain 2.6 to 5.1 |
| Hair Info | `AttributeMap` on primitive attributes the device computes for every curve geometry (`anari_curve_intercept`, `_random`, `_length`, `anari_is_curve`) | 1.12 to 1.30 on a Diffuse BSDF, which is the diffuse BSDF on curves |
| Subsurface Scattering, Principled subsurface | `DwaBaseMaterial` with the radius converted per method, see below | lit color 0.87 to 1.20 (median 1.02) |

Approximate, each with a note: rough diffuse is Lambertian, sheen, anisotropy (constants
only, another highlight shape), the reflection color of glass, multiscatter GGX, ray depth
(0 for camera rays, 1 for all others), reflective caustics. Constant with a note, as
moonshine has no counterpart: Brick, Magic, Particle Info, Wireframe, Ambient Occlusion, UDIM
images, sphere and tube projection. A color read together with its alpha is darker at half
transparent texels (the arrays hold the alpha associated in the encoded space).

**Hair.** The Hair BSDF of Cycles (the man of Cosmos Laundromat: reflection plus
transmission, one orange color faded by the intercept) becomes a `HairDiffuseMaterial`: the
color times a factor measured per component and roughness, as its front and back color.
MoonRay's hair lobes (`HairMaterial_v3`) were tried first; their reflection with a unit
tint is about fifty times weaker than the one of Cycles.

* Cycles' Hair BSDF gains energy when its lobes add up to more than one, and MoonRay clamps
  every channel: the orange came out yellow. The writer brings the brightest channel to one
  with the hue kept, moves what one lobe has above one to the other, and saturates the
  color with the gain. In Cosmos the chroma of the hair went from 0.53 / 0.31 (red and
  green as a share of the sum) to 0.69 / 0.21, against 0.70 / 0.21 in Cycles; the hair is
  about half as bright (0.46 to 0.55), as a fiber of MoonRay does not send on more light
  than it receives.
* A hair BSDF mixed with other BSDFs: `DwaLayerMaterial` does not take hair materials, only
  the diffuse color of the rest goes into the hair material.
* The Hair BSDF has no highlight of its own this way. A dense curtain of Principled Hair lit
  from behind is 2.6 to 5 times too bright, a single layer is right; the cause is not found.

**Subsurface.** MoonRay's random walk has a mean free path of a third of its radius times
s(A) = 1.9 - A + 3.5 (A - 0.8)^2 for an albedo A. The radius of Cycles is converted per
channel and per method: *Random Walk* 3 R / s(A); the legacy random walk (Monster) with its
mean free path of R / 4 pi; *Random Walk (Skin)* through the dipole radius; *Christensen-
Burley* to MoonRay's normalized diffusion. With a textured albedo the factor is built from
maps. Before this the radius was passed through, between 2.7 times too short and 12 times
too long. On test blocks the lit color is 0.87 to 1.20 of Cycles and the length light bleeds
into a shadow 0.89 to 1.25. Thin parts stay approximate (0.3 to 1.9 of the light through a
wedge, by albedo): MoonRay attenuates paths which leave without scattering by the albedo and
the radius, not by the extinction of the volume.

**Emitters sampled by lights.** A closed room of 4 x 4 x 3 m lit by an emitting plane,
192x144, 16 samples; relative RMS of the lower part of the frame against Cycles with 1024
samples, and the brightness against Cycles:

| Lamp | Cycles | MoonRay, surface only | MoonRay, with the light |
|---|---|---|---|
| 1 m, constant | 0.141 | 0.674 (0.98) | 0.118 (1.00) |
| 0.2 m, the same power | 0.161 | 3.371 (1.02) | 0.132 (1.00) |
| image texture | 0.258 | 1.010 | 0.207 |
| gradient in object coordinates | 0.146 | 0.694 | 0.124 |
| five instances of one mesh | 0.144 | 0.991 | 0.118 |
| flattened (`materials=physicallyBased`) | 0.141 | 0.678 (0.99) | 0.118 (1.01) |

On the demo scenes (480 pixels, 64 samples; brightness / relative difference against
Cycles, the noise as the standard deviation of a pixel over the mean estimated from the
Laplacian of the image, the time of the whole run):

| Scene | Lights | Surface only | With lights | Noise (Cycles) | Time |
|---|---|---|---|---|---|
| Monster | 3 | 0.76 / 0.56 | 0.75 / 0.55; 0.92 / 0.22 with the falloff of its nine lamps | 0.409 to 0.098 (0.169) | 76 to 98 s |
| Classroom | 4 | 1.44 / 2.38 | 1.27 / 2.32 | 0.202 to 0.167 (0.176) | 76 to 192 s |
| Lone Monk | 2 | 1.37 / 0.39 | 1.39 / 0.40 | 0.144 to 0.140 (0.146) | 122 to 132 s |
| Barcelona | 0 | 1.04 / 0.07 | the same | 0.041 (0.063) | 126 to 132 s |
| Italian Flat | 1 | 1.30 / 0.56 | 1.30 / 0.56 | 0.275 to 0.274 (0.419) | 202 to 229 s |
| Cosmos Laundromat | 3 | 0.99 / 0.09 | 1.00 / 0.09 | 0.102 to 0.076 (0.094) | 949 to 1043 s |

The softboxes of Monster are its main lights: a quarter of the noise for a third more
time. The lamps of Classroom matter little next to its sun and cost a shadow ray each at
every hit; with one light (`moonray.maxMeshLights=1`, the daylight plane) it takes 103 s
for a noise of 0.173. A light also gives the scene one more bounce of the emission than
the surface did at the same diffuse depth (+2 % at depth 8, +26 % at depth 2 in the room),
and an edit of an emitter rebuilds its light in MoonRay, about a second per 250 000
triangles (`moonray.maxMeshLightTriangles`).

Every surface shader of the seven demo scenes is exported this way (Classroom 79,
Barcelona 23, Lone Monk 34, Monster 19, Italian Flat 105, Cosmos 82, Barbershop 263), with
6 to 34 scene objects per material in the median.

### 5.6 What the smoke test shows

`test_shader_graph` renders four materials which need the graph and compares them with
Blender's Cycles (relative difference, 32 samples):

| Graph | cycles | rpr | moonray | barney (flattened) |
|---|---|---|---|---|
| Procedural checker | 0.005 | 0.035 | 0.013 | 2.504 |
| Noise through a color ramp | 0.005 | 0.808 (fallback) | 1.112 (another noise pattern) | 0.822 |
| Non-linear math on an image | 0.006 | 0.058 | 0.066 | 0.840 |
| Mix Shader with a procedural mask | 0.002 | 0.015 | 0.004 | 0.550 |

For comparison, the basic render differs by 0.021 (cycles), 0.084 (rpr) and 0.065 (moonray).
The moonray column is the translation to moonshine maps of section 5.5: the noise goes
through `NoiseMap_v2`, which has the statistics of the Cycles noise but another pattern, so
a pixel comparison of a ramp over it says nothing (the flattened fallback was at 0.815).
With barney the baking of section 1.8 brings the first, second and fourth row to 0.04,
0.04 and 0.03.

The Cycles demo scenes of blender.org were rendered with
`tests/python/anari_scene_compare.py` (480 pixels wide, 64 samples; brightness ratio and
relative difference to Blender's Cycles):

| Scene | cycles, graphs | cycles, flattened | Noise of the metric |
|---|---|---|---|
| Monster Under The Bed | 1.002 / 0.012 | 6.62 / 6.04 | 0.037 |
| Barcelona Pavillion | 0.999 / 0.011 | 0.37 / 0.78 | 0.008 |
| Lone Monk | 1.016 / 0.051 | 0.20 / 0.84 | 0.019 |
| Classroom | 1.020 / 0.173 | 1.42 / 0.84 | not measured |
| Italian Flat | 1.019 / 0.098 | not measured | not measured |
| Cosmos Laundromat (benchmark) | 1.007 / 0.057 | not measured | not measured |
| Barbershop | 1.007 / 0.062 | not measured | not measured |

All materials of these scenes are exported as graphs. The flattened numbers also lack the
world and light graphs, and they were measured on 2026-10-09, before most of the work
listed in [TODO.md](TODO.md#6-real-scenes).

The same scenes on the other devices, after the work listed in
[TODO.md](TODO.md#62-the-other-devices) (brightness ratio / relative difference; graphs is
the number of materials sent as graph, the others are flattened):

| Device | Classroom | Barcelona | Lone Monk | Monster |
|---|---|---|---|---|
| cyclesphi-anari (graphs) | 1.02 / 0.17 | 1.00 / 0.01 | 1.02 / 0.05 | 1.00 / 0.01 |
| barney | 0.66 / 0.63 | 1.06 / 0.16 | 1.13 / 0.46 | 0.77 / 0.58 |
| VisRTX (`quality`) | 0.79 / 0.63 | 1.12 / 0.20 | 1.07 / 0.42 | 0.75 / 0.59 |
| OSPRay | 1.06 / 1.02 | 0.96 / 0.09 | 0.98 / 0.39 | 0.69 / 0.60 |
| Photon | 1.47 / 0.95 | 0.97 / 0.11 | 0.71 / 0.50 | 0.68 / 0.62 |
| Visionaray (CUDA) | 1.63 / 0.83 | 1.31 / 0.60 | 1.13 / 0.44 | 0.42 / 0.86 |
| RadeonProRenderANARI (59, 13, 18, 3 graphs) | 1.54 / 2.33 | 1.25 / 0.45 | 1.21 / 0.31 | 0.16 / 0.94 |
| openmoonray-anari (79, 23, 34, 19 graphs) | 1.27 / 2.32 | 1.04 / 0.07 | 1.39 / 0.40 | 0.92 / 0.22 |

| Device | Italian Flat | Cosmos Laundromat | Barbershop |
|---|---|---|---|
| cyclesphi-anari (graphs) | 1.02 / 0.10 | 1.01 / 0.06 | 1.01 / 0.06 |
| barney | 1.06 / 0.53 | 1.07 / 0.18 | 1.79 / 1.16 |
| VisRTX (`quality`) | 1.04 / 0.49 | 1.06 / 0.17 | 3.98 / 10.9 |
| OSPRay | 0.94 / 0.52 | 1.53 / 0.59 | 3.50 / 2.37 |
| Photon | 0.97 / 0.50 | no image after 443 s | 2.13 / 2.19 |
| Visionaray (CUDA) | 0.85 / 0.59 | 1.21 / 0.30 | 4.54 / 11.6 |
| RadeonProRenderANARI (73, 24, 105 graphs) | not finished in 450 s | not finished in 450 s | 3.05 / 3.05 |
| openmoonray-anari (105, 82, 263 graphs) | 1.30 / 0.56 | 1.00 / 0.07 | not finished (about 28 s per sample) |

All rows were measured on 2026-10-10 with everything listed in TODO.md; the MoonRay row
of the first table on 2026-10-11, with lights on the emitting surfaces and the falloff of
the lamps of Monster (before: 1.44 / 2.38, 1.04 / 0.07, 1.37 / 0.39, 0.76 / 0.56). MoonRay with the
flattened materials of the day before was at 5.17 / 5.60 (Classroom), 1.08 / 0.31
(Barcelona), 1.98 / 1.09 (Lone Monk) and 0.57 / 0.97 (Monster). Before the work of that day the three
scenes of the second table were at 2.3 to 5.2 times the brightness of Cycles on barney and
VisRTX (Italian Flat 2.57 / 3.01, Cosmos 3.44 / 2.39, Barbershop 2.29 / 2.28 on barney).
Radeon ProRender's Barcelona was at 0.93 / 0.13 the day before: its trees have no leaves
now, see TODO.md.

How to read it:

* **Barcelona** is lit by the world alone and matches everywhere. Visionaray samples one
  light per hit uniformly; its numbers are mostly noise at 64 samples.
* **Lone Monk** was a white frame on the devices without camera `near`, 3.6 times too
  bright where it rendered, and framed wrongly without `imageRegion`. With one gray
  material for all objects VisRTX is at 1.05 / 0.17 and MoonRay at 1.04 / 0.04: what
  remains is in the flattened materials, most of which change a texture with nodes.
* **Classroom** is rendered by Cycles with two diffuse bounces and with clamps of 5.5 and
  3.0. A device without these limits is two to three times brighter, with fireflies from
  the lamps behind glass (the large relative differences of the older rows). With the
  limits barney and VisRTX are darker than Cycles: a clamp acts on a light sample, and
  what a sample is depends on how the device picks lights. Radeon ProRender has the
  diffuse limit and one clamp. On MoonRay the materials are translated node by node, what
  is left is the lighting. The daylight plane and the lamps are sampled with mesh lights
  now (1.44 / 2.38 before, when they were only found by sampling the BSDF); the difference
  which stays is fireflies behind glass (the medians of image blocks were at 1.19 / 0.73).
  The sun alone matches there (0.995 / 0.026 with the gray material).
* **Monster** has nine area lights which fade out over 2 to 3 m through a node graph. One
  average strength per light gives less light than Cycles, distributed differently
  (0.52 / 0.88 on barney with the gray material, the same as cyclesphi-anari without light
  graphs). MoonRay takes the fade as a ramp over the distance (section 1.7) and its
  softboxes as mesh lights: 0.92 / 0.22. The gradient of the wall and the softboxes are baked. Radeon ProRender is darker
  than the others for a reason not found.
* **Italian Flat** uses Fast GI after two bounces; with the limits sent for it the
  brightness matches. The relative difference of 0.5 is noise: the room is lit through the
  windows only.
* **Cosmos Laundromat** shows a darker sky to the camera than the one which lights the
  scene. It matches since the backdrop is the background image of the renderer. OSPRay's
  1.53 was not looked into.
* **Barbershop** has 279 materials built from node groups which mix a diffuse and a glossy
  shader by a shaped Fresnel, most with base colors from textures changed by procedural
  nodes, and lamps whose emission depends on distance and direction. What remains is
  listed in [TODO.md](TODO.md#62-the-other-devices); VisRTX and OSPRay are much further
  off than barney there, which was not looked into.
* mitsuba-anari is not in the table: Dr.Jit needs minutes per frame on these scenes.

**Blender 5.2 splash scene** (`external/demo/splash52`, 2026-10-11): the head of a lion
with fur from geometry nodes (Principled Hair BSDF, colored by images looked up with the
`surface_uv_coordinate` attribute of the curves and by Hair Info), a sky with a sun of 30
degrees and two area lights. 480x240, 64 samples, brightness ratio / relative difference:

| Device | Result | Note |
|---|---|---|
| cyclesphi-anari (OptiX) | 0.92 / 0.16 | 1.94 / 1.06 before the curves carried their attributes |
| VisRTX (`quality`) | 0.69 / 0.45 | fur of one brown, small white speckles |
| OSPRay | 0.62 / 0.48 | |
| Visionaray (CUDA) | 0.63 / 1.16 | the fur is missing, the cornea is white |
| Photon | 1.27 / 0.51 | 14 minutes |
| MoonRay | 0.41 / 0.66 | 13 minutes; rendered before the hair materials of the writer were final |
| helide | 0.72 / 0.70 | |
| barney | 25 / 24 | white speckles everywhere, see [TODO.md](TODO.md#62-the-other-devices) |
| Radeon ProRender | no image | one sample of 64 in 15 minutes |
| Mitsuba (`cuda_ad_rgb`) | no image | Dr.Jit asked for 8 to 17 GB on a card of 11 GB |

### 5.7 Viewport

**Flattened materials.** The averages over the surface and the baking are part of the scene
update of a changed shader or mesh. Moving the camera or an object does not run them.
`tests/python/anari_viewport_bench.py` (run without `--background`) switches a viewport to rendered mode with an
ANARI library and then orbits the view, moves an object and drags a value of a material at
20 Hz, counting the redraws and the longest gap between two changes. barney, viewport of
1311x775, before and after the changes of 2026-10-10 (surface averages, baking of view
dependent colors), on a machine which other jobs were using:

| Scene, action | Redraws per second | Longest gap |
|---|---|---|
| Cosmos Laundromat, orbit | 20.5 → 19.7 | 285 → 173 ms |
| Cosmos Laundromat, move an object of 72k faces | 20.8 → 20.1 | 188 → 261 ms |
| Cosmos Laundromat, edit the jacket | 14.4 → 22.8 | 368 → 130 ms |
| Cosmos Laundromat, edit the tree (1.2 million triangles, displaced) | 10.3 → 10.5 | 1144 → 1040 ms |
| Barbershop, orbit | 21.4 → 29.5 | 127 → 123 ms |
| Barbershop, move a lamp | 14.1 → 19.0 | 350 → 104 ms |
| Barbershop, edit the sofa | 18.3 → 14.5 | 175 → 182 ms |

The differences are within what the load of the machine changes between two runs. Loading
did not get slower either (Barbershop until 32 samples are done: 42 s before, 36 s after),
although more shaders are baked now: 251 bakes in 6.9 s for Barbershop.

**Shader graphs.**

* A graph is written only for a shader Cycles reports as modified, in 0.02 to 0.3 ms (1.6 ms
  with a color ramp).
* The exporter keeps a hash of the text it sent. An unchanged text is not sent again.
* What a changed text costs on the device, measured outside of Blender:

| Device | Cost of a changed constant |
|---|---|
| cyclesphi-anari | about 1 ms, the geometry is not touched (1 million triangles) |
| RadeonProRenderANARI | 5–17 ms for the document, the next frame 10–20 ms slower |
| openmoonray-anari | MoonRay reloads the geometry of the material: 40 ms at 180k triangles, about 1 s at 4.5 million; the same as for `physicallyBased` |

* A material that changes between graph and flattened export gets a new material object,
  which costs a geometry update on the device.
