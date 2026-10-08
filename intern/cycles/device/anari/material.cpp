/* SPDX-FileCopyrightText: 2011-2026 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

#ifdef WITH_ANARI

#  include "device/anari/material.h"
#  include "device/anari/device_impl.h"

#  include "scene/image.h"
#  include "scene/scene.h"
#  include "scene/shader.h"
#  include "scene/shader_graph.h"
#  include "scene/shader_nodes.h"

#  include "util/color.h"
#  include "util/half.h"
#  include "util/log.h"
#  include "util/progress.h"

CCL_NAMESPACE_BEGIN

/* --------------------------------------------------------------------
 * Attribute slots.
 */

int AnariAttributeSlots::slot_for(ustring name)
{
  if (name.empty()) {
    return 0;
  }
  for (int i = 1; i < num_used; i++) {
    if (slots[i] == name) {
      return i;
    }
  }
  if (num_used >= num_slots) {
    LOG_WARNING << "ANARI: no free attribute slot for attribute \"" << name.string() << "\"";
    return -1;
  }
  slots[num_used] = name;
  version++;
  return num_used++;
}

static string attribute_slot_name(const int slot)
{
  return string_printf("attribute%d", slot);
}

/* --------------------------------------------------------------------
 * Image cache.
 */

AnariImageCache::AnariImageCache(AnariDevice &device) : device_(device) {}

AnariImageCache::~AnariImageCache()
{
  for (auto &it : entries_) {
    if (it.second.array) {
      anariRelease(device_.anari_device(), it.second.array);
    }
  }
}

/* Convert the pixels of an image to one of the formats supported by all back-ends:
 * 8-bit RGBA (linear or sRGB) or 32-bit float RGBA. */
static ANARIArray2D create_image_array(ANARIDevice device, const AnariImage &image, bool srgb)
{
  const size_t width = size_t(image.width);
  const size_t height = size_t(image.height);
  const size_t num_pixels = width * height;
  if (num_pixels == 0 || image.pixels == nullptr) {
    return nullptr;
  }

  switch (image.data_type) {
    case IMAGE_DATA_TYPE_BYTE4: {
      ANARIArray2D array = anariNewArray2D(device,
                                           nullptr,
                                           nullptr,
                                           nullptr,
                                           srgb ? ANARI_UFIXED8_RGBA_SRGB : ANARI_UFIXED8_VEC4,
                                           width,
                                           height);
      memcpy(anariMapArray(device, array), image.pixels, num_pixels * 4);
      anariUnmapArray(device, array);
      return array;
    }
    case IMAGE_DATA_TYPE_BYTE: {
      ANARIArray2D array = anariNewArray2D(device,
                                           nullptr,
                                           nullptr,
                                           nullptr,
                                           srgb ? ANARI_UFIXED8_RGBA_SRGB : ANARI_UFIXED8_VEC4,
                                           width,
                                           height);
      uchar *dst = static_cast<uchar *>(anariMapArray(device, array));
      const uchar *src = static_cast<const uchar *>(image.pixels);
      for (size_t i = 0; i < num_pixels; i++) {
        dst[i * 4 + 0] = dst[i * 4 + 1] = dst[i * 4 + 2] = src[i];
        dst[i * 4 + 3] = 255;
      }
      anariUnmapArray(device, array);
      return array;
    }
    default:
      break;
  }

  ANARIArray2D array = anariNewArray2D(
      device, nullptr, nullptr, nullptr, ANARI_FLOAT32_VEC4, width, height);
  float4 *dst = static_cast<float4 *>(anariMapArray(device, array));

  switch (image.data_type) {
    case IMAGE_DATA_TYPE_FLOAT4:
      memcpy(dst, image.pixels, num_pixels * sizeof(float4));
      break;
    case IMAGE_DATA_TYPE_FLOAT: {
      const float *src = static_cast<const float *>(image.pixels);
      for (size_t i = 0; i < num_pixels; i++) {
        dst[i] = make_float4(src[i], src[i], src[i], 1.0f);
      }
      break;
    }
    case IMAGE_DATA_TYPE_HALF4: {
      const half *src = static_cast<const half *>(image.pixels);
      for (size_t i = 0; i < num_pixels; i++) {
        dst[i] = make_float4(half_to_float(src[i * 4 + 0]),
                             half_to_float(src[i * 4 + 1]),
                             half_to_float(src[i * 4 + 2]),
                             half_to_float(src[i * 4 + 3]));
      }
      break;
    }
    case IMAGE_DATA_TYPE_HALF: {
      const half *src = static_cast<const half *>(image.pixels);
      for (size_t i = 0; i < num_pixels; i++) {
        const float f = half_to_float(src[i]);
        dst[i] = make_float4(f, f, f, 1.0f);
      }
      break;
    }
    case IMAGE_DATA_TYPE_USHORT4: {
      const uint16_t *src = static_cast<const uint16_t *>(image.pixels);
      for (size_t i = 0; i < num_pixels; i++) {
        dst[i] = make_float4(src[i * 4 + 0], src[i * 4 + 1], src[i * 4 + 2], src[i * 4 + 3]) *
                 (1.0f / 65535.0f);
      }
      break;
    }
    case IMAGE_DATA_TYPE_USHORT: {
      const uint16_t *src = static_cast<const uint16_t *>(image.pixels);
      for (size_t i = 0; i < num_pixels; i++) {
        const float f = src[i] * (1.0f / 65535.0f);
        dst[i] = make_float4(f, f, f, 1.0f);
      }
      break;
    }
    default:
      memset(dst, 0, num_pixels * sizeof(float4));
      break;
  }

  anariUnmapArray(device, array);
  return array;
}

static bool image_is_srgb(ImageSlotTextureNode *node)
{
  Progress progress;
  const ImageMetaData metadata = node->handle.metadata(progress);
  return metadata.colorspace == u_colorspace_scene_linear_srgb &&
         (metadata.type == IMAGE_DATA_TYPE_BYTE4 || metadata.type == IMAGE_DATA_TYPE_BYTE);
}

ANARIArray2D AnariImageCache::get(Scene *scene, ImageSlotTextureNode *node)
{
  if (node == nullptr || node->handle.empty()) {
    return nullptr;
  }

  const int kernel_id = node->handle.kernel_id();
  if (kernel_id < 0 || size_t(kernel_id) >= scene->dscene.image_textures.size()) {
    /* UDIM tiles or images which were not loaded. */
    return nullptr;
  }

  const uint image_info_id = scene->dscene.image_textures[kernel_id].image_info_id;
  const AnariImage *image = device_.find_image(image_info_id);
  if (image == nullptr) {
    return nullptr;
  }

  const bool srgb = image_is_srgb(node);

  Entry &entry = entries_[image_info_id];
  if (entry.array == nullptr || entry.version != image->version || entry.srgb != srgb) {
    if (entry.array) {
      anariRelease(device_.anari_device(), entry.array);
    }
    entry.array = create_image_array(device_.anari_device(), *image, srgb);
    entry.version = image->version;
    entry.srgb = srgb;
  }
  entry.used = true;
  return entry.array;
}

void AnariImageCache::garbage_collect()
{
  for (auto it = entries_.begin(); it != entries_.end();) {
    if (!it->second.used) {
      if (it->second.array) {
        anariRelease(device_.anari_device(), it->second.array);
      }
      it = entries_.erase(it);
    }
    else {
      it->second.used = false;
      ++it;
    }
  }
}

/* --------------------------------------------------------------------
 * Values.
 */

void AnariValue::apply_linear(const float4 scale, const float4 offset)
{
  if (type == CONSTANT) {
    constant = constant * scale + offset;
    return;
  }
  for (int i = 0; i < 4; i++) {
    out_row[i] *= scale[i];
  }
  out_offset = out_offset * scale + offset;
}

void AnariValue::apply_dot(const float4 weights)
{
  if (type == CONSTANT) {
    const float d = dot(constant, weights);
    constant = make_float4(d, d, d, 1.0f);
    return;
  }

  float4 row = zero_float4();
  float offset = 0.0f;
  for (int i = 0; i < 4; i++) {
    row += out_row[i] * weights[i];
    offset += out_offset[i] * weights[i];
  }
  out_row[0] = out_row[1] = out_row[2] = row;
  out_row[3] = make_float4(0.0f, 0.0f, 0.0f, 0.0f);
  out_offset = make_float4(offset, offset, offset, 1.0f);
}

static float4 luminance_weights()
{
  return make_float4(0.2126f, 0.7152f, 0.0722f, 0.0f);
}

static float4 average_weights()
{
  return make_float4(1.0f / 3.0f, 1.0f / 3.0f, 1.0f / 3.0f, 0.0f);
}

static AnariValue mix_values(const AnariValue &a, const AnariValue &b, const float fac)
{
  if (a.is_constant() && b.is_constant()) {
    return AnariValue::from_constant(a.constant * (1.0f - fac) + b.constant * fac);
  }
  if (a.is_constant()) {
    AnariValue result = b;
    result.apply_linear(make_float4(fac), a.constant * (1.0f - fac));
    return result;
  }
  if (b.is_constant()) {
    AnariValue result = a;
    result.apply_linear(make_float4(1.0f - fac), b.constant * fac);
    return result;
  }
  /* Both inputs are textures, ANARI samplers can't be combined: use the dominant one. */
  return (fac < 0.5f) ? a : b;
}

AnariPrincipled AnariPrincipled::mix(const AnariPrincipled &a,
                                     const AnariPrincipled &b,
                                     const float fac)
{
  if (!a.has_bsdf && !a.has_emission) {
    return b;
  }
  if (!b.has_bsdf && !b.has_emission) {
    return a;
  }

  AnariPrincipled r = (fac < 0.5f) ? a : b;
  r.base_color = mix_values(a.base_color, b.base_color, fac);
  r.metallic = mix_values(a.metallic, b.metallic, fac);
  r.roughness = mix_values(a.roughness, b.roughness, fac);
  r.opacity = mix_values(a.opacity, b.opacity, fac);
  r.transmission = mix_values(a.transmission, b.transmission, fac);
  r.specular = mix_values(a.specular, b.specular, fac);
  r.specular_color = mix_values(a.specular_color, b.specular_color, fac);
  r.clearcoat = mix_values(a.clearcoat, b.clearcoat, fac);
  r.clearcoat_roughness = mix_values(a.clearcoat_roughness, b.clearcoat_roughness, fac);
  r.sheen_color = mix_values(a.sheen_color, b.sheen_color, fac);
  r.sheen_roughness = mix_values(a.sheen_roughness, b.sheen_roughness, fac);
  r.emissive = mix_values(a.emissive, b.emissive, fac);
  r.ior = a.ior * (1.0f - fac) + b.ior * fac;
  r.has_bsdf = a.has_bsdf || b.has_bsdf;
  r.has_emission = a.has_emission || b.has_emission;
  r.is_diffuse = (a.is_diffuse || !a.has_bsdf) && (b.is_diffuse || !b.has_bsdf);
  return r;
}

/* --------------------------------------------------------------------
 * Material builder.
 */

AnariMaterialBuilder::AnariMaterialBuilder(AnariDevice &device,
                                           Scene *scene,
                                           AnariAttributeSlots &slots,
                                           AnariImageCache &images)
    : device_(device), scene_(scene), slots_(slots), images_(images)
{
}

static AnariValue input_constant(ShaderInput *input, const AnariValue &default_value)
{
  ShaderNode *node = input->parent;
  const SocketType &socket = input->socket_type;
  switch (input->type()) {
    case SocketType::FLOAT:
      return AnariValue::from_float(node->get_float(socket));
    case SocketType::INT:
      return AnariValue::from_float(float(node->get_int(socket)));
    case SocketType::BOOLEAN:
      return AnariValue::from_float(node->get_bool(socket) ? 1.0f : 0.0f);
    case SocketType::COLOR:
    case SocketType::VECTOR:
    case SocketType::POINT:
    case SocketType::NORMAL:
      return AnariValue::from_float3(node->get_float3(socket));
    default:
      return default_value;
  }
}

AnariValue AnariMaterialBuilder::evaluate_input(ShaderInput *input,
                                                const AnariValue &default_value)
{
  if (input == nullptr) {
    return default_value;
  }
  if (input->link) {
    return evaluate_output(input->link, default_value, 0);
  }
  return input_constant(input, default_value);
}

static float input_float(ShaderNode *node, const char *name, const float default_value)
{
  ShaderInput *input = node->input(name);
  if (input == nullptr || input->link) {
    return default_value;
  }
  return input_constant(input, AnariValue::from_float(default_value)).constant.x;
}

/* Build the texture coordinate transform from a mapping node. */
static Transform mapping_node_transform(ShaderNode *node)
{
  MappingNode *mapping = static_cast<MappingNode *>(node);
  const float3 location = mapping->get_location();
  const float3 rotation = mapping->get_rotation();
  const float3 scale = mapping->get_scale();
  const Transform rot = transform_euler(rotation);

  switch (mapping->get_mapping_type()) {
    case NODE_MAPPING_TYPE_POINT:
      return transform_translate(location) * rot * transform_scale(scale);
    case NODE_MAPPING_TYPE_TEXTURE:
      return transform_inverse(transform_translate(location) * rot * transform_scale(scale));
    case NODE_MAPPING_TYPE_VECTOR:
      return rot * transform_scale(scale);
    case NODE_MAPPING_TYPE_NORMAL:
    default:
      return rot;
  }
}

void AnariMaterialBuilder::evaluate_texture_coordinate(ShaderInput *vector_input,
                                                       AnariValue &value)
{
  value.attribute = attribute_slot_name(0);

  Transform transform = transform_identity();
  ShaderOutput *link = vector_input ? vector_input->link : nullptr;

  for (int depth = 0; link && depth < 16; depth++) {
    ShaderNode *node = link->parent;

    if (node->type == MappingNode::get_node_type()) {
      transform = transform * mapping_node_transform(node);
      link = node->input("Vector") ? node->input("Vector")->link : nullptr;
      continue;
    }
    if (node->type == UVMapNode::get_node_type()) {
      const ustring name = static_cast<UVMapNode *>(node)->get_attribute();
      const int slot = slots_.slot_for(name);
      value.attribute = attribute_slot_name(max(slot, 0));
      break;
    }
    if (node->type == AttributeNode::get_node_type()) {
      const ustring name = static_cast<AttributeNode *>(node)->get_attribute();
      const int slot = slots_.slot_for(name);
      value.attribute = attribute_slot_name(max(slot, 0));
      break;
    }
    if (node->type == TextureCoordinateNode::get_node_type()) {
      const string output = link->name().string();
      if (output == "Generated" || output == "Object") {
        value.attribute = "objectPosition";
      }
      else if (output == "Normal") {
        value.attribute = "objectNormal";
      }
      break;
    }
    if (dynamic_cast<ConvertNode *>(node) && !node->inputs.empty()) {
      link = node->inputs[0]->link;
      continue;
    }

    /* Unsupported coordinate source, fall back to the default UV map. */
    LOG_DEBUG << "ANARI: unsupported texture coordinate node " << node->type->name;
    break;
  }

  value.in_transform = transform;
}

AnariValue AnariMaterialBuilder::evaluate_output(ShaderOutput *output,
                                                 const AnariValue &default_value,
                                                 const int depth)
{
  if (output == nullptr || depth > 32) {
    return default_value;
  }

  ShaderNode *node = output->parent;
  const NodeType *type = node->type;
  const string output_name = output->name().string();

  auto input = [&](const char *name, const AnariValue &def) -> AnariValue {
    ShaderInput *in = node->input(name);
    if (in == nullptr) {
      return def;
    }
    if (in->link) {
      return evaluate_output(in->link, def, depth + 1);
    }
    return input_constant(in, def);
  };

  /* Image textures. */
  if (type == ImageTextureNode::get_node_type() || type == EnvironmentTextureNode::get_node_type())
  {
    ImageSlotTextureNode *image = static_cast<ImageSlotTextureNode *>(node);
    if (image->handle.empty()) {
      return default_value;
    }
    AnariValue value;
    value.type = AnariValue::IMAGE;
    value.image = image;
    evaluate_texture_coordinate(node->input("Vector"), value);
    if (!image->tex_mapping.skip()) {
      value.in_transform = image->tex_mapping.compute_transform() * value.in_transform;
    }
    if (output_name == "Alpha") {
      value.apply_dot(make_float4(0.0f, 0.0f, 0.0f, 1.0f));
    }
    return value;
  }

  /* Attributes. */
  if (type == VertexColorNode::get_node_type()) {
    const ustring layer = static_cast<VertexColorNode *>(node)->get_layer_name();
    AnariValue value;
    value.type = AnariValue::ATTRIBUTE;
    /* The default color attribute is exported as ANARI vertex color. */
    const int slot = layer.empty() ? -1 : slots_.slot_for(layer);
    value.attribute = (slot > 0) ? attribute_slot_name(slot) : string("color");
    if (output_name == "Alpha") {
      value.apply_dot(make_float4(0.0f, 0.0f, 0.0f, 1.0f));
    }
    return value;
  }
  if (type == AttributeNode::get_node_type()) {
    const ustring name = static_cast<AttributeNode *>(node)->get_attribute();
    const int slot = slots_.slot_for(name);
    if (slot < 0) {
      return default_value;
    }
    AnariValue value;
    value.type = AnariValue::ATTRIBUTE;
    value.attribute = (slot == 0) ? attribute_slot_name(0) : attribute_slot_name(slot);
    if (output_name == "Fac") {
      value.apply_dot(average_weights());
    }
    else if (output_name == "Alpha") {
      value.apply_dot(make_float4(0.0f, 0.0f, 0.0f, 1.0f));
    }
    return value;
  }

  /* Constants. */
  if (type == ColorNode::get_node_type()) {
    return AnariValue::from_float3(static_cast<ColorNode *>(node)->get_value());
  }
  if (type == ValueNode::get_node_type()) {
    return AnariValue::from_float(static_cast<ValueNode *>(node)->get_value());
  }

  /* Conversions. */
  if (dynamic_cast<ConvertNode *>(node)) {
    if (node->inputs.empty()) {
      return default_value;
    }
    ShaderInput *in = node->inputs[0];
    AnariValue value = in->link ? evaluate_output(in->link, default_value, depth + 1) :
                                  input_constant(in, default_value);
    const SocketType::Type from = in->type();
    const SocketType::Type to = output->type();
    if (to == SocketType::FLOAT || to == SocketType::INT) {
      if (from == SocketType::COLOR) {
        value.apply_dot(luminance_weights());
      }
      else if (from == SocketType::VECTOR || from == SocketType::POINT ||
               from == SocketType::NORMAL)
      {
        value.apply_dot(average_weights());
      }
    }
    return value;
  }
  if (type == RGBToBWNode::get_node_type()) {
    AnariValue value = input("Color", default_value);
    value.apply_dot(luminance_weights());
    return value;
  }
  if (type == SeparateColorNode::get_node_type()) {
    AnariValue value = input("Color", default_value);
    if (static_cast<SeparateColorNode *>(node)->get_color_type() == NODE_COMBSEP_COLOR_RGB) {
      const int channel = (output_name == "Red") ? 0 : (output_name == "Green") ? 1 : 2;
      float4 weights = zero_float4();
      weights[channel] = 1.0f;
      value.apply_dot(weights);
    }
    else {
      value.apply_dot(luminance_weights());
    }
    return value;
  }

  /* Linear operations. */
  if (type == InvertNode::get_node_type()) {
    const float fac = input_float(node, "Fac", 1.0f);
    AnariValue value = input("Color", default_value);
    value.apply_linear(make_float4(1.0f - 2.0f * fac, 1.0f - 2.0f * fac, 1.0f - 2.0f * fac, 1.0f),
                       make_float4(fac, fac, fac, 0.0f));
    return value;
  }
  if (type == MixColorNode::get_node_type() || type == MixFloatNode::get_node_type() ||
      type == MixVectorNode::get_node_type() || type == MixNode::get_node_type())
  {
    const bool legacy = (type == MixNode::get_node_type());
    const float fac = input_float(node, legacy ? "Fac" : "Factor", 0.5f);
    const AnariValue a = input(legacy ? "Color1" : "A", default_value);
    const AnariValue b = input(legacy ? "Color2" : "B", default_value);

    NodeMix blend = NODE_MIX_BLEND;
    if (type == MixColorNode::get_node_type()) {
      blend = static_cast<MixColorNode *>(node)->get_blend_type();
    }
    else if (legacy) {
      blend = static_cast<MixNode *>(node)->get_mix_type();
    }

    switch (blend) {
      case NODE_MIX_MUL:
        /* mix(a, a * b, fac) = a * (1 - fac + fac * b) */
        if (b.is_constant()) {
          AnariValue r = a;
          r.apply_linear(make_float4(1.0f - fac) + b.constant * fac, zero_float4());
          return r;
        }
        if (a.is_constant()) {
          AnariValue r = b;
          r.apply_linear(a.constant * fac, a.constant * (1.0f - fac));
          return r;
        }
        return a;
      case NODE_MIX_ADD:
        if (b.is_constant()) {
          AnariValue r = a;
          r.apply_linear(one_float4(), b.constant * fac);
          return r;
        }
        if (a.is_constant()) {
          AnariValue r = b;
          r.apply_linear(make_float4(fac), a.constant);
          return r;
        }
        return a;
      case NODE_MIX_SUB:
        if (b.is_constant()) {
          AnariValue r = a;
          r.apply_linear(one_float4(), -b.constant * fac);
          return r;
        }
        if (a.is_constant()) {
          AnariValue r = b;
          r.apply_linear(make_float4(-fac), a.constant);
          return r;
        }
        return a;
      case NODE_MIX_BLEND:
        return mix_values(a, b, fac);
      default:
        /* Non-linear blend modes can't be expressed with samplers, use the base color. */
        return a.is_constant() && !b.is_constant() ? b : a;
    }
  }
  if (type == MathNode::get_node_type()) {
    MathNode *math = static_cast<MathNode *>(node);
    AnariValue a = input("Value1", default_value);
    AnariValue b = input("Value2", default_value);
    const NodeMathType math_type = math->get_math_type();
    if (a.is_constant() && b.is_constant()) {
      /* Constant folding has already happened in most cases. */
      return a;
    }
    AnariValue &v = a.is_constant() ? b : a;
    const float c = a.is_constant() ? a.constant.x : b.constant.x;
    switch (math_type) {
      case NODE_MATH_MULTIPLY:
        v.apply_linear(make_float4(c, c, c, 1.0f), zero_float4());
        return v;
      case NODE_MATH_ADD:
        v.apply_linear(one_float4(), make_float4(c, c, c, 0.0f));
        return v;
      case NODE_MATH_SUBTRACT:
        if (a.is_constant()) {
          v.apply_linear(make_float4(-1.0f, -1.0f, -1.0f, 1.0f), make_float4(c, c, c, 0.0f));
        }
        else {
          v.apply_linear(one_float4(), make_float4(-c, -c, -c, 0.0f));
        }
        return v;
      case NODE_MATH_DIVIDE:
        if (!a.is_constant() && c != 0.0f) {
          v.apply_linear(make_float4(1.0f / c, 1.0f / c, 1.0f / c, 1.0f), zero_float4());
        }
        return v;
      default:
        return v;
    }
  }
  if (type == RGBRampNode::get_node_type()) {
    /* Linear approximation of the color ramp from its first and last color. */
    RGBRampNode *ramp_node = static_cast<RGBRampNode *>(node);
    const auto &ramp = ramp_node->get_ramp();
    const array<float> &ramp_alpha = ramp_node->get_ramp_alpha();
    if (ramp.empty()) {
      return default_value;
    }
    const float4 c0 = make_float4(
        ramp[0].x, ramp[0].y, ramp[0].z, ramp_alpha.empty() ? 1.0f : ramp_alpha[0]);
    const size_t n = ramp.size() - 1;
    const float4 c1 = make_float4(
        ramp[n].x, ramp[n].y, ramp[n].z, ramp_alpha.empty() ? 1.0f : ramp_alpha[n]);
    AnariValue fac = input("Fac", AnariValue::from_float(0.5f));
    fac.apply_dot(make_float4(1.0f, 0.0f, 0.0f, 0.0f));
    if (output_name == "Alpha") {
      fac.apply_linear(make_float4(c1.w - c0.w), make_float4(c0.w));
    }
    else {
      fac.apply_linear(c1 - c0, c0);
    }
    return fac;
  }

  /* Color operations which can't be expressed, pass the color through. */
  if (type == GammaNode::get_node_type() || type == BrightContrastNode::get_node_type() ||
      type == HSVNode::get_node_type() || type == RGBCurvesNode::get_node_type() ||
      type == ClampNode::get_node_type() || type == MapRangeNode::get_node_type())
  {
    if (node->input("Color")) {
      return input("Color", default_value);
    }
    if (node->input("Value")) {
      return input("Value", default_value);
    }
    return default_value;
  }

  LOG_DEBUG << "ANARI: unsupported shader node " << type->name << ", using default value";
  return default_value;
}

AnariPrincipled AnariMaterialBuilder::evaluate_closure(ShaderOutput *output, const int depth)
{
  AnariPrincipled p;
  if (output == nullptr || depth > 32) {
    return p;
  }

  ShaderNode *node = output->parent;
  const NodeType *type = node->type;

  auto value = [&](const char *name, const AnariValue &def) -> AnariValue {
    return evaluate_input(node->input(name), def);
  };
  auto constant = [&](const char *name, const float def) -> float {
    return input_float(node, name, def);
  };
  auto normal_map = [&](const char *name) {
    ShaderInput *in = node->input(name);
    if (in == nullptr || in->link == nullptr) {
      return;
    }
    ShaderNode *normal_node = in->link->parent;
    if (normal_node->type != NormalMapNode::get_node_type() ||
        static_cast<NormalMapNode *>(normal_node)->get_space() != NODE_NORMAL_MAP_TANGENT)
    {
      return;
    }
    AnariValue color = evaluate_input(normal_node->input("Color"),
                                      AnariValue::from_float3(make_float3(0.5f, 0.5f, 1.0f)));
    if (color.is_constant()) {
      return;
    }
    /* Tangent space normal from [0, 1] color to [-1, 1] vector. */
    color.apply_linear(make_float4(2.0f, 2.0f, 2.0f, 1.0f), make_float4(-1.0f, -1.0f, -1.0f, 0.0f));
    p.normal_map = color;
    p.has_normal_map = true;
  };

  if (type == PrincipledBsdfNode::get_node_type()) {
    p.has_bsdf = true;
    p.base_color = value("Base Color", p.base_color);
    p.metallic = value("Metallic", p.metallic);
    p.roughness = value("Roughness", p.roughness);
    p.ior = constant("IOR", 1.5f);
    p.opacity = value("Alpha", p.opacity);
    p.transmission = value("Transmission Weight", p.transmission);

    /* Cycles specular IOR level 0.5 corresponds to the unmodified Fresnel of the IOR, which
     * is a specular weight of 1 in ANARI. */
    p.specular = value("Specular IOR Level", AnariValue::from_float(0.5f));
    p.specular.apply_linear(make_float4(2.0f, 2.0f, 2.0f, 1.0f), zero_float4());
    p.specular_color = value("Specular Tint", p.specular_color);

    p.clearcoat = value("Coat Weight", p.clearcoat);
    p.clearcoat_roughness = value("Coat Roughness", AnariValue::from_float(0.03f));

    const float sheen_weight = constant("Sheen Weight", 0.0f);
    p.sheen_color = value("Sheen Tint", AnariValue::from_float3(one_float3()));
    p.sheen_color.apply_linear(make_float4(sheen_weight, sheen_weight, sheen_weight, 1.0f),
                               zero_float4());
    p.sheen_roughness = value("Sheen Roughness", AnariValue::from_float(0.5f));

    const float emission_strength = constant("Emission Strength", 0.0f);
    if (emission_strength != 0.0f) {
      p.emissive = value("Emission Color", AnariValue::from_float3(one_float3()));
      p.emissive.apply_linear(
          make_float4(emission_strength, emission_strength, emission_strength, 1.0f),
          zero_float4());
      p.has_emission = true;
    }

    const float thin_film = constant("Thin Film Thickness", 0.0f);
    if (thin_film > 0.0f) {
      p.iridescence = 1.0f;
      p.iridescence_thickness = thin_film;
      p.iridescence_ior = constant("Thin Film IOR", 1.33f);
    }

    normal_map("Normal");
    return p;
  }

  if (type == DiffuseBsdfNode::get_node_type() || type == TranslucentBsdfNode::get_node_type() ||
      type == SubsurfaceScatteringNode::get_node_type() || type == ToonBsdfNode::get_node_type())
  {
    p.has_bsdf = true;
    p.is_diffuse = type == DiffuseBsdfNode::get_node_type() ||
                   type == SubsurfaceScatteringNode::get_node_type();
    p.base_color = value("Color", p.base_color);
    p.roughness = AnariValue::from_float(1.0f);
    p.specular = AnariValue::from_float(0.0f);
    normal_map("Normal");
    return p;
  }

  if (type == GlossyBsdfNode::get_node_type() || type == MetallicBsdfNode::get_node_type()) {
    p.has_bsdf = true;
    p.base_color = value(type == MetallicBsdfNode::get_node_type() ? "Base Color" : "Color",
                         p.base_color);
    p.metallic = AnariValue::from_float(1.0f);
    p.roughness = value("Roughness", p.roughness);
    normal_map("Normal");
    return p;
  }

  if (type == GlassBsdfNode::get_node_type() || type == RefractionBsdfNode::get_node_type()) {
    p.has_bsdf = true;
    p.base_color = value("Color", AnariValue::from_float3(one_float3()));
    p.roughness = value("Roughness", AnariValue::from_float(0.0f));
    p.ior = constant("IOR", 1.5f);
    p.transmission = AnariValue::from_float(1.0f);
    normal_map("Normal");
    return p;
  }

  if (type == SheenBsdfNode::get_node_type()) {
    p.has_bsdf = true;
    p.base_color = AnariValue::from_float3(zero_float3());
    p.sheen_color = value("Color", AnariValue::from_float3(one_float3()));
    p.sheen_roughness = value("Roughness", AnariValue::from_float(0.5f));
    return p;
  }

  if (type == TransparentBsdfNode::get_node_type() || type == RayPortalBsdfNode::get_node_type()) {
    p.has_bsdf = true;
    p.opacity = AnariValue::from_float(0.0f);
    return p;
  }

  if (type == HoldoutNode::get_node_type()) {
    p.is_holdout = true;
    p.opacity = AnariValue::from_float(0.0f);
    return p;
  }

  if (type == EmissionNode::get_node_type()) {
    const float strength = constant("Strength", 1.0f);
    p.emissive = value("Color", AnariValue::from_float3(one_float3()));
    p.emissive.apply_linear(make_float4(strength, strength, strength, 1.0f), zero_float4());
    p.base_color = AnariValue::from_float3(zero_float3());
    p.specular = AnariValue::from_float(0.0f);
    p.has_emission = true;
    return p;
  }

  if (type == AddClosureNode::get_node_type()) {
    const AnariPrincipled a = evaluate_closure(
        node->input("Closure1") ? node->input("Closure1")->link : nullptr, depth + 1);
    const AnariPrincipled b = evaluate_closure(
        node->input("Closure2") ? node->input("Closure2")->link : nullptr, depth + 1);
    /* Emission added to a BSDF. */
    if (a.has_bsdf && !b.has_bsdf && b.has_emission) {
      AnariPrincipled r = a;
      r.emissive = b.emissive;
      r.has_emission = true;
      return r;
    }
    if (b.has_bsdf && !a.has_bsdf && a.has_emission) {
      AnariPrincipled r = b;
      r.emissive = a.emissive;
      r.has_emission = true;
      return r;
    }
    return AnariPrincipled::mix(a, b, 0.5f);
  }

  if (type == MixClosureNode::get_node_type()) {
    ShaderInput *fac_input = node->input("Fac");
    const AnariPrincipled a = evaluate_closure(
        node->input("Closure1") ? node->input("Closure1")->link : nullptr, depth + 1);
    const AnariPrincipled b = evaluate_closure(
        node->input("Closure2") ? node->input("Closure2")->link : nullptr, depth + 1);

    const auto is_transparent = [](const AnariPrincipled &c) {
      return c.has_bsdf && c.opacity.is_constant() && c.opacity.constant.x == 0.0f &&
             !c.has_emission;
    };

    if (fac_input && fac_input->link) {
      /* Mixing with transparency driven by a texture: alpha mask. */
      AnariValue fac = evaluate_input(fac_input, AnariValue::from_float(0.5f));
      if (is_transparent(a) && !is_transparent(b)) {
        AnariPrincipled r = b;
        r.opacity = fac;
        return r;
      }
      if (is_transparent(b) && !is_transparent(a)) {
        AnariPrincipled r = a;
        fac.apply_linear(make_float4(-1.0f, -1.0f, -1.0f, 1.0f), make_float4(1.0f, 1.0f, 1.0f, 0.0f));
        r.opacity = fac;
        return r;
      }
      return AnariPrincipled::mix(a, b, 0.5f);
    }

    const float fac = input_float(node, "Fac", 0.5f);
    if (is_transparent(a) && !is_transparent(b)) {
      AnariPrincipled r = b;
      r.opacity.apply_linear(make_float4(fac), zero_float4());
      return r;
    }
    if (is_transparent(b) && !is_transparent(a)) {
      AnariPrincipled r = a;
      r.opacity.apply_linear(make_float4(1.0f - fac), zero_float4());
      return r;
    }
    return AnariPrincipled::mix(a, b, fac);
  }

  /* Other closures: approximate with a diffuse material using their color. */
  if (node->input("Color")) {
    p.has_bsdf = true;
    p.base_color = value("Color", p.base_color);
    p.specular = AnariValue::from_float(0.0f);
    LOG_DEBUG << "ANARI: approximating closure " << type->name << " with a diffuse material";
    return p;
  }

  LOG_DEBUG << "ANARI: unsupported closure " << type->name;
  return p;
}

AnariPrincipled AnariMaterialBuilder::evaluate_surface(Shader *shader)
{
  AnariPrincipled p;
  if (shader == nullptr || !shader->graph) {
    return p;
  }

  OutputNode *output = shader->graph->output();
  ShaderInput *surface = output ? output->input("Surface") : nullptr;
  if (surface == nullptr || surface->link == nullptr) {
    /* No surface: invisible object (volume only or empty). */
    p.has_bsdf = false;
    p.opacity = AnariValue::from_float(shader->has_volume ? 0.0f : 1.0f);
    p.base_color = AnariValue::from_float3(zero_float3());
    return p;
  }

  p = evaluate_closure(surface->link, 0);
  if (!p.has_bsdf && !p.has_emission && !p.is_holdout) {
    /* Unknown closure, fall back to the default gray material. */
    p = AnariPrincipled();
    p.has_bsdf = true;
  }
  return p;
}

static bool out_transform_is_identity(const AnariValue &value)
{
  for (int row = 0; row < 4; row++) {
    for (int column = 0; column < 4; column++) {
      if (value.out_row[row][column] != ((row == column) ? 1.0f : 0.0f)) {
        return false;
      }
    }
  }
  return is_zero(value.out_offset);
}

ANARISampler AnariMaterialBuilder::create_sampler(const AnariValue &value)
{
  ANARIDevice device = device_.anari_device();

  ANARISampler sampler = nullptr;
  if (value.type == AnariValue::IMAGE) {
    ANARIArray2D image = images_.get(scene_, value.image);
    if (image == nullptr) {
      return nullptr;
    }

    sampler = anariNewSampler(device, "image2D");
    anariSetParameter(device, sampler, "image", ANARI_ARRAY2D, &image);

    ImageTextureNode *image_node = (value.image->type == ImageTextureNode::get_node_type()) ?
                                       static_cast<ImageTextureNode *>(value.image) :
                                       nullptr;
    const char *wrap = "repeat";
    if (image_node) {
      switch (image_node->get_extension()) {
        case EXTENSION_EXTEND:
          wrap = "clampToEdge";
          break;
        case EXTENSION_MIRROR:
          wrap = "mirrorRepeat";
          break;
        case EXTENSION_CLIP:
          wrap = "clampToBorder";
          break;
        case EXTENSION_REPEAT:
        default:
          wrap = "repeat";
          break;
      }
    }
    anariSetParameter(device, sampler, "wrapMode1", ANARI_STRING, wrap);
    anariSetParameter(device, sampler, "wrapMode2", ANARI_STRING, wrap);

    const bool nearest = image_node && image_node->get_interpolation() == INTERPOLATION_CLOSEST;
    anariSetParameter(device, sampler, "filter", ANARI_STRING, nearest ? "nearest" : "linear");
  }
  else if (value.type == AnariValue::ATTRIBUTE) {
    sampler = anariNewSampler(device, "transform");
  }
  else {
    return nullptr;
  }

  anariSetParameter(device, sampler, "inAttribute", ANARI_STRING, value.attribute.c_str());

  /* Column-major 4x4 matrices. */
  const Transform &t = value.in_transform;
  const float in_transform[16] = {t.x.x,
                                  t.y.x,
                                  t.z.x,
                                  0.0f,
                                  t.x.y,
                                  t.y.y,
                                  t.z.y,
                                  0.0f,
                                  t.x.z,
                                  t.y.z,
                                  t.z.z,
                                  0.0f,
                                  t.x.w,
                                  t.y.w,
                                  t.z.w,
                                  1.0f};
  /* Transforms are only set when they do something, not all devices support them. */
  if (value.type == AnariValue::IMAGE && !transform_equal_threshold(t, transform_identity(), 0.0f))
  {
    anariSetParameter(device, sampler, "inTransform", ANARI_FLOAT32_MAT4, in_transform);
  }

  if (!out_transform_is_identity(value)) {
    float out_transform[16];
    for (int column = 0; column < 4; column++) {
      for (int row = 0; row < 4; row++) {
        out_transform[column * 4 + row] = value.out_row[row][column];
      }
    }
    anariSetParameter(device, sampler, "outTransform", ANARI_FLOAT32_MAT4, out_transform);
    anariSetParameter(device, sampler, "outOffset", ANARI_FLOAT32_VEC4, &value.out_offset);
  }

  anariCommitParameters(device, sampler);
  return sampler;
}


void AnariMaterialBuilder::set_value(ANARIMaterial material,
                                     const char *name,
                                     const AnariValue &value,
                                     const int num_components)
{
  ANARIDevice device = device_.anari_device();
  ANARI_TRACE("material %s (%s)", name, value.is_constant() ? "constant" : "sampler");

  if (value.is_constant()) {
    switch (num_components) {
      case 1:
        anariSetParameter(device, material, name, ANARI_FLOAT32, &value.constant.x);
        break;
      case 3:
        anariSetParameter(device, material, name, ANARI_FLOAT32_VEC3, &value.constant);
        break;
      default:
        anariSetParameter(device, material, name, ANARI_FLOAT32_VEC4, &value.constant);
        break;
    }
    return;
  }

  /* Attributes without any transformation can be referenced by name directly. */
  if (value.type == AnariValue::ATTRIBUTE && out_transform_is_identity(value)) {
    anariSetParameter(device, material, name, ANARI_STRING, value.attribute.c_str());
    return;
  }

  ANARISampler sampler = create_sampler(value);
  if (sampler) {
    anariSetParameter(device, material, name, ANARI_SAMPLER, &sampler);
    anariRelease(device, sampler);
  }
}

void AnariMaterialBuilder::set_material(ANARIMaterial material,
                                        const char *subtype,
                                        const AnariPrincipled &params)
{
  ANARIDevice device = device_.anari_device();
  anariUnsetAllParameters(device, material);

  const bool use_alpha = !params.opacity.is_constant() || params.opacity.constant.x < 1.0f;

  if (strcmp(subtype, "matte") == 0) {
    set_value(material, "color", params.base_color, 3);
    set_value(material, "opacity", params.opacity, 1);
    if (use_alpha) {
      anariSetParameter(device, material, "alphaMode", ANARI_STRING, "blend");
    }
    anariCommitParameters(device, material);
    return;
  }

  set_value(material, "baseColor", params.base_color, 3);
  set_value(material, "metallic", params.metallic, 1);
  set_value(material, "roughness", params.roughness, 1);
  set_value(material, "opacity", params.opacity, 1);
  if (use_alpha) {
    anariSetParameter(device, material, "alphaMode", ANARI_STRING, "blend");
  }
  anariSetParameter(device, material, "ior", ANARI_FLOAT32, &params.ior);
  set_value(material, "transmission", params.transmission, 1);
  if (!params.transmission.is_constant() || params.transmission.constant.x > 0.0f) {
    /* The transmission of Cycles is refractive: the surface bounds a volume. The default wall
     * thickness of 0 means thin-walled glass without refraction, any other value makes the
     * object solid. Ray tracers only use it as that switch. */
    const float thickness = 1.0f;
    anariSetParameter(device, material, "thickness", ANARI_FLOAT32, &thickness);
  }
  set_value(material, "specular", params.specular, 1);
  set_value(material, "specularColor", params.specular_color, 3);
  set_value(material, "clearcoat", params.clearcoat, 1);
  set_value(material, "clearcoatRoughness", params.clearcoat_roughness, 1);
  set_value(material, "sheenColor", params.sheen_color, 3);
  set_value(material, "sheenRoughness", params.sheen_roughness, 1);
  if (params.has_emission) {
    set_value(material, "emissive", params.emissive, 3);
  }
  if (params.iridescence > 0.0f) {
    anariSetParameter(device, material, "iridescence", ANARI_FLOAT32, &params.iridescence);
    anariSetParameter(
        device, material, "iridescenceThickness", ANARI_FLOAT32, &params.iridescence_thickness);
    anariSetParameter(
        device, material, "iridescenceIor", ANARI_FLOAT32, &params.iridescence_ior);
  }
  if (params.has_normal_map) {
    set_value(material, "normal", params.normal_map, 3);
  }

  anariCommitParameters(device, material);
}

static float3 input_float3(ShaderNode *node, const char *name, const float3 default_value)
{
  ShaderInput *input = node->input(name);
  if (input == nullptr || input->link) {
    return default_value;
  }
  return make_float3(input_constant(input, AnariValue::from_float3(default_value)).constant);
}

/* Volume closure of a node, combining mix and add closures by weighting the coefficients. */
static AnariMaterialBuilder::Volume evaluate_volume_closure(ShaderOutput *output, const int depth)
{
  AnariMaterialBuilder::Volume volume;
  if (output == nullptr || depth > 16) {
    return volume;
  }
  ShaderNode *node = output->parent;
  const NodeType *type = node->type;

  if (type == PrincipledVolumeNode::get_node_type()) {
    const PrincipledVolumeNode *principled = static_cast<const PrincipledVolumeNode *>(node);
    volume.valid = true;
    volume.color = input_float3(node, "Color", volume.color);
    volume.density = input_float(node, "Density", 1.0f);
    volume.anisotropy = input_float(node, "Anisotropy", 0.0f);
    volume.absorption_color = input_float3(node, "Absorption Color", zero_float3());
    volume.emission_color = input_float3(node, "Emission Color", one_float3());
    volume.emission_strength = input_float(node, "Emission Strength", 0.0f);
    if (!principled->get_density_attribute().empty()) {
      volume.density_attribute = principled->get_density_attribute();
    }
    return volume;
  }
  if (type == ScatterVolumeNode::get_node_type()) {
    volume.valid = true;
    volume.color = input_float3(node, "Color", volume.color);
    volume.density = input_float(node, "Density", 1.0f);
    volume.anisotropy = input_float(node, "Anisotropy", 0.0f);
    return volume;
  }
  if (type == AbsorptionVolumeNode::get_node_type()) {
    /* Absorption of the complementary color, without scattering. */
    volume.valid = true;
    volume.absorption_color = input_float3(node, "Color", volume.color);
    volume.color = zero_float3();
    volume.density = input_float(node, "Density", 1.0f);
    return volume;
  }
  if (type == EmissionNode::get_node_type()) {
    volume.valid = true;
    volume.density = 0.0f;
    volume.emission_color = input_float3(node, "Color", one_float3());
    volume.emission_strength = input_float(node, "Strength", 1.0f);
    return volume;
  }
  if (type == MixClosureNode::get_node_type() || type == AddClosureNode::get_node_type()) {
    const bool mix = type == MixClosureNode::get_node_type();
    const float fac = mix ? clamp(input_float(node, "Fac", 0.5f), 0.0f, 1.0f) : 0.5f;
    ShaderInput *in1 = node->input("Closure1");
    ShaderInput *in2 = node->input("Closure2");
    const AnariMaterialBuilder::Volume a = evaluate_volume_closure(in1 ? in1->link : nullptr,
                                                                   depth + 1);
    const AnariMaterialBuilder::Volume b = evaluate_volume_closure(in2 ? in2->link : nullptr,
                                                                   depth + 1);
    if (!a.valid || !b.valid) {
      return a.valid ? a : b;
    }
    /* Weighted coefficients: an add closure sums them, a mix closure interpolates them. */
    const float wa = mix ? 1.0f - fac : 1.0f;
    const float wb = mix ? fac : 1.0f;
    volume = a;
    const float density = wa * a.density + wb * b.density;
    if (density > 0.0f) {
      volume.color = (wa * a.density * a.color + wb * b.density * b.color) / density;
      volume.absorption_color = (wa * a.density * a.absorption_color +
                                 wb * b.density * b.absorption_color) /
                                density;
    }
    volume.density = density;
    volume.emission_color = wa * a.emission_color * a.emission_strength +
                            wb * b.emission_color * b.emission_strength;
    volume.emission_strength = 1.0f;
    return volume;
  }
  return volume;
}

AnariMaterialBuilder::Volume AnariMaterialBuilder::evaluate_volume(Shader *shader)
{
  if (shader == nullptr || !shader->graph) {
    return Volume();
  }
  OutputNode *output = shader->graph->output();
  ShaderInput *input = output ? output->input("Volume") : nullptr;
  return evaluate_volume_closure(input ? input->link : nullptr, 0);
}

AnariMaterialBuilder::Background AnariMaterialBuilder::evaluate_background(Shader *shader)
{
  Background background;
  if (shader == nullptr || !shader->graph) {
    return background;
  }

  OutputNode *output = shader->graph->output();
  ShaderInput *surface = output ? output->input("Surface") : nullptr;
  ShaderOutput *link = surface ? surface->link : nullptr;

  /* Find the background node, looking through mix and add closures. */
  for (int depth = 0; link && depth < 16; depth++) {
    ShaderNode *node = link->parent;
    if (node->type == BackgroundNode::get_node_type()) {
      break;
    }
    if (node->type == MixClosureNode::get_node_type() ||
        node->type == AddClosureNode::get_node_type())
    {
      ShaderInput *in1 = node->input("Closure1");
      ShaderInput *in2 = node->input("Closure2");
      link = (in2 && in2->link) ? in2->link : (in1 ? in1->link : nullptr);
      continue;
    }
    link = nullptr;
  }

  if (link == nullptr) {
    return background;
  }

  ShaderNode *node = link->parent;
  background.strength = input_float(node, "Strength", 1.0f);

  ShaderInput *color = node->input("Color");
  if (color && color->link) {
    const AnariValue value = evaluate_output(color->link, AnariValue::from_float3(zero_float3()), 0);
    if (value.type == AnariValue::IMAGE &&
        value.image->type == EnvironmentTextureNode::get_node_type())
    {
      background.image = value.image;
      background.mapping = value.in_transform;
      background.color = make_float3(value.out_row[0].x, value.out_row[1].y, value.out_row[2].z);
    }
    else if (value.is_constant()) {
      background.color = make_float3(value.constant);
    }
    else {
      /* Unsupported procedural background (sky texture, ...), use a neutral gray. */
      background.color = make_float3(0.05f, 0.05f, 0.05f);
      LOG_WARNING << "ANARI: unsupported world shader, using a constant background";
    }
  }
  else if (color) {
    background.color = node->get_float3(color->socket_type);
  }

  return background;
}

CCL_NAMESPACE_END

#endif /* WITH_ANARI */
