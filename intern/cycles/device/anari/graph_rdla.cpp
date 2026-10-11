/* SPDX-FileCopyrightText: 2011-2026 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

#ifdef WITH_ANARI

#  include "device/anari/device_impl.h"
#  include "device/anari/graph_export.h"
#  include "device/anari/material.h"

#  include "scene/shader.h"
#  include "scene/shader_graph.h"
#  include "scene/shader_nodes.h"

#  include "kernel/svm/types.h"

#  include "util/color.h"
#  include "util/colorspace.h"
#  include "util/log.h"
#  include "util/map.h"
#  include "util/set.h"
#  include "util/transform.h"

/* The sky model, which is not in the include path of this library. */
#  include "../../../sky/include/sky_hosek.h"

#  include <algorithm>
#  include <cctype>
#  include <cmath>
#  include <memory>

CCL_NAMESPACE_BEGIN

namespace {

/* The surface shader of a material is written as MoonRay scene objects in RDLA, one statement per
 * object:
 *
 *   DwaBaseMaterial("/blender/Name/DwaBaseMaterial3") {
 *       ["albedo"] = bind(ImageMap("/blender/Name/ImageMap1"), Rgb(1, 1, 1)),
 *   }
 *
 * Only classes of MoonRay and moonshine are used, and the bump map of openmoonray-anari
 * (AnariBumpNormalMap). The closures of the graph become one DwaBaseMaterial: a Mix Shader mixes
 * the attributes of its two sides, weighted by the lobes they belong to. The value nodes become
 * maps, often several for one node. What has no counterpart is replaced by the closest thing
 * there is and reported in the notes of the graph source. */

/* A bindable attribute has a value and optionally a map, the two are multiplied. Values of the
 * Cycles graph are therefore kept as a constant factor times a map. */
struct RdlValue {
  float3 scale = one_float3();
  /* Object expression of the map, empty for a constant. */
  string map;
  /* All channels are equal: the value of a float socket. */
  bool scalar = false;
  /* A direction in the render space of MoonRay, which is the world space rotated: normals and
   * the viewing direction are kept there as long as they only meet each other, which saves the
   * maps which transform them. */
  bool render_space = false;
  /* The third component is zero. */
  bool planar = false;

  bool is_constant() const
  {
    return map.empty();
  }
};

/* Operations of the OpMap class. */
enum {
  OP_ADD = 0,
  OP_SUBTRACT = 1,
  OP_MULTIPLY = 2,
  OP_DIVIDE = 3,
  OP_MAXIMUM = 4,
  OP_MINIMUM = 5,
  OP_POWER = 6,
  OP_CROSS = 7,
  OP_DOT = 8,
  OP_INVERT = 9,
  OP_NORMALIZE = 10,
  OP_OP1 = 11,
  OP_OVERLAY = 13,
  OP_SCREEN = 14,
  OP_ABS = 15,
  OP_CEIL = 16,
  OP_FLOOR = 17,
  OP_MODULO = 18,
  OP_FRACTION = 19,
  OP_LENGTH = 20,
  OP_SINE = 21,
  OP_COSINE = 22,
  OP_ROUND = 23,
  OP_ACOS = 24,
  OP_LESS_THAN = 25,
  OP_LESS_EQUAL = 26,
  OP_GREATER_THAN = 27,
  OP_GREATER_EQUAL = 28,
  OP_EQUAL = 29,
};

/* Modes of the RgbToFloatMap class. */
enum {
  CHANNEL_R = 0,
  CHANNEL_G = 1,
  CHANNEL_B = 2,
  CHANNEL_MIN = 3,
  CHANNEL_MAX = 4,
  CHANNEL_AVERAGE = 5,
  CHANNEL_SUM = 6,
  CHANNEL_LUMINANCE = 7
};

/* Spaces of the TransformSpaceMap class. */
enum {
  SPACE_RENDER = 0,
  SPACE_CAMERA = 1,
  SPACE_WORLD = 2,
  SPACE_SCREEN = 3,
  SPACE_OBJECT = 4,
  SPACE_INSTANCE = 7,
};
enum { TRANSFORM_POINT = 0, TRANSFORM_VECTOR = 1, TRANSFORM_NORMAL = 2 };

/* The kinds of ray MoonRay can have another material for (RaySwitchMaterial), and shadow rays,
 * which only see the presence of a material. */
enum { RAY_CAMERA = 0, RAY_DIFFUSE, RAY_GLOSSY, RAY_SINGULAR, RAY_SHADOW, RAY_NUM };

/* A RampMap takes this many control points. */
const int RAMP_MAX_POINTS = 10;

string number(const float value)
{
  return string_printf("%.9g", double(value));
}

string rgb(const float3 value)
{
  return string_printf("Rgb(%.9g, %.9g, %.9g)", double(value.x), double(value.y), double(value.z));
}

string vec3(const float3 value)
{
  return string_printf(
      "Vec3(%.9g, %.9g, %.9g)", double(value.x), double(value.y), double(value.z));
}

bool is_uniform(const float3 value)
{
  return value.x == value.y && value.y == value.z;
}

float luminance(const float3 value)
{
  return 0.2126f * value.x + 0.7152f * value.y + 0.0722f * value.z;
}

/* A mapping of texture coordinates which changes nothing yet. */
TextureMapping identity_mapping()
{
  TextureMapping mapping;
  mapping.translation = zero_float3();
  mapping.rotation = zero_float3();
  mapping.scale = one_float3();
  mapping.min = zero_float3();
  mapping.max = one_float3();
  mapping.use_minmax = false;
  mapping.type = TextureMapping::POINT;
  mapping.x_mapping = TextureMapping::X;
  mapping.y_mapping = TextureMapping::Y;
  mapping.z_mapping = TextureMapping::Z;
  mapping.projection = TextureMapping::FLAT;
  return mapping;
}

ShaderInput *find_input(ShaderNode *node, const char *name)
{
  for (ShaderInput *input : node->inputs) {
    if (input->socket_type.name == name) {
      return input;
    }
  }
  return nullptr;
}

/* The weight of a lobe of the material. Weights mix linearly, and most of them are never needed
 * as a map: they are kept as expressions, and written when an attribute needs one. */
struct Weight;
using WeightP = std::shared_ptr<Weight>;
struct Weight {
  enum Kind { CONSTANT, VALUE, MIX };
  Kind kind = CONSTANT;
  float constant = 0.0f;
  /* VALUE, and the factor of MIX. */
  RdlValue value;
  WeightP a, b;
  bool written = false;
  RdlValue written_value;
};

/* The closures of the graph as lobes of one DwaBaseMaterial. The material is a hierarchy: a
 * part of the surface is there at all (presence), a part of that is a conductor, a part of the
 * rest refracts, a part of what is left then is diffuse transmission and the rest diffuse
 * reflection. The fractions are values of their own; the weights say how much of the whole each
 * part is, which is what mixing two materials weights their attributes with. */
struct Lobes {
  /* Weights: the surface, the conductor, what is not conductor, refraction, the diffuse part,
   * diffuse reflection, diffuse transmission, and what has a specular roughness. */
  WeightP w_surface, w_conductor, w_rest, w_refraction, w_diffuse_part, w_diffuse, w_translucent,
      w_rough, w_fuzz, w_coat, w_subsurface;
  /* The surface is hair for a part, which is a material of its own, and the lobes above are
   * the rest of it, the body. */
  WeightP w_body, w_hair, w_hair_r, w_hair_tt;
  /* The part of the surface which is hair. */
  RdlValue hair = RdlValue();
  /* Color of the fiber (what it absorbs), and the tints of the reflection, of the transmission
   * and of the second reflection: for the Hair BSDF of Cycles these are its colors. */
  RdlValue hair_color, hair_front, hair_back, hair_second;
  /* How bright a channel of the two colors of the Hair BSDF can be, with what comes out of a
   * map taken to be at most one. */
  float hair_front_most = 0.0f;
  float hair_back_most = 0.0f;
  RdlValue hair_roughness, hair_back_roughness, hair_azimuthal_roughness;
  float hair_second_roughness = 1.0f;
  float hair_offset = 0.0f;
  float hair_ior = 1.55f;
  /* The Principled Hair BSDF, a fiber with absorption, rather than the two lobes of the Hair
   * BSDF. */
  bool hair_principled = false;

  /* Fractions, each of the part above it. */
  RdlValue conductor = RdlValue();
  RdlValue refraction = RdlValue();
  RdlValue translucent = RdlValue();
  /* Weight of the dielectric reflection on what is not conductor. */
  RdlValue dielectric = RdlValue();

  RdlValue albedo;
  RdlValue translucent_color;
  RdlValue refraction_color;
  RdlValue conductor_color;
  RdlValue conductor_edge_color;
  RdlValue roughness;
  RdlValue anisotropy;
  RdlValue emission;
  RdlValue fuzz, fuzz_color, fuzz_roughness;
  RdlValue coat, coat_roughness;
  /* Subsurface scattering: the longest radius of Cycles, the radius of each channel relative
   * to it, and what gives the radius its meaning: the method of Cycles, with its index of
   * refraction and anisotropy. */
  RdlValue subsurface_radius, subsurface_color;
  int subsurface_method = CLOSURE_BSSRDF_RANDOM_WALK_ID;
  float subsurface_ior = 1.4f;
  float subsurface_anisotropy = 0.0f;
  float ior = 1.5f;
  float coat_ior = 1.5f;
  bool has_ior = false;
  bool beckmann = false;
  bool has_distribution = false;
  /* Object expression of the normal map, and how much of it is used. */
  string normal;
  RdlValue normal_dial;

  /* Nothing but emission: added to another material by an Add Shader. */
  bool only_emission = false;
  /* Nothing but transparency. */
  bool only_transparent = false;
  /* One glossy BSDF with a constant color: `conductor` is its weight. */
  bool leaf_glossy = false;
};

class RdlaWriter {
 public:
  RdlaWriter(const Shader *shader, AnariAttributeSlots &slots, AnariGraphSource &source)
      : shader_(shader), slots_(slots), source_(source)
  {
    /* Object names are unique per material and never in the name space of the device. */
    prefix_ = "/blender/";
    for (const char c : shader->name.string()) {
      prefix_ += (isalnum(uchar(c)) || c == '_' || c == '-' || c == '.') ? c : '_';
    }
    prefix_ += '/';
    use_bump_ = getenv("CYCLES_ANARI_RDLA_NO_BUMP") == nullptr;
  }

  bool write()
  {
    ShaderGraph *graph = shader_->graph.get();
    if (graph == nullptr) {
      return false;
    }
    ShaderInput *surface = graph->output()->input("Surface");
    if (surface == nullptr || surface->link == nullptr) {
      return false;
    }

    const string root = root_material(surface->link);
    if (root.empty()) {
      return false;
    }
    write_emitter(surface->link);

    string &text = source_.text;
    text.clear();
    for (const Object &object : objects_) {
      text += object.expr;
      text += " {\n";
      text += object.body;
      text += "}\n";
    }

    source_.format = ANARI_GRAPH_RDLA;
    source_.hash = anari_graph_hash(text);
    source_.material_name = root;
    source_.graph = graph;
    for (const string &note : source_.notes) {
      ANARI_TRACE("graph %s: %s", shader_->name.c_str(), note.c_str());
    }
    return true;
  }

 private:
  struct Object {
    string name;
    string expr;
    string body;
  };

  /* Objects. */

  int new_object(const char *class_name)
  {
    /* The class is part of the name: a MoonRay object can't change its class, and the same
     * names are used again when the material is edited. */
    Object object;
    object.name = prefix_ + class_name + to_string(objects_.size());
    object.expr = string(class_name) + "(\"" + object.name + "\")";
    objects_.push_back(object);
    return int(objects_.size()) - 1;
  }

  void set(const int object, const char *attribute, const string &value)
  {
    string &body = objects_[object].body;
    body += "    [\"";
    body += attribute;
    body += "\"] = ";
    body += value;
    body += ",\n";
  }

  void set(const int object, const char *attribute, const int value)
  {
    set(object, attribute, to_string(value));
  }

  void set(const int object, const char *attribute, const bool value)
  {
    set(object, attribute, string(value ? "true" : "false"));
  }

  void set(const int object, const char *attribute, const char *value)
  {
    set(object, attribute, string(value));
  }

  static string quoted(const string &text)
  {
    string result = "\"";
    for (const char c : text) {
      if (c == '"' || c == '\\') {
        result += '\\';
      }
      result += c;
    }
    return result + "\"";
  }

  RdlValue map_value(const int object, const bool scalar = false)
  {
    RdlValue result;
    result.map = objects_[object].expr;
    result.scalar = scalar;
    return result;
  }

  /* The expression of the object of this name. */
  string expr_of(const string &name) const
  {
    for (const Object &object : objects_) {
      if (object.name == name) {
        return object.expr;
      }
    }
    return string();
  }

  /* What the export had to approximate, once each. */
  void note(const string &text)
  {
    if (std::find(source_.notes.begin(), source_.notes.end(), text) == source_.notes.end()) {
      source_.notes.push_back(text);
    }
  }

  void note(const ShaderNode *node, const string &text)
  {
    note(node->type->name.string() + ": " + text);
  }

  /* Constants. */

  static RdlValue constant(const float3 value, const bool scalar = false)
  {
    RdlValue result;
    result.scale = value;
    result.scalar = scalar;
    return result;
  }

  static RdlValue constant(const float value)
  {
    return constant(make_float3(value), true);
  }

  static bool is_constant(const RdlValue &value, const float f)
  {
    return value.is_constant() && value.scale == make_float3(f);
  }

  static bool same_value(const RdlValue &a, const RdlValue &b)
  {
    return a.map == b.map && a.scale == b.scale;
  }

  static RdlValue scaled(const RdlValue &value, const float3 factor)
  {
    RdlValue result = value;
    result.scale *= factor;
    result.scalar = value.scalar && is_uniform(factor);
    result.planar = value.planar;
    return result;
  }

  static RdlValue scaled(const RdlValue &value, const float factor)
  {
    return scaled(value, make_float3(factor));
  }

  static RdlValue as_scalar(const RdlValue &value)
  {
    RdlValue result = value;
    result.scalar = true;
    result.render_space = false;
    return result;
  }

  /* Bindings. */

  /* Value of a bindable Rgb attribute. A map is not evaluated when its factor is black. */
  string bind_rgb(const RdlValue &value)
  {
    if (value.is_constant() || is_zero(value.scale)) {
      return rgb(value.scale);
    }
    return "bind(" + value.map + ", " + rgb(value.scale) + ")";
  }

  /* Value of a bindable Float attribute: the average of the channels of a bound map, times the
   * value. Colors are converted with their luminance, like in Cycles. */
  string bind_float(const RdlValue &value)
  {
    if (value.is_constant()) {
      return number(value.scalar ? value.scale.x : luminance(value.scale));
    }
    if (value.scalar && is_uniform(value.scale)) {
      if (value.scale.x == 0.0f) {
        return "0";
      }
      return "bind(" + value.map + ", " + number(value.scale.x) + ")";
    }
    const RdlValue channel = to_float(value, value.scalar ? CHANNEL_AVERAGE : CHANNEL_LUMINANCE);
    return "bind(" + channel.map + ", 1)";
  }

  /* Value of a bindable Vec3f attribute. The value of the attribute is not multiplied with a
   * bound map there, so the factor has to be in the map. */
  string bind_vec3(const RdlValue &value)
  {
    if (value.is_constant()) {
      return vec3(value.scale);
    }
    if (value.scale == one_float3()) {
      return "bind(" + value.map + ", Vec3(1, 1, 1))";
    }
    const int object = new_object("OpMap");
    set(object, "operation", OP_OP1);
    set(object, "op1", bind_rgb(value));
    return "bind(" + objects_[object].expr + ", Vec3(1, 1, 1))";
  }

  /* Operations. */

  static float fold_channel(const int operation, const float a, const float b)
  {
    switch (operation) {
      case OP_ADD:
        return a + b;
      case OP_SUBTRACT:
        return a - b;
      case OP_MULTIPLY:
        return a * b;
      case OP_DIVIDE:
        return a / max(b, 0.00001f);
      case OP_MAXIMUM:
        return max(a, b);
      case OP_MINIMUM:
        return min(a, b);
      case OP_POWER:
        return powf(max(a, 1e-6f), b);
      case OP_INVERT:
        return 1.0f - a;
      case OP_ABS:
        return fabsf(a);
      case OP_CEIL:
        return ceilf(a);
      case OP_FLOOR:
        return floorf(a);
      case OP_MODULO:
        return fmodf(a, max(b, 0.00001f));
      case OP_FRACTION:
        return a - floorf(a);
      case OP_SINE:
        return sinf(a);
      case OP_COSINE:
        return cosf(a);
      case OP_ROUND:
        return float(int(a < 0.0f ? a - 0.5f : a + 0.5f));
      case OP_ACOS:
        return acosf(clamp(a, -1.0f, 1.0f));
      case OP_LESS_THAN:
        return (a < b) ? 1.0f : 0.0f;
      case OP_LESS_EQUAL:
        return (a <= b) ? 1.0f : 0.0f;
      case OP_GREATER_THAN:
        return (a > b) ? 1.0f : 0.0f;
      case OP_GREATER_EQUAL:
        return (a >= b) ? 1.0f : 0.0f;
      case OP_OP1:
        return a;
      default:
        return a;
    }
  }

  static float3 fold(const int operation, const float3 a, const float3 b)
  {
    switch (operation) {
      case OP_CROSS:
        return cross(a, b);
      case OP_DOT:
        return make_float3(dot(a, b));
      case OP_LENGTH:
        return make_float3(len(a));
      case OP_NORMALIZE:
        return safe_normalize(a);
      case OP_OVERLAY:
      case OP_SCREEN: {
        float3 result;
        for (int i = 0; i < 3; i++) {
          if (operation == OP_SCREEN) {
            result[i] = 1.0f - (1.0f - a[i]) * (1.0f - b[i]);
          }
          else {
            result[i] = (b[i] < 0.5f) ? a[i] * b[i] * 2.0f :
                                        1.0f - (1.0f - a[i]) * (1.0f - b[i]) * 2.0f;
          }
        }
        return saturate(result);
      }
      default:
        return make_float3(fold_channel(operation, a.x, b.x),
                           fold_channel(operation, a.y, b.y),
                           fold_channel(operation, a.z, b.z));
    }
  }

  /* An OpMap, or the result itself when the operands are constant. */
  RdlValue op(const int operation, const RdlValue &a, const RdlValue *b = nullptr)
  {
    const bool to_scalar = operation == OP_DOT || operation == OP_LENGTH;
    const bool vector_op = to_scalar || operation == OP_CROSS || operation == OP_NORMALIZE;
    RdlValue result;
    if (a.is_constant() && (b == nullptr || b->is_constant())) {
      result.scale = fold(operation, a.scale, b ? b->scale : one_float3());
    }
    else {
      const int object = new_object("OpMap");
      set(object, "operation", operation);
      set(object, "op1", bind_rgb(a));
      if (b) {
        set(object, "op2", bind_rgb(*b));
      }
      result.map = objects_[object].expr;
    }
    result.scalar = to_scalar || (!vector_op && a.scalar && (b == nullptr || b->scalar));
    return result;
  }

  RdlValue op(const int operation, const RdlValue &a, const RdlValue &b)
  {
    return op(operation, a, &b);
  }

  RdlValue multiply(const RdlValue &a, const RdlValue &b)
  {
    if (a.is_constant()) {
      RdlValue result = scaled(b, a.scale);
      result.scalar = a.scalar && b.scalar;
      result.render_space = b.render_space && a.scalar;
      return result;
    }
    if (b.is_constant()) {
      RdlValue result = scaled(a, b.scale);
      result.scalar = a.scalar && b.scalar;
      result.render_space = a.render_space && b.scalar;
      return result;
    }
    return op(OP_MULTIPLY, a, b);
  }

  RdlValue add(const RdlValue &a, const RdlValue &b)
  {
    if (is_constant(a, 0.0f)) {
      return b;
    }
    if (is_constant(b, 0.0f)) {
      return a;
    }
    return op(OP_ADD, a, b);
  }

  RdlValue subtract(const RdlValue &a, const RdlValue &b)
  {
    if (is_constant(b, 0.0f)) {
      return a;
    }
    return op(OP_SUBTRACT, a, b);
  }

  /* a / b where b is not negative: the OpMap divides by at least a small positive number. */
  RdlValue divide(const RdlValue &a, const RdlValue &b)
  {
    if (b.is_constant() && b.scale.x > 0.0f && b.scale.y > 0.0f && b.scale.z > 0.0f) {
      return multiply(a, constant(one_float3() / b.scale, b.scalar));
    }
    return op(OP_DIVIDE, a, b);
  }

  /* 1 - a */
  RdlValue invert(const RdlValue &a)
  {
    return op(OP_INVERT, a);
  }

  /* a * factor + offset */
  RdlValue affine(const RdlValue &a, const float3 factor, const float3 offset)
  {
    RdlValue result = add(scaled(a, factor), constant(offset, is_uniform(offset)));
    result.scalar = a.scalar && is_uniform(factor) && is_uniform(offset);
    return result;
  }

  RdlValue affine(const RdlValue &a, const float factor, const float offset)
  {
    return affine(a, make_float3(factor), make_float3(offset));
  }

  RdlValue clamp_value(const RdlValue &a, const float low, const float high)
  {
    if (a.is_constant()) {
      return constant(clamp(a.scale, make_float3(low), make_float3(high)), a.scalar);
    }
    const int object = new_object("ClampMap");
    set(object, "input", bind_rgb(a));
    set(object, "clamp_min", number(low));
    set(object, "clamp_max", number(high));
    return map_value(object, a.scalar);
  }

  /* One channel, or a weighted sum of the channels, as a float value. */
  RdlValue to_float(const RdlValue &value, const int mode)
  {
    const float3 c = value.scale;
    if (value.is_constant()) {
      float f = 0.0f;
      switch (mode) {
        case CHANNEL_R:
          f = c.x;
          break;
        case CHANNEL_G:
          f = c.y;
          break;
        case CHANNEL_B:
          f = c.z;
          break;
        case CHANNEL_MIN:
          f = reduce_min(c);
          break;
        case CHANNEL_MAX:
          f = reduce_max(c);
          break;
        case CHANNEL_SUM:
          f = c.x + c.y + c.z;
          break;
        case CHANNEL_LUMINANCE:
          f = luminance(c);
          break;
        default:
          f = (c.x + c.y + c.z) / 3.0f;
          break;
      }
      return constant(f);
    }
    if (value.scalar && is_uniform(c) && mode != CHANNEL_SUM) {
      /* A grey value already. */
      return as_scalar(value);
    }
    if (mode == CHANNEL_LUMINANCE) {
      /* The luminance of MoonRay has other weights than the one of Cycles. */
      return op(OP_DOT, value, constant(make_float3(0.2126f, 0.7152f, 0.0722f)));
    }
    const int object = new_object("RgbToFloatMap");
    set(object, "input", bind_rgb(value));
    set(object, "mode", mode);
    return map_value(object, true);
  }

  RdlValue combine(const RdlValue &x, const RdlValue &y, const RdlValue &z)
  {
    if (x.is_constant() && y.is_constant() && z.is_constant()) {
      return constant(make_float3(x.scale.x, y.scale.x, z.scale.x));
    }
    const int object = new_object("FloatToRgbMap");
    set(object, "R", bind_float(x));
    set(object, "G", bind_float(y));
    set(object, "B", bind_float(z));
    RdlValue result = map_value(object);
    result.planar = is_constant(z, 0.0f);
    return result;
  }

  /* lerp(a, b, factor), the factor is clamped to 0..1. */
  RdlValue blend(const RdlValue &a, const RdlValue &b, const RdlValue &factor)
  {
    if (factor.is_constant()) {
      const float f = factor.scalar ? factor.scale.x : luminance(factor.scale);
      if (f <= 0.0f) {
        return a;
      }
      if (f >= 1.0f) {
        return b;
      }
      if (a.is_constant() && b.is_constant()) {
        return constant(mix(a.scale, b.scale, f), a.scalar && b.scalar);
      }
      if (is_constant(a, 0.0f)) {
        return scaled(b, f);
      }
      if (is_constant(b, 0.0f)) {
        return scaled(a, 1.0f - f);
      }
    }
    else if (same_value(a, b)) {
      return a;
    }
    const int object = new_object("BlendMap");
    set(object, "color_A", bind_rgb(a));
    set(object, "color_B", bind_rgb(b));
    set(object, "blend_amount", bind_float(factor));
    RdlValue result = map_value(object, a.scalar && b.scalar);
    result.render_space = a.render_space && b.render_space;
    result.planar = a.planar && b.planar;
    return result;
  }

  /* Directions in render space become directions in world space. */
  RdlValue to_world(const RdlValue &value)
  {
    if (!value.render_space) {
      return value;
    }
    if (value.is_constant()) {
      RdlValue result = value;
      result.render_space = false;
      return result;
    }
    const int object = new_object("TransformSpaceMap");
    set(object, "input", bind_vec3(value));
    set(object, "input_type", TRANSFORM_VECTOR);
    set(object, "from_space", SPACE_RENDER);
    set(object, "to_space", SPACE_WORLD);
    return map_value(object);
  }

  /* Two vectors which are both in render space can stay there for what does not depend on the
   * space, all others are taken to world space. */
  bool same_space(RdlValue &a, RdlValue &b)
  {
    if (a.render_space && b.render_space) {
      return true;
    }
    a = to_world(a);
    b = to_world(b);
    return false;
  }

  /* A lookup table as a RampMap: the control points which reproduce it best with linear
   * interpolation, as many as the map takes. Returns the largest error. */
  float ramp_points(const vector<float3> &table, const bool interpolate, vector<int> &points)
  {
    const int size = int(table.size());
    points.clear();
    if (!interpolate) {
      /* Steps: a point where the value changes. */
      points.push_back(0);
      for (int i = 1; i < size; i++) {
        if (len(table[i] - table[points.back()]) > 1e-5f) {
          points.push_back(i);
        }
      }
      float error = 0.0f;
      while (int(points.size()) > RAMP_MAX_POINTS) {
        /* Drop the smallest step. */
        int smallest = 1;
        float smallest_step = FLT_MAX;
        for (int i = 1; i < int(points.size()); i++) {
          const float step = len(table[points[i]] - table[points[i - 1]]);
          if (step < smallest_step) {
            smallest_step = step;
            smallest = i;
          }
        }
        error = max(error, smallest_step);
        points.erase(points.begin() + smallest);
      }
      return error;
    }

    /* Add the entry with the largest distance to the polyline until it is close enough. */
    points.push_back(0);
    points.push_back(size - 1);
    float error = 0.0f;
    while (true) {
      error = 0.0f;
      int worst = -1;
      for (size_t segment = 0; segment + 1 < points.size(); segment++) {
        const int start = points[segment];
        const int end = points[segment + 1];
        for (int i = start + 1; i < end; i++) {
          const float t = float(i - start) / float(end - start);
          const float distance = len(mix(table[start], table[end], t) - table[i]);
          if (distance > error) {
            error = distance;
            worst = i;
          }
        }
      }
      if (worst < 0 || error < 2e-4f || int(points.size()) >= RAMP_MAX_POINTS) {
        break;
      }
      points.insert(std::upper_bound(points.begin(), points.end(), worst), worst);
    }
    return error;
  }

  /* The table over the input range from..to, looked up with the float value `input`. */
  RdlValue ramp_map(const RdlValue &input,
                    const vector<float3> &table,
                    const bool interpolate,
                    const float from,
                    const float to,
                    const bool scalar,
                    float *max_error = nullptr)
  {
    const int size = int(table.size());
    if (max_error) {
      *max_error = 0.0f;
    }
    if (size == 0) {
      return constant(0.0f);
    }
    if (size == 1) {
      return constant(table[0], scalar);
    }
    const float range = (to - from != 0.0f) ? to - from : 1.0f;
    if (input.is_constant()) {
      /* Look it up here. */
      const float in = input.scalar ? input.scale.x : luminance(input.scale);
      const float t = clamp((in - from) / range, 0.0f, 1.0f) * float(size - 1);
      const int i = clamp(int(t), 0, size - 2);
      return constant((interpolate && t > float(i)) ? mix(table[i], table[i + 1], t - float(i)) :
                                                      table[clamp(int(t), 0, size - 1)],
                      scalar);
    }
    /* The positions are in 0..1. */
    RdlValue normalized = to_float(input, CHANNEL_LUMINANCE);
    if (from != 0.0f || range != 1.0f) {
      normalized = affine(normalized, 1.0f / range, -from / range);
    }
    float error = 0.0f;
    const RdlValue result = ramp_segment(normalized, table, interpolate, 0, size - 1, scalar, 2,
                                         error);
    if (max_error) {
      *max_error = error;
    }
    return result;
  }

  /* The entries first..last of a table as a RampMap of the normalized input. A table which ten
   * control points do not reproduce is cut in two halves with a map each, and a BlendMap which
   * switches between them. */
  RdlValue ramp_segment(const RdlValue &normalized,
                        const vector<float3> &whole,
                        const bool interpolate,
                        const int first,
                        const int last,
                        const bool scalar,
                        const int depth,
                        float &max_error)
  {
    const int size = int(whole.size());
    const vector<float3> table(whole.begin() + first, whole.begin() + last + 1);
    vector<int> points;
    const float error = (table.size() > 1) ? ramp_points(table, interpolate, points) : 0.0f;
    if (points.size() < 2) {
      return constant(table[0], scalar);
    }
    if (error > 0.004f && depth > 0 && last - first >= 8) {
      const int middle = (first + last) / 2;
      const RdlValue low = ramp_segment(
          normalized, whole, interpolate, first, middle, scalar, depth - 1, max_error);
      const RdlValue high = ramp_segment(
          normalized, whole, interpolate, middle, last, scalar, depth - 1, max_error);
      const float position = float(middle) / float(size - 1);
      const int object = new_object("BlendMap");
      set(object, "color_A", bind_rgb(low));
      set(object, "color_B", bind_rgb(high));
      set(object, "blend_amount", bind_float(normalized));
      set(object, "threshold_min", number(position));
      set(object, "threshold_max", number(position));
      return map_value(object, scalar);
    }
    max_error = max(max_error, error);

    string positions = "{";
    string colors = "{";
    string interpolations = "{";
    for (size_t p = 0; p < points.size(); p++) {
      const int i = points[p];
      const char *separator = (p == 0) ? "" : ", ";
      /* A step is at the entry where the value changes. */
      positions += separator + number(float(first + i) / float(size - 1));
      colors += separator + rgb(table[i]);
      /* Linear or none, see RampInterpolatorMode. */
      interpolations += separator + string(interpolate ? "1" : "0");
    }

    const int object = new_object("RampMap");
    set(object, "ramp_type", 8);
    set(object, "input", bind_float(normalized));
    set(object, "positions", positions + "}");
    set(object, "colors", colors + "}");
    set(object, "interpolations", interpolations + "}");
    set(object, "wrap_type", 1);
    return map_value(object, scalar);
  }

  /* A function of a float value in 0..1, as a RampMap. */
  template<typename Function>
  RdlValue function_ramp(const RdlValue &input, Function function, const int size = 256)
  {
    vector<float3> table(size);
    for (int i = 0; i < size; i++) {
      table[i] = make_float3(function(float(i) / float(size - 1)));
    }
    return ramp_map(input, table, true, 0.0f, 1.0f, true);
  }

  /* What the shading point has. */

  template<typename Function> RdlValue cached(const char *key, Function function)
  {
    auto it = shared_.find(key);
    if (it != shared_.end()) {
      return it->second;
    }
    const RdlValue value = function();
    shared_[key] = value;
    return value;
  }

  /* One of the values of the AttributeMap class, see AttributeMapType. */
  RdlValue state_value(const int map_type)
  {
    const int object = new_object("AttributeMap");
    set(object, "map_type", map_type);
    return map_value(object);
  }

  RdlValue primitive_attribute(const string &name, const int type, const float3 missing)
  {
    const int object = new_object("AttributeMap");
    set(object, "map_type", 0);
    set(object, "primitive_attribute_name", quoted(name));
    set(object, "primitive_attribute_type", type);
    set(object, "default_value", rgb(missing));
    return map_value(object, type == 0);
  }

  RdlValue transform_space(const RdlValue &value, const int type, const int from, const int to)
  {
    const int object = new_object("TransformSpaceMap");
    set(object, "input", bind_vec3(value));
    set(object, "input_type", type);
    set(object, "from_space", from);
    set(object, "to_space", to);
    return map_value(object);
  }

  /* From the render space to the space of the object. The object space of MoonRay is the one
   * of the geometry, which for an instanced geometry is not that of the instance. */
  RdlValue to_object_space(const RdlValue &value, const int type)
  {
    const RdlValue result = transform_space(
        transform_space(value, type, SPACE_RENDER, SPACE_OBJECT), type, SPACE_WORLD, SPACE_INSTANCE);
    /* A scaled object changes the length of a normal. */
    return (type == TRANSFORM_NORMAL) ? op(OP_NORMALIZE, result) : result;
  }

  RdlValue render_position()
  {
    return cached("P", [&] { return state_value(1); });
  }

  /* Directions in render space. */
  RdlValue render_normal()
  {
    return cached("N", [&] {
      RdlValue value = state_value(3);
      value.render_space = true;
      return value;
    });
  }

  RdlValue render_incoming()
  {
    return cached("wo", [&] {
      RdlValue value = state_value(20);
      value.render_space = true;
      return value;
    });
  }

  RdlValue world_position()
  {
    return cached("P world", [&] {
      return transform_space(render_position(), TRANSFORM_POINT, SPACE_RENDER, SPACE_WORLD);
    });
  }

  RdlValue object_position()
  {
    return cached("P object",
                  [&] { return to_object_space(render_position(), TRANSFORM_POINT); });
  }

  RdlValue object_normal()
  {
    return cached("N object", [&] {
      RdlValue normal = render_normal();
      normal.render_space = false;
      return to_object_space(normal, TRANSFORM_NORMAL);
    });
  }

  RdlValue camera_position()
  {
    return cached("P camera", [&] {
      /* The camera of MoonRay looks along -z, the one of Cycles along z. */
      return scaled(
          transform_space(render_position(), TRANSFORM_POINT, SPACE_RENDER, SPACE_CAMERA),
          make_float3(1.0f, 1.0f, -1.0f));
    });
  }

  /* The default UV map: the texture coordinates of the MoonRay mesh. */
  RdlValue uv_value()
  {
    return cached("st", [&] {
      RdlValue value = state_value(2);
      value.planar = true;
      return value;
    });
  }

  /* An attribute of the mesh, which the scene export puts into one of the generic attributes of
   * the ANARI geometry. The device has them as primitive attributes of these names. */
  bool slot_value(const ustring name, RdlValue &value)
  {
    const int slot = slots_.slot_for(name);
    if (slot < 0) {
      return false;
    }
    if (slot == 0) {
      value = uv_value();
      return true;
    }
    const string key = string_printf("anari_attribute%d", slot);
    value = cached(key.c_str(), [&] { return primitive_attribute(key, 2, zero_float3()); });
    return true;
  }

  RdlValue generated_value()
  {
    RdlValue value;
    if (slot_value(ustring("generated"), value)) {
      return value;
    }
    note("generated coordinates: no attribute slot is left, object coordinates are used");
    return object_position();
  }

  /* Inputs. */

  bool is_linked(ShaderNode *node, const char *name)
  {
    ShaderInput *input = find_input(node, name);
    return input && input->link;
  }

  RdlValue socket_value(ShaderNode *node, const SocketType &socket)
  {
    switch (socket.type) {
      case SocketType::FLOAT:
        return constant(node->get_float(socket));
      case SocketType::INT:
        return constant(float(node->get_int(socket)));
      case SocketType::BOOLEAN:
        return constant(node->get_bool(socket) ? 1.0f : 0.0f);
      case SocketType::COLOR:
      case SocketType::VECTOR:
      case SocketType::POINT:
      case SocketType::NORMAL:
        return constant(node->get_float3(socket));
      default:
        return constant(0.0f);
    }
  }

  RdlValue input_value(ShaderNode *node, const char *name)
  {
    ShaderInput *input = find_input(node, name);
    if (input == nullptr) {
      return constant(0.0f);
    }
    if (input->link) {
      return output_value(input->link);
    }
    return socket_value(node, input->socket_type);
  }

  /* The same as a float value. */
  RdlValue input_float(ShaderNode *node, const char *name)
  {
    const RdlValue value = input_value(node, name);
    return value.scalar ? value : to_float(to_world(value), CHANNEL_AVERAGE);
  }

  /* The value of an input which has to be a constant here. */
  float input_constant(ShaderNode *node, const char *name, const float fallback = 0.0f)
  {
    ShaderInput *input = find_input(node, name);
    if (input == nullptr) {
      return fallback;
    }
    if (input->link == nullptr && input->socket_type.type == SocketType::FLOAT) {
      return node->get_float(input->socket_type);
    }
    const RdlValue value = input_value(node, name);
    if (value.is_constant()) {
      return value.scalar ? value.scale.x : luminance(value.scale);
    }
    note(node, string("the input '") + name + "' is not a constant, its default " +
                   number(fallback) + " is used");
    return fallback;
  }

  float3 input_constant3(ShaderNode *node, const char *name, const float3 fallback)
  {
    ShaderInput *input = find_input(node, name);
    if (input == nullptr) {
      return fallback;
    }
    const RdlValue value = input_value(node, name);
    if (value.is_constant()) {
      return value.scale;
    }
    note(node, string("the input '") + name + "' is not a constant, its default is used");
    return fallback;
  }

  /* The texture coordinates of a texture node, with its own mapping. */
  RdlValue texture_vector(ShaderNode *node, TextureMapping *mapping)
  {
    ShaderInput *input = find_input(node, "vector");
    RdlValue value = constant(zero_float3());
    if (input && input->link) {
      value = to_world(output_value(input->link));
    }
    else if (input && (input->flags() & SocketType::LINK_TEXTURE_UV)) {
      value = uv_value();
    }
    else if (input && (input->flags() & SocketType::LINK_TEXTURE_GENERATED)) {
      value = generated_value();
    }
    if (mapping && !mapping->skip()) {
      value = transformed(value, mapping->compute_transform());
    }
    return value;
  }

  /* matrix * value */
  RdlValue transformed(const RdlValue &value, const Transform &matrix)
  {
    const float3 rows[3] = {make_float3(matrix.x.x, matrix.x.y, matrix.x.z),
                            make_float3(matrix.y.x, matrix.y.y, matrix.y.z),
                            make_float3(matrix.z.x, matrix.z.y, matrix.z.z)};
    const float3 offset = make_float3(matrix.x.w, matrix.y.w, matrix.z.w);
    if (value.is_constant()) {
      return constant(make_float3(dot(rows[0], value.scale),
                                  dot(rows[1], value.scale),
                                  dot(rows[2], value.scale)) +
                      offset);
    }
    const bool diagonal = rows[0].y == 0.0f && rows[0].z == 0.0f && rows[1].x == 0.0f &&
                          rows[1].z == 0.0f && rows[2].x == 0.0f && rows[2].y == 0.0f;
    RdlValue result;
    if (diagonal) {
      result = scaled(value, make_float3(rows[0].x, rows[1].y, rows[2].z));
      result.scalar = false;
    }
    else {
      RdlValue channels[3];
      for (int i = 0; i < 3; i++) {
        channels[i] = is_zero(rows[i]) ? constant(0.0f) : op(OP_DOT, value, constant(rows[i]));
      }
      result = combine(channels[0], channels[1], channels[2]);
    }
    result.planar = (value.planar || is_zero(rows[2])) && offset.z == 0.0f;
    if (!is_zero(offset)) {
      const bool planar = result.planar;
      result = add(result, constant(offset));
      result.planar = planar;
    }
    return result;
  }

  /* Values. */

  /* Whether what the node computes depends on the kind of ray, by a Light Path node. */
  bool depends_on_ray(ShaderNode *node)
  {
    auto it = ray_dependent_.find(node);
    if (it != ray_dependent_.end()) {
      return it->second;
    }
    /* For a graph with a cycle. */
    ray_dependent_[node] = false;
    bool result = node->type == LightPathNode::get_node_type();
    for (ShaderInput *input : node->inputs) {
      if (!result && input->link) {
        result = depends_on_ray(input->link->parent);
      }
    }
    ray_dependent_[node] = result;
    return result;
  }

  /* The kind of ray a result of the node is kept for: the current one where it matters. */
  int ray_of(ShaderNode *node)
  {
    return depends_on_ray(node) ? ray_type_ : int(RAY_CAMERA);
  }

  RdlValue output_value(ShaderOutput *output)
  {
    const std::pair<const ShaderOutput *, int> key(output, ray_of(output->parent));
    auto it = values_.find(key);
    if (it != values_.end()) {
      return it->second;
    }
    /* A graph with a cycle is not valid, but must not hang the export. */
    if (!visiting_.insert(output).second) {
      return constant(0.0f);
    }
    const RdlValue value = evaluate_output(output);
    visiting_.erase(output);
    values_[key] = value;
    return value;
  }

  /* A node without a counterpart: the input it mostly passes on, or a constant. */
  RdlValue fallback_value(ShaderNode *node, ShaderOutput *output, const char *reason = nullptr)
  {
    for (const char *name : {"color", "value", "vector", "color1", "a"}) {
      ShaderInput *input = find_input(node, name);
      if (input && input->socket_type.type != SocketType::CLOSURE) {
        note(node, string(reason ? reason : "no counterpart in MoonRay") + ", its input '" +
                       name + "' is used unchanged");
        return input_value(node, name);
      }
    }
    const bool is_float = output->type() == SocketType::FLOAT || output->type() == SocketType::INT;
    const bool is_color = output->type() == SocketType::COLOR;
    note(node, string(reason ? reason : "no counterpart in MoonRay") + ", the output '" +
                   output->socket_type.name.string() + "' is a constant");
    return is_float ? constant(0.5f) : constant(make_float3(is_color ? 0.5f : 0.0f));
  }

  RdlValue convert_value(ShaderNode *node, ShaderOutput *output)
  {
    ShaderInput *input = node->inputs[0];
    RdlValue in = input->link ? output_value(input->link) : socket_value(node, input->socket_type);
    const SocketType::Type from = input->type();
    const bool to_float_type = output->type() == SocketType::FLOAT ||
                               output->type() == SocketType::INT;
    if (to_float_type && from == SocketType::COLOR) {
      return to_float(to_world(in), CHANNEL_LUMINANCE);
    }
    if (to_float_type && from != SocketType::FLOAT && from != SocketType::INT) {
      return to_float(to_world(in), CHANNEL_AVERAGE);
    }
    if (!to_float_type && (from == SocketType::FLOAT || from == SocketType::INT)) {
      /* A float value is grey here already, but no longer one float. */
      in.scalar = in.scalar && is_uniform(in.scale);
    }
    return in;
  }

  RdlValue evaluate_output(ShaderOutput *output)
  {
    ShaderNode *node = output->parent;
    const NodeType *type = node->type;
    const ustring name = output->socket_type.name;

    if (node->special_type == SHADER_SPECIAL_TYPE_AUTOCONVERT) {
      return convert_value(node, output);
    }

    /* Constants. */
    if (type == ColorNode::get_node_type()) {
      return constant(static_cast<ColorNode *>(node)->get_value());
    }
    if (type == ValueNode::get_node_type()) {
      return constant(static_cast<ValueNode *>(node)->get_value());
    }

    /* Textures. */
    if (type == ImageTextureNode::get_node_type()) {
      return image_value(static_cast<ImageTextureNode *>(node), name == "alpha");
    }
    if (type == CheckerTextureNode::get_node_type()) {
      return checker_value(static_cast<CheckerTextureNode *>(node), name == "fac");
    }
    if (type == NoiseTextureNode::get_node_type()) {
      return noise_value(static_cast<NoiseTextureNode *>(node), name == "color");
    }
    if (type == VoronoiTextureNode::get_node_type()) {
      return voronoi_value(static_cast<VoronoiTextureNode *>(node), output);
    }
    if (type == WhiteNoiseTextureNode::get_node_type()) {
      return white_noise_value(static_cast<WhiteNoiseTextureNode *>(node), name == "color");
    }
    if (type == GradientTextureNode::get_node_type()) {
      return gradient_value(static_cast<GradientTextureNode *>(node));
    }
    if (type == WaveTextureNode::get_node_type()) {
      return wave_value(static_cast<WaveTextureNode *>(node));
    }
    if (type == MagicTextureNode::get_node_type() || type == BrickTextureNode::get_node_type()) {
      return procedural_fallback(node, output);
    }
    if (type == SkyTextureNode::get_node_type()) {
      return sky_value(static_cast<SkyTextureNode *>(node));
    }

    /* Geometry. */
    if (type == AttributeNode::get_node_type() || type == VertexColorNode::get_node_type()) {
      return attribute_value(node, output);
    }
    if (type == UVMapNode::get_node_type()) {
      return uv_map_value(static_cast<UVMapNode *>(node));
    }
    if (type == TextureCoordinateNode::get_node_type()) {
      return texture_coordinate_value(static_cast<TextureCoordinateNode *>(node), output);
    }
    if (type == GeometryNode::get_node_type()) {
      return geometry_value(node, output);
    }
    if (type == ObjectInfoNode::get_node_type()) {
      return object_info_value(node, output);
    }
    if (type == HairInfoNode::get_node_type()) {
      return hair_info_value(node, output);
    }
    if (type == LightPathNode::get_node_type()) {
      return light_path_value(node, output);
    }
    if (type == CameraNode::get_node_type()) {
      return camera_value(node, output);
    }
    if (type == FresnelNode::get_node_type()) {
      return fresnel_value(node, input_constant(node, "IOR", 1.5f));
    }
    if (type == LayerWeightNode::get_node_type()) {
      return layer_weight_value(node, name == "fresnel");
    }

    /* Colors and numbers. */
    if (type == RGBToBWNode::get_node_type()) {
      return to_float(input_value(node, "color"), CHANNEL_LUMINANCE);
    }
    if (type == SeparateColorNode::get_node_type() || type == SeparateXYZNode::get_node_type()) {
      return separate_value(node, output);
    }
    if (type == CombineColorNode::get_node_type() || type == CombineXYZNode::get_node_type()) {
      return combine_value(node);
    }
    if (type == InvertNode::get_node_type()) {
      const RdlValue color = input_value(node, "color");
      return blend(color, invert(color), input_float(node, "fac"));
    }
    if (type == MathNode::get_node_type()) {
      return math_value(static_cast<MathNode *>(node));
    }
    if (type == VectorMathNode::get_node_type()) {
      return vector_math_value(static_cast<VectorMathNode *>(node), name == "value");
    }
    if (type == MixColorNode::get_node_type() || type == MixFloatNode::get_node_type() ||
        type == MixNode::get_node_type() || type == MixVectorNode::get_node_type() ||
        type == MixVectorNonUniformNode::get_node_type())
    {
      return mix_value(node);
    }
    if (type == RGBRampNode::get_node_type()) {
      return ramp_value(static_cast<RGBRampNode *>(node), name == "alpha");
    }
    if (type == RGBCurvesNode::get_node_type() || type == VectorCurvesNode::get_node_type()) {
      return curves_value(static_cast<CurvesNode *>(node));
    }
    if (type == FloatCurveNode::get_node_type()) {
      return float_curve_value(static_cast<FloatCurveNode *>(node));
    }
    if (type == ClampNode::get_node_type()) {
      return clamp_node_value(static_cast<ClampNode *>(node));
    }
    if (type == MapRangeNode::get_node_type()) {
      return map_range_value(static_cast<MapRangeNode *>(node));
    }
    if (type == HSVNode::get_node_type()) {
      return hsv_value(node);
    }
    if (type == GammaNode::get_node_type()) {
      /* Cycles leaves what is not positive as it is, the OpMap makes it zero. */
      RdlValue result = op(OP_POWER, input_value(node, "color"), input_float(node, "gamma"));
      result.scalar = false;
      return result;
    }
    if (type == BrightContrastNode::get_node_type()) {
      return bright_contrast_value(node);
    }
    if (type == MappingNode::get_node_type()) {
      return mapping_value(static_cast<MappingNode *>(node));
    }
    if (type == VectorTransformNode::get_node_type()) {
      return vector_transform_value(static_cast<VectorTransformNode *>(node));
    }
    if (type == NormalNode::get_node_type()) {
      const float3 direction = static_cast<NormalNode *>(node)->get_direction();
      if (name == "dot") {
        return op(OP_DOT, to_world(input_value(node, "normal")), constant(direction));
      }
      return constant(direction);
    }
    if (type == AmbientOcclusionNode::get_node_type()) {
      note(node, "there is no ambient occlusion in a MoonRay shader, nothing is occluded");
      return (name == "ao") ? constant(1.0f) : input_value(node, "color");
    }
    if (type == BumpNode::get_node_type() || type == NormalMapNode::get_node_type()) {
      /* The normal map these are for a BSDF, see normal_of(), read back as a vector. */
      const string normal = normal_of(output, nullptr);
      if (normal.empty()) {
        return render_normal();
      }
      const int object = new_object("NormalToRgbMap");
      set(object, "input", normal);
      RdlValue value = map_value(object);
      value.render_space = true;
      return value;
    }
    if (type == BevelNode::get_node_type() || type == TangentNode::get_node_type()) {
      note(node, "no counterpart in MoonRay, the normal or tangent of the surface is used");
      return (type == TangentNode::get_node_type()) ? tangent_value() : render_normal();
    }
    if (type == LightFalloffNode::get_node_type()) {
      return input_float(node, "strength");
    }
    if (type == WireframeNode::get_node_type()) {
      note(node, "no counterpart in MoonRay, no wire is drawn");
      return constant(0.0f);
    }

    return fallback_value(node, output);
  }

  /* Geometry. */

  RdlValue tangent_value()
  {
    return cached("tangent", [&] {
      RdlValue dpds = state_value(5);
      dpds.render_space = true;
      RdlValue tangent = op(OP_NORMALIZE, dpds);
      tangent.render_space = true;
      return to_world(tangent);
    });
  }

  RdlValue attribute_value(ShaderNode *node, ShaderOutput *output)
  {
    const bool is_color = node->type == VertexColorNode::get_node_type();
    const ustring name = is_color ? static_cast<VertexColorNode *>(node)->get_layer_name() :
                                    static_cast<AttributeNode *>(node)->get_attribute();
    const ustring output_name = output->socket_type.name;

    RdlValue value;
    if (name.empty() && is_color) {
      /* The default color attribute is the vertex color of the ANARI geometry. */
      if (output_name == "alpha") {
        return cached("color alpha",
                      [&] { return primitive_attribute("anari_color_alpha", 0, one_float3()); });
      }
      value = cached("Cd", [&] { return primitive_attribute("Cd", 3, one_float3()); });
    }
    else if (name.empty() || uchar(name[0]) < 0x20) {
      /* Attributes of the object, the instancer or the view layer. */
      note(node, "the attribute '" + name.string() + "' is not exported, it is zero");
      return (output_name == "alpha") ? constant(1.0f) : constant(zero_float3());
    }
    else if (!slot_value(name, value)) {
      note(node, "no attribute slot is left for '" + name.string() + "', it is zero");
      return (output_name == "alpha") ? constant(1.0f) : constant(zero_float3());
    }
    else if (output_name == "alpha") {
      note(node, "the alpha of the attribute '" + name.string() + "' is not exported, it is 1");
      return constant(1.0f);
    }
    return (output_name == "fac") ? to_float(value, CHANNEL_AVERAGE) : value;
  }

  RdlValue uv_map_value(UVMapNode *node)
  {
    RdlValue value;
    if (node->get_from_dupli()) {
      note(node, "the coordinates of the instancer are not exported, the UV map is used");
      return uv_value();
    }
    if (node->get_attribute().empty() || !slot_value(node->get_attribute(), value)) {
      return uv_value();
    }
    value.planar = true;
    return value;
  }

  RdlValue texture_coordinate_value(TextureCoordinateNode *node, ShaderOutput *output)
  {
    const ustring name = output->socket_type.name;
    if (node->get_from_dupli()) {
      note(node, "the coordinates of the instancer are not exported, those of the object are used");
    }
    if (name == "UV") {
      return uv_value();
    }
    if (name == "generated") {
      return generated_value();
    }
    if (name == "object") {
      if (node->get_use_transform()) {
        note(node, "the coordinates of another object are not exported, those of the object "
                   "itself are used");
      }
      return object_position();
    }
    if (name == "normal") {
      return object_normal();
    }
    if (name == "camera") {
      return camera_position();
    }
    if (name == "window") {
      note(node, "window coordinates are approximate");
      return cached("P window", [&] {
        return affine(
            transform_space(render_position(), TRANSFORM_POINT, SPACE_RENDER, SPACE_SCREEN),
            make_float3(0.5f, 0.5f, 0.0f),
            make_float3(0.5f, 0.5f, 0.0f));
      });
    }
    if (name == "reflection") {
      /* 2 (N . I) N - I */
      return cached("reflection", [&] {
        const RdlValue normal = render_normal();
        const RdlValue incoming = render_incoming();
        RdlValue projected = multiply(op(OP_DOT, normal, incoming), normal);
        RdlValue reflection = subtract(scaled(projected, 2.0f), incoming);
        reflection.render_space = true;
        return to_world(reflection);
      });
    }
    return fallback_value(node, output);
  }

  RdlValue geometry_value(ShaderNode *node, ShaderOutput *output)
  {
    const ustring name = output->socket_type.name;
    if (name == "position") {
      return world_position();
    }
    if (name == "normal") {
      return render_normal();
    }
    if (name == "true_normal") {
      return cached("Ng", [&] {
        RdlValue value = state_value(4);
        value.render_space = true;
        return value;
      });
    }
    if (name == "incoming") {
      return render_incoming();
    }
    if (name == "tangent") {
      note(node, "the tangent is the one of the UV map");
      return tangent_value();
    }
    if (name == "backfacing") {
      return cached("backfacing", [&] {
        const int object = new_object("TwoSidedMap");
        set(object, "front_color", rgb(zero_float3()));
        set(object, "back_color", rgb(one_float3()));
        return map_value(object, true);
      });
    }
    if (name == "pointiness" || name == "random_per_island") {
      /* Values Cycles computes for the mesh, exported as an attribute. */
      RdlValue value;
      if (slot_value(name, value)) {
        return to_float(value, CHANNEL_R);
      }
      note(node, "no attribute slot is left for '" + name.string() + "', it is 0.5");
      return constant(0.5f);
    }
    return fallback_value(node, output);
  }

  /* What the MoonRay device computes for every curve geometry: where along its curve a point
   * is, a random number and the length of the curve. On a mesh they are not there, and the
   * values are the ones Cycles has for what is not a strand. */
  RdlValue hair_info_value(ShaderNode *node, ShaderOutput *output)
  {
    const ustring name = output->socket_type.name;
    if (name == "is_strand") {
      return cached("is curve",
                    [&] { return primitive_attribute("anari_is_curve", 0, zero_float3()); });
    }
    if (name == "intercept") {
      return cached("curve intercept", [&] {
        return primitive_attribute("anari_curve_intercept", 0, zero_float3());
      });
    }
    if (name == "size") {
      return cached("curve length",
                    [&] { return primitive_attribute("anari_curve_length", 0, zero_float3()); });
    }
    if (name == "index") {
      return cached("curve random",
                    [&] { return primitive_attribute("anari_curve_random", 0, zero_float3()); });
    }
    return fallback_value(node, output);
  }

  RdlValue object_info_value(ShaderNode *node, ShaderOutput *output)
  {
    const ustring name = output->socket_type.name;
    if (name == "location") {
      return cached("object location", [&] {
        return transform_space(
            transform_space(constant(zero_float3()), TRANSFORM_POINT, SPACE_OBJECT, SPACE_WORLD),
            TRANSFORM_POINT,
            SPACE_INSTANCE,
            SPACE_WORLD);
      });
    }
    if (name == "color") {
      return cached("object color",
                    [&] { return primitive_attribute("anari_object_color", 3, one_float3()); });
    }
    if (name == "random") {
      return cached("object random",
                    [&] { return primitive_attribute("anari_object_random", 0, zero_float3()); });
    }
    if (name == "object_index") {
      return cached("object index",
                    [&] { return primitive_attribute("anari_object_index", 0, zero_float3()); });
    }
    if (name == "material_index") {
      return constant(float(shader_->get_pass_id()));
    }
    if (name == "alpha") {
      note(node, "the alpha of the object color is not exported, it is 1");
      return constant(1.0f);
    }
    return fallback_value(node, output);
  }

  /* A map of MoonRay does not know the kind of ray. The material is written once for each kind
   * MoonRay can switch materials by, see root_material(), with the value of that kind here. */
  RdlValue light_path_value(ShaderNode *node, ShaderOutput *output)
  {
    const ustring name = output->socket_type.name;
    const int ray = ray_type_;
    const bool bounced = ray == RAY_DIFFUSE || ray == RAY_GLOSSY || ray == RAY_SINGULAR;
    if (name == "ray_length") {
      note(node, "the ray length is the distance to the camera for every kind of ray");
      return cached("ray length", [&] { return op(OP_LENGTH, camera_position()); });
    }
    if (name == "is_camera_ray") {
      return constant(ray == RAY_CAMERA ? 1.0f : 0.0f);
    }
    if (name == "is_shadow_ray") {
      return constant(ray == RAY_SHADOW ? 1.0f : 0.0f);
    }
    if (name == "is_diffuse_ray" || name == "diffuse_depth") {
      return constant(ray == RAY_DIFFUSE ? 1.0f : 0.0f);
    }
    if (name == "is_glossy_ray" || name == "glossy_depth") {
      return constant((ray == RAY_GLOSSY || ray == RAY_SINGULAR) ? 1.0f : 0.0f);
    }
    if (name == "is_singular_ray") {
      return constant(ray == RAY_SINGULAR ? 1.0f : 0.0f);
    }
    if (name == "is_reflection_ray") {
      note(node, "reflection and transmission rays are not told apart, both are reflection rays");
      return constant(bounced ? 1.0f : 0.0f);
    }
    if (name == "ray_depth") {
      note(node, "the ray depth is 0 for camera rays and 1 for all others");
      return constant(ray == RAY_CAMERA ? 0.0f : 1.0f);
    }
    note(node, "the output '" + name.string() + "' has no counterpart, it is 0");
    return constant(0.0f);
  }

  RdlValue camera_value(ShaderNode *node, ShaderOutput *output)
  {
    const ustring name = output->socket_type.name;
    if (name == "view_vector") {
      return op(OP_NORMALIZE, camera_position());
    }
    if (name == "view_z_depth") {
      return to_float(camera_position(), CHANNEL_B);
    }
    if (name == "view_distance") {
      return op(OP_LENGTH, camera_position());
    }
    return fallback_value(node, output);
  }

  /* The cosine between the viewing direction and the normal input of the node. */
  RdlValue facing_cosine(ShaderNode *node)
  {
    RdlValue normal = is_linked(node, "normal") ? input_value(node, "normal") : render_normal();
    RdlValue incoming = render_incoming();
    same_space(normal, incoming);
    return op(OP_ABS, op(OP_DOT, normal, incoming));
  }

  static float fresnel_dielectric(const float cosine, const float eta)
  {
    const float c = fabsf(cosine);
    float g = eta * eta - 1.0f + c * c;
    if (g <= 0.0f) {
      return 1.0f;
    }
    g = sqrtf(g);
    const float a = (g - c) / (g + c);
    const float b = (c * (g + c) - 1.0f) / (c * (g - c) + 1.0f);
    return 0.5f * a * a * (1.0f + b * b);
  }

  /* The Fresnel reflectance of a dielectric, a function of the cosine: a ramp. The inside of
   * the surface (back faces) is not told apart. */
  RdlValue fresnel_value(ShaderNode *node, const float ior)
  {
    const float eta = max(ior, 1e-5f);
    return function_ramp(facing_cosine(node),
                         [eta](const float cosine) { return fresnel_dielectric(cosine, eta); });
  }

  RdlValue layer_weight_value(ShaderNode *node, const bool fresnel)
  {
    const float blend_input = input_constant(node, "blend", 0.5f);
    if (fresnel) {
      const float eta = 1.0f / max(1.0f - blend_input, 1e-5f);
      return function_ramp(facing_cosine(node),
                           [eta](const float cosine) { return fresnel_dielectric(cosine, eta); });
    }
    const RdlValue cosine = facing_cosine(node);
    if (blend_input == 0.5f) {
      return invert(cosine);
    }
    float exponent = clamp(blend_input, 0.0f, 1.0f - 1e-5f);
    exponent = (exponent < 0.5f) ? 2.0f * exponent : 0.5f / (1.0f - exponent);
    return invert(op(OP_POWER, cosine, constant(exponent)));
  }

  /* Images. */

  /* Record the image of the node, returns the name the text uses for it. */
  bool image_parameter(ImageTextureNode *node, string &parameter)
  {
    if (node->handle.empty() && node->get_filename().empty()) {
      return false;
    }
    if (node->get_tiles().size() > 1) {
      note(node, "UDIM tiles are not exported");
      return false;
    }
    for (const AnariGraphImage &image : source_.images) {
      if (image.node == node) {
        parameter = image.parameter;
        return true;
      }
    }
    AnariGraphImage binding;
    binding.parameter = string_printf("image.%d", int(source_.images.size()));
    binding.node = node;
    source_.images.push_back(binding);
    parameter = binding.parameter;
    return true;
  }

  static bool is_default_uv(const ShaderOutput *output)
  {
    const ShaderNode *source = output->parent;
    if (source->type == TextureCoordinateNode::get_node_type()) {
      return output->socket_type.name == "UV" &&
             !static_cast<const TextureCoordinateNode *>(source)->get_from_dupli();
    }
    if (source->type == UVMapNode::get_node_type()) {
      return static_cast<const UVMapNode *>(source)->get_attribute().empty() &&
             !static_cast<const UVMapNode *>(source)->get_from_dupli();
    }
    return false;
  }

  /* Whether the texture coordinates of the image are the default UV map, moved and scaled at
   * most: then the ImageMap looks the texture up with the texture coordinates of the mesh,
   * which have the footprint of the pixel for filtering. */
  bool default_uv_mapping(ImageTextureNode *node, float2 &scale, float2 &offset)
  {
    Transform transform = transform_identity();
    ShaderInput *vector = find_input(node, "vector");
    if (vector == nullptr) {
      return false;
    }
    const ShaderOutput *source = vector->link;
    if (source && source->parent->type == MappingNode::get_node_type()) {
      MappingNode *mapping = static_cast<MappingNode *>(source->parent);
      if (is_linked(mapping, "location") || is_linked(mapping, "rotation") ||
          is_linked(mapping, "scale") || !is_linked(mapping, "vector"))
      {
        return false;
      }
      TextureMapping tex_mapping = identity_mapping();
      tex_mapping.translation = mapping->get_location();
      tex_mapping.rotation = mapping->get_rotation();
      tex_mapping.scale = mapping->get_scale();
      tex_mapping.type = TextureMapping::Type(mapping->get_mapping_type());
      transform = tex_mapping.compute_transform();
      source = find_input(mapping, "vector")->link;
    }
    if (source ? !is_default_uv(source) : !(vector->flags() & SocketType::LINK_TEXTURE_UV)) {
      return false;
    }
    if (!node->tex_mapping.skip()) {
      transform = node->tex_mapping.compute_transform() * transform;
    }
    if (fabsf(transform.x.y) > 1e-6f || fabsf(transform.y.x) > 1e-6f) {
      /* Rotated. */
      return false;
    }
    scale = make_float2(transform.x.x, transform.y.y);
    offset = make_float2(transform.x.w, transform.y.w);
    return true;
  }

  /* 1 inside of the unit square of the texture coordinates, 0 outside. */
  RdlValue clip_mask(const RdlValue &uv)
  {
    RdlValue inside = op(OP_MULTIPLY,
                         op(OP_GREATER_EQUAL, uv, constant(zero_float3())),
                         op(OP_LESS_EQUAL, uv, constant(one_float3())));
    return to_float(inside, CHANNEL_MIN);
  }

  RdlValue image_value(ImageTextureNode *node, const bool alpha)
  {
    string key = string_printf("image %p %d", (void *)node, int(alpha));
    auto it = shared_.find(key);
    if (it != shared_.end()) {
      return it->second;
    }
    const RdlValue value = image_lookup(node, alpha);
    shared_[key] = value;
    return value;
  }

  RdlValue image_lookup(ImageTextureNode *node, const bool alpha)
  {
    string parameter;
    if (!image_parameter(node, parameter)) {
      /* The color of a missing image. */
      return alpha ? constant(node->get_missing_alpha()) : constant(node->get_missing());
    }

    const ExtensionType extension = node->get_extension();
    const bool wrap = extension == EXTENSION_REPEAT || extension == EXTENSION_MIRROR;
    if (extension == EXTENSION_MIRROR) {
      note(node, "the image is repeated instead of mirrored");
    }

    if (node->get_projection() == NODE_IMAGE_PROJ_BOX) {
      if (alpha) {
        note(node, "a box projected image has no alpha in MoonRay, it is 1");
        return constant(1.0f);
      }
      return box_projection(node, parameter, wrap);
    }
    if (node->get_projection() != NODE_IMAGE_PROJ_FLAT) {
      note(node, "sphere and tube projections are not exported, the image is projected flat");
    }

    const int object = new_object("ImageMap");
    set(object, "texture", quoted(parameter));
    set(object, "wrap_around", wrap);
    /* The pixels are the ones Cycles loaded, linear already. */
    set(object, "gamma", 0);
    if (alpha) {
      set(object, "alpha_only", true);
    }

    float2 scale = make_float2(1.0f, 1.0f);
    float2 offset = zero_float2();
    RdlValue uv;
    if (default_uv_mapping(node, scale, offset)) {
      if (scale != make_float2(1.0f, 1.0f)) {
        set(object,
            "scale",
            string_printf("Vec2(%.9g, %.9g)", double(scale.x), double(scale.y)));
      }
      if (offset != zero_float2()) {
        set(object,
            "offset",
            string_printf("Vec2(%.9g, %.9g)", double(offset.x), double(offset.y)));
      }
      if (extension == EXTENSION_CLIP) {
        uv = affine(uv_value(),
                    make_float3(scale.x, scale.y, 0.0f),
                    make_float3(offset.x, offset.y, 0.0f));
      }
    }
    else {
      uv = texture_vector(node, &node->tex_mapping);
      if (!uv.planar) {
        /* A negative third component makes the ImageMap black. */
        uv = scaled(uv, make_float3(1.0f, 1.0f, 0.0f));
        uv.planar = true;
      }
      set(object, "texture_coordinates", 2);
      set(object, "input_texture_coordinates", bind_vec3(uv));
    }

    RdlValue result = map_value(object, alpha);
    if (extension == EXTENSION_CLIP) {
      /* Black and transparent outside of the image. */
      const string mask_key = "clip " + uv.map + rgb(uv.scale);
      const RdlValue mask = cached(mask_key.c_str(), [&] { return clip_mask(uv); });
      result = op(OP_MULTIPLY, result, mask);
      result.scalar = alpha;
    }
    if (!alpha) {
      /* Cycles has the colors with the alpha associated, and gives the color without it when
       * the alpha is used as well. */
      ShaderOutput *alpha_output = node->output("Alpha");
      if (alpha_output && !alpha_output->links.empty()) {
        result = op(OP_DIVIDE, result, image_value(node, true));
        result.scalar = false;
      }
    }
    return result;
  }

  /* Box projection as a ProjectTriplanarMap: it is given the texture coordinates as position
   * and the normal in object space, and looks the image up on the plane the normal points
   * along most. The six faces are turned so that they are the planes of Cycles. */
  RdlValue box_projection(ImageTextureNode *node, const string &parameter, const bool wrap)
  {
    /* The map adds a half to the coordinates of each plane. */
    const RdlValue position = add(to_world(texture_vector(node, &node->tex_mapping)),
                                  constant(make_float3(-0.5f)));

    const int object = new_object("ProjectTriplanarMap_v2");
    set(object, "projection_mode", 1);
    set(object, "input_position_source", 2);
    set(object, "input_position", bind_vec3(position));
    set(object, "input_normal", bind_vec3(object_normal()));
    set(object, "number_of_textures", 6);
    set(object, "transition_width", number(clamp(node->get_projection_blend(), 0.0f, 1.0f)));
    set(object, "gamma", 0);
    for (const char *face :
         {"positive_x", "negative_x", "positive_y", "negative_y", "positive_z", "negative_z"})
    {
      set(object, (string(face) + "_texture").c_str(), quoted(parameter));
      if (!wrap) {
        set(object, (string(face) + "_wrap_around").c_str(), false);
      }
    }
    set(object, "positive_x_rotation", "-90");
    set(object, "negative_x_rotation", "90");
    set(object, "positive_y_invert_s", true);
    set(object, "positive_y_invert_t", true);
    set(object, "positive_z_rotation", "90");
    set(object, "negative_z_rotation", "-90");
    if (node->get_extension() == EXTENSION_CLIP) {
      note(node, "a box projected image is not clipped, its border is extended");
    }
    return map_value(object);
  }

  /* Procedural textures. */

  /* Cycles moves the coordinates of its lattice textures a little off the lattice. */
  RdlValue checker_value(CheckerTextureNode *node, const bool fac)
  {
    RdlValue coordinates = multiply(to_world(texture_vector(node, &node->tex_mapping)),
                                    input_float(node, "scale"));
    coordinates = affine(coordinates, 0.999999f, 0.000001f * 0.999999f);
    /* The parity of the sum of the cell indices. */
    const RdlValue cells = to_float(op(OP_FLOOR, coordinates), CHANNEL_SUM);
    const RdlValue odd = op(OP_MODULO, op(OP_ABS, cells), constant(2.0f));
    if (fac) {
      return odd;
    }
    return blend(input_value(node, "color2"), input_value(node, "color1"), odd);
  }

  /* The coordinates of a noise map, with the dimensions of the Cycles texture. */
  RdlValue noise_coordinates(ShaderNode *node, TextureMapping *mapping, const int dimensions)
  {
    RdlValue coordinates = to_world(texture_vector(node, mapping));
    if (dimensions == 1) {
      coordinates = combine(input_float(node, "w"), constant(0.0f), constant(0.0f));
    }
    else if (dimensions == 2 && !coordinates.planar) {
      coordinates = scaled(coordinates, make_float3(1.0f, 1.0f, 0.0f));
    }
    return coordinates;
  }

  RdlValue noise_value(NoiseTextureNode *node, const bool color)
  {
    const int dimensions = node->get_dimensions();
    if (node->get_type() != NODE_NOISE_FBM) {
      note(node, "only the fBM noise has a counterpart, it is used for the other types too");
    }
    const float detail = clamp(input_constant(node, "detail", 2.0f), 0.0f, 15.0f);
    const float roughness = max(input_constant(node, "roughness", 0.5f), 0.0f);

    const int object = new_object("NoiseMap_v2");
    set(object, "space", 7);
    set(object, "input_texture_coordinates",
        bind_vec3(noise_coordinates(node, &node->tex_mapping, dimensions)));
    set(object, "frequency_multiplier", bind_float(input_float(node, "scale")));
    set(object, "max_level", number(detail + 1.0f));
    set(object, "persistence", number(roughness));
    set(object, "lacunarity", bind_float(input_float(node, "lacunarity")));
    const RdlValue distortion = input_float(node, "distortion");
    if (!is_constant(distortion, 0.0f)) {
      set(object, "distortion", bind_float(distortion));
    }
    if (dimensions == 4) {
      set(object, "use_4D_noise", true);
      set(object, "time", bind_float(input_float(node, "w")));
    }

    /* MoonRay adds up the octaves, Cycles divides the sum by the largest value it can have. */
    float amplitude = 0.0f;
    float octave = 1.0f;
    for (int i = 0; i <= int(detail); i++) {
      amplitude += octave;
      octave *= roughness;
    }
    amplitude += octave * (detail - floorf(detail));
    /* The Perlin noise of MoonRay has about three quarters of the contrast of the one of
     * Cycles (measured with the node tests). */
    const float contrast = 1.3f;
    const float gain = contrast * (node->get_use_normalize() ? 0.5f / max(amplitude, 1e-6f) : 1.0f);
    const float center = node->get_use_normalize() ? 0.5f : 0.0f;
    if (color) {
      /* Three noises in 0..1, around a half. */
      set(object, "color", true);
      return affine(map_value(object), 2.0f * gain, center - gain);
    }
    set(object, "color_A", rgb(make_float3(center - gain)));
    set(object, "color_B", rgb(make_float3(center + gain)));
    return map_value(object, true);
  }

  RdlValue voronoi_value(VoronoiTextureNode *node, ShaderOutput *output)
  {
    const ustring name = output->socket_type.name;
    if (name == "w" || name == "radius") {
      note(node, "the output '" + name.string() + "' has no counterpart, it is a constant");
      return constant(0.5f);
    }
    const int dimensions = node->get_dimensions();
    const float detail = clamp(input_constant(node, "detail", 0.0f), 0.0f, 15.0f);

    const int object = new_object("NoiseWorleyMap_v3");
    set(object, "space", 7);
    set(object, "input_texture_coordinates",
        bind_vec3(noise_coordinates(node, &node->tex_mapping, dimensions)));
    set(object, "frequency", bind_float(input_float(node, "scale")));
    set(object, "jitter", bind_float(input_float(node, "randomness")));
    set(object, "max_level", number(detail + 1.0f));
    switch (node->get_metric()) {
      case NODE_VORONOI_EUCLIDEAN:
        /* What MoonRay calls linear is the squared distance, and the other way round. */
        set(object, "distance_method", 1);
        break;
      case NODE_VORONOI_MANHATTAN:
        set(object, "distance_method", 2);
        break;
      case NODE_VORONOI_CHEBYCHEV:
        set(object, "distance_method", 3);
        break;
      case NODE_VORONOI_MINKOWSKI:
        set(object, "distance_method", 5);
        set(object, "minkowski_number", number(input_constant(node, "exponent", 0.5f)));
        break;
      default:
        break;
    }

    if (name != "distance") {
      /* A random color for each cell. */
      if (name == "position") {
        note(node, "the position of the cell is a random vector");
      }
      set(object, "output_mode", 2);
      return map_value(object);
    }
    /* The getter which is not const is the one of the kernel features of the node. */
    const NodeVoronoiFeature feature = static_cast<const VoronoiTextureNode *>(node)->get_feature();
    if (feature == NODE_VORONOI_F2) {
      set(object, "F1", "0");
      set(object, "F2", "1");
    }
    else if (feature == NODE_VORONOI_DISTANCE_TO_EDGE) {
      set(object, "F1", "-0.5");
      set(object, "F2", "0.5");
    }
    else if (feature == NODE_VORONOI_N_SPHERE_RADIUS) {
      set(object, "F1", "0.5");
    }
    else if (feature == NODE_VORONOI_SMOOTH_F1) {
      note(node, "smooth F1 is the plain F1");
    }
    return map_value(object, true);
  }

  RdlValue white_noise_value(WhiteNoiseTextureNode *node, const bool color)
  {
    const int dimensions = node->get_dimensions();
    RdlValue input = (dimensions == 1) ? constant(0.0f) : to_world(input_value(node, "vector"));
    /* One number which differs between points. */
    RdlValue hashed = op(OP_DOT, input, constant(make_float3(12.9898f, 78.233f, 37.719f)));
    if (dimensions == 1 || dimensions == 4) {
      hashed = add(hashed, scaled(input_float(node, "w"), 53.3147f));
    }
    const int object = new_object("RandomMap");
    set(object, "input", bind_rgb(op(OP_ABS, hashed)));
    set(object, "monochrome", !color);
    return map_value(object, !color);
  }

  RdlValue gradient_value(GradientTextureNode *node)
  {
    const RdlValue p = to_world(texture_vector(node, &node->tex_mapping));
    const RdlValue x = to_float(p, CHANNEL_R);
    RdlValue f;
    switch (node->get_gradient_type()) {
      case NODE_BLEND_QUADRATIC: {
        const RdlValue r = op(OP_MAXIMUM, x, constant(0.0f));
        f = op(OP_MULTIPLY, r, r);
        break;
      }
      case NODE_BLEND_EASING:
        f = function_ramp(x, [](const float r) { return r * r * (3.0f - 2.0f * r); });
        break;
      case NODE_BLEND_DIAGONAL:
        f = scaled(add(x, to_float(p, CHANNEL_G)), 0.5f);
        break;
      case NODE_BLEND_SPHERICAL:
      case NODE_BLEND_QUADRATIC_SPHERE: {
        const RdlValue r = op(OP_MAXIMUM, affine(op(OP_LENGTH, p), -1.0f, 0.999999f),
                              constant(0.0f));
        f = (node->get_gradient_type() == NODE_BLEND_SPHERICAL) ? r : op(OP_MULTIPLY, r, r);
        break;
      }
      case NODE_BLEND_RADIAL:
        /* atan2(y, x) / (2 pi) + 0.5 */
        f = affine(arctan2(to_float(p, CHANNEL_G), x), 1.0f / M_2PI_F, 0.5f);
        break;
      default:
        f = x;
        break;
    }
    return clamp_value(f, 0.0f, 1.0f);
  }

  RdlValue wave_value(WaveTextureNode *node)
  {
    RdlValue p = multiply(to_world(texture_vector(node, &node->tex_mapping)),
                          input_float(node, "scale"));
    p = affine(p, 0.999999f, 0.000001f * 0.999999f);

    RdlValue n;
    if (node->get_wave_type() == NODE_WAVE_BANDS) {
      switch (node->get_bands_direction()) {
        case NODE_WAVE_BANDS_DIRECTION_X:
          n = scaled(to_float(p, CHANNEL_R), 20.0f);
          break;
        case NODE_WAVE_BANDS_DIRECTION_Y:
          n = scaled(to_float(p, CHANNEL_G), 20.0f);
          break;
        case NODE_WAVE_BANDS_DIRECTION_Z:
          n = scaled(to_float(p, CHANNEL_B), 20.0f);
          break;
        default:
          n = scaled(to_float(p, CHANNEL_SUM), 10.0f);
          break;
      }
    }
    else {
      float3 mask = one_float3();
      switch (node->get_rings_direction()) {
        case NODE_WAVE_RINGS_DIRECTION_X:
          mask.x = 0.0f;
          break;
        case NODE_WAVE_RINGS_DIRECTION_Y:
          mask.y = 0.0f;
          break;
        case NODE_WAVE_RINGS_DIRECTION_Z:
          mask.z = 0.0f;
          break;
        default:
          break;
      }
      n = scaled(op(OP_LENGTH, scaled(p, mask)), 20.0f);
    }
    n = add(n, input_float(node, "phase"));

    const RdlValue distortion = input_float(node, "distortion");
    if (!is_constant(distortion, 0.0f)) {
      const float detail = clamp(input_constant(node, "detail", 2.0f), 0.0f, 15.0f);
      const float roughness = max(input_constant(node, "detail_roughness", 0.5f), 0.0f);
      const int object = new_object("NoiseMap_v2");
      set(object, "space", 7);
      set(object, "input_texture_coordinates", bind_vec3(p));
      set(object, "frequency_multiplier", bind_float(input_float(node, "detail_scale")));
      set(object, "max_level", number(detail + 1.0f));
      set(object, "persistence", number(roughness));
      /* The noise in -1..1. */
      set(object, "color_A", rgb(make_float3(-1.0f)));
      set(object, "color_B", rgb(make_float3(1.0f)));
      n = add(n, multiply(map_value(object, true), distortion));
    }

    switch (node->get_profile()) {
      case NODE_WAVE_PROFILE_SAW:
        return op(OP_FRACTION, scaled(n, 1.0f / M_2PI_F));
      case NODE_WAVE_PROFILE_TRI: {
        const RdlValue turns = scaled(n, 1.0f / M_2PI_F);
        const RdlValue nearest = op(OP_FLOOR, add(turns, constant(0.5f)));
        return scaled(op(OP_ABS, subtract(turns, nearest)), 2.0f);
      }
      default:
        return affine(op(OP_SINE, add(n, constant(-M_PI_2_F))), 0.5f, 0.5f);
    }
  }

  /* A sky on a surface (a light portal, a backdrop). MoonRay has no sky model in its maps, but
   * the Preetham model is a product of a function of the elevation and one of the angle to the
   * sun for its luminance: two ramps, with the color of the first one. It is used whatever the
   * type of the sky is. The formulas are those of #sky_texture_precompute_preetham. */
  RdlValue sky_value(SkyTextureNode *node)
  {
    if (node->get_sky_type() == NODE_SKY_HOSEK) {
      return hosek_sky_value(node);
    }
    note(node, "a sky on a surface is the Preetham model, with its color by the elevation only");
    float3 sun = node->get_sun_direction();
    if (node->get_sky_type() != NODE_SKY_PREETHAM && node->get_sky_type() != NODE_SKY_HOSEK) {
      const float elevation = node->get_sun_elevation();
      const float rotation = node->get_sun_rotation();
      sun = make_float3(sinf(rotation) * cosf(elevation),
                        cosf(rotation) * cosf(elevation),
                        sinf(elevation));
    }
    sun = safe_normalize(sun);
    if (is_zero(sun)) {
      sun = make_float3(0.0f, 0.0f, 1.0f);
    }
    const float T = node->get_turbidity();
    const float T2 = T * T;
    const float theta = acosf(clamp(sun.z, -1.0f, 1.0f));
    const float theta2 = theta * theta;
    const float theta3 = theta2 * theta;
    const float chi = (4.0f / 9.0f - T / 120.0f) * (M_PI_F - 2.0f * theta);
    float zenith_Y = ((4.0453f * T - 4.9710f) * tanf(chi) - 0.2155f * T + 2.4192f) * 0.06f;
    float zenith_x = (0.00166f * theta3 - 0.00375f * theta2 + 0.00209f * theta) * T2 +
                     (-0.02903f * theta3 + 0.06377f * theta2 - 0.03202f * theta + 0.00394f) * T +
                     (0.11693f * theta3 - 0.21196f * theta2 + 0.06052f * theta + 0.25886f);
    float zenith_y = (0.00275f * theta3 - 0.00610f * theta2 + 0.00317f * theta) * T2 +
                     (-0.04214f * theta3 + 0.08970f * theta2 - 0.04153f * theta + 0.00516f) * T +
                     (0.15346f * theta3 - 0.26756f * theta2 + 0.06670f * theta + 0.26688f);
    const float perez_Y[5] = {0.1787f * T - 1.4630f, -0.3554f * T + 0.4275f,
                              -0.0227f * T + 5.3251f, 0.1206f * T - 2.5771f,
                              -0.0670f * T + 0.3703f};
    const float perez_x[5] = {-0.0193f * T - 0.2592f, -0.0665f * T + 0.0008f,
                              -0.0004f * T + 0.2125f, -0.0641f * T - 0.8989f,
                              -0.0033f * T + 0.0452f};
    const float perez_y[5] = {-0.0167f * T - 0.2608f, -0.0950f * T + 0.0092f,
                              -0.0079f * T + 0.2102f, -0.0441f * T - 1.6537f,
                              -0.0109f * T + 0.0529f};
    const auto perez = [](const float lam[5], const float view_theta, const float gamma) {
      return (1.0f + lam[0] * expf(lam[1] / cosf(view_theta))) *
             (1.0f + lam[2] * expf(lam[3] * gamma) + lam[4] * cosf(gamma) * cosf(gamma));
    };
    /* The part of the luminance which depends on the angle to the sun. */
    const auto sun_factor = [&](const float gamma) {
      return 1.0f + perez_Y[2] * expf(perez_Y[3] * gamma) + perez_Y[4] * cosf(gamma) * cosf(gamma);
    };
    zenith_Y /= perez(perez_Y, 0.0f, theta);
    zenith_x /= perez(perez_x, 0.0f, theta);
    zenith_y /= perez(perez_y, 0.0f, theta);

    const auto radiance = [&](const float3 dir) {
      const float view_theta = min(acosf(clamp(dir.z, -1.0f, 1.0f)), M_PI_2_F - 0.001f);
      const float gamma = acosf(clamp(dot(dir, sun), -1.0f, 1.0f));
      const float x = zenith_x * perez(perez_x, view_theta, gamma);
      const float y = zenith_y * perez(perez_y, view_theta, gamma);
      const float Y = zenith_Y * perez(perez_Y, view_theta, gamma);
      if (!(y > 1e-6f) || !std::isfinite(Y)) {
        return zero_float3();
      }
      /* xyY to XYZ to linear Rec.709 */
      const float X = x / y * Y;
      const float Z = (1.0f - x - y) / y * Y;
      return max(make_float3(3.2404542f * X - 1.5371385f * Y - 0.4985314f * Z,
                             -0.9692660f * X + 1.8760108f * Y + 0.0415560f * Z,
                             0.0556434f * X - 0.2040259f * Y + 1.0572252f * Z),
                 zero_float3());
    };

    /* By the elevation: the mean color around the horizon, without the factor of the sun. */
    const int size = 64;
    const int turns = 48;
    vector<float3> by_elevation(size);
    for (int i = 0; i < size; i++) {
      const float z = float(i) / float(size - 1);
      const float r = sqrtf(max(1.0f - z * z, 0.0f));
      float3 color = zero_float3();
      float factor = 0.0f;
      for (int j = 0; j < turns; j++) {
        const float phi = M_2PI_F * (float(j) + 0.5f) / float(turns);
        const float3 dir = make_float3(r * cosf(phi), r * sinf(phi), z);
        color += radiance(dir);
        factor += sun_factor(acosf(clamp(dot(dir, sun), -1.0f, 1.0f)));
      }
      by_elevation[i] = color / max(factor, 1e-6f);
    }
    /* By the cosine of the angle to the sun, from -1 to 1. */
    vector<float3> by_sun(size);
    for (int i = 0; i < size; i++) {
      const float cosine = 2.0f * float(i) / float(size - 1) - 1.0f;
      by_sun[i] = make_float3(sun_factor(acosf(cosine)));
    }

    const RdlValue vector = is_linked(node, "vector") ? to_world(input_value(node, "vector")) :
                                                        to_world(render_normal());
    if (vector.is_constant()) {
      return constant(radiance(safe_normalize(vector.scale)));
    }
    const RdlValue dir = op(OP_NORMALIZE, vector);
    const RdlValue color = ramp_map(
        to_float(dir, CHANNEL_B), by_elevation, true, 0.0f, 1.0f, false);
    const RdlValue factor = ramp_map(
        as_scalar(op(OP_DOT, dir, constant(sun))), by_sun, true, -1.0f, 1.0f, true);
    RdlValue result = multiply(color, factor);
    result.scalar = false;
    return result;
  }

  /* The Hosek and Wilkie sky: each of its X, Y and Z is A(elevation) * (Q(angle to the sun) +
   * H(elevation)), which is three ramps. See #sky_radiance_hosek. */
  RdlValue hosek_sky_value(SkyTextureNode *node)
  {
    float3 sun = safe_normalize(node->get_sun_direction());
    if (is_zero(sun)) {
      sun = make_float3(0.0f, 0.0f, 1.0f);
    }
    const float sun_theta = clamp(acosf(clamp(sun.z, -1.0f, 1.0f)), 0.0f, M_PI_2_F);
    SKY_ArHosekSkyModelState *state = SKY_arhosek_xyz_skymodelstate_alloc_init(
        double(clamp(node->get_turbidity(), 0.0f, 10.0f)),
        double(node->get_ground_albedo()),
        double(M_PI_2_F - sun_theta));
    float config[3][9];
    float3 radiance;
    for (int c = 0; c < 3; c++) {
      for (int i = 0; i < 9; i++) {
        config[c][i] = float(state->configs[c][i]);
      }
      radiance[c] = float(state->radiances[c]);
    }
    SKY_arhosekskymodelstate_free(state);

    const auto by_elevation = [&](const float z, float3 &scale, float3 &zenith) {
      const float cos_theta = max(z, cosf(M_PI_2_F - 0.001f));
      for (int c = 0; c < 3; c++) {
        scale[c] = (1.0f + config[c][0] * expf(config[c][1] / (cos_theta + 0.01f))) * radiance[c];
        zenith[c] = config[c][7] * sqrtf(cos_theta);
      }
    };
    const auto by_sun = [&](const float cos_gamma) {
      const float gamma = acosf(clamp(cos_gamma, -1.0f, 1.0f));
      float3 result;
      for (int c = 0; c < 3; c++) {
        const float mie = (1.0f + cos_gamma * cos_gamma) /
                          powf(1.0f + config[c][8] * config[c][8] -
                                   2.0f * config[c][8] * cos_gamma,
                               1.5f);
        result[c] = config[c][2] + config[c][3] * expf(config[c][4] * gamma) +
                    config[c][5] * cos_gamma * cos_gamma + config[c][6] * mie;
      }
      return result;
    };
    /* XYZ to linear Rec.709, with the strength of the model in Cycles. */
    const float strength = M_2PI_F / 683.0f;
    const float3 to_rgb[3] = {make_float3(3.2404542f, -1.5371385f, -0.4985314f) * strength,
                              make_float3(-0.9692660f, 1.8760108f, 0.0415560f) * strength,
                              make_float3(0.0556434f, -0.2040259f, 1.0572252f) * strength};
    const auto rgb_of = [&](const float3 xyz) {
      return max(make_float3(dot(to_rgb[0], xyz), dot(to_rgb[1], xyz), dot(to_rgb[2], xyz)),
                 zero_float3());
    };

    const RdlValue lookup = is_linked(node, "vector") ? to_world(input_value(node, "vector")) :
                                                        to_world(render_normal());
    if (lookup.is_constant()) {
      const float3 dir = safe_normalize(lookup.scale);
      float3 scale, zenith;
      by_elevation(dir.z, scale, zenith);
      return constant(rgb_of(scale * (by_sun(dot(dir, sun)) + zenith)));
    }

    const int size = 256;
    vector<float3> scale_table(size), zenith_table(size), sun_table(size);
    for (int i = 0; i < size; i++) {
      const float t = float(i) / float(size - 1);
      float3 scale, zenith;
      by_elevation(t, scale, zenith);
      scale_table[i] = scale;
      zenith_table[i] = scale * zenith;
      sun_table[i] = by_sun(2.0f * t - 1.0f);
    }
    const RdlValue dir = op(OP_NORMALIZE, lookup);
    const RdlValue elevation = to_float(dir, CHANNEL_B);
    const RdlValue cosine = as_scalar(op(OP_DOT, dir, constant(sun)));
    RdlValue xyz = add(multiply(ramp_map(elevation, scale_table, true, 0.0f, 1.0f, false),
                                ramp_map(cosine, sun_table, true, -1.0f, 1.0f, false)),
                       ramp_map(elevation, zenith_table, true, 0.0f, 1.0f, false));
    xyz.scalar = false;
    RdlValue result = combine(op(OP_DOT, xyz, constant(to_rgb[0])),
                              op(OP_DOT, xyz, constant(to_rgb[1])),
                              op(OP_DOT, xyz, constant(to_rgb[2])));
    result = op(OP_MAXIMUM, result, constant(zero_float3()));
    result.scalar = false;
    return result;
  }

  RdlValue procedural_fallback(ShaderNode *node, ShaderOutput *output)
  {
    if (output->socket_type.name == "fac") {
      note(node, "no counterpart in MoonRay, its factor is 0.5");
      return constant(0.5f);
    }
    if (find_input(node, "color1")) {
      note(node, "no counterpart in MoonRay, it has its first color everywhere");
      return input_value(node, "color1");
    }
    note(node, "no counterpart in MoonRay, it is grey");
    return constant(make_float3(0.5f));
  }

  /* Colors. */

  RdlValue rgb_to_hsv_value(const RdlValue &color)
  {
    if (color.is_constant()) {
      return constant(rgb_to_hsv(color.scale));
    }
    const int object = new_object("RgbToHsvMap");
    set(object, "input", bind_rgb(color));
    return map_value(object);
  }

  RdlValue hsv_to_rgb_value(const RdlValue &color)
  {
    if (color.is_constant()) {
      return constant(hsv_to_rgb(color.scale));
    }
    const int object = new_object("HsvToRgbMap");
    set(object, "input", bind_rgb(color));
    return map_value(object);
  }

  RdlValue separate_value(ShaderNode *node, ShaderOutput *output)
  {
    const bool is_color = node->type == SeparateColorNode::get_node_type();
    RdlValue in = to_world(input_value(node, is_color ? "color" : "vector"));
    if (is_color) {
      const NodeCombSepColorType color_type =
          static_cast<SeparateColorNode *>(node)->get_color_type();
      if (color_type != NODE_COMBSEP_COLOR_RGB) {
        if (color_type == NODE_COMBSEP_COLOR_HSL) {
          note(node, "HSL has no counterpart, HSV is used");
        }
        in = rgb_to_hsv_value(in);
      }
    }
    const int index = int(std::find(node->outputs.begin(), node->outputs.end(), output) -
                          node->outputs.begin());
    return to_float(in, (index == 0) ? CHANNEL_R : (index == 1) ? CHANNEL_G : CHANNEL_B);
  }

  RdlValue combine_value(ShaderNode *node)
  {
    const bool is_color = node->type == CombineColorNode::get_node_type();
    RdlValue channels[3];
    for (int i = 0; i < 3; i++) {
      ShaderInput *input = node->inputs[i];
      channels[i] = input->link ? output_value(input->link) :
                                  socket_value(node, input->socket_type);
      if (!channels[i].scalar) {
        channels[i] = to_float(to_world(channels[i]), CHANNEL_AVERAGE);
      }
    }
    RdlValue result = combine(channels[0], channels[1], channels[2]);
    if (is_color) {
      const NodeCombSepColorType color_type =
          static_cast<CombineColorNode *>(node)->get_color_type();
      if (color_type != NODE_COMBSEP_COLOR_RGB) {
        if (color_type == NODE_COMBSEP_COLOR_HSL) {
          note(node, "HSL has no counterpart, HSV is used");
        }
        result = hsv_to_rgb_value(result);
      }
    }
    return result;
  }

  RdlValue hsv_value(ShaderNode *node)
  {
    const RdlValue color = input_value(node, "color");
    const RdlValue hue = input_float(node, "hue");
    const RdlValue saturation = input_float(node, "saturation");
    const RdlValue value = input_float(node, "value");
    const RdlValue fac = input_float(node, "fac");

    RdlValue result;
    if (color.is_constant() && hue.is_constant() && saturation.is_constant() &&
        value.is_constant())
    {
      float3 hsv = rgb_to_hsv(color.scale);
      hsv.x = fmodf(hsv.x + hue.scale.x + 0.5f, 1.0f);
      hsv.y = saturatef(hsv.y * saturation.scale.x);
      hsv.z *= value.scale.x;
      result = constant(max(hsv_to_rgb(hsv), zero_float3()));
    }
    else if (is_constant(hue, 0.5f) && is_constant(saturation, 1.0f)) {
      /* Only brighter or darker. */
      result = multiply(color, value);
      result.scalar = false;
    }
    else if (!(saturation.is_constant() && saturation.scale.x <= 1.0f)) {
      /* The saturation is limited to 1, which the ColorCorrectHsvMap does not do. */
      const RdlValue hsv = rgb_to_hsv_value(color);
      RdlValue h = to_float(hsv, CHANNEL_R);
      if (!is_constant(hue, 0.5f)) {
        h = op(OP_FRACTION, add(add(h, hue), constant(0.5f)));
      }
      const RdlValue s = clamp_value(multiply(to_float(hsv, CHANNEL_G), saturation), 0.0f, 1.0f);
      const RdlValue v = multiply(to_float(hsv, CHANNEL_B), value);
      result = hsv_to_rgb_value(combine(h, s, v));
    }
    else {
      const int object = new_object("ColorCorrectHsvMap");
      set(object, "input", bind_rgb(color));
      /* Degrees, and a hue of a half is no change. */
      if (hue.is_constant()) {
        const float shift = fmodf(hue.scale.x + 0.5f, 1.0f);
        if (shift != 0.0f) {
          set(object, "hue_shift", number(shift * 360.0f));
        }
      }
      else {
        set(object, "hue_shift", bind_float(affine(hue, 360.0f, 180.0f)));
      }
      if (!is_constant(saturation, 1.0f)) {
        set(object, "saturation_factor", bind_float(saturation));
      }
      if (!is_constant(value, 1.0f)) {
        set(object, "value_factor", bind_float(value));
      }
      result = map_value(object);
    }
    return blend(color, result, fac);
  }

  RdlValue bright_contrast_value(ShaderNode *node)
  {
    const RdlValue color = input_value(node, "color");
    const RdlValue bright = input_float(node, "bright");
    const RdlValue contrast = input_float(node, "contrast");
    RdlValue result;
    if (bright.is_constant() && contrast.is_constant()) {
      result = affine(color, 1.0f + contrast.scale.x, bright.scale.x - contrast.scale.x * 0.5f);
    }
    else {
      result = add(multiply(color, add(contrast, constant(1.0f))),
                   subtract(bright, scaled(contrast, 0.5f)));
    }
    result = op(OP_MAXIMUM, result, constant(zero_float3()));
    result.scalar = false;
    return result;
  }

  /* Ramps and curves are lookup tables in Cycles. */

  RdlValue ramp_value(RGBRampNode *node, const bool alpha)
  {
    const RdlValue fac = input_float(node, "fac");
    vector<float3> table;
    if (alpha) {
      for (const float a : node->get_ramp_alpha()) {
        table.push_back(make_float3(a));
      }
    }
    else {
      for (const packed_float3 &color : node->get_ramp()) {
        table.push_back(float3(color));
      }
    }
    float error = 0.0f;
    const RdlValue result = ramp_map(
        fac, table, node->get_interpolate(), 0.0f, 1.0f, alpha, &error);
    if (error > 0.02f) {
      note(node, string_printf("the ramp is approximated with %d control points, off by up to %.2f",
                               RAMP_MAX_POINTS, double(error)));
    }
    return result;
  }

  RdlValue curve_channel(ShaderNode *node,
                         const RdlValue &input,
                         const vector<float3> &table,
                         const float min_x,
                         const float max_x)
  {
    /* The curve which changes nothing needs no map. */
    bool identity = min_x <= 0.0f && max_x >= 1.0f;
    const int size = int(table.size());
    for (int i = 0; i < size && identity; i++) {
      const float x = min_x + (max_x - min_x) * float(i) / float(max(size - 1, 1));
      identity = fabsf(table[i].x - x) < 1e-3f;
    }
    if (identity) {
      return input;
    }
    float error = 0.0f;
    const RdlValue result = ramp_map(input, table, true, min_x, max_x, true, &error);
    if (error > 0.02f) {
      note(node,
           string_printf("a curve is approximated with %d control points, off by up to %.2f",
                         RAMP_MAX_POINTS, double(error)));
    }
    return result;
  }

  RdlValue curves_value(CurvesNode *node)
  {
    const RdlValue value = to_world(input_value(node, "value"));
    const RdlValue fac = input_float(node, "fac");
    const array<packed_float3> &curves = node->get_curves();
    RdlValue channels[3];
    for (int c = 0; c < 3; c++) {
      vector<float3> table;
      for (const packed_float3 &entry : curves) {
        table.push_back(make_float3(float3(entry)[c]));
      }
      channels[c] = curve_channel(node,
                                  to_float(value, CHANNEL_R + c),
                                  table,
                                  node->get_min_x(),
                                  node->get_max_x());
    }
    return blend(value, combine(channels[0], channels[1], channels[2]), fac);
  }

  RdlValue float_curve_value(FloatCurveNode *node)
  {
    const RdlValue value = input_float(node, "value");
    vector<float3> table;
    for (const float entry : node->get_curve()) {
      table.push_back(make_float3(entry));
    }
    const RdlValue result = curve_channel(
        node, value, table, node->get_min_x(), node->get_max_x());
    return blend(value, result, input_float(node, "fac"));
  }

  /* Numbers. */

  RdlValue clamp_node_value(ClampNode *node)
  {
    const RdlValue value = input_float(node, "value");
    RdlValue low = input_float(node, "min");
    RdlValue high = input_float(node, "max");
    if (low.is_constant() && high.is_constant()) {
      float a = low.scale.x;
      float b = high.scale.x;
      if (node->get_clamp_type() == NODE_CLAMP_RANGE && a > b) {
        std::swap(a, b);
      }
      return clamp_value(value, a, b);
    }
    if (node->get_clamp_type() == NODE_CLAMP_RANGE) {
      const RdlValue smaller = op(OP_MINIMUM, low, high);
      high = op(OP_MAXIMUM, low, high);
      low = smaller;
    }
    return op(OP_MINIMUM, op(OP_MAXIMUM, value, low), high);
  }

  RdlValue map_range_value(MapRangeNode *node)
  {
    const RdlValue value = input_float(node, "value");
    const RdlValue from_min = input_float(node, "from_min");
    const RdlValue from_max = input_float(node, "from_max");
    const RdlValue to_min = input_float(node, "to_min");
    const RdlValue to_max = input_float(node, "to_max");
    const NodeMapRangeType type = node->get_range_type();
    const bool constant_range = from_min.is_constant() && from_max.is_constant() &&
                                to_min.is_constant() && to_max.is_constant();

    if (!constant_range) {
      if (type != NODE_MAP_RANGE_LINEAR) {
        note(node, "with a range which is not constant only the linear type is exported");
      }
      const int object = new_object("RemapMap");
      set(object, "input", bind_rgb(value));
      set(object, "input_min", bind_float(from_min));
      set(object, "input_max", bind_float(from_max));
      set(object, "output_min", bind_float(to_min));
      set(object, "output_max", bind_float(to_max));
      set(object, "clamp", false);
      RdlValue result = map_value(object, true);
      if (node->get_clamp()) {
        result = op(OP_MINIMUM,
                    op(OP_MAXIMUM, result, op(OP_MINIMUM, to_min, to_max)),
                    op(OP_MAXIMUM, to_min, to_max));
      }
      return result;
    }

    const float a = from_min.scale.x;
    const float b = from_max.scale.x;
    const float c = to_min.scale.x;
    const float d = to_max.scale.x;
    if (a == b) {
      return constant(0.0f);
    }
    /* The position in the range. */
    RdlValue t = affine(value, 1.0f / (b - a), -a / (b - a));
    switch (type) {
      case NODE_MAP_RANGE_STEPPED: {
        const float steps = input_constant(node, "steps", 4.0f);
        if (steps > 0.0f) {
          t = scaled(op(OP_FLOOR, scaled(t, steps + 1.0f)), 1.0f / steps);
        }
        break;
      }
      case NODE_MAP_RANGE_SMOOTHSTEP:
        t = function_ramp(t, [](const float x) { return x * x * (3.0f - 2.0f * x); });
        break;
      case NODE_MAP_RANGE_SMOOTHERSTEP:
        t = function_ramp(
            t, [](const float x) { return x * x * x * (x * (x * 6.0f - 15.0f) + 10.0f); });
        break;
      default:
        break;
    }
    RdlValue result = affine(t, d - c, c);
    if (node->get_clamp() && type == NODE_MAP_RANGE_LINEAR) {
      result = clamp_value(result, min(c, d), max(c, d));
    }
    return result;
  }

  /* a / b as Cycles divides: zero when b is. */
  RdlValue safe_divide_value(ShaderNode *node, const RdlValue &a, const RdlValue &b)
  {
    if (b.is_constant()) {
      float3 inverse = zero_float3();
      for (int i = 0; i < 3; i++) {
        inverse[i] = (b.scale[i] != 0.0f) ? 1.0f / b.scale[i] : 0.0f;
      }
      RdlValue result = multiply(a, constant(inverse, b.scalar));
      result.scalar = a.scalar && b.scalar;
      return result;
    }
    /* The OpMap divides by at least a small positive number: a b / b^2 */
    (void)node;
    return op(OP_DIVIDE, multiply(a, b), multiply(b, b));
  }

  /* a^b as Cycles has it: also for a negative a when b is a whole number, zero otherwise. The
   * OpMap makes the base positive. */
  RdlValue power_value(ShaderNode *node, const RdlValue &a, const RdlValue &b)
  {
    if (a.is_constant() && b.is_constant()) {
      float3 result;
      for (int i = 0; i < 3; i++) {
        const float base = a.scale[i];
        const float exponent = b.scale[i];
        result[i] = (base < 0.0f && exponent != floorf(exponent)) ? 0.0f : powf(base, exponent);
        if (!std::isfinite(result[i])) {
          result[i] = 0.0f;
        }
      }
      return constant(result, a.scalar && b.scalar);
    }
    if (a.is_constant() && reduce_min(a.scale) > 0.0f) {
      return op(OP_POWER, a, b);
    }
    if (b.is_constant() && is_uniform(b.scale)) {
      const float exponent = b.scale.x;
      const bool whole = exponent == floorf(exponent);
      const bool odd = whole && fmodf(fabsf(exponent), 2.0f) == 1.0f;
      if (whole && !odd) {
        return op(OP_POWER, op(OP_ABS, a), b);
      }
      const RdlValue positive = op(OP_GREATER_THAN, a, constant(zero_float3()));
      if (odd) {
        /* |a|^b with the sign of a */
        const RdlValue sign = subtract(positive, op(OP_LESS_THAN, a, constant(zero_float3())));
        return multiply(op(OP_POWER, op(OP_ABS, a), b), sign);
      }
      if (exponent > 0.0f) {
        /* A small positive base gives next to nothing already. */
        return op(OP_POWER, a, b);
      }
      return multiply(op(OP_POWER, a, b), positive);
    }
    note(node, "a power with an exponent which is not constant is zero for a negative base");
    return multiply(op(OP_POWER, a, b), op(OP_GREATER_THAN, a, constant(zero_float3())));
  }

  /* atan2(y, x) = sign(y) acos(x / sqrt(x^2 + y^2)) */
  RdlValue arctan2(const RdlValue &y, const RdlValue &x)
  {
    const RdlValue length = op(OP_POWER, add(multiply(x, x), multiply(y, y)), constant(0.5f));
    const RdlValue angle = op(OP_ACOS, op(OP_DIVIDE, x, length));
    const RdlValue sign = affine(op(OP_GREATER_EQUAL, y, constant(0.0f)), 2.0f, -1.0f);
    return as_scalar(multiply(angle, sign));
  }

  /* The natural logarithm, which the OpMap does not have: n (x^(1/n) - 1) for a large n, zero
   * for what is not positive. */
  RdlValue logarithm(const RdlValue &a)
  {
    if (a.is_constant()) {
      return constant((a.scale.x > 0.0f) ? logf(a.scale.x) : 0.0f);
    }
    const float n = 1024.0f;
    const RdlValue root = op(OP_POWER, a, constant(1.0f / n));
    return multiply(affine(root, n, -n), op(OP_GREATER_THAN, a, constant(0.0f)));
  }

  /* a - floor(a / b) * b */
  RdlValue floored_modulo(ShaderNode *node, const RdlValue &a, const RdlValue &b)
  {
    return subtract(a, multiply(op(OP_FLOOR, safe_divide_value(node, a, b)), b));
  }

  RdlValue math_value(MathNode *node)
  {
    const RdlValue a = input_float(node, "value1");
    const RdlValue b = input_float(node, "value2");
    const RdlValue c = input_float(node, "value3");
    const RdlValue e = constant(2.718281828f);

    RdlValue result;
    switch (node->get_math_type()) {
      case NODE_MATH_ADD:
        result = add(a, b);
        break;
      case NODE_MATH_SUBTRACT:
        result = subtract(a, b);
        break;
      case NODE_MATH_MULTIPLY:
        result = multiply(a, b);
        break;
      case NODE_MATH_DIVIDE:
        result = safe_divide_value(node, a, b);
        break;
      case NODE_MATH_SINE:
        result = op(OP_SINE, a);
        break;
      case NODE_MATH_COSINE:
        result = op(OP_COSINE, a);
        break;
      case NODE_MATH_TANGENT:
        result = safe_divide_value(node, op(OP_SINE, a), op(OP_COSINE, a));
        break;
      case NODE_MATH_ARCTAN2:
        result = arctan2(a, b);
        break;
      case NODE_MATH_ARCSINE:
        result = affine(op(OP_ACOS, a), -1.0f, M_PI_2_F);
        break;
      case NODE_MATH_ARCCOSINE:
        result = op(OP_ACOS, a);
        break;
      case NODE_MATH_ARCTANGENT: {
        /* atan(x) = asin(x / sqrt(1 + x^2)) */
        const RdlValue length = op(OP_POWER, add(multiply(a, a), constant(1.0f)), constant(0.5f));
        result = affine(op(OP_ACOS, op(OP_DIVIDE, a, length)), -1.0f, M_PI_2_F);
        break;
      }
      case NODE_MATH_POWER:
        result = power_value(node, a, b);
        break;
      case NODE_MATH_LOGARITHM:
        /* log(a) / log(b), zero unless both are positive. */
        result = safe_divide_value(node, logarithm(a), logarithm(b));
        break;
      case NODE_MATH_MINIMUM:
        result = op(OP_MINIMUM, a, b);
        break;
      case NODE_MATH_MAXIMUM:
        result = op(OP_MAXIMUM, a, b);
        break;
      case NODE_MATH_SMOOTH_MIN:
        note(node, "the smooth minimum is the plain minimum");
        result = op(OP_MINIMUM, a, b);
        break;
      case NODE_MATH_SMOOTH_MAX:
        note(node, "the smooth maximum is the plain maximum");
        result = op(OP_MAXIMUM, a, b);
        break;
      case NODE_MATH_ROUND:
        result = op(OP_ROUND, a);
        break;
      case NODE_MATH_LESS_THAN:
        result = op(OP_LESS_THAN, a, b);
        break;
      case NODE_MATH_GREATER_THAN:
        result = op(OP_GREATER_THAN, a, b);
        break;
      case NODE_MATH_MODULO:
        result = op(OP_MODULO, a, op(OP_ABS, b));
        break;
      case NODE_MATH_FLOORED_MODULO:
        result = floored_modulo(node, a, b);
        break;
      case NODE_MATH_ABSOLUTE:
        result = op(OP_ABS, a);
        break;
      case NODE_MATH_FLOOR:
        result = op(OP_FLOOR, a);
        break;
      case NODE_MATH_CEIL:
        result = op(OP_CEIL, a);
        break;
      case NODE_MATH_FRACTION:
        result = op(OP_FRACTION, a);
        break;
      case NODE_MATH_TRUNC:
        result = subtract(a, op(OP_MODULO, a, constant(1.0f)));
        break;
      case NODE_MATH_SQRT:
        result = op(OP_POWER, a, constant(0.5f));
        break;
      case NODE_MATH_INV_SQRT:
        result = power_value(node, a, constant(-0.5f));
        break;
      case NODE_MATH_SIGN:
        result = subtract(op(OP_GREATER_THAN, a, constant(0.0f)),
                          op(OP_LESS_THAN, a, constant(0.0f)));
        break;
      case NODE_MATH_EXPONENT:
        result = op(OP_POWER, e, a);
        break;
      case NODE_MATH_RADIANS:
        result = scaled(a, M_PI_F / 180.0f);
        break;
      case NODE_MATH_DEGREES:
        result = scaled(a, 180.0f / M_PI_F);
        break;
      case NODE_MATH_SINH:
      case NODE_MATH_COSH:
      case NODE_MATH_TANH: {
        const RdlValue up = op(OP_POWER, e, a);
        const RdlValue down = op(OP_POWER, e, scaled(a, -1.0f));
        if (node->get_math_type() == NODE_MATH_SINH) {
          result = scaled(subtract(up, down), 0.5f);
        }
        else if (node->get_math_type() == NODE_MATH_COSH) {
          result = scaled(add(up, down), 0.5f);
        }
        else {
          result = op(OP_DIVIDE, subtract(up, down), add(up, down));
        }
        break;
      }
      case NODE_MATH_SNAP:
        result = multiply(op(OP_FLOOR, safe_divide_value(node, a, b)), b);
        break;
      case NODE_MATH_WRAP: {
        /* value, max, min */
        const RdlValue range = subtract(b, c);
        result = subtract(
            a, multiply(range, op(OP_FLOOR, safe_divide_value(node, subtract(a, c), range))));
        break;
      }
      case NODE_MATH_PINGPONG: {
        /* fabs(fract((a - b) / (b * 2)) * b * 2 - b) */
        const RdlValue twice = scaled(b, 2.0f);
        const RdlValue turns = op(OP_FRACTION, safe_divide_value(node, subtract(a, b), twice));
        result = op(OP_ABS, subtract(multiply(turns, twice), b));
        break;
      }
      case NODE_MATH_COMPARE:
        result = op(OP_LESS_EQUAL,
                    op(OP_ABS, subtract(a, b)),
                    op(OP_MAXIMUM, c, constant(1e-5f)));
        break;
      case NODE_MATH_MULTIPLY_ADD:
        result = add(multiply(a, b), c);
        break;
      default:
        note(node, "an operation without a counterpart, its first value is used unchanged");
        result = a;
        break;
    }
    result = as_scalar(result);
    if (node->get_use_clamp()) {
      result = clamp_value(result, 0.0f, 1.0f);
    }
    return result;
  }

  RdlValue vector_math_value(VectorMathNode *node, const bool want_value)
  {
    const NodeVectorMathType type = node->get_math_type();
    RdlValue a = input_value(node, "vector1");
    RdlValue b = input_value(node, "vector2");
    a.scalar = false;
    b.scalar = false;

    /* What does not depend on the space the vectors are in. */
    bool render_space = false;
    switch (type) {
      case NODE_VECTOR_MATH_ADD:
      case NODE_VECTOR_MATH_SUBTRACT:
      case NODE_VECTOR_MATH_CROSS_PRODUCT:
      case NODE_VECTOR_MATH_PROJECT:
      case NODE_VECTOR_MATH_REFLECT:
      case NODE_VECTOR_MATH_DOT_PRODUCT:
      case NODE_VECTOR_MATH_DISTANCE:
        render_space = same_space(a, b);
        break;
      case NODE_VECTOR_MATH_LENGTH:
      case NODE_VECTOR_MATH_SCALE:
      case NODE_VECTOR_MATH_NORMALIZE:
        render_space = a.render_space;
        break;
      default:
        a = to_world(a);
        b = to_world(b);
        break;
    }

    RdlValue vector = constant(zero_float3());
    RdlValue value = constant(0.0f);
    switch (type) {
      case NODE_VECTOR_MATH_ADD:
        vector = add(a, b);
        break;
      case NODE_VECTOR_MATH_SUBTRACT:
        vector = subtract(a, b);
        break;
      case NODE_VECTOR_MATH_MULTIPLY:
        vector = multiply(a, b);
        break;
      case NODE_VECTOR_MATH_DIVIDE:
        vector = safe_divide_value(node, a, b);
        break;
      case NODE_VECTOR_MATH_CROSS_PRODUCT:
        vector = op(OP_CROSS, a, b);
        break;
      case NODE_VECTOR_MATH_PROJECT:
        vector = multiply(b, op(OP_DIVIDE, op(OP_DOT, a, b), op(OP_DOT, b, b)));
        break;
      case NODE_VECTOR_MATH_REFLECT: {
        const RdlValue normal = op(OP_NORMALIZE, b);
        vector = subtract(a, scaled(multiply(normal, op(OP_DOT, a, normal)), 2.0f));
        break;
      }
      case NODE_VECTOR_MATH_DOT_PRODUCT:
        value = op(OP_DOT, a, b);
        break;
      case NODE_VECTOR_MATH_DISTANCE:
        value = op(OP_LENGTH, subtract(a, b));
        break;
      case NODE_VECTOR_MATH_LENGTH:
        value = op(OP_LENGTH, a);
        break;
      case NODE_VECTOR_MATH_SCALE:
        vector = multiply(a, input_float(node, "scale"));
        break;
      case NODE_VECTOR_MATH_NORMALIZE:
        vector = op(OP_NORMALIZE, a);
        break;
      case NODE_VECTOR_MATH_SNAP:
        vector = multiply(op(OP_FLOOR, safe_divide_value(node, a, b)), b);
        break;
      case NODE_VECTOR_MATH_FLOOR:
        vector = op(OP_FLOOR, a);
        break;
      case NODE_VECTOR_MATH_CEIL:
        vector = op(OP_CEIL, a);
        break;
      case NODE_VECTOR_MATH_ROUND:
        vector = op(OP_ROUND, a);
        break;
      case NODE_VECTOR_MATH_MODULO:
        vector = op(OP_MODULO, a, op(OP_ABS, b));
        break;
      case NODE_VECTOR_MATH_WRAP: {
        /* value, max, min */
        const RdlValue c = to_world(input_value(node, "vector3"));
        const RdlValue range = subtract(b, c);
        vector = subtract(
            a, multiply(range, op(OP_FLOOR, safe_divide_value(node, subtract(a, c), range))));
        break;
      }
      case NODE_VECTOR_MATH_FRACTION:
        vector = op(OP_FRACTION, a);
        break;
      case NODE_VECTOR_MATH_ABSOLUTE:
        vector = op(OP_ABS, a);
        break;
      case NODE_VECTOR_MATH_MINIMUM:
        vector = op(OP_MINIMUM, a, b);
        break;
      case NODE_VECTOR_MATH_MAXIMUM:
        vector = op(OP_MAXIMUM, a, b);
        break;
      case NODE_VECTOR_MATH_SINE:
        vector = op(OP_SINE, a);
        break;
      case NODE_VECTOR_MATH_COSINE:
        vector = op(OP_COSINE, a);
        break;
      case NODE_VECTOR_MATH_TANGENT:
        vector = safe_divide_value(node, op(OP_SINE, a), op(OP_COSINE, a));
        break;
      case NODE_VECTOR_MATH_POWER:
        vector = power_value(node, a, b);
        break;
      case NODE_VECTOR_MATH_SIGN:
        vector = subtract(op(OP_GREATER_THAN, a, constant(zero_float3())),
                          op(OP_LESS_THAN, a, constant(zero_float3())));
        break;
      case NODE_VECTOR_MATH_MULTIPLY_ADD:
        vector = add(multiply(a, b), to_world(input_value(node, "vector3")));
        break;
      default:
        note(node, "an operation without a counterpart, its first vector is used unchanged");
        vector = a;
        break;
    }
    if (want_value) {
      return as_scalar(value);
    }
    vector.scalar = false;
    vector.render_space = render_space && !vector.is_constant();
    return vector;
  }

  /* The HSV of `base` with channels taken from `other`, weighted by `factor`. */
  RdlValue mix_hsv(const NodeMix blend_type,
                   const RdlValue &base,
                   const RdlValue &other,
                   const RdlValue &factor)
  {
    const RdlValue hsv_base = rgb_to_hsv_value(base);
    const RdlValue hsv_other = rgb_to_hsv_value(other);
    const RdlValue h1 = to_float(hsv_base, CHANNEL_R);
    const RdlValue s1 = to_float(hsv_base, CHANNEL_G);
    const RdlValue v1 = to_float(hsv_base, CHANNEL_B);
    const RdlValue h2 = to_float(hsv_other, CHANNEL_R);
    const RdlValue s2 = to_float(hsv_other, CHANNEL_G);
    const RdlValue v2 = to_float(hsv_other, CHANNEL_B);
    switch (blend_type) {
      case NODE_MIX_VAL:
        return hsv_to_rgb_value(combine(h1, s1, blend(v1, v2, factor)));
      case NODE_MIX_SAT:
        return hsv_to_rgb_value(combine(h1, blend(s1, s2, factor), v1));
      case NODE_MIX_HUE:
        return blend(base, hsv_to_rgb_value(combine(h2, s1, v1)), factor);
      default:
        return blend(base, hsv_to_rgb_value(combine(h2, s2, v1)), factor);
    }
  }

  RdlValue mix_value(ShaderNode *node)
  {
    const bool legacy = node->type == MixNode::get_node_type();
    RdlValue a = input_value(node, legacy ? "color1" : "a");
    RdlValue b = input_value(node, legacy ? "color2" : "b");
    RdlValue factor = input_value(node, "fac");
    if (node->type != MixVectorNonUniformNode::get_node_type() && !factor.scalar) {
      factor = to_float(to_world(factor), CHANNEL_AVERAGE);
    }

    NodeMix blend_type = NODE_MIX_BLEND;
    bool clamp_result = false;
    if (node->type == MixColorNode::get_node_type()) {
      blend_type = static_cast<MixColorNode *>(node)->get_blend_type();
      clamp_result = static_cast<MixColorNode *>(node)->get_use_clamp_result();
    }
    else if (legacy) {
      blend_type = static_cast<MixNode *>(node)->get_mix_type();
      clamp_result = static_cast<MixNode *>(node)->get_use_clamp();
    }

    if (blend_type != NODE_MIX_BLEND) {
      a = to_world(a);
      b = to_world(b);
    }
    else if (a.render_space != b.render_space) {
      same_space(a, b);
    }

    if (node->type == MixVectorNonUniformNode::get_node_type() && !factor.scalar) {
      /* A factor for each component: a + (b - a) * factor */
      RdlValue result = add(a, multiply(subtract(b, a), clamp_value(factor, 0.0f, 1.0f)));
      result.scalar = false;
      return result;
    }

    const bool full = is_constant(factor, 1.0f);
    const RdlValue one = constant(one_float3());
    RdlValue result;
    /* Most of these are lerp(a, f(a, b), factor) in Cycles. */
    switch (blend_type) {
      case NODE_MIX_BLEND:
        result = blend(a, b, factor);
        break;
      case NODE_MIX_ADD:
        result = factor.is_constant() ? add(a, multiply(b, clamp_value(factor, 0.0f, 1.0f))) :
                                        blend(a, add(a, b), factor);
        break;
      case NODE_MIX_SUB:
        result = factor.is_constant() ?
                     subtract(a, multiply(b, clamp_value(factor, 0.0f, 1.0f))) :
                     blend(a, subtract(a, b), factor);
        break;
      case NODE_MIX_MUL:
        result = blend(a, multiply(a, b), factor);
        break;
      case NODE_MIX_SCREEN:
        result = blend(a, op(OP_SCREEN, a, b), factor);
        break;
      case NODE_MIX_OVERLAY:
        result = blend(a, op(OP_OVERLAY, b, a), factor);
        break;
      case NODE_MIX_DIV:
        result = blend(a, op(OP_DIVIDE, a, b), factor);
        break;
      case NODE_MIX_DIFF:
        result = blend(a, op(OP_ABS, subtract(a, b)), factor);
        break;
      case NODE_MIX_EXCLUSION:
        result = op(OP_MAXIMUM,
                    blend(a, subtract(add(a, b), scaled(multiply(a, b), 2.0f)), factor),
                    constant(zero_float3()));
        break;
      case NODE_MIX_DARK:
        result = blend(a, op(OP_MINIMUM, a, b), factor);
        break;
      case NODE_MIX_LIGHT:
        result = blend(a, op(OP_MAXIMUM, a, b), factor);
        break;
      case NODE_MIX_DODGE: {
        /* a / (1 - factor * b), at most 1 */
        const RdlValue weighted = full ? b : multiply(b, clamp_value(factor, 0.0f, 1.0f));
        result = op(OP_MINIMUM, op(OP_DIVIDE, a, invert(weighted)), one);
        break;
      }
      case NODE_MIX_BURN: {
        /* 1 - (1 - a) / (1 - factor + factor * b), in 0..1 */
        const RdlValue divisor = blend(one, b, factor);
        result = clamp_value(invert(op(OP_DIVIDE, invert(a), divisor)), 0.0f, 1.0f);
        break;
      }
      case NODE_MIX_SOFT: {
        const RdlValue screen = op(OP_SCREEN, a, b);
        const RdlValue soft = add(multiply(multiply(invert(a), b), a), multiply(a, screen));
        result = blend(a, soft, factor);
        break;
      }
      case NODE_MIX_LINEAR:
        result = add(a, multiply(affine(b, 2.0f, -1.0f), clamp_value(factor, 0.0f, 1.0f)));
        break;
      case NODE_MIX_HUE:
      case NODE_MIX_SAT:
      case NODE_MIX_VAL:
      case NODE_MIX_COL:
        result = mix_hsv(blend_type, a, b, factor);
        break;
      default:
        note(node, "a blend type without a counterpart, the plain mix is used");
        result = blend(a, b, factor);
        break;
    }
    if (node->type == MixFloatNode::get_node_type()) {
      result = as_scalar(result);
    }
    else if (blend_type != NODE_MIX_BLEND) {
      result.scalar = false;
    }
    if (clamp_result) {
      const bool render_space = result.render_space;
      result = clamp_value(result, 0.0f, 1.0f);
      result.render_space = render_space;
    }
    return result;
  }

  /* Vectors. */

  RdlValue mapping_value(MappingNode *node)
  {
    RdlValue vector = to_world(input_value(node, "vector"));
    TextureMapping mapping = identity_mapping();
    mapping.type = TextureMapping::Type(node->get_mapping_type());
    mapping.rotation = input_constant3(node, "rotation", zero_float3());
    mapping.scale = input_constant3(node, "scale", one_float3());

    /* A location which is not constant is added or subtracted with a map. */
    const bool has_location = mapping.type == TextureMapping::POINT ||
                              mapping.type == TextureMapping::TEXTURE;
    const RdlValue location = has_location ? to_world(input_value(node, "location")) :
                                             constant(zero_float3());
    if (location.is_constant()) {
      mapping.translation = location.scale;
    }
    else if (mapping.type == TextureMapping::TEXTURE) {
      vector = subtract(vector, location);
    }

    RdlValue result = transformed(vector, mapping.compute_transform());
    if (!location.is_constant() && mapping.type == TextureMapping::POINT) {
      result = add(result, location);
    }
    if (mapping.type == TextureMapping::NORMAL) {
      result = op(OP_NORMALIZE, result);
    }
    return result;
  }

  RdlValue vector_transform_value(VectorTransformNode *node)
  {
    RdlValue value = to_world(input_value(node, "vector"));
    const NodeVectorTransformConvertSpace from = node->get_convert_from();
    const NodeVectorTransformConvertSpace to = node->get_convert_to();
    if (from == to) {
      return value;
    }
    int type = TRANSFORM_VECTOR;
    if (node->get_transform_type() == NODE_VECTOR_TRANSFORM_TYPE_POINT) {
      type = TRANSFORM_POINT;
    }
    else if (node->get_transform_type() == NODE_VECTOR_TRANSFORM_TYPE_NORMAL) {
      type = TRANSFORM_NORMAL;
    }
    const float3 flip_z = make_float3(1.0f, 1.0f, -1.0f);

    /* To world space first. */
    if (from == NODE_VECTOR_TRANSFORM_CONVERT_SPACE_OBJECT) {
      value = transform_space(transform_space(value, type, SPACE_OBJECT, SPACE_WORLD),
                              type,
                              SPACE_INSTANCE,
                              SPACE_WORLD);
    }
    else if (from == NODE_VECTOR_TRANSFORM_CONVERT_SPACE_CAMERA) {
      value = transform_space(scaled(value, flip_z), type, SPACE_CAMERA, SPACE_WORLD);
    }
    if (to == NODE_VECTOR_TRANSFORM_CONVERT_SPACE_OBJECT) {
      value = transform_space(transform_space(value, type, SPACE_WORLD, SPACE_OBJECT),
                              type,
                              SPACE_WORLD,
                              SPACE_INSTANCE);
    }
    else if (to == NODE_VECTOR_TRANSFORM_CONVERT_SPACE_CAMERA) {
      value = scaled(transform_space(value, type, SPACE_WORLD, SPACE_CAMERA), flip_z);
    }
    return value;
  }

  /* Weights. */

  static WeightP weight(const float value)
  {
    WeightP result = std::make_shared<Weight>();
    result->constant = value;
    return result;
  }

  static WeightP weight(const RdlValue &value)
  {
    if (value.is_constant()) {
      return weight(value.scalar ? value.scale.x : luminance(value.scale));
    }
    WeightP result = std::make_shared<Weight>();
    result->kind = Weight::VALUE;
    result->value = value;
    return result;
  }

  static bool weight_is(const WeightP &w, const float value)
  {
    return w->kind == Weight::CONSTANT && w->constant == value;
  }

  static bool same_weight(const WeightP &a, const WeightP &b)
  {
    if (a == b) {
      return true;
    }
    if (a->kind != b->kind) {
      return false;
    }
    switch (a->kind) {
      case Weight::CONSTANT:
        return a->constant == b->constant;
      case Weight::VALUE:
        return same_value(a->value, b->value);
      default:
        return same_value(a->value, b->value) && same_weight(a->a, b->a) &&
               same_weight(a->b, b->b);
    }
  }

  /* lerp(a, b, factor) */
  static WeightP mix_weight(const WeightP &a, const WeightP &b, const RdlValue &factor)
  {
    if (same_weight(a, b)) {
      return a;
    }
    if (factor.is_constant()) {
      const float f = clamp(factor.scale.x, 0.0f, 1.0f);
      if (f == 0.0f) {
        return a;
      }
      if (f == 1.0f) {
        return b;
      }
      if (a->kind == Weight::CONSTANT && b->kind == Weight::CONSTANT) {
        return weight(mix(a->constant, b->constant, f));
      }
    }
    WeightP result = std::make_shared<Weight>();
    result->kind = Weight::MIX;
    result->value = factor;
    result->a = a;
    result->b = b;
    return result;
  }

  /* The weight as a value, written when it is asked for the first time. */
  RdlValue weight_value(const WeightP &w)
  {
    if (w->kind == Weight::CONSTANT) {
      return constant(w->constant);
    }
    if (w->kind == Weight::VALUE) {
      return w->value;
    }
    if (!w->written) {
      const RdlValue a = weight_value(w->a);
      const RdlValue b = weight_value(w->b);
      if (!w->value.is_constant() && is_constant(a, 0.0f) && is_constant(b, 1.0f)) {
        /* The factor itself: what takes it clamps it to 0..1. */
        w->written_value = w->value;
      }
      else {
        w->written_value = blend(a, b, w->value);
      }
      w->written = true;
    }
    return w->written_value;
  }

  /* Mix an attribute of two materials whose lobe has the weights `wa` and `wb` in them: by the
   * factor when both have as much of it, and by the share of the lobe otherwise. */
  RdlValue mix_attribute(const RdlValue &a,
                         const RdlValue &b,
                         const WeightP &wa,
                         const WeightP &wb,
                         const RdlValue &factor)
  {
    if (weight_is(wb, 0.0f)) {
      return a;
    }
    if (weight_is(wa, 0.0f)) {
      return b;
    }
    if (same_value(a, b)) {
      return a;
    }
    if (same_weight(wa, wb)) {
      return blend(a, b, factor);
    }
    /* factor * wb / lerp(wa, wb, factor) */
    const RdlValue share = multiply(clamp_factor(factor), weight_value(wb));
    const RdlValue total = weight_value(mix_weight(wa, wb, factor));
    return blend(a, b, divide(share, total));
  }

  RdlValue clamp_factor(const RdlValue &factor)
  {
    return factor.is_constant() ? clamp_value(factor, 0.0f, 1.0f) : factor;
  }

  /* Normals. */

  /* The normal every BSDF starts from: the bump of the displacement output, if there is one. */
  string base_normal()
  {
    if (base_normal_done_) {
      return base_normal_;
    }
    base_normal_done_ = true;
    ShaderInput *displacement = shader_->graph->output()->input("Displacement");
    if (displacement == nullptr || displacement->link == nullptr) {
      return base_normal_;
    }
    ShaderNode *node = displacement->link->parent;
    if (node->type != DisplacementNode::get_node_type()) {
      note(node, "as the displacement of the material it is not exported, the surface is smooth");
      return base_normal_;
    }
    if (shader_->get_displacement_method() != DISPLACE_BUMP) {
      note("true displacement is not exported, the displacement is a bump map only");
    }
    if (!use_bump_) {
      return base_normal_;
    }
    const RdlValue height = multiply(
        subtract(input_float(node, "height"), input_float(node, "midlevel")),
        input_float(node, "scale"));
    if (height.is_constant()) {
      return base_normal_;
    }
    const int object = new_object("AnariBumpNormalMap");
    set(object, "input_height", bind_float(as_scalar(height)));
    /* A displacement along the normal in object space has the size of the object. */
    set(object,
        "object_space",
        static_cast<DisplacementNode *>(node)->get_space() == NODE_NORMAL_MAP_OBJECT);
    base_normal_ = objects_[object].expr;
    return base_normal_;
  }

  /* The base normal with a normal map on top of it. The expression is taken by value: it is
   * often the one of an object, which a new object moves. */
  string on_base_normal(const string normal_map)
  {
    const string base = base_normal();
    if (base.empty()) {
      return normal_map;
    }
    const int object = new_object("CombineNormalMap");
    set(object, "input_1", base);
    set(object, "input_2", normal_map);
    return objects_[object].expr;
  }

  string bump_normal(BumpNode *node)
  {
    string inner = normal_map(find_input(node, "normal"), nullptr);
    /* Simplifying the graph moves the link of the height to the first of three samples, see
     * #ShaderGraph::refine_bump_nodes. */
    const RdlValue height = input_float(node,
                                        is_linked(node, "sample_center") ? "sample_center" :
                                                                           "height");
    if (height.is_constant() || !use_bump_) {
      return inner;
    }
    const int object = new_object("AnariBumpNormalMap");
    set(object, "input_height", bind_float(height));
    const RdlValue strength = input_float(node, "strength");
    if (!is_constant(strength, 1.0f)) {
      set(object, "strength", bind_float(strength));
    }
    const RdlValue distance = input_float(node, "distance");
    if (!is_constant(distance, 1.0f)) {
      set(object, "distance", bind_float(distance));
    }
    if (node->get_invert()) {
      set(object, "invert", true);
    }
    set(object, "object_space", node->get_use_object_space());
    const float filter_width = input_constant(node, "filter_width", 0.1f);
    if (filter_width != 0.1f) {
      set(object, "filter_width", number(filter_width));
    }
    if (!inner.empty()) {
      set(object, "input_normal", inner);
    }
    return objects_[object].expr;
  }

  /* A vector in render space as a normal map. */
  string vector_normal(const RdlValue &render_vector)
  {
    const int object = new_object("RgbToNormalMap");
    set(object, "input", bind_rgb(render_vector));
    return objects_[object].expr;
  }

  string normal_map_node(NormalMapNode *node, RdlValue *dial)
  {
    const RdlValue strength = input_float(node, "strength");
    if (dial) {
      if (strength.is_constant() && strength.scale.x > 1.0f) {
        note(node, "a strength above 1 is 1");
      }
      *dial = strength.is_constant() ? clamp_value(strength, 0.0f, 1.0f) : strength;
    }
    else if (!is_constant(strength, 1.0f)) {
      note(node, "under a bump map the strength of the normal map is 1");
    }

    const NodeNormalMapSpace space = node->get_space();
    const bool directx = node->get_convention() == NODE_NORMAL_MAP_CONVENTION_DIRECTX;
    ShaderInput *color = find_input(node, "color");
    if (color == nullptr || color->link == nullptr) {
      return base_normal();
    }

    if (space == NODE_NORMAL_MAP_TANGENT) {
      ShaderNode *source = color->link->parent;
      string parameter;
      if (!directx && source->type == ImageTextureNode::get_node_type() &&
          color->link->socket_type.name == "color" &&
          static_cast<ImageTextureNode *>(source)->get_projection() == NODE_IMAGE_PROJ_FLAT &&
          image_parameter(static_cast<ImageTextureNode *>(source), parameter))
      {
        /* The texture itself: looked up with the footprint of the pixel. */
        ImageTextureNode *image = static_cast<ImageTextureNode *>(source);
        const int object = new_object("ImageNormalMap");
        set(object, "tangent_space_normal_texture", quoted(parameter));
        set(object, "wrap_around", image->get_extension() != EXTENSION_EXTEND &&
                                       image->get_extension() != EXTENSION_CLIP);
        float2 scale = make_float2(1.0f, 1.0f);
        float2 offset = zero_float2();
        if (default_uv_mapping(image, scale, offset)) {
          if (scale != make_float2(1.0f, 1.0f)) {
            set(object,
                "scale",
                string_printf("Vec2(%.9g, %.9g)", double(scale.x), double(scale.y)));
          }
          if (offset != zero_float2()) {
            set(object,
                "offset",
                string_printf("Vec2(%.9g, %.9g)", double(offset.x), double(offset.y)));
          }
        }
        else {
          RdlValue uv = texture_vector(image, &image->tex_mapping);
          if (!uv.planar) {
            uv = scaled(uv, make_float3(1.0f, 1.0f, 0.0f));
          }
          set(object, "texture_coordinates", 1);
          set(object, "input_texture_coordinates", bind_vec3(uv));
        }
        return on_base_normal(objects_[object].expr);
      }

      /* A color which something computes: decoded and taken from the tangent space of the
       * UV map to render space. */
      RdlValue encoded = to_world(output_value(color->link));
      if (directx) {
        encoded = affine(encoded, make_float3(1.0f, -1.0f, 1.0f), make_float3(0.0f, 1.0f, 0.0f));
      }
      const int object = new_object("TransformNormalMap");
      set(object, "input_normal", bind_vec3(encoded));
      set(object, "transform", 0);
      set(object, "decode_input_normal", true);
      return on_base_normal(vector_normal(map_value(object)));
    }

    /* Object and world space: -1..1 from the color, then to render space. */
    float3 factor = make_float3(2.0f);
    float3 offset = make_float3(-1.0f);
    if (space == NODE_NORMAL_MAP_BLENDER_OBJECT || space == NODE_NORMAL_MAP_BLENDER_WORLD) {
      factor = make_float3(2.0f, -2.0f, -2.0f);
      offset = make_float3(-1.0f, 1.0f, 1.0f);
    }
    RdlValue normal = affine(to_world(output_value(color->link)), factor, offset);
    if (space == NODE_NORMAL_MAP_OBJECT || space == NODE_NORMAL_MAP_BLENDER_OBJECT) {
      normal = transform_space(
          transform_space(normal, TRANSFORM_NORMAL, SPACE_INSTANCE, SPACE_WORLD),
          TRANSFORM_NORMAL,
          SPACE_OBJECT,
          SPACE_RENDER);
    }
    else {
      normal = transform_space(normal, TRANSFORM_NORMAL, SPACE_WORLD, SPACE_RENDER);
    }
    return vector_normal(op(OP_NORMALIZE, normal));
  }

  /* The normal input of a BSDF as a normal map of MoonRay: the expression of the object, empty
   * for the normal of the geometry. `dial` receives how much of it is used, where the node has
   * such a strength and the caller can take it. */
  string normal_map(ShaderInput *input, RdlValue *dial)
  {
    if (input == nullptr || input->link == nullptr) {
      if (dial) {
        *dial = constant(1.0f);
      }
      return base_normal();
    }
    return normal_of(input->link, dial);
  }

  /* The same for the output the normal comes from. */
  string normal_of(ShaderOutput *link, RdlValue *dial)
  {
    if (dial) {
      *dial = constant(1.0f);
    }
    ShaderNode *node = link->parent;
    const string key = string_printf(
        "%p %d %d", (void *)link, int(dial != nullptr), ray_of(node));
    auto it = normals_.find(key);
    if (it != normals_.end()) {
      if (dial) {
        *dial = it->second.second;
      }
      return it->second.first;
    }

    string result;
    RdlValue strength = constant(1.0f);
    if (node->type == BumpNode::get_node_type()) {
      result = bump_normal(static_cast<BumpNode *>(node));
    }
    else if (node->type == NormalMapNode::get_node_type()) {
      result = normal_map_node(static_cast<NormalMapNode *>(node), dial ? &strength : nullptr);
    }
    else if (node->type == GeometryNode::get_node_type() && link->socket_type.name == "normal") {
      result = base_normal();
    }
    else {
      /* A vector which something computes. */
      RdlValue vector = output_value(link);
      if (vector.is_constant()) {
        result = base_normal();
      }
      else {
        if (!vector.render_space) {
          vector = transform_space(vector, TRANSFORM_NORMAL, SPACE_WORLD, SPACE_RENDER);
        }
        vector.render_space = false;
        result = vector_normal(op(OP_NORMALIZE, vector));
      }
    }
    normals_[key] = std::make_pair(result, strength);
    if (dial) {
      *dial = strength;
    }
    return result;
  }

  /* Closures. */

  static Lobes empty_lobes()
  {
    Lobes lobes;
    lobes.w_surface = lobes.w_conductor = lobes.w_rest = lobes.w_refraction =
        lobes.w_diffuse_part = lobes.w_diffuse = lobes.w_translucent = lobes.w_rough =
            lobes.w_fuzz = lobes.w_coat = lobes.w_subsurface = weight(0.0f);
    lobes.w_body = lobes.w_hair = lobes.w_hair_r = lobes.w_hair_tt = weight(0.0f);
    lobes.hair = constant(0.0f);
    lobes.hair_color = constant(one_float3());
    lobes.hair_front = lobes.hair_back = lobes.hair_second = constant(zero_float3());
    lobes.hair_roughness = lobes.hair_back_roughness = constant(0.5f);
    lobes.hair_azimuthal_roughness = constant(1.0f);
    lobes.conductor = lobes.refraction = lobes.translucent = lobes.dielectric = constant(0.0f);
    lobes.albedo = constant(zero_float3());
    lobes.translucent_color = lobes.refraction_color = constant(one_float3());
    lobes.conductor_color = lobes.conductor_edge_color = constant(one_float3());
    lobes.roughness = constant(0.5f);
    lobes.anisotropy = constant(0.0f);
    lobes.emission = constant(zero_float3());
    lobes.fuzz = lobes.coat = constant(0.0f);
    lobes.fuzz_color = constant(one_float3());
    lobes.fuzz_roughness = constant(0.5f);
    lobes.coat_roughness = constant(0.03f);
    lobes.subsurface_radius = constant(0.0f);
    lobes.subsurface_color = constant(one_float3());
    lobes.normal_dial = constant(1.0f);
    return lobes;
  }

  static Lobes diffuse_lobes(const RdlValue &color)
  {
    Lobes lobes = empty_lobes();
    lobes.w_surface = lobes.w_body = lobes.w_rest = lobes.w_diffuse_part = lobes.w_diffuse =
        weight(1.0f);
    lobes.albedo = color;
    return lobes;
  }

  /* The glossy BSDF of Cycles has no Fresnel term: its color is the reflectance at every angle.
   * A conductor of MoonRay reflects fully at grazing angles whatever its color is, which is far
   * too bright for a dark one, and less than its color in between. So the BSDF is a white
   * conductor, which reflects everything at every angle, of the weight of the color. With
   * `fresnel` the color is the one of a conductor with a Fresnel term. */
  Lobes glossy_lobes(const RdlValue &color, const RdlValue &edge_color, const bool fresnel)
  {
    Lobes lobes = empty_lobes();
    lobes.w_surface = lobes.w_body = lobes.w_rough = weight(1.0f);
    lobes.leaf_glossy = !fresnel && color.is_constant();
    if (fresnel || (color.is_constant() && reduce_min(color.scale) >= 1.0f)) {
      lobes.w_conductor = weight(1.0f);
      lobes.conductor = constant(1.0f);
      lobes.conductor_color = color;
      lobes.conductor_edge_color = edge_color;
      return lobes;
    }
    RdlValue amount;
    if (color.is_constant()) {
      const float brightest = max(reduce_max(color.scale), 0.0f);
      amount = constant(brightest);
      lobes.conductor_color = constant(brightest > 0.0f ? color.scale / brightest :
                                                            one_float3());
    }
    else {
      /* The brightest channel is the weight, the color divided by it the hue. */
      amount = to_float(color, CHANNEL_MAX);
      if (!color.scalar) {
        lobes.conductor_color = op(OP_DIVIDE, color, amount);
        lobes.conductor_color.scalar = false;
      }
    }
    lobes.conductor_edge_color = lobes.conductor_color;
    lobes.conductor = amount;
    lobes.w_conductor = weight(amount);
    /* The rest is black. */
    lobes.w_rest = lobes.w_diffuse_part = lobes.w_diffuse = mix_weight(
        weight(1.0f), weight(0.0f), amount);
    return lobes;
  }

  Lobes mix_lobes(const Lobes &a, const Lobes &b, const RdlValue &factor_in)
  {
    const RdlValue factor = as_scalar(factor_in.scalar ? factor_in :
                                                         to_float(factor_in, CHANNEL_LUMINANCE));
    if (factor.is_constant() && factor.scale.x <= 0.0f) {
      return a;
    }
    if (factor.is_constant() && factor.scale.x >= 1.0f) {
      return b;
    }

    Lobes lobes = empty_lobes();
    lobes.w_surface = mix_weight(a.w_surface, b.w_surface, factor);
    lobes.w_conductor = mix_weight(a.w_conductor, b.w_conductor, factor);
    lobes.w_rest = mix_weight(a.w_rest, b.w_rest, factor);
    lobes.w_refraction = mix_weight(a.w_refraction, b.w_refraction, factor);
    lobes.w_diffuse_part = mix_weight(a.w_diffuse_part, b.w_diffuse_part, factor);
    lobes.w_diffuse = mix_weight(a.w_diffuse, b.w_diffuse, factor);
    lobes.w_translucent = mix_weight(a.w_translucent, b.w_translucent, factor);
    lobes.w_rough = mix_weight(a.w_rough, b.w_rough, factor);
    lobes.w_fuzz = mix_weight(a.w_fuzz, b.w_fuzz, factor);
    lobes.w_coat = mix_weight(a.w_coat, b.w_coat, factor);
    lobes.w_subsurface = mix_weight(a.w_subsurface, b.w_subsurface, factor);
    lobes.w_body = mix_weight(a.w_body, b.w_body, factor);
    lobes.w_hair = mix_weight(a.w_hair, b.w_hair, factor);
    lobes.w_hair_r = mix_weight(a.w_hair_r, b.w_hair_r, factor);
    lobes.w_hair_tt = mix_weight(a.w_hair_tt, b.w_hair_tt, factor);

#  define MIX_ATTRIBUTE(name, weight_name) \
    lobes.name = mix_attribute(a.name, b.name, a.weight_name, b.weight_name, factor)
    MIX_ATTRIBUTE(hair, w_surface);
    MIX_ATTRIBUTE(emission, w_surface);
    MIX_ATTRIBUTE(conductor, w_body);
    MIX_ATTRIBUTE(fuzz, w_body);
    MIX_ATTRIBUTE(coat, w_body);
    MIX_ATTRIBUTE(hair_color, w_hair);
    MIX_ATTRIBUTE(hair_front, w_hair);
    MIX_ATTRIBUTE(hair_back, w_hair);
    MIX_ATTRIBUTE(hair_second, w_hair);
    MIX_ATTRIBUTE(hair_roughness, w_hair_r);
    MIX_ATTRIBUTE(hair_back_roughness, w_hair_tt);
    MIX_ATTRIBUTE(hair_azimuthal_roughness, w_hair_tt);
    MIX_ATTRIBUTE(dielectric, w_rest);
    MIX_ATTRIBUTE(refraction, w_rest);
    MIX_ATTRIBUTE(translucent, w_diffuse_part);
    MIX_ATTRIBUTE(albedo, w_diffuse);
    MIX_ATTRIBUTE(subsurface_radius, w_diffuse);
    MIX_ATTRIBUTE(translucent_color, w_translucent);
    MIX_ATTRIBUTE(refraction_color, w_refraction);
    MIX_ATTRIBUTE(conductor_color, w_conductor);
    MIX_ATTRIBUTE(conductor_edge_color, w_conductor);
    MIX_ATTRIBUTE(roughness, w_rough);
    MIX_ATTRIBUTE(anisotropy, w_rough);
    MIX_ATTRIBUTE(fuzz_color, w_fuzz);
    MIX_ATTRIBUTE(fuzz_roughness, w_fuzz);
    MIX_ATTRIBUTE(coat_roughness, w_coat);
    MIX_ATTRIBUTE(subsurface_color, w_subsurface);
#  undef MIX_ATTRIBUTE

    /* What is one setting of the material. */
    const bool more_of_b = factor.is_constant() && factor.scale.x > 0.5f;
    if (a.has_ior && b.has_ior && a.ior != b.ior) {
      note("a mix of two indices of refraction has their mean");
      lobes.ior = 0.5f * (a.ior + b.ior);
    }
    else {
      lobes.ior = a.has_ior ? a.ior : b.ior;
    }
    lobes.has_ior = a.has_ior || b.has_ior;
    if (weight_is(a.w_hair, 0.0f) || weight_is(b.w_hair, 0.0f)) {
      /* The colors of the side which is hair. */
      const Lobes &fibers = weight_is(a.w_hair, 0.0f) ? b : a;
      lobes.hair_front_most = fibers.hair_front_most;
      lobes.hair_back_most = fibers.hair_back_most;
    }
    else if (factor.is_constant()) {
      lobes.hair_front_most = mix(a.hair_front_most, b.hair_front_most, factor.scale.x);
      lobes.hair_back_most = mix(a.hair_back_most, b.hair_back_most, factor.scale.x);
    }
    else {
      lobes.hair_front_most = max(a.hair_front_most, b.hair_front_most);
      lobes.hair_back_most = max(a.hair_back_most, b.hair_back_most);
    }
    const Lobes &scattering = weight_is(a.w_subsurface, 0.0f) ? b : a;
    lobes.subsurface_method = scattering.subsurface_method;
    lobes.subsurface_ior = scattering.subsurface_ior;
    lobes.subsurface_anisotropy = scattering.subsurface_anisotropy;
    lobes.coat_ior = weight_is(a.w_coat, 0.0f) ? b.coat_ior : a.coat_ior;
    if (a.has_distribution && b.has_distribution) {
      lobes.beckmann = more_of_b ? b.beckmann : a.beckmann;
    }
    else {
      lobes.beckmann = a.has_distribution ? a.beckmann : b.beckmann;
    }
    lobes.has_distribution = a.has_distribution || b.has_distribution;

    const bool a_shaded = !weight_is(a.w_surface, 0.0f);
    const bool b_shaded = !weight_is(b.w_surface, 0.0f);
    if (a_shaded && b_shaded && a.normal != b.normal && !a.normal.empty() && !b.normal.empty()) {
      note("a mix of two shaders with different normal or bump maps has the one of one side");
    }
    const bool use_b = !a_shaded || (b_shaded && (a.normal.empty() || more_of_b) &&
                                     !b.normal.empty());
    lobes.normal = use_b ? b.normal : a.normal;
    lobes.normal_dial = use_b ? b.normal_dial : a.normal_dial;

    const bool hair_of_b = weight_is(a.w_hair, 0.0f);
    lobes.hair_principled = a.hair_principled || b.hair_principled;
    lobes.hair_offset = hair_of_b ? b.hair_offset : a.hair_offset;
    lobes.hair_ior = hair_of_b ? b.hair_ior : a.hair_ior;
    lobes.hair_second_roughness = hair_of_b ? b.hair_second_roughness : a.hair_second_roughness;

    lobes.only_emission = a.only_emission && b.only_emission;
    lobes.only_transparent = a.only_transparent && b.only_transparent;
    return lobes;
  }

  static bool distribution_is_beckmann(const ClosureType distribution)
  {
    return distribution == CLOSURE_BSDF_MICROFACET_BECKMANN_ID ||
           distribution == CLOSURE_BSDF_MICROFACET_BECKMANN_GLASS_ID ||
           distribution == CLOSURE_BSDF_MICROFACET_BECKMANN_REFRACTION_ID;
  }

  void set_normal(Lobes &lobes, ShaderNode *node)
  {
    lobes.normal = normal_map(find_input(node, "normal"), &lobes.normal_dial);
  }

  /* Roughness and anisotropy of a microfacet BSDF. MoonRay stretches one roughness, Cycles
   * derives two from the anisotropy: `stretch` is how it does that. */
  void set_roughness(Lobes &lobes, ShaderNode *node, const char *anisotropy_name,
                     const bool principled)
  {
    lobes.roughness = input_float(node, "roughness");
    lobes.has_distribution = true;
    if (!find_input(node, anisotropy_name)) {
      return;
    }
    const RdlValue anisotropy = input_float(node, anisotropy_name);
    if (is_constant(anisotropy, 0.0f)) {
      return;
    }
    if (!anisotropy.is_constant() || !lobes.roughness.is_constant()) {
      note(node, "anisotropy is exported for a constant anisotropy and roughness only");
      return;
    }
    const float a = clamp(anisotropy.scale.x, -0.99f, 0.99f);
    if (principled) {
      const float aspect = sqrtf(1.0f - 0.9f * fabsf(a));
      lobes.anisotropy = constant(-(1.0f - aspect));
      lobes.roughness = constant(min(lobes.roughness.scale.x / sqrtf(aspect), 1.0f));
    }
    else {
      lobes.anisotropy = constant(a);
      lobes.roughness = constant(min(lobes.roughness.scale.x / sqrtf(1.0f - fabsf(a)), 1.0f));
    }
    /* The GGX of MoonRay is isotropic. */
    lobes.beckmann = true;
  }

  Lobes principled_lobes(ShaderNode *node)
  {
    const RdlValue base_color = input_value(node, "base_color");
    const RdlValue metallic = input_float(node, "metallic");
    const RdlValue transmission = input_float(node, "transmission_weight");
    const RdlValue alpha = input_float(node, "alpha");
    const RdlValue specular = input_float(node, "specular_ior_level");
    const RdlValue specular_tint = input_value(node, "specular_tint");
    const float ior = input_constant(node, "ior", 1.5f);
    const bool beckmann = distribution_is_beckmann(
        static_cast<PrincipledBsdfNode *>(node)->get_distribution());

    /* The diffuse part under the dielectric coating. A specular level of a half is the Fresnel
     * reflectance of the index of refraction. */
    Lobes base = diffuse_lobes(base_color);
    base.dielectric = specular.is_constant() ?
                          constant(clamp(specular.scale.x * 2.0f, 0.0f, 1.0f)) :
                          clamp_value(scaled(specular, 2.0f), 0.0f, 1.0f);
    base.w_rough = weight(1.0f);
    base.ior = ior;
    base.has_ior = true;

    const RdlValue subsurface = input_float(node, "subsurface_weight");
    if (!is_constant(subsurface, 0.0f)) {
      /* MoonRay scatters over the distance radius * color. */
      const float3 radius = input_constant3(node, "subsurface_radius", make_float3(1.0f, 0.2f, 0.1f));
      const float longest = max(reduce_max(radius), 1e-6f);
      base.subsurface_radius = multiply(
          multiply(subsurface, input_float(node, "subsurface_scale")), constant(longest));
      base.subsurface_color = constant(radius / longest);
      base.w_subsurface = weight(1.0f);
      PrincipledBsdfNode *principled = static_cast<PrincipledBsdfNode *>(node);
      base.subsurface_method = principled->get_subsurface_method();
      base.subsurface_ior = (base.subsurface_method == CLOSURE_BSSRDF_RANDOM_WALK_SKIN_ID) ?
                                input_constant(node, "subsurface_ior", 1.4f) :
                                ior;
      base.subsurface_anisotropy = input_constant(node, "subsurface_anisotropy", 0.0f);
      if (!subsurface.is_constant() || subsurface.scale.x < 1.0f) {
        note(node,
             "a subsurface weight below one is a shorter scattering radius, not a mix with a "
             "diffuse BSDF");
      }
    }

    Lobes result = base;
    if (!is_constant(transmission, 0.0f)) {
      /* Cycles tints the light at each of the two interfaces with the square root. */
      Lobes glass = empty_lobes();
      glass.w_surface = glass.w_body = glass.w_rest = glass.w_refraction = glass.w_rough =
          weight(1.0f);
      glass.refraction = constant(1.0f);
      glass.dielectric = base.dielectric;
      glass.refraction_color = op(OP_POWER, base_color, constant(0.5f));
      glass.refraction_color.scalar = false;
      glass.ior = ior;
      glass.has_ior = true;
      result = mix_lobes(base, glass, transmission);
    }
    if (!is_constant(metallic, 0.0f)) {
      Lobes metal = empty_lobes();
      metal.w_surface = metal.w_body = metal.w_conductor = metal.w_rough = weight(1.0f);
      metal.conductor = constant(1.0f);
      metal.conductor_color = base_color;
      metal.conductor_edge_color = multiply(base_color, specular_tint);
      metal.conductor_edge_color.scalar = false;
      result = mix_lobes(result, metal, metallic);
    }

    set_roughness(result, node, "anisotropic", true);
    result.beckmann = result.beckmann || beckmann;

    const RdlValue sheen = input_float(node, "sheen_weight");
    if (!is_constant(sheen, 0.0f)) {
      /* The fuzz of MoonRay is about ten times as bright as the sheen of Cycles. */
      result.fuzz = scaled(sheen, 0.1f);
      result.fuzz_color = input_value(node, "sheen_tint");
      result.fuzz_roughness = input_float(node, "sheen_roughness");
      result.w_fuzz = weight(1.0f);
    }
    const RdlValue coat = input_float(node, "coat_weight");
    if (!is_constant(coat, 0.0f)) {
      result.coat = coat;
      result.coat_roughness = input_float(node, "coat_roughness");
      result.coat_ior = input_constant(node, "coat_ior", 1.5f);
      result.w_coat = weight(1.0f);
    }
    const RdlValue emission = multiply(input_value(node, "emission_color"),
                                       input_float(node, "emission_strength"));
    if (!is_constant(emission, 0.0f)) {
      result.emission = emission;
      result.emission.scalar = false;
    }
    set_normal(result, node);

    if (!is_constant(alpha, 1.0f)) {
      result = mix_lobes(empty_lobes(), result, alpha);
    }
    return result;
  }

  Lobes closure(ShaderOutput *output)
  {
    const std::pair<const ShaderOutput *, int> key(output, ray_of(output->parent));
    auto it = closures_.find(key);
    if (it != closures_.end()) {
      return it->second;
    }
    const Lobes lobes = evaluate_closure(output);
    closures_[key] = lobes;
    return lobes;
  }

  /* Nothing connected: black. */
  Lobes input_closure(ShaderNode *node, const char *name)
  {
    ShaderInput *input = find_input(node, name);
    if (input == nullptr || input->link == nullptr) {
      return diffuse_lobes(constant(zero_float3()));
    }
    return closure(input->link);
  }

  Lobes evaluate_closure(ShaderOutput *output)
  {
    ShaderNode *node = output->parent;
    const NodeType *type = node->type;

    if (type == PrincipledBsdfNode::get_node_type()) {
      return principled_lobes(node);
    }
    if (type == DiffuseBsdfNode::get_node_type()) {
      /* The rough diffuse BSDF of MoonRay is a fifth darker than the one of Cycles, the plain
       * one is closer to it. */
      Lobes lobes = diffuse_lobes(input_value(node, "color"));
      set_normal(lobes, node);
      return lobes;
    }
    if (type == GlossyBsdfNode::get_node_type()) {
      const RdlValue color = input_value(node, "color");
      Lobes lobes = glossy_lobes(color, color, false);
      set_roughness(lobes, node, "anisotropy", false);
      const ClosureType distribution = static_cast<GlossyBsdfNode *>(node)->get_distribution();
      lobes.beckmann = lobes.beckmann || distribution_is_beckmann(distribution);
      set_normal(lobes, node);
      return lobes;
    }
    if (type == MetallicBsdfNode::get_node_type()) {
      Lobes lobes = glossy_lobes(input_value(node, "color"), input_value(node, "edge_tint"),
                                 true);
      set_roughness(lobes, node, "anisotropy", false);
      lobes.beckmann = lobes.beckmann ||
                       distribution_is_beckmann(
                           static_cast<MetallicBsdfNode *>(node)->get_distribution());
      set_normal(lobes, node);
      return lobes;
    }
    if (type == GlassBsdfNode::get_node_type() || type == RefractionBsdfNode::get_node_type()) {
      Lobes lobes = empty_lobes();
      lobes.w_surface = lobes.w_body = lobes.w_rest = lobes.w_refraction = weight(1.0f);
      lobes.refraction = constant(1.0f);
      lobes.refraction_color = input_value(node, "color");
      /* The color of the glass of Cycles is on its reflection too, which MoonRay can only
       * weight. */
      lobes.dielectric = (type == RefractionBsdfNode::get_node_type()) ?
                             constant(0.0f) :
                             clamp_factor(to_float(lobes.refraction_color, CHANNEL_LUMINANCE));
      lobes.w_rough = weight(1.0f);
      lobes.roughness = input_float(node, "roughness");
      lobes.ior = input_constant(node, "IOR", 1.5f);
      lobes.has_ior = true;
      lobes.has_distribution = true;
      lobes.beckmann = distribution_is_beckmann(
          (type == GlassBsdfNode::get_node_type()) ?
              static_cast<GlassBsdfNode *>(node)->get_distribution() :
              static_cast<RefractionBsdfNode *>(node)->get_distribution());
      set_normal(lobes, node);
      return lobes;
    }
    if (type == TransparentBsdfNode::get_node_type() ||
        type == RayPortalBsdfNode::get_node_type() || type == HoldoutNode::get_node_type())
    {
      /* Mixed with other materials by their presence. */
      Lobes lobes = empty_lobes();
      RdlValue color = constant(1.0f);
      if (type == TransparentBsdfNode::get_node_type()) {
        color = input_value(node, "color");
        if (!color.scalar && !(color.is_constant() && is_uniform(color.scale))) {
          note(node, "colored transparency has no counterpart, it has the mean of the color");
        }
        color = to_float(color, CHANNEL_AVERAGE);
      }
      else {
        note(node, "no counterpart in MoonRay, it is transparent");
      }
      lobes.w_surface = lobes.w_body = mix_weight(weight(1.0f), weight(0.0f), color);
      /* What is left of the surface is black. */
      lobes.w_rest = lobes.w_diffuse_part = lobes.w_diffuse = lobes.w_surface;
      lobes.only_transparent = true;
      return lobes;
    }
    if (type == TranslucentBsdfNode::get_node_type()) {
      Lobes lobes = empty_lobes();
      lobes.w_surface = lobes.w_body = lobes.w_rest = lobes.w_diffuse_part = lobes.w_translucent =
          weight(1.0f);
      lobes.translucent = constant(1.0f);
      lobes.translucent_color = input_value(node, "color");
      set_normal(lobes, node);
      return lobes;
    }
    if (type == EmissionNode::get_node_type()) {
      Lobes lobes = diffuse_lobes(constant(zero_float3()));
      lobes.emission = multiply(input_value(node, "color"), input_float(node, "strength"));
      lobes.emission.scalar = false;
      lobes.only_emission = true;
      return lobes;
    }
    if (type == SubsurfaceScatteringNode::get_node_type()) {
      Lobes lobes = diffuse_lobes(input_value(node, "color"));
      const float3 radius = input_constant3(node, "radius", make_float3(1.0f, 0.2f, 0.1f));
      const float longest = max(reduce_max(radius), 1e-6f);
      lobes.subsurface_radius = multiply(input_float(node, "scale"), constant(longest));
      lobes.subsurface_color = constant(radius / longest);
      lobes.w_subsurface = weight(1.0f);
      lobes.subsurface_method = static_cast<SubsurfaceScatteringNode *>(node)->get_method();
      lobes.subsurface_ior = input_constant(node, "subsurface_ior", 1.4f);
      lobes.subsurface_anisotropy = input_constant(node, "subsurface_anisotropy", 0.0f);
      set_normal(lobes, node);
      return lobes;
    }
    if (type == SheenBsdfNode::get_node_type()) {
      Lobes lobes = diffuse_lobes(constant(zero_float3()));
      lobes.fuzz = constant(0.1f);
      lobes.fuzz_color = input_value(node, "color");
      lobes.fuzz_roughness = input_float(node, "roughness");
      lobes.w_fuzz = weight(1.0f);
      set_normal(lobes, node);
      return lobes;
    }
    if (type == HairBsdfNode::get_node_type()) {
      return hair_lobes(static_cast<HairBsdfNode *>(node));
    }
    if (type == PrincipledHairBsdfNode::get_node_type()) {
      return principled_hair_lobes(static_cast<PrincipledHairBsdfNode *>(node));
    }
    if (type == ToonBsdfNode::get_node_type()) {
      note(node, "no counterpart is exported, it is a diffuse BSDF of its color");
      Lobes lobes = diffuse_lobes(input_value(node, "color"));
      if (find_input(node, "normal")) {
        set_normal(lobes, node);
      }
      return lobes;
    }
    if (type == MixClosureNode::get_node_type()) {
      return mix_lobes(input_closure(node, "closure1"),
                       input_closure(node, "closure2"),
                       input_float(node, "fac"));
    }
    if (type == AddClosureNode::get_node_type()) {
      return add_lobes(node);
    }

    note(node, "a closure without a counterpart, it is black");
    return diffuse_lobes(constant(zero_float3()));
  }

  /* One material can't reflect more than all the light. Emission is simply added; two BSDFs
   * each cover half of the surface with twice their color, which is their sum as long as the
   * colors can be doubled. */
  Lobes add_lobes(ShaderNode *node)
  {
    Lobes a = input_closure(node, "closure1");
    Lobes b = input_closure(node, "closure2");
    if (a.only_emission && !b.only_emission) {
      std::swap(a, b);
    }
    if (b.only_emission) {
      a.emission = add(a.emission, b.emission);
      a.emission.scalar = false;
      return a;
    }
    if (a.only_transparent && !b.only_transparent) {
      std::swap(a, b);
    }
    if (b.only_transparent && weight_is(b.w_surface, 0.0f) && a.leaf_glossy &&
        a.conductor.is_constant())
    {
      /* All the light passes and a part of it is reflected on top of that: of 1 + g parts, g
       * are a full reflection. */
      const float g = a.conductor.scale.x;
      a.w_conductor = weight(1.0f);
      a.w_rest = a.w_diffuse_part = a.w_diffuse = weight(0.0f);
      a.conductor = constant(1.0f);
      return mix_lobes(b, a, constant(g / (1.0f + g)));
    }
    const float3 twice = make_float3(2.0f);
    for (Lobes *lobes : {&a, &b}) {
      lobes->albedo = scaled(lobes->albedo, twice);
      lobes->translucent_color = scaled(lobes->translucent_color, twice);
      lobes->refraction_color = scaled(lobes->refraction_color, twice);
      lobes->emission = scaled(lobes->emission, twice);
      lobes->hair_front = scaled(lobes->hair_front, twice);
      lobes->hair_back = scaled(lobes->hair_back, twice);
      lobes->hair_second = scaled(lobes->hair_second, twice);
      lobes->hair_front_most *= 2.0f;
      lobes->hair_back_most *= 2.0f;
      if (lobes->leaf_glossy && lobes->conductor.is_constant() &&
          lobes->conductor.scale.x <= 0.5f)
      {
        /* Twice the weight of the white conductor. */
        const float amount = 2.0f * lobes->conductor.scale.x;
        lobes->conductor = constant(amount);
        lobes->w_conductor = weight(amount);
        lobes->w_rest = lobes->w_diffuse_part = lobes->w_diffuse = weight(1.0f - amount);
      }
      else if (!weight_is(lobes->w_conductor, 0.0f)) {
        note(node, "the glossy part of an added BSDF is half as strong");
      }
    }
    return mix_lobes(a, b, constant(0.5f));
  }

  /* Subsurface scattering.
   *
   * Cycles and MoonRay both take a surface albedo and a radius for each channel, and mean
   * different lengths with the radius:
   *
   *  - The random walk of MoonRay scatters in a volume with the mean free path
   *    radius / 3 * s(A), s(A) = 1.9 - A + 3.5 (A - 0.8)^2 for the albedo A (Chiang et al.,
   *    Practical and Controllable Subsurface Scattering for Production Path Tracing).
   *  - The random walk of Cycles has the radius itself as the mean free path, its older random
   *    walk and its diffusion profile the radius / (4 pi), and its random walk for skin the
   *    radius times the sqrt(3 (1 - alpha')) of the dipole whose reflectance is the albedo.
   *    An anisotropic phase function is the isotropic one of a longer mean free path.
   *  - The normalized diffusion of MoonRay has the profile of Christensen and Burley with
   *    d = radius / (3.5 + 100 (A - 0.33)^4), the one of Cycles d = radius / (4 pi).
   *
   * The single scattering albedo follows from the surface albedo in both renderers by fits
   * which agree to a percent. */

  /* 1 - the single scattering albedo of an isotropic volume with the surface albedo A (van de
   * Hulst, as fitted by Chiang et al.). */
  static float subsurface_absorption(const float A)
  {
    return sqr(4.20863f * A - sqrtf(9.59217f + 41.6808f * A + 17.7126f * A * A) + 4.09712f);
  }

  /* sqrt(3 (1 - alpha')) of the dipole of Jensen et al. with the diffuse reflectance A under
   * a surface with an index of refraction, found by bisection in as many steps as Cycles
   * takes. */
  static float subsurface_dipole_factor(const float A, const float ior)
  {
    if (A < 1e-4f) {
      return sqrtf(3.0f);
    }
    if (A >= 0.995f) {
      return sqrtf(3.0f * 1e-6f);
    }
    const float inv_eta = 1.0f / ior;
    const float F_dr = inv_eta * (-1.440f * inv_eta + 0.710f) + 0.668f + 0.0636f * ior;
    const float boundary = (4.0f / 3.0f) * (1.0f + F_dr) / (1.0f - F_dr);
    float low = 0.0f, high = 1.0f, alpha = 0.5f;
    for (int i = 0; i < 12; i++) {
      alpha = 0.5f * (low + high);
      const float k = sqrtf(3.0f * (1.0f - alpha));
      const float reflectance = 0.5f * alpha * (1.0f + expf(-boundary * k)) * expf(-k);
      if (reflectance < A) {
        low = alpha;
      }
      else {
        high = alpha;
      }
    }
    return sqrtf(3.0f * (1.0f - alpha));
  }

  /* Scattering radius of MoonRay for a radius of one in Cycles and the albedo A. */
  static float subsurface_factor(const Lobes &lobes, const float albedo)
  {
    const float A = clamp(albedo, 0.0f, 1.0f);
    if (lobes.subsurface_method == CLOSURE_BSSRDF_BURLEY_ID) {
      return (3.5f + 100.0f * sqr(sqr(A - 0.33f))) / (4.0f * M_PI_F);
    }
    return 3.0f * subsurface_path(lobes, A) / (1.9f - A + 3.5f * sqr(A - 0.8f));
  }

  /* Mean free path of the isotropic random walk which does what the one of Cycles does with a
   * radius of one. */
  static float subsurface_path(const Lobes &lobes, const float A)
  {
    const float g = (lobes.subsurface_method == CLOSURE_BSSRDF_RANDOM_WALK_LEGACY_ID) ?
                        clamp(lobes.subsurface_anisotropy, -0.99f, 0.9f) :
                        clamp(lobes.subsurface_anisotropy, -0.99f, 0.99f);
    /* Similarity: the reduced scattering coefficient is the one of the isotropic volume. */
    float path = 1.0f - g * subsurface_absorption(A);
    switch (lobes.subsurface_method) {
      case CLOSURE_BSSRDF_RANDOM_WALK_LEGACY_ID:
        /* The free path was divided by 1 - g already, except for a negative g. */
        path *= 0.25f * M_1_PI_F / ((g < 0.0f) ? 1.0f - g : 1.0f);
        break;
      case CLOSURE_BSSRDF_RANDOM_WALK_SKIN_ID:
        path *= subsurface_dipole_factor(A, clamp(lobes.subsurface_ior, 1.01f, 3.8f)) / (1.0f - g);
        break;
      default:
        path /= 1.0f - g;
        break;
    }
    return path;
  }

  /* The same for the albedo as a value: a color, of which the highest possible channel is
   * returned as well. */
  RdlValue subsurface_factor(const Lobes &lobes, const RdlValue &albedo, float *highest)
  {
    if (albedo.is_constant()) {
      const float3 A = albedo.scale;
      const float3 factor = make_float3(subsurface_factor(lobes, A.x),
                                        subsurface_factor(lobes, A.y),
                                        subsurface_factor(lobes, A.z));
      *highest = reduce_max(factor);
      return constant(factor);
    }
    RdlValue factor;
    if (lobes.subsurface_method == CLOSURE_BSSRDF_BURLEY_ID) {
      const RdlValue t = affine(albedo, 1.0f, -0.33f);
      const RdlValue t2 = multiply(t, t);
      factor = affine(multiply(t2, t2), 100.0f / (4.0f * M_PI_F), 3.5f / (4.0f * M_PI_F));
      *highest = subsurface_factor(lobes, 1.0f);
    }
    else if (lobes.subsurface_method == CLOSURE_BSSRDF_RANDOM_WALK_SKIN_ID) {
      /* No closed form: the cubic which fits it best. */
      const int count = 25;
      double sums[7] = {0.0}, right[4] = {0.0};
      for (int i = 0; i < count; i++) {
        const float A = 0.02f + 0.96f * i / (count - 1);
        const double f = subsurface_factor(lobes, A);
        double power = 1.0;
        for (int k = 0; k < 7; k++) {
          sums[k] += power;
          if (k < 4) {
            right[k] += power * f;
          }
          power *= A;
        }
      }
      double matrix[4][5];
      for (int row = 0; row < 4; row++) {
        for (int column = 0; column < 4; column++) {
          matrix[row][column] = sums[row + column];
        }
        matrix[row][4] = right[row];
      }
      for (int pivot = 0; pivot < 4; pivot++) {
        for (int row = 0; row < 4; row++) {
          if (row != pivot) {
            const double f = matrix[row][pivot] / matrix[pivot][pivot];
            for (int column = pivot; column < 5; column++) {
              matrix[row][column] -= f * matrix[pivot][column];
            }
          }
        }
      }
      float c[4];
      for (int k = 0; k < 4; k++) {
        c[k] = float(matrix[k][4] / matrix[k][k]);
      }
      *highest = 0.0f;
      for (int i = 0; i <= 50; i++) {
        const float A = 0.02f * i;
        *highest = max(*highest, c[0] + A * (c[1] + A * (c[2] + A * c[3])));
      }
      factor = affine(multiply(albedo, affine(multiply(albedo, affine(albedo, c[3], c[2])), 1.0f, c[1])),
                      1.0f,
                      c[0]);
      note("the scattering radius of skin follows a textured color approximately");
    }
    else {
      /* The anisotropy for a medium albedo. */
      const float path = subsurface_path(lobes, 0.5f);
      const RdlValue s = affine(multiply(albedo, affine(albedo, 3.5f, -6.6f)), 1.0f, 4.14f);
      factor = divide(constant(make_float3(3.0f * path)), s);
      /* s is 1.0286 where it is least. */
      *highest = 3.0f * path / 1.0286f;
    }
    factor.scalar = false;
    return factor;
  }

  /* The lobes as a material: a DwaBaseMaterial, or a hair material. Returns the name of the
   * object.
   *
   * The Hair BSDF of Cycles is a HairDiffuseMaterial, whose two colors are what the fiber
   * sends back to the side of the light and what it lets through, and the Principled Hair BSDF
   * a HairMaterial. Neither can be layered with a DwaBaseMaterial: where a shader mixes hair
   * with other BSDFs, their diffuse color goes into the hair material and the rest of them is
   * left out. */
  string write_material(const Lobes &lobes)
  {
    const RdlValue presence = weight_value(lobes.w_surface);
    if (weight_is(lobes.w_hair, 0.0f)) {
      return write_body(lobes, presence);
    }
    const bool has_body = !weight_is(lobes.w_body, 0.0f);
    RdlValue body_front = constant(zero_float3());
    if (has_body) {
      if (!weight_is(lobes.w_conductor, 0.0f) || !weight_is(lobes.w_refraction, 0.0f) ||
          !weight_is(lobes.w_translucent, 0.0f))
      {
        note("of a shader which mixes hair with other BSDFs only their diffuse color is exported");
      }
      if (!weight_is(lobes.w_diffuse, 0.0f)) {
        /* A diffuse fiber, measured against the HairDiffuseMaterial. */
        body_front = scaled(lobes.albedo, 2.4f);
        body_front.scalar = false;
      }
    }
    const RdlValue hair = clamp_factor(lobes.hair);
    if (!lobes.hair_principled) {
      RdlValue front = lobes.hair_front;
      RdlValue back = lobes.hair_back;
      /* How bright the two colors can be. */
      float front_most = lobes.hair_front_most;
      float back_most = lobes.hair_back_most;
      /* A fiber of Cycles whose color times the factor of a lobe is above one sends on more
       * light than it gets. A fiber of MoonRay cannot: its brightest channel is kept within
       * one below, and what one side has above one goes to the other side as far as there is
       * room, until the fiber absorbs nothing. A head of such hair is also far more saturated
       * in Cycles than the color of its fibers, the more the further they are above one: the
       * color is saturated by that. */
      const RdlValue power = hair_saturation(front, back, max(front_most, back_most));
      if (back_most > 1.0f && front_most > 0.0f && front_most < 1.0f) {
        const float boost = min(front_most + back_most - 1.0f, 1.0f) / front_most;
        front = scaled(front, boost);
        front_most *= boost;
      }
      else if (front_most > 1.0f && back_most > 0.0f && back_most < 1.0f) {
        const float boost = min(back_most + front_most - 1.0f, 1.0f) / back_most;
        back = scaled(back, boost);
        back_most *= boost;
      }
      front = saturated(front, power);
      back = saturated(back, power);
      if (has_body) {
        const float body_most = reduce_max(body_front.scale);
        if (hair.is_constant()) {
          front_most = mix(body_most, front_most, hair.scale.x);
          back_most *= hair.scale.x;
        }
        else {
          front_most = max(body_most, front_most);
        }
        front = blend(body_front, front, hair);
        back = blend(constant(zero_float3()), back, hair);
      }
      return write_hair_diffuse(
          lobes, presence, within_one(front, front_most), within_one(back, back_most));
    }
    const string fiber = write_principled_hair(lobes, presence);
    if (!has_body) {
      return fiber;
    }
    const string diffuse = write_hair_diffuse(lobes,
                                              presence,
                                              within_one(body_front, reduce_max(body_front.scale)),
                                              constant(zero_float3()));
    const int object = new_object("HairLayerMaterial");
    /* The mask is the weight of the first one. */
    set(object, "hair_material_A", expr_of(fiber));
    set(object, "hair_material_B", expr_of(diffuse));
    set(object, "mask", bind_float(hair));
    return objects_[object].name;
  }

  string write_hair_diffuse(const Lobes &lobes,
                            const RdlValue &presence,
                            const RdlValue &front,
                            const RdlValue &back)
  {
    const int object = new_object("HairDiffuseMaterial");
    const string name = objects_[object].name;
    if (!is_constant(presence, 1.0f)) {
      set(object, "presence", bind_float(presence));
    }
    if (!is_constant(lobes.emission, 0.0f)) {
      set(object, "show_emission", true);
      set(object, "emission", bind_rgb(lobes.emission));
    }
    set(object, "use_independent_front_and_back_hair_color", true);
    set(object, "front_hair_color", bind_rgb(front));
    set(object, "back_hair_color", bind_rgb(back));
    return name;
  }

  /* How much the exponent of the saturation grows with what a fiber sends on, measured on
   * orange hair whose transmission is 1.2 and 1.6 times its color. */
  static constexpr float HAIR_SATURATION = 1.5f;

  /* Exponent for saturated(): one for a fiber which sends on no more than it gets. `most` is
   * how bright a channel of the two colors can be. */
  RdlValue hair_saturation(const RdlValue &front, const RdlValue &back, const float most)
  {
    if (most <= 1.0f) {
      return constant(1.0f);
    }
    if (front.is_constant() && back.is_constant()) {
      const float gain = max(reduce_max(front.scale), reduce_max(back.scale));
      return constant(clamp(1.0f + HAIR_SATURATION * (gain - 1.0f), 1.0f, 2.0f));
    }
    const RdlValue gain = to_float(op(OP_MAXIMUM, front, back), CHANNEL_MAX);
    return clamp_value(affine(gain, HAIR_SATURATION, 1.0f - HAIR_SATURATION), 1.0f, 2.0f);
  }

  /* The color with its channels other than the brightest one lowered: M (c / M)^power for
   * the brightest channel M. */
  RdlValue saturated(const RdlValue &color, const RdlValue &power)
  {
    if (is_constant(power, 1.0f)) {
      return color;
    }
    if (color.is_constant() && power.is_constant()) {
      const float3 c = max(color.scale, zero_float3());
      const float most = reduce_max(c);
      if (most <= 0.0f) {
        return color;
      }
      const float p = power.scale.x;
      return constant(most *
                      make_float3(powf(c.x / most, p), powf(c.y / most, p), powf(c.z / most, p)));
    }
    /* The OpMap divides by at least a small number: black stays black. */
    const RdlValue most = to_float(color, CHANNEL_MAX);
    RdlValue result = multiply(op(OP_POWER, op(OP_DIVIDE, color, most), power), most);
    result.scalar = false;
    return result;
  }

  /* A color whose brightest channel is at most one, with the hue kept: the hair materials of
   * MoonRay clamp their colors channel by channel, which turns a bright orange yellow. `most`
   * is how bright a channel of the color can be. */
  RdlValue within_one(const RdlValue &color, const float most)
  {
    if (most <= 1.0f) {
      return color;
    }
    note("a hair color is brighter than the hair material of MoonRay can be: its hue is kept, "
         "the hair is darker");
    if (color.is_constant()) {
      return constant(color.scale / max(reduce_max(color.scale), 1.0f));
    }
    RdlValue result = op(OP_DIVIDE,
                         color,
                         op(OP_MAXIMUM, to_float(color, CHANNEL_MAX), constant(1.0f)));
    result.scalar = false;
    return result;
  }

  /* The longitudinal roughness of the Principled Hair BSDF and of MoonRay's hair are the same
   * parameter (Chiang et al.). */
  string write_principled_hair(const Lobes &lobes, const RdlValue &presence)
  {
    const int object = new_object("HairMaterial_v3");
    const string name = objects_[object].name;
    if (!is_constant(presence, 1.0f)) {
      set(object, "presence", bind_float(presence));
    }
    if (!is_constant(lobes.emission, 0.0f)) {
      set(object, "show_emission", true);
      set(object, "emission", bind_rgb(lobes.emission));
    }
    set(object, "hair_color", bind_rgb(lobes.hair_color));
    set(object, "refractive_index", number(lobes.hair_ior));
    const float offset = lobes.hair_offset * 180.0f / M_PI_F;
    set(object, "primary_specular_roughness", bind_float(lobes.hair_roughness));
    set(object, "primary_specular_offset", number(-2.0f * offset));
    set(object, "use_independent_transmission_roughness", true);
    set(object, "independent_transmission_roughness", bind_float(lobes.hair_back_roughness));
    set(object, "transmission_azimuthal_roughness", bind_float(lobes.hair_azimuthal_roughness));
    set(object, "transmission_offset", number(-offset));
    set(object, "use_independent_secondary_specular_roughness", true);
    set(object, "independent_secondary_specular_roughness", number(lobes.hair_second_roughness));
    set(object, "secondary_specular_offset", number(-4.0f * offset));
    return name;
  }

  /* The Hair BSDF of Cycles does not keep the energy of its color: how much it sends to the
   * side of the light and to the other side depends on its two roughnesses. These are the
   * factors for the two colors of a HairDiffuseMaterial which give the same brightness, measured
   * on a sheet of fibers lit from the front and from behind. Rows: roughness U 0.1, 0.25, 0.5
   * and 1, columns: roughness V 0.3, 0.6 and 1, each with the factor for the front and for the
   * back color. */
  static float2 hair_factors(const bool transmission, const float u, const float v)
  {
    static const float reflection_table[4][3][2] = {
        {{1.818f, 0.000f}, {1.299f, 0.135f}, {0.901f, 0.344f}},
        {{2.582f, 0.000f}, {1.713f, 0.005f}, {1.159f, 0.318f}},
        {{3.198f, 0.000f}, {2.107f, 0.000f}, {1.448f, 0.212f}},
        {{3.592f, 0.000f}, {2.401f, 0.000f}, {1.627f, 0.047f}}};
    static const float transmission_table[4][3][2] = {
        {{0.000f, 2.797f}, {0.000f, 2.278f}, {0.000f, 1.915f}},
        {{0.000f, 3.513f}, {0.000f, 2.837f}, {0.000f, 2.354f}},
        {{0.000f, 3.120f}, {0.000f, 2.523f}, {0.000f, 2.095f}},
        {{0.000f, 2.660f}, {0.000f, 2.158f}, {0.000f, 1.797f}}};
    static const float us[4] = {0.1f, 0.25f, 0.5f, 1.0f};
    static const float vs[3] = {0.3f, 0.6f, 1.0f};
    const auto &table = transmission ? transmission_table : reflection_table;
    const float cu = clamp(u, us[0], us[3]);
    const float cv = clamp(v, vs[0], vs[2]);
    int i = 0;
    while (i < 2 && cu > us[i + 1]) {
      i++;
    }
    const int j = (cv > vs[1]) ? 1 : 0;
    const float tu = (cu - us[i]) / (us[i + 1] - us[i]);
    const float tv = (cv - vs[j]) / (vs[j + 1] - vs[j]);
    float2 result;
    for (int k = 0; k < 2; k++) {
      result[k] = mix(mix(table[i][j][k], table[i + 1][j][k], tu),
                      mix(table[i][j + 1][k], table[i + 1][j + 1][k], tu),
                      tv);
    }
    return result;
  }

  Lobes hair_lobes(HairBsdfNode *node)
  {
    Lobes lobes = empty_lobes();
    lobes.w_surface = lobes.w_hair = weight(1.0f);
    lobes.hair = constant(1.0f);
    const RdlValue color = input_value(node, "color");
    const bool transmission = node->get_component() == CLOSURE_BSDF_HAIR_TRANSMISSION_ID;
    const float2 factors = hair_factors(transmission,
                                        input_constant(node, "roughness_u", 0.5f),
                                        input_constant(node, "roughness_v", 1.0f));
    /* In a head of hair the fibers light each other, which makes it a little brighter than
     * the sheet the factors were measured on. */
    const float bulk = 0.88f;
    lobes.hair_front = scaled(color, factors.x * bulk);
    lobes.hair_back = scaled(color, factors.y * bulk);
    lobes.hair_front.scalar = lobes.hair_back.scalar = false;
    lobes.hair_front_most = reduce_max(lobes.hair_front.scale);
    lobes.hair_back_most = reduce_max(lobes.hair_back.scale);
    return lobes;
  }

  /* The fiber of Chiang et al., which MoonRay's hair is as well: the color is what the fiber
   * absorbs, by the same formula in both. */
  Lobes principled_hair_lobes(PrincipledHairBsdfNode *node)
  {
    Lobes lobes = empty_lobes();
    lobes.w_surface = lobes.w_hair = lobes.w_hair_r = lobes.w_hair_tt = weight(1.0f);
    lobes.hair = constant(1.0f);
    lobes.hair_principled = true;
    if (node->get_model() != NODE_PRINCIPLED_HAIR_CHIANG) {
      note(node, "the model of Huang has no counterpart, the one of Chiang is used");
    }

    const float roughness = clamp(input_constant(node, "roughness", 0.3f), 0.001f, 1.0f);
    const float radial = clamp(input_constant(node, "radial_roughness", 0.3f), 0.01f, 1.0f);
    const float coat = clamp(input_constant(node, "coat", 0.0f), 0.0f, 1.0f);
    lobes.hair_roughness = constant(roughness * (1.0f - coat));
    lobes.hair_back_roughness = constant(0.5f * roughness);
    lobes.hair_second_roughness = min(2.0f * roughness, 1.0f);
    lobes.hair_azimuthal_roughness = constant(radial);
    lobes.hair_offset = input_constant(node, "offset", 2.0f * M_PI_F / 180.0f);
    lobes.hair_ior = input_constant(node, "ior", 1.55f);
    lobes.hair_front = lobes.hair_back = lobes.hair_second = constant(one_float3());

    /* The absorption for a color, and back. */
    const float r2 = radial * radial;
    const float scale = 5.969f - 0.215f * radial + 2.532f * r2 - 10.73f * r2 * radial +
                        5.574f * r2 * r2 + 0.245f * r2 * r2 * radial;
    const auto absorption_of = [scale](const float3 color) {
      const float3 sigma = log(max(color, make_float3(1e-5f))) / scale;
      return sigma * sigma;
    };
    const auto color_of = [scale](const float3 sigma) {
      return exp(-sqrt(max(sigma, zero_float3())) * scale);
    };
    switch (node->get_parametrization()) {
      case NODE_PRINCIPLED_HAIR_PIGMENT_CONCENTRATION: {
        const float melanin = -logf(max(1.0f - input_constant(node, "melanin", 0.8f), 0.0001f));
        const float redness = input_constant(node, "melanin_redness", 1.0f);
        const float3 tint = input_constant3(node, "tint", one_float3());
        const float3 sigma = melanin * (1.0f - redness) * make_float3(0.506f, 0.841f, 1.653f) +
                             melanin * redness * make_float3(0.343f, 0.733f, 1.924f) +
                             absorption_of(tint);
        lobes.hair_color = constant(color_of(sigma));
        break;
      }
      case NODE_PRINCIPLED_HAIR_DIRECT_ABSORPTION:
        lobes.hair_color = constant(color_of(
            input_constant3(node, "absorption_coefficient", make_float3(0.245531f, 0.52f, 1.365f))));
        break;
      default:
        lobes.hair_color = input_value(node, "color");
        break;
    }
    if (is_linked(node, "random_color") || is_linked(node, "random_roughness") ||
        input_constant(node, "random_color", 0.0f) != 0.0f ||
        input_constant(node, "random_roughness", 0.0f) != 0.0f)
    {
      note(node, "the random color and roughness per fiber are not exported");
    }
    return lobes;
  }

  /* The lobes of the body as the attributes of a DwaBaseMaterial. */
  string write_body(const Lobes &lobes, const RdlValue &presence)
  {
    const int object = new_object("DwaBaseMaterial");
    const string name = objects_[object].name;

    if (!is_constant(presence, 1.0f)) {
      set(object, "presence", bind_float(presence));
    }
    if (presence.is_constant() && presence.scale.x <= 0.0f) {
      return name;
    }

    const bool no_conductor = weight_is(lobes.w_conductor, 0.0f);
    const bool all_conductor = !no_conductor && weight_is(lobes.w_rest, 0.0f);
    const bool has_refraction = !weight_is(lobes.w_refraction, 0.0f);
    const bool has_dielectric = !all_conductor &&
                                (has_refraction || !is_constant(lobes.dielectric, 0.0f));
    /* Most of the surface is conductor. */
    const bool strong_conductor = lobes.conductor.is_constant() &&
                                  lobes.conductor.scale.x >= 0.5f;
    const RdlValue conductor = clamp_factor(lobes.conductor);

    /* The conductor is the part `specular * metallic` of the material, the rest has a
     * dielectric reflection of the weight `specular`. */
    if (all_conductor) {
      set(object, "metallic", "1");
    }
    else if (!has_dielectric) {
      if (no_conductor) {
        set(object, "specular", "0");
      }
      else {
        /* No dielectric reflection: one with the index of refraction of the surroundings. */
        set(object, "specular", bind_float(conductor));
        set(object, "metallic", "0.999");
        set(object, "refractive_index", "1");
      }
    }
    else {
      const RdlValue dielectric = clamp_factor(lobes.dielectric);
      if (no_conductor) {
        set(object, "specular", bind_float(dielectric));
      }
      else if (is_constant(dielectric, 1.0f)) {
        set(object, "metallic", bind_float(conductor));
      }
      else {
        /* specular = 1 - (1 - dielectric) (1 - conductor), metallic = conductor / specular */
        const RdlValue specular = invert(multiply(invert(dielectric), invert(conductor)));
        set(object, "specular", bind_float(specular));
        set(object, "metallic", bind_float(divide(conductor, specular)));
      }
      set(object, "refractive_index", number(lobes.ior));
      if (!has_refraction) {
        /* An opaque surface reflects from its back side as from its front, as in Cycles. MoonRay
         * takes a hit on the back side as a ray which leaves the material and turns the index
         * of refraction around: several times the reflection, total beyond 44 degrees. Meshes
         * with faces turned the wrong way are common (Lone Monk). */
        set(object, "thin_geometry", true);
      }
    }

    if (!weight_is(lobes.w_rough, 0.0f)) {
      RdlValue roughness = lobes.roughness;
      if (!no_conductor && !all_conductor && !strong_conductor) {
        /* MoonRay adds the energy rough microfacets lose at full strength whatever the weight
         * of the lobe is, which shows above a roughness of a half. */
        roughness = roughness.is_constant() ?
                        constant(min(roughness.scale.x, 0.5f)) :
                        clamp_value(roughness, 0.0f, 0.5f);
      }
      set(object, "roughness", bind_float(roughness));
      if (!is_constant(lobes.anisotropy, 0.0f)) {
        set(object, "anisotropy", bind_float(lobes.anisotropy));
      }
      if (lobes.beckmann) {
        set(object, "specular_model", 0);
      }
    }
    if (!no_conductor) {
      set(object, "metallic_color", bind_rgb(lobes.conductor_color));
      set(object, "metallic_edge_color", bind_rgb(lobes.conductor_edge_color));
    }

    /* The diffuse part: reflection and transmission. MoonRay takes the transmitted color off
     * the reflected one. */
    RdlValue albedo = weight_is(lobes.w_diffuse, 0.0f) ? constant(zero_float3()) : lobes.albedo;
    if (weight_is(lobes.w_translucent, 0.0f)) {
      set(object, "diffuse_transmission", "0");
    }
    else {
      const RdlValue translucent = clamp_factor(lobes.translucent);
      set(object, "diffuse_transmission", bind_float(translucent));
      set(object, "diffuse_transmission_color", bind_rgb(lobes.translucent_color));
      set(object, "diffuse_transmission_blending_behavior", 0);
      if (!is_constant(albedo, 0.0f)) {
        /* albedo (1 - translucent) / (1 - translucent * color) */
        const RdlValue transmitted = multiply(lobes.translucent_color, translucent);
        albedo = multiply(albedo, divide(invert(translucent), invert(transmitted)));
        albedo.scalar = false;
      }
    }
    set(object, "albedo", bind_rgb(albedo));
    if (!weight_is(lobes.w_subsurface, 0.0f) && !is_constant(lobes.subsurface_radius, 0.0f)) {
      /* The radius of each channel is the color times the radius, and the color is clamped
       * to one. */
      float highest = 1.0f;
      RdlValue radius = multiply(lobes.subsurface_color,
                                 subsurface_factor(lobes, albedo, &highest));
      radius.scalar = false;
      if (radius.is_constant()) {
        highest = max(reduce_max(radius.scale), 1e-6f);
      }
      else {
        highest *= max(reduce_max(lobes.subsurface_color.scale), 1e-6f);
      }
      set(object, "scattering_radius", bind_float(scaled(lobes.subsurface_radius, highest)));
      set(object, "scattering_color", bind_rgb(scaled(radius, 1.0f / highest)));
      /* The diffusion profile of Christensen and Burley is the normalized diffusion of
       * MoonRay; the other methods are random walks, which let light through thin parts. */
      set(object, "bssrdf", (lobes.subsurface_method == CLOSURE_BSSRDF_BURLEY_ID) ? 0 : 2);
    }

    if (has_refraction) {
      set(object, "transmission", bind_float(clamp_factor(lobes.refraction)));
      set(object, "transmission_color", bind_rgb(lobes.refraction_color));
      /* Without this the light behind glass is lost. */
      set(object, "casts_caustics", true);
    }
    if (!is_constant(lobes.emission, 0.0f)) {
      set(object, "show_emission", true);
      set(object, "emission", bind_rgb(lobes.emission));
    }
    if (!weight_is(lobes.w_fuzz, 0.0f) && !is_constant(lobes.fuzz, 0.0f)) {
      set(object, "show_fuzz", true);
      set(object, "fuzz", bind_float(lobes.fuzz));
      set(object, "fuzz_albedo", bind_rgb(lobes.fuzz_color));
      set(object, "fuzz_roughness", bind_float(lobes.fuzz_roughness));
    }
    if (!weight_is(lobes.w_coat, 0.0f) && !is_constant(lobes.coat, 0.0f)) {
      set(object, "show_clearcoat", true);
      set(object, "clearcoat", bind_float(lobes.coat));
      set(object, "clearcoat_roughness", bind_float(lobes.coat_roughness));
      set(object, "clearcoat_refractive_index", number(lobes.coat_ior));
    }
    if (!lobes.normal.empty()) {
      set(object, "input_normal", lobes.normal);
      if (!is_constant(lobes.normal_dial, 1.0f)) {
        set(object, "input_normal_dial", bind_float(lobes.normal_dial));
      }
    }
    return name;
  }

  /* The material of the surface output, returns the name of its object. A shader which uses
   * the Light Path node is written for each kind of ray, as a RaySwitchMaterial of the results
   * which differ. Shadow rays only see the presence of a material, which the RaySwitchMaterial
   * takes from its default material: that makes a shader which is transparent for shadow rays
   * cast no shadow. */
  string root_material(ShaderOutput *output)
  {
    if (!depends_on_ray(output->parent)) {
      return write_material(closure(output));
    }
    string names[RAY_NUM];
    string bodies[RAY_NUM];
    bool differ = false;
    for (int ray = 0; ray < RAY_NUM; ray++) {
      ray_type_ = ray;
      names[ray] = write_material(closure(output));
      bodies[ray] = objects_.back().body;
      const bool is_last = objects_.back().name == names[ray];
      for (int other = 0; other < ray && is_last; other++) {
        if (bodies[other] == bodies[ray]) {
          /* The same material again. */
          objects_.pop_back();
          names[ray] = names[other];
          break;
        }
      }
      differ = differ || names[ray] != names[0];
    }
    ray_type_ = RAY_CAMERA;
    if (!differ) {
      return names[0];
    }
    const int object = new_object("RaySwitchMaterial");
    const auto material = [&](const int ray) { return expr_of(names[ray]); };
    set(object, "default_material", material(RAY_SHADOW));
    set(object, "camera_ray_material", material(RAY_CAMERA));
    set(object, "indirect_diffuse_ray_material", material(RAY_DIFFUSE));
    set(object, "indirect_glossy_ray_material", material(RAY_GLOSSY));
    set(object, "indirect_mirror_ray_material", material(RAY_SINGULAR));
    return objects_[object].name;
  }

  /* A path tracer which does not sample emitting surfaces only finds a lamp by chance. The
   * MoonRay device can put a light on the surface (the material parameter `emissionSampling`);
   * the surface then must not emit for the rays which the light is seen by, or its light would
   * be counted twice. This writes what that takes: a root which emits for camera and mirror
   * rays only - a light is not seen by those - and the emission the lights have: the one a
   * diffuse ray sees, as a color and a map. Whether the device uses any of it is decided when
   * the scene is exported, from what Cycles samples. */
  void write_emitter(ShaderOutput *output)
  {
    ray_type_ = RAY_DIFFUSE;
    const Lobes lit = closure(output);
    RdlValue radiance = lit.emission;
    if (!is_constant(radiance, 0.0f)) {
      /* A surface which is only there in part emits in part. */
      const RdlValue presence = weight_value(lit.w_surface);
      if (!is_constant(presence, 1.0f)) {
        radiance = multiply(radiance, presence);
      }
    }
    ray_type_ = RAY_CAMERA;
    if (is_constant(radiance, 0.0f)) {
      return;
    }
    string map_name;
    if (!radiance.map.empty()) {
      for (const Object &object : objects_) {
        if (object.expr == radiance.map) {
          map_name = object.name;
        }
      }
      if (map_name.empty()) {
        /* Not a map of this text. */
        return;
      }
    }

    string names[RAY_NUM];
    for (int ray = 0; ray < RAY_NUM; ray++) {
      ray_type_ = ray;
      Lobes lobes = closure(output);
      if (ray != RAY_CAMERA && ray != RAY_SINGULAR) {
        lobes.emission = constant(zero_float3());
      }
      names[ray] = write_material(lobes);
      /* The material which is there already, when nothing differs. */
      if (objects_.back().name == names[ray]) {
        const string class_name = objects_.back().expr.substr(0, objects_.back().expr.find('('));
        for (size_t other = 0; other + 1 < objects_.size(); other++) {
          if (objects_[other].body == objects_.back().body &&
              objects_[other].expr.compare(0, class_name.size() + 1, class_name + "(") == 0)
          {
            names[ray] = objects_[other].name;
            objects_.pop_back();
            break;
          }
        }
      }
    }
    ray_type_ = RAY_CAMERA;

    const int object = new_object("RaySwitchMaterial");
    const auto material = [&](const int ray) { return expr_of(names[ray]); };
    set(object, "default_material", material(RAY_SHADOW));
    set(object, "camera_ray_material", material(RAY_CAMERA));
    set(object, "indirect_diffuse_ray_material", material(RAY_DIFFUSE));
    set(object, "indirect_glossy_ray_material", material(RAY_GLOSSY));
    set(object, "indirect_mirror_ray_material", material(RAY_SINGULAR));

    source_.sampled_material_name = objects_[object].name;
    source_.emission_color = radiance.scale;
    source_.emission_map = map_name;
  }

  const Shader *shader_;
  AnariAttributeSlots &slots_;
  AnariGraphSource &source_;
  string prefix_;
  bool use_bump_ = true;

  vector<Object> objects_;
  /* The kind of ray the graph is evaluated for, see root_material(). */
  int ray_type_ = RAY_CAMERA;
  map<const ShaderNode *, bool> ray_dependent_;
  map<std::pair<const ShaderOutput *, int>, RdlValue> values_;
  std::set<const ShaderOutput *> visiting_;
  map<std::pair<const ShaderOutput *, int>, Lobes> closures_;
  /* Values many nodes share, such as the position. */
  map<string, RdlValue> shared_;
  /* Normal maps by the output they come from. */
  map<string, std::pair<string, RdlValue>> normals_;
  string base_normal_;
  bool base_normal_done_ = false;
};

}  // namespace

bool anari_graph_write_rdla(const Shader *shader,
                            AnariAttributeSlots &slots,
                            AnariGraphSource &source)
{
  source = AnariGraphSource();
  RdlaWriter writer(shader, slots, source);
  if (!writer.write()) {
    const string failure = source.failure;
    source = AnariGraphSource();
    source.failure = failure;
    return false;
  }
  return true;
}

CCL_NAMESPACE_END

#endif /* WITH_ANARI */
