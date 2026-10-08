/* SPDX-FileCopyrightText: 2011-2026 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

#pragma once

#ifdef WITH_ANARI

#  include <anari/anari.h>

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
 * Materials request attributes (UV maps, generic attributes), geometries export them into the
 * assigned slot. `attribute0` is always the default UV map. */
class AnariAttributeSlots {
 public:
  static constexpr int num_slots = 4;

  /* Name of the attribute in the slot, empty for the default UV map in slot 0. */
  ustring slots[num_slots];
  int num_used = 1;

  /* Find or assign a slot for the attribute, returns -1 when all slots are used. */
  int slot_for(ustring name);

  /* Incremented whenever a new slot gets assigned, geometries need to be re-exported then. */
  int version = 0;
};

/* Cache of ANARI image arrays created from the images loaded by the image manager. */
class AnariImageCache {
 public:
  AnariImageCache(AnariDevice &device);
  ~AnariImageCache();

  /* Get the array for the image of the texture node, nullptr if not available. */
  ANARIArray2D get(Scene *scene, ImageSlotTextureNode *node);

  /* Release arrays of images which were not used since the last call. */
  void garbage_collect();

 private:
  struct Entry {
    ANARIArray2D array = nullptr;
    uint64_t version = 0;
    bool srgb = false;
    bool used = false;
  };

  AnariDevice &device_;
  map<uint, Entry> entries_;
};

/* A value which drives a material parameter: either a constant or a sampler. */
struct AnariValue {
  enum Type { CONSTANT, IMAGE, ATTRIBUTE };

  Type type = CONSTANT;
  float4 constant = zero_float4();

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
  /* Only a Lambertian diffuse BSDF, which is exported as a matte material so all back-ends
   * evaluate it exactly instead of approximating it with their physically based model. */
  bool is_diffuse = false;
  bool is_holdout = false;

  /* Linear blend of the constant parameters of two materials, used for mix shaders. */
  static AnariPrincipled mix(const AnariPrincipled &a, const AnariPrincipled &b, float fac);
};

/* Converts Cycles shader graphs into ANARI materials. */
class AnariMaterialBuilder {
 public:
  AnariMaterialBuilder(AnariDevice &device,
                       Scene *scene,
                       AnariAttributeSlots &slots,
                       AnariImageCache &images);

  /* Evaluate the surface of the shader into principled parameters. */
  AnariPrincipled evaluate_surface(Shader *shader);

  /* Set the parameters of the material (all parameters are unset first). */
  void set_material(ANARIMaterial material, const char *subtype, const AnariPrincipled &params);

  /* Background: constant radiance or an environment image with a mapping transform. */
  struct Background {
    float3 color = zero_float3();
    float strength = 1.0f;
    ImageSlotTextureNode *image = nullptr;
    /* Transform from world direction to the texture lookup direction. */
    Transform mapping = transform_identity();
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

  /* Create a sampler for a non-constant value, nullptr if not possible. */
  ANARISampler create_sampler(const AnariValue &value);

 private:
  AnariValue evaluate_input(ShaderInput *input, const AnariValue &default_value);
  AnariValue evaluate_output(ShaderOutput *output, const AnariValue &default_value, int depth);
  AnariPrincipled evaluate_closure(ShaderOutput *output, int depth);
  void evaluate_texture_coordinate(ShaderInput *vector_input, AnariValue &value);

  void set_value(ANARIMaterial material,
                 const char *name,
                 const AnariValue &value,
                 int num_components);

  AnariDevice &device_;
  Scene *scene_;
  AnariAttributeSlots &slots_;
  AnariImageCache &images_;
};

CCL_NAMESPACE_END

#endif /* WITH_ANARI */
