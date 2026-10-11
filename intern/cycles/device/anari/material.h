/* SPDX-FileCopyrightText: 2011-2026 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

#pragma once

#ifdef WITH_ANARI

#  include <anari/anari.h>

#  include <functional>

#  include "util/map.h"
#  include "util/param.h"
#  include "util/string.h"
#  include "util/transform.h"
#  include "util/types.h"
#  include "util/vector.h"

CCL_NAMESPACE_BEGIN

class AnariDevice;
class ImageSlotTextureNode;
class Scene;
class Shader;
class ShaderInput;
class ShaderOutput;

/* Assignment of Cycles attributes to the generic ANARI attributes `attribute0..3`.
 * A material requests attributes (UV maps, generic attributes), the geometries which have the
 * material export them into the assigned slots. `attribute0` is always the default UV map.
 *
 * The slots are per shader: a scene has many more attributes than slots, and a mesh is exported
 * as one ANARI geometry per shader anyway. */
class AnariAttributeSlots {
 public:
  /* ANARI has four, devices can declare more: `vertex.attribute4` and following. */
  static constexpr int max_slots = 8;
  int num_slots = 4;

  /* Name of the attribute in the slot, empty for the default UV map in slot 0. */
  ustring slots[max_slots];
  int num_used = 1;

  /* Find or assign a slot for the attribute, returns -1 when all slots are used. */
  int slot_for(ustring name);
  /* Find the slot of the attribute, returns -1 when it has none. */
  int find(ustring name) const;
  /* Whether the same attributes are in the same slots. */
  bool same_slots(const AnariAttributeSlots &other) const;

  /* Changes whenever the slots change, geometries need to be exported again then. */
  int version = 0;
};

class AnariShaderSlots {
 public:
  AnariAttributeSlots &get(const Shader *shader)
  {
    AnariAttributeSlots &table = tables_[shader];
    table.num_slots = num_slots_;
    return table;
  }
  /* Number of attribute slots the geometry of the device has. */
  void set_num_slots(const int num_slots)
  {
    num_slots_ = num_slots;
  }
  /* Start over for a shader which changed: its attributes are requested again. The slots are
   * compared with the ones before in `finish`, so that the version only changes with them. */
  void restart(const Shader *shader);
  void finish();
  void forget(const Shader *shader);

 private:
  map<const Shader *, AnariAttributeSlots> tables_;
  map<const Shader *, AnariAttributeSlots> restarted_;
  int num_slots_ = 4;
};

/* Cache of ANARI image arrays created from the images loaded by the image manager. */
class AnariImageCache {
 public:
  AnariImageCache(AnariDevice &device);
  ~AnariImageCache();

  /* Get the array for the image of the texture node, nullptr if not available. */
  ANARIArray2D get(Scene *scene, ImageSlotTextureNode *node);

  /* Get a sampler which only carries the image, for materials which get their shader graph as
   * text: the graph has its own texture coordinates and filtering. The same sampler is returned
   * as long as the image does not change, so the device can tell an unchanged image from a new
   * one. nullptr if the image is not available. */
  ANARISampler get_sampler(Scene *scene, ImageSlotTextureNode *node);

  /* Texels of the image on a regular grid, as the linear colors a sampler returns for them.
   * Returns false if the image is not available. */
  bool sample_texels(Scene *scene,
                     ImageSlotTextureNode *node,
                     int max_samples,
                     vector<float4> &texels);

  /* Texels of the image at the given texture coordinates, the nearest ones and with the
   * extension of the image node. Returns false if the image is not available. */
  bool lookup_texels(Scene *scene,
                     ImageSlotTextureNode *node,
                     const vector<float2> &coordinates,
                     vector<float4> &texels);

  /* Release arrays of images which were not used since the last call. */
  void garbage_collect();

 private:
  struct Entry {
    ANARIArray2D array = nullptr;
    ANARISampler sampler = nullptr;
    uint64_t version = 0;
    bool srgb = false;
    bool used = false;
  };

  Entry *find_entry(Scene *scene, ImageSlotTextureNode *node);
  void release_entry(Entry &entry);

  AnariDevice &device_;
  map<uint, Entry> entries_;
};

/* A value which drives a material parameter: either a constant or a sampler. */
struct AnariValue {
  enum Type { CONSTANT, IMAGE, ATTRIBUTE, ARRAY };

  Type type = CONSTANT;
  float4 constant = zero_float4();
  /* The constant stands in for a node which can't be evaluated (a procedural texture). Nodes
   * which combine two values use the other one alone then. */
  bool unknown = false;
  /* How far the values of such a node spread around the constant, as a standard deviation. A
   * Color Ramp behind it is averaged over that range: the color at the middle of a ramp says
   * little about a ramp of many stops. */
  float unknown_spread = 0.0f;

  /* An image which was made for the export (a baked one), looked up with `attribute`. */
  ANARIArray2D array = nullptr;

  /* Image or attribute input. */
  ImageSlotTextureNode *image = nullptr;
  string attribute;
  /* Transform of the attribute (texture coordinates) before the image lookup. */
  Transform in_transform = transform_identity();
  /* Linear transform of the result: `out = out_transform * value + out_offset`, with rows in
   * the order of the RGBA channels. */
  float4 out_row[4] = {make_float4(1, 0, 0, 0),
                       make_float4(0, 1, 0, 0),
                       make_float4(0, 0, 1, 0),
                       make_float4(0, 0, 0, 1)};
  float4 out_offset = zero_float4();

  static AnariValue from_constant(const float4 value)
  {
    AnariValue v;
    v.constant = value;
    return v;
  }
  static AnariValue from_float(const float value)
  {
    return from_constant(make_float4(value, value, value, 1.0f));
  }
  static AnariValue from_float3(const float3 value)
  {
    return from_constant(make_float4(value.x, value.y, value.z, 1.0f));
  }

  bool is_constant() const
  {
    return type == CONSTANT;
  }

  /* Apply `value * scale + offset` per channel. */
  void apply_linear(const float4 scale, const float4 offset);
  /* Replace the value by a weighted sum of its channels, written to all channels. */
  void apply_dot(const float4 weights);
};

/* Parameters of the ANARI physicallyBased material. */
struct AnariPrincipled {
  AnariValue base_color = AnariValue::from_float3(make_float3(0.8f, 0.8f, 0.8f));
  AnariValue metallic = AnariValue::from_float(0.0f);
  AnariValue roughness = AnariValue::from_float(0.5f);
  AnariValue opacity = AnariValue::from_float(1.0f);
  AnariValue transmission = AnariValue::from_float(0.0f);
  AnariValue specular = AnariValue::from_float(1.0f);
  AnariValue specular_color = AnariValue::from_float3(one_float3());
  AnariValue clearcoat = AnariValue::from_float(0.0f);
  AnariValue clearcoat_roughness = AnariValue::from_float(0.0f);
  AnariValue sheen_color = AnariValue::from_float3(zero_float3());
  AnariValue sheen_roughness = AnariValue::from_float(0.0f);
  AnariValue emissive = AnariValue::from_float3(zero_float3());
  AnariValue normal_map;
  bool has_normal_map = false;
  float ior = 1.5f;
  float iridescence = 0.0f;
  float iridescence_thickness = 0.0f;
  float iridescence_ior = 1.3f;
  bool has_bsdf = false;
  bool has_emission = false;
  /* The base color is an approximation: the shader computes it with nodes which were left out
   * (procedural textures, a textured mix factor, two textures combined). */
  bool base_color_lossy = false;
  /* The same for the emission. */
  bool emissive_lossy = false;
  /* Only a Lambertian diffuse BSDF, which is exported as a matte material so all back-ends
   * evaluate it exactly instead of approximating it with their physically based model. */
  bool is_diffuse = false;
  bool is_holdout = false;

  /* Linear blend of the constant parameters of two materials, used for mix shaders. */
  static AnariPrincipled mix(const AnariPrincipled &a, const AnariPrincipled &b, float fac);
  /* Blend of two materials of constant parameters by a mask which is a texture or an attribute:
   * every parameter which differs is the mask with its own output transform. */
  static AnariPrincipled mix_masked(const AnariPrincipled &a,
                                    const AnariPrincipled &b,
                                    const AnariValue &mask);
};

/* Converts Cycles shader graphs into ANARI materials. */
class AnariMaterialBuilder {
 public:
  AnariMaterialBuilder(AnariDevice &device,
                       Scene *scene,
                       AnariShaderSlots &slots,
                       AnariImageCache &images);

  /* Evaluate the surface of the shader into principled parameters. */
  AnariPrincipled evaluate_surface(Shader *shader);

  /* Whether the surface is a Mix Shader of two shaders of constant parameters with a factor
   * which is not constant, and the two shaders. Such a factor can be baked as a mask, see
   * `AnariPrincipled::mix_masked`. */
  bool evaluate_mask_mix(Shader *shader, AnariPrincipled &a, AnariPrincipled &b);

  /* The value of the Ray Length output of the Light Path node while the shader of a light is
   * evaluated, negative for none. Lights of ANARI have a fixed falloff: a shader which makes
   * its own from the ray length can only be evaluated for one distance. */
  void set_light_ray_length(const float length)
  {
    light_ray_length_ = length;
  }

  /* The cosine between the normal and the direction of view for which the Layer Weight and
   * Fresnel nodes are evaluated, negative for none (they are left out then). For emission
   * which depends on the direction, which is averaged over the hemisphere. */
  void set_view_cosine(const float cosine)
  {
    view_cosine_ = cosine;
  }

  /* Evaluate shaders whose images are not loaded yet, before the scene update: only what the
   * material is made of is of interest then, not the values of textures. */
  void set_analysis(const bool analysis)
  {
    analysis_ = analysis;
  }

  /* Set the parameters of the material (all parameters are unset first). */
  void set_material(ANARIMaterial material, const char *subtype, const AnariPrincipled &params);

  /* Background: constant radiance or an environment image with a mapping transform. */
  struct Background {
    float3 color = zero_float3();
    float strength = 1.0f;
    ImageSlotTextureNode *image = nullptr;
    /* Transform from world direction to the texture lookup direction. */
    Transform mapping = transform_identity();
    /* The color, strength and image above are the whole world shader. Otherwise they are an
     * approximation, and the shader has to be evaluated to get the environment. */
    bool exact = true;
  };
  Background evaluate_background(Shader *shader);

  /* Homogeneous parameters of a volume shader, in the terms of the Principled Volume node. The
   * density is multiplied by the density grid. */
  struct Volume {
    bool valid = false;
    float3 color = make_float3(0.5f, 0.5f, 0.5f);
    float density = 1.0f;
    float anisotropy = 0.0f;
    float3 absorption_color = zero_float3();
    float3 emission_color = one_float3();
    float emission_strength = 0.0f;
    ustring density_attribute = ustring("density");
  };
  Volume evaluate_volume(Shader *shader);

  /* Values of an attribute of the export (by the name a sampler reads it with) at some points
   * of the meshes which have the shader. An image often is an atlas of which one shader only
   * uses a part, and a mask may cover next to nothing of a mesh: averages have to be taken
   * over the surface, not over the image. */
  using SurfaceAttribute =
      std::function<bool(const Shader *shader, const string &attribute, vector<float4> &values)>;
  void set_surface_attribute(const SurfaceAttribute &function)
  {
    surface_attribute_ = function;
  }

  /* The average of a value: the constant, or the mean over the surface of the meshes which
   * have the shader (the last one evaluated when none is given). Without the attributes of
   * the surface it is the mean over the texels of the image, and not known for attributes:
   * false is returned then. */
  bool average_value(const AnariValue &value, float4 &average, const Shader *shader = nullptr);
  /* The same with the variance of the color around the average, summed over the channels:
   * how much of a pattern the value carries. */
  bool value_statistics(const AnariValue &value,
                        float4 &average,
                        float &variance,
                        const Shader *shader = nullptr);

  /* Create a sampler for a non-constant value, nullptr if not possible. */
  ANARISampler create_sampler(const AnariValue &value);

 private:
  AnariValue evaluate_input(ShaderInput *input, const AnariValue &default_value);
  AnariValue evaluate_output(ShaderOutput *output, const AnariValue &default_value, int depth);
  AnariPrincipled evaluate_closure(ShaderOutput *output, int depth);
  void evaluate_texture_coordinate(ShaderInput *vector_input, AnariValue &value);

  /* Apply a function of the color which is not linear. A sampler only has a linear output
   * transform: it is set to the one which fits the function best for the colors the texture
   * has, which is exact for constants and close for the narrow range of colors of most
   * textures. */
  void apply_function(AnariValue &value, const std::function<float4(const float4 &)> &function);

  /* What a value is before its output transform at the points of `SurfaceAttribute`. */
  bool surface_values(const Shader *shader, const AnariValue &value, vector<float4> &values);
  /* A texture as the constant of its average, for where a node combines two textures and a
   * sampler can only keep one of them. The value itself when the average is not known. */
  AnariValue averaged(const AnariValue &value);

  void set_value(ANARIMaterial material,
                 const char *name,
                 const AnariValue &value,
                 int num_components);

  AnariDevice &device_;
  Scene *scene_;
  AnariShaderSlots &shader_slots_;
  /* Slots of the shader which is evaluated. */
  AnariAttributeSlots unused_slots_;
  AnariAttributeSlots *slots_ = &unused_slots_;
  const Shader *shader_ = nullptr;
  SurfaceAttribute surface_attribute_;
  /* Attributes at the surface points of the shaders, which several values of a shader ask
   * for and which shaders are evaluated for more than once in an update. */
  map<std::pair<const Shader *, string>, vector<float4>> surface_cache_;
  AnariImageCache &images_;
  float light_ray_length_ = -1.0f;
  float view_cosine_ = -1.0f;
  bool analysis_ = false;
  /* Set while a value is evaluated when something of it had to be left out. */
  bool lossy_ = false;
};

/* For flattened materials whose base color is computed by nodes a sampler can't express: link
 * the color of the closures to the displacement output of the shader. The Cycles kernel
 * evaluates the displacement of a shader at any point of a mesh (`SHADER_EVAL_DISPLACE`), which
 * gives the base color at the vertices, see `AnariScene::bake_base_color`. Only for shaders
 * with bump displacement, where the output does not move the mesh: the bump it adds to the
 * shader of Cycles is not exported anyway. Has to be called before the scene update compiles
 * the shader. Returns false when the color can't be linked.
 *
 * What is linked: the base color of the closures, their emission (for shaders which only emit),
 * or the factor of the Mix Shader at the surface output (see `evaluate_mask_mix`). Nothing which
 * depends on the view direction is linked, it can't be baked. */
enum AnariBakeMode { ANARI_BAKE_BASE_COLOR, ANARI_BAKE_EMISSION, ANARI_BAKE_MASK };
bool anari_link_bake(Shader *shader, AnariBakeMode mode);

CCL_NAMESPACE_END

#endif /* WITH_ANARI */
