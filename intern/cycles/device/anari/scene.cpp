/* SPDX-FileCopyrightText: 2011-2026 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

#ifdef WITH_ANARI

#  include "device/anari/scene.h"

#  include "integrator/shader_eval.h"

#  include "scene/attribute.h"
#  include "scene/background.h"
#  include "scene/camera.h"
#  include "scene/film.h"
#  include "scene/hair.h"
#  include "scene/integrator.h"
#  include "scene/light.h"
#  include "scene/mesh.h"
#  include "scene/object.h"
#  include "scene/pointcloud.h"
#  include "scene/scene.h"
#  include "scene/shader.h"
#  include "scene/shader_graph.h"
#  include "scene/shader_nodes.h"
#  include "scene/volume.h"

#  include "util/color.h"
#  include "util/half.h"
#  include "util/log.h"
#  include "util/path.h"
#  include "util/progress.h"
#  include "util/projection.h"
#  include "util/tbb.h"
#  include "util/time.h"

#  ifdef WITH_NANOVDB
#    include <nanovdb/NanoVDB.h>
#  endif

CCL_NAMESPACE_BEGIN

namespace {

/* Create a device owned 1D array and fill it with the given data. */
ANARIArray1D new_array_1d(ANARIDevice device,
                          ANARIDataType type,
                          const void *data,
                          const size_t num_elements,
                          const size_t element_size)
{
  ANARIArray1D array = anariNewArray1D(device, nullptr, nullptr, nullptr, type, num_elements);
  void *mapped = anariMapArray(device, array);
  memcpy(mapped, data, num_elements * element_size);
  anariUnmapArray(device, array);
  return array;
}

template<typename T>
void set_array_1d(ANARIDevice device,
                  ANARIObject object,
                  const char *name,
                  ANARIDataType type,
                  const vector<T> &data)
{
  if (data.empty()) {
    return;
  }
  ANARIArray1D array = new_array_1d(device, type, data.data(), data.size(), sizeof(T));
  anariSetParameter(device, object, name, ANARI_ARRAY1D, &array);
  anariRelease(device, array);
}

/* Column-major 4x4 matrix from a Cycles transform. */
void transform_to_mat4(const Transform &t, float m[16])
{
  m[0] = t.x.x;
  m[1] = t.y.x;
  m[2] = t.z.x;
  m[3] = 0.0f;
  m[4] = t.x.y;
  m[5] = t.y.y;
  m[6] = t.z.y;
  m[7] = 0.0f;
  m[8] = t.x.z;
  m[9] = t.y.z;
  m[10] = t.z.z;
  m[11] = 0.0f;
  m[12] = t.x.w;
  m[13] = t.y.w;
  m[14] = t.z.w;
  m[15] = 1.0f;
}

bool geometry_attributes_modified(const Geometry *geom)
{
  for (const Attribute &attr : geom->attributes.attributes) {
    if (attr.modified) {
      return true;
    }
  }
  return false;
}

/* Read a generic attribute value as float4, for the given element index. */
float4 attribute_value(const Attribute &attr, const size_t index)
{
  if (attr.element & ATTR_ELEMENT_IS_BYTE) {
    const uchar4 c = attr.data<uchar4>()[index];
    return color_srgb_to_linear_v4(color_uchar4_to_float4(c));
  }
  if (attr.element & ATTR_ELEMENT_IS_NORMAL) {
    const float3 n = attr.data<packed_normal>()[index].decode();
    return make_float4(n.x, n.y, n.z, 1.0f);
  }
  if (attr.type == TypeFloat) {
    const float f = attr.data<float>()[index];
    return make_float4(f, f, f, 1.0f);
  }
  if (attr.type == TypeFloat2) {
    const float2 f = attr.data<float2>()[index];
    return make_float4(f.x, f.y, 0.0f, 1.0f);
  }
  if (attr.type == TypeFloat4 || attr.type == TypeRGBA) {
    return attr.data<float4>()[index];
  }
  if (attr.type == TypeMatrix) {
    return zero_float4();
  }
  const float3 f = attr.data<packed_float3>()[index];
  return make_float4(f.x, f.y, f.z, 1.0f);
}

/* Index of the attribute element for a mesh triangle corner, -1 if the attribute is not
 * per-vertex or per-corner data that can be interpolated over the triangle. */
int64_t mesh_attribute_index(const Attribute &attr,
                             const Mesh *mesh,
                             const size_t triangle,
                             const int corner)
{
  switch (attr.element & ~(ATTR_ELEMENT_IS_BYTE | ATTR_ELEMENT_IS_NORMAL)) {
    case ATTR_ELEMENT_VERTEX:
      return mesh->get_triangles()[triangle * 3 + corner];
    case ATTR_ELEMENT_CORNER:
      return int64_t(triangle * 3 + corner);
    case ATTR_ELEMENT_FACE:
      return int64_t(triangle);
    case ATTR_ELEMENT_MESH:
    case ATTR_ELEMENT_OBJECT:
      return 0;
    default:
      return -1;
  }
}

}  // namespace

/* --------------------------------------------------------------------
 * Camera state.
 */

bool AnariScene::CameraState::operator==(const CameraState &other) const
{
  return type == other.type && panorama_type == other.panorama_type &&
         position == other.position && direction == other.direction && up == other.up &&
         x0 == other.x0 && x1 == other.x1 && y0 == other.y0 && y1 == other.y1 &&
         aperture_radius == other.aperture_radius && focus_distance == other.focus_distance &&
         near_clip == other.near_clip && far_clip == other.far_clip;
}

/* --------------------------------------------------------------------
 * Scene.
 */

AnariScene::AnariScene(AnariDevice &device) : device_(device), images_(device)
{
  ANARIDevice d = anari();
  ANARI_TRACE("scene: world and renderer");

  world_ = anariNewWorld(d);
  anariCommitParameters(d, world_);

  renderer_ = anariNewRenderer(d, device_.renderer_subtype().c_str());
  anariCommitParameters(d, renderer_);

  material_subtype_ = device_.has_subtype(ANARI_MATERIAL, "physicallyBased") ?
                          "physicallyBased" :
                          "matte";

  /* Devices which evaluate shader graphs get them as text, see `graph_export.h`. */
  if (device_.use_graph_materials()) {
    if (device_.has_subtype(ANARI_MATERIAL, anari_graph_subtype(ANARI_GRAPH_CYCLES_XML))) {
      graph_format_ = ANARI_GRAPH_CYCLES_XML;
    }
    else if (device_.has_subtype(ANARI_MATERIAL, anari_graph_subtype(ANARI_GRAPH_RDLA))) {
      graph_format_ = ANARI_GRAPH_RDLA;
    }
    else if (device_.has_subtype(ANARI_MATERIAL, anari_graph_subtype(ANARI_GRAPH_MATERIALX))) {
      graph_format_ = ANARI_GRAPH_MATERIALX;
    }
  }
  /* For debugging the export: force a format, also on a device which does not take it. */
  if (const char *env_format = getenv("CYCLES_ANARI_GRAPH_FORMAT")) {
    for (const AnariGraphFormat format :
         {ANARI_GRAPH_NONE, ANARI_GRAPH_CYCLES_XML, ANARI_GRAPH_MATERIALX, ANARI_GRAPH_RDLA})
    {
      if (strcmp(env_format, anari_graph_subtype(format)) == 0) {
        graph_format_ = format;
      }
    }
  }
  /* Not in the ANARI standard: devices which have the flags declare the parameters. */
  instance_visibility_ = device_.has_parameter(
      ANARI_INSTANCE, "transform", "visibility.camera", ANARI_BOOL);
  instance_object_random_ = device_.has_parameter(
      ANARI_INSTANCE, "transform", "objectRandom", ANARI_FLOAT32);
  light_visibility_ = device_.has_parameter(
      ANARI_LIGHT, "point", "visibility.glossy", ANARI_BOOL);
  background_image_ = device_.has_extension("ANARI_KHR_RENDERER_BACKGROUND_IMAGE");
  /* More attributes than the four of ANARI, for materials which are exported as graph. */
  int num_slots = 4;
  while (num_slots < AnariAttributeSlots::max_slots &&
         device_.has_parameter(ANARI_GEOMETRY,
                               "triangle",
                               string_printf("vertex.attribute%d", num_slots).c_str(),
                               ANARI_ARRAY1D))
  {
    num_slots++;
  }
  slots_.set_num_slots(num_slots);
  light_max_bounce_ = device_.has_parameter(ANARI_LIGHT, "point", "maxBounce", ANARI_INT32);
  light_cast_shadow_ = device_.has_parameter(ANARI_LIGHT, "point", "castShadow", ANARI_BOOL);
  light_falloff_ = device_.has_parameter(ANARI_LIGHT, "point", "falloff.distance", ANARI_ARRAY1D);
  /* A path tracer without light sampling of emitting surfaces only finds a lamp by chance. A
   * device which can sample one with lights of its own says so with this parameter. */
  emission_sampling_ = device_.has_parameter(
      ANARI_MATERIAL, material_subtype_.c_str(), "emissionSampling", ANARI_STRING);
  emission_sampling_graph_ = graph_format_ != ANARI_GRAPH_NONE &&
                             device_.has_parameter(ANARI_MATERIAL,
                                                   anari_graph_subtype(graph_format_),
                                                   "emissionSampling",
                                                   ANARI_STRING);
  /* For measuring what it does: leave the emission to the surfaces. */
  if (getenv("CYCLES_ANARI_NO_EMISSION_SAMPLING")) {
    emission_sampling_ = emission_sampling_graph_ = false;
  }
  light_world_graphs_ = graph_format_ == ANARI_GRAPH_CYCLES_XML &&
                        (device_.has_extension("CYCLES_SHADER_XML") ||
                         device_.has_extension("ANARI_CYCLES_SHADER_XML"));
  ANARI_TRACE("scene: material graphs \"%s\", lights and world %d, emission sampling %d (graphs %d)",
              anari_graph_subtype(graph_format_),
              int(light_world_graphs_),
              int(emission_sampling_),
              int(emission_sampling_graph_));

  /* The default surface of Cycles is a diffuse BSDF. */
  AnariPrincipled default_params;
  default_params.has_bsdf = true;
  default_params.is_diffuse = true;
  const string default_subtype = material_subtype_for(default_params);
  ANARI_TRACE("scene: default material %s", default_subtype.c_str());
  default_material_ = anariNewMaterial(d, default_subtype.c_str());
  AnariMaterialBuilder builder(device_, nullptr, slots_, images_);
  builder.set_material(default_material_, default_subtype.c_str(), default_params);
}

AnariScene::~AnariScene()
{
  ANARIDevice d = anari();

  for (auto &it : geometries_) {
    release_geometry(it.second);
  }
  for (auto &it : materials_) {
    anariRelease(d, it.second);
  }
  release_lights();
  for (ANARIInstance instance : instances_) {
    anariRelease(d, instance);
  }
  if (background_light_) {
    anariRelease(d, background_light_);
  }
  if (background_sun_light_) {
    anariRelease(d, background_sun_light_);
  }
  anariRelease(d, default_material_);
  if (camera_) {
    anariRelease(d, camera_);
  }
  anariRelease(d, renderer_);
  anariRelease(d, world_);
}

/* Texel budget of the atlas of one mesh and shader, the range of the cell size, and the number
 * of texels along the diagonal of the scene which sets the texel size. Below the smallest cell
 * the triangles are small enough for vertex colors. */
static const size_t BAKE_ATLAS_TEXELS = 1 << 20;
static const int BAKE_ATLAS_MIN_CELL = 6;
static const int BAKE_ATLAS_MAX_CELL = 256;
static const float BAKE_SCENE_TEXELS = 4096.0f;
/* Name of the slot with the texture coordinates of baked images, not one of an attribute. */
static const char *BAKE_UV_ATTRIBUTE = "anari bake uv";
/* Opacity of an object which casts no shadows around a light, see #update_light_shells. */
static const float LIGHT_SHELL_OPACITY = 0.5f;

void AnariScene::prepare_update(Scene *scene)
{
  for (Geometry *geom : scene->geometry) {
    if (geom->is_modified() || geometry_attributes_modified(geom)) {
      modified_geometry_.insert(geom);
      if (geom->is_light()) {
        lights_modified_ = true;
      }
    }
  }

  if (scene->objects.size() != num_objects_) {
    objects_modified_ = true;
    lights_modified_ = true;
  }
  for (const Object *object : scene->objects) {
    if (object->is_modified()) {
      objects_modified_ = true;
      if (object->get_geometry() && object->get_geometry()->is_light()) {
        lights_modified_ = true;
      }
    }
  }

  for (const Shader *shader : scene->shaders) {
    if (shader->is_modified()) {
      modified_shaders_.insert(shader);
      /* The attributes of the shader get their slots again. */
      slots_.restart(shader);
    }
  }

  if (graph_format_ != ANARI_GRAPH_NONE) {
    export_graph_sources(scene);
  }
  prepare_base_color_bakes(scene);
  /* Lights are exported again when the graph of their shader changed. */
  for (const Shader *shader : light_world_shaders_) {
    if (modified_shaders_.count(shader)) {
      lights_modified_ = true;
    }
  }

  if (scene->background->is_modified()) {
    background_modified_ = true;
  }
  if (scene->integrator->is_modified() || scene->film->is_modified() ||
      scene->background->is_modified())
  {
    renderer_modified_ = true;
  }
}

void AnariScene::update(Scene *scene)
{
  AnariMaterialBuilder builder(device_, scene, slots_, images_);
  builder.set_surface_attribute(
      [this, scene](const Shader *shader, const string &attribute, vector<float4> &values) {
        return surface_attribute(scene, shader, attribute, values);
      });

  const double update_start_time = time_dt();
  if ((full_update_ || objects_modified_ || lights_modified_ ||
       num_objects_ != scene->objects.size()) &&
      update_light_shells(scene))
  {
    lights_modified_ = true;
    objects_modified_ = true;
  }
  ANARI_TRACE("update: materials");
  /* Materials first, they assign the attribute slots used by the geometry export. */
  sync_materials(scene, builder);
  update_graph_images(scene);

  /* The shaders whose attributes are in other slots than the geometry was exported with. */
  slots_.finish();
  set<const Shader *> slots_changed;
  for (const Shader *shader : scene->shaders) {
    const int version = slots_.get(shader).version;
    auto exported = exported_slots_versions_.find(shader);
    if (exported == exported_slots_versions_.end()) {
      exported_slots_versions_[shader] = version;
    }
    else if (exported->second != version) {
      exported->second = version;
      slots_changed.insert(shader);
    }
  }

  /* Texel size of baked images, from the size of the scene. */
  if (!baked_graphs_.empty()) {
    BoundBox bounds = BoundBox::empty;
    for (const Object *object : scene->objects) {
      const Geometry *geom = object->get_geometry();
      if (geom && !geom->is_light() && object->bounds.valid()) {
        bounds.grow(object->bounds);
      }
    }
    bake_texel_size_ = bounds.valid() ? len(bounds.size()) / BAKE_SCENE_TEXELS : 0.0f;
  }

  ANARI_TRACE("update: geometry");
  /* Geometry. */
  bool geometry_changed = false;
  {
    set<const Geometry *> current;
    for (Geometry *geom : scene->geometry) {
      current.insert(geom);
      if (geom->is_light()) {
        continue;
      }

      auto it = geometries_.find(geom);
      const bool is_new = (it == geometries_.end());
      GeometryData &data = geometries_[geom];

      bool need_export = full_update_ || is_new || modified_geometry_.count(geom) != 0;
      /* Rebuild surfaces when one of their materials was recreated, wants other attributes or
       * is baked again. */
      for (const Node *node : geom->get_used_shaders()) {
        const Shader *shader = static_cast<const Shader *>(node);
        if ((modified_shaders_.count(shader) && materials_.find(shader) == materials_.end()) ||
            recreated_materials_.count(shader) || rebake_shaders_.count(shader) ||
            slots_changed.count(shader))
        {
          need_export = true;
        }
      }

      if (need_export) {
        sync_geometry(scene, geom, data);
        geometry_changed = true;
      }
    }

    for (auto it = geometries_.begin(); it != geometries_.end();) {
      if (current.find(it->first) == current.end()) {
        release_geometry(it->second);
        it = geometries_.erase(it);
        geometry_changed = true;
      }
      else {
        ++it;
      }
    }
  }

  ANARI_TRACE("update: lights");
  /* Lights and world. */
  bool world_lights_changed = false;
  bool light_card_modified = false;
  if (!full_update_ && !objects_modified_ && !modified_shaders_.empty()) {
    /* The emission of a light card is the one of its material. The objects are the ones of the
     * last update here, otherwise the lights are exported again anyway. */
    for (const Object *object : light_cards_) {
      for (const Node *node : object->get_geometry()->get_used_shaders()) {
        light_card_modified |= modified_shaders_.count(static_cast<const Shader *>(node)) != 0;
      }
    }
  }
  if (full_update_ || lights_modified_ || objects_modified_ || light_card_modified) {
    sync_lights(scene, builder);
    world_lights_changed = true;
  }

  const Shader *background_shader = scene->background->get_shader(scene);
  const ShaderGraph *background_graph = background_shader ? background_shader->graph.get() :
                                                            nullptr;
  if (full_update_ || background_modified_ || background_graph != background_graph_ ||
      (background_shader && modified_shaders_.count(background_shader)))
  {
    ANARI_TRACE("update: background");
    sync_background(scene, builder);
    background_graph_ = background_graph;
    world_lights_changed = true;
    renderer_modified_ = true;
  }

  if (full_update_ || renderer_modified_) {
    ANARI_TRACE("update: renderer");
    sync_renderer(scene);
  }

  if (full_update_ || objects_modified_ || geometry_changed || world_lights_changed) {
    ANARI_TRACE("update: instances");
    sync_instances(scene);
    ANARI_TRACE("update: world");
    commit_world();
  }

  ANARI_TRACE("update: camera");
  sync_camera(scene);

  images_.garbage_collect();
  ANARI_TRACE("update: done in %.1f ms", (time_dt() - update_start_time) * 1000.0);

  full_update_ = false;
  modified_geometry_.clear();
  modified_shaders_.clear();
  rebake_shaders_.clear();
  objects_modified_ = false;
  lights_modified_ = false;
  background_modified_ = false;
  renderer_modified_ = false;
  num_objects_ = scene->objects.size();
}

/* --------------------------------------------------------------------
 * Materials.
 */

ANARIMaterial AnariScene::material_for_shader(const Shader *shader)
{
  auto it = materials_.find(shader);
  return (it != materials_.end()) ? it->second : default_material_;
}

string AnariScene::material_subtype_for(const AnariPrincipled &params) const
{
  if (params.is_diffuse && !params.has_emission && !params.has_normal_map &&
      device_.has_subtype(ANARI_MATERIAL, "matte"))
  {
    return "matte";
  }
  return material_subtype_;
}

/* Only shaders of surfaces become materials, light and background shaders are exported with the
 * lights and the world. */
static set<const Shader *> find_surface_shaders(const Scene *scene)
{
  set<const Shader *> surface_shaders;
  for (const Geometry *geom : scene->geometry) {
    if (geom->is_light()) {
      continue;
    }
    for (const Node *node : geom->get_used_shaders()) {
      surface_shaders.insert(static_cast<const Shader *>(node));
    }
  }
  return surface_shaders;
}

void AnariScene::export_graph_sources(Scene *scene)
{
  /* The graphs are written here, before the scene update: afterwards they are finalized for the
   * Cycles kernels, with nodes the device has to create itself. */
  const set<const Shader *> surface_shaders = find_surface_shaders(scene);

  /* Shaders of lights and of the world, for devices which take them as graphs too. */
  light_world_shaders_.clear();
  if (light_world_graphs_) {
    for (const Geometry *geom : scene->geometry) {
      if (geom->is_light()) {
        const Shader *shader = static_cast<const Light *>(geom)->get_shader();
        if (shader && shader != scene->default_light) {
          light_world_shaders_.insert(shader);
        }
      }
    }
    if (const Shader *shader = scene->background->get_shader(scene)) {
      light_world_shaders_.insert(shader);
    }
  }

  for (Shader *shader : scene->shaders) {
    const bool is_surface = surface_shaders.find(shader) != surface_shaders.end();
    if (!is_surface && light_world_shaders_.find(shader) == light_world_shaders_.end()) {
      continue;
    }

    ShaderGraph *graph = shader->graph.get();
    auto it = graph_sources_.find(shader);
    if (it != graph_sources_.end() && it->second.graph == graph &&
        (!shader->is_modified() || graph == nullptr || graph->finalized))
    {
      /* Still the graph the text was written from. */
      continue;
    }

    const double start_time = time_dt();
    if (graph && !graph->finalized) {
      /* In the viewport the graph is only simplified when it is compiled, in the scene update.
       * Do it now: simplifying removes nodes, and the text refers to the image nodes. */
      graph->simplify(scene);
    }
    AnariGraphSource source;
    bool exported = false;
    switch (graph_format_) {
      case ANARI_GRAPH_CYCLES_XML:
        exported = anari_graph_write_cycles_xml(shader, slots_.get(shader), source, is_surface);
        break;
      case ANARI_GRAPH_MATERIALX:
        exported = anari_graph_from_materialx(shader, source);
        break;
      case ANARI_GRAPH_RDLA:
        exported = anari_graph_write_rdla(shader, slots_.get(shader), source);
        break;
      case ANARI_GRAPH_NONE:
        break;
    }
    ANARI_TRACE("graph %s: %s, %d bytes, %d images, %.3f ms",
                shader->name.c_str(),
                exported ? "exported" : ("flattened (" + source.failure + ")").c_str(),
                int(source.text.size()),
                int(source.images.size()),
                (time_dt() - start_time) * 1000.0);

    if (exported) {
      /* For debugging the export: write the text of every graph to a directory. */
      if (const char *dump_directory = getenv("CYCLES_ANARI_GRAPH_DUMP")) {
        const string filepath = path_join(
            dump_directory, shader->name.string() + "." + anari_graph_subtype(graph_format_));
        path_write_text(filepath, source.text);
      }
      graph_sources_[shader] = std::move(source);
    }
    else if (it != graph_sources_.end()) {
      graph_sources_.erase(it);
    }
  }
}

const AnariGraphSource *AnariScene::graph_source_for(const Shader *shader) const
{
  auto it = graph_sources_.find(shader);
  if (it == graph_sources_.end() || it->second.graph != shader->graph.get()) {
    return nullptr;
  }
  return &it->second;
}

/* Which sides of a surface with this shader Cycles samples as a light, as the value of the
 * material parameter `emissionSampling`. Cycles decides it from the setting of the material and
 * from an estimate of the emission, when the shaders are updated. */
static const char *emission_sampling_name(const Shader *shader)
{
  switch (shader ? shader->emission_sampling : EMISSION_SAMPLING_NONE) {
    case EMISSION_SAMPLING_FRONT:
      return "front";
    case EMISSION_SAMPLING_BACK:
      return "back";
    case EMISSION_SAMPLING_FRONT_BACK:
      return "both";
    default:
      return "none";
  }
}

void AnariScene::set_graph_material(Scene *scene,
                                    ANARIMaterial material,
                                    const Shader *shader,
                                    const AnariGraphSource &source)
{
  ANARIDevice d = anari();
  GraphMaterialState &state = graph_states_[shader];

  vector<ANARISampler> samplers(source.images.size(), nullptr);
  for (size_t i = 0; i < samplers.size(); i++) {
    samplers[i] = images_.get_sampler(scene, source.images[i].node);
  }

  /* The sides of the surface which lights of the device sample, when the text has what that
   * takes. */
  const string emission_sampling = (emission_sampling_graph_ &&
                                    !source.sampled_material_name.empty()) ?
                                       emission_sampling_name(shader) :
                                       "none";

  /* An unchanged text is not sent again: the device would rebuild and recompile the shader. */
  const bool text_changed = !state.sent || state.hash != source.hash;
  if (!text_changed && samplers == state.samplers && emission_sampling == state.emission_sampling)
  {
    return;
  }

  if (text_changed) {
    anariUnsetAllParameters(d, material);
    anariSetParameter(d, material, "source", ANARI_STRING, source.text.c_str());
    if (!source.material_name.empty()) {
      anariSetParameter(d, material, "materialName", ANARI_STRING, source.material_name.c_str());
    }
    if (source.format == ANARI_GRAPH_MATERIALX) {
      anariSetParameter(d, material, "sourceType", ANARI_STRING, "documentInline");
    }
    if (emission_sampling_graph_ && !source.sampled_material_name.empty()) {
      anariSetParameter(d,
                        material,
                        "sampledMaterialName",
                        ANARI_STRING,
                        source.sampled_material_name.c_str());
      anariSetParameter(d, material, "emissionColor", ANARI_FLOAT32_VEC3, &source.emission_color);
      if (!source.emission_map.empty()) {
        anariSetParameter(d, material, "emissionMap", ANARI_STRING, source.emission_map.c_str());
      }
    }
  }
  if (text_changed || emission_sampling != state.emission_sampling) {
    if (emission_sampling_graph_) {
      anariSetParameter(d, material, "emissionSampling", ANARI_STRING, emission_sampling.c_str());
    }
  }
  for (size_t i = 0; i < samplers.size(); i++) {
    if (!text_changed && samplers[i] == state.samplers[i]) {
      continue;
    }
    const char *name = source.images[i].parameter.c_str();
    if (samplers[i]) {
      anariSetParameter(d, material, name, ANARI_SAMPLER, &samplers[i]);
    }
    else {
      anariUnsetParameter(d, material, name);
    }
  }
  anariCommitParameters(d, material);
  ANARI_TRACE("graph material %s: %s, emission sampling %s",
              shader->name.c_str(),
              text_changed ? "text" : "images",
              emission_sampling.c_str());

  state.sent = true;
  state.hash = source.hash;
  state.samplers = std::move(samplers);
  state.emission_sampling = emission_sampling;
}

void AnariScene::update_graph_images(Scene *scene)
{
  /* Images change without their shader being modified. This also keeps the samplers of all graph
   * materials in the image cache, which only keeps what was used since the last update. */
  for (auto &it : graph_states_) {
    const AnariGraphSource *source = graph_source_for(it.first);
    auto material = materials_.find(it.first);
    if (source && material != materials_.end()) {
      set_graph_material(scene, material->second, it.first, *source);
    }
  }

  /* The same for the images of the world shader. */
  if (light_world_graphs_ && !background_samplers_.empty()) {
    const Shader *shader = scene->background->get_shader(scene);
    const AnariGraphSource *source = shader ? graph_source_for(shader) : nullptr;
    vector<ANARISampler> samplers;
    if (source) {
      for (const AnariGraphImage &image : source->images) {
        samplers.push_back(images_.get_sampler(scene, image.node));
      }
    }
    if (samplers != background_samplers_) {
      renderer_modified_ = true;
    }
  }
}

void AnariScene::set_graph_source(Scene *scene,
                                  ANARIObject object,
                                  const char *parameter,
                                  const AnariGraphSource &source)
{
  /* The device compares the text and the images with what it has: setting them again does not
   * rebuild the shader. */
  ANARIDevice d = anari();
  anariSetParameter(d, object, parameter, ANARI_STRING, source.text.c_str());
  for (const AnariGraphImage &image : source.images) {
    ANARISampler sampler = images_.get_sampler(scene, image.node);
    if (sampler) {
      anariSetParameter(d, object, image.parameter.c_str(), ANARI_SAMPLER, &sampler);
    }
  }
}

/* --------------------------------------------------------------------
 * Baked base color.
 */

void AnariScene::prepare_base_color_bakes(Scene *scene)
{
  /* Only for shaders of meshes: the color is evaluated at the corners of triangles. */
  set<const Shader *> mesh_shaders;
  set<const Shader *> other_shaders;
  for (const Geometry *geom : scene->geometry) {
    if (geom->is_light()) {
      continue;
    }
    for (const Node *node : geom->get_used_shaders()) {
      (geom->is_mesh() ? mesh_shaders : other_shaders).insert(static_cast<const Shader *>(node));
    }
  }

  AnariMaterialBuilder builder(device_, scene, slots_, images_);
  builder.set_analysis(true);

  for (Shader *shader : scene->shaders) {
    ShaderGraph *graph = shader->graph.get();
    const auto it = baked_graphs_.find(shader);
    const bool was_baked = it != baked_graphs_.end();
    if (was_baked && it->second == graph && (!shader->is_modified() || graph->finalized)) {
      /* Still the graph which was linked for baking. */
      continue;
    }
    if (was_baked) {
      baked_graphs_.erase(it);
      baked_modes_.erase(shader);
      rebake_shaders_.insert(shader);
    }
    const auto calibrated = calibrated_graphs_.find(shader);
    if (calibrated != calibrated_graphs_.end()) {
      if (calibrated->second == graph && (!shader->is_modified() || graph->finalized)) {
        continue;
      }
      calibrated_graphs_.erase(calibrated);
    }
    if (graph == nullptr || graph->finalized || !shader->is_modified() ||
        mesh_shaders.count(shader) == 0 || other_shaders.count(shader) != 0 ||
        graph_source_for(shader) != nullptr)
    {
      continue;
    }

    /* What the flattened material would be, from the simplified graph as it is evaluated
     * after the scene update. */
    graph->simplify(scene);
    const AnariPrincipled params = builder.evaluate_surface(shader);
    /* The factor of a Mix Shader of two shaders with constant parameters, as a mask. Otherwise
     * the base color, when it is not exact and not a texture which a baked image would blur.
     * Of shaders which only emit the emission. */
    AnariPrincipled mix_a;
    AnariPrincipled mix_b;
    const bool bake_color = params.has_bsdf && params.base_color_lossy &&
                            params.base_color.is_constant();
    const bool bake_emission = !params.has_bsdf && params.has_emission &&
                               params.emissive_lossy && params.emissive.is_constant();
    AnariBakeMode mode;
    if (builder.evaluate_mask_mix(shader, mix_a, mix_b)) {
      mode = ANARI_BAKE_MASK;
    }
    else if (bake_color) {
      mode = ANARI_BAKE_BASE_COLOR;
    }
    else if (bake_emission) {
      mode = ANARI_BAKE_EMISSION;
    }
    else {
      /* An image which other nodes change keeps its sampler, see #calibrate_base_color. */
      if (params.has_bsdf && params.base_color_lossy &&
          params.base_color.type == AnariValue::IMAGE &&
          anari_link_bake(shader, ANARI_BAKE_BASE_COLOR))
      {
        ANARI_TRACE("bake %s: base color linked for its average", shader->name.c_str());
        calibrated_graphs_[shader] = graph;
      }
      continue;
    }
    if (anari_link_bake(shader, mode)) {
      ANARI_TRACE("bake %s: %s linked for baking",
                  shader->name.c_str(),
                  (mode == ANARI_BAKE_MASK)     ? "mix factor" :
                  (mode == ANARI_BAKE_EMISSION) ? "emission" :
                                                  "base color");
      baked_graphs_[shader] = graph;
      baked_modes_[shader] = mode;
      rebake_shaders_.insert(shader);
      /* A slot for the texture coordinates of baked images, after the attributes of the
       * shader. Without a free one vertex colors are used. */
      slots_.get(shader).slot_for(ustring(BAKE_UV_ATTRIBUTE));
    }
  }

  set<const Shader *> current;
  for (const Shader *shader : scene->shaders) {
    current.insert(shader);
  }
  for (auto it = baked_graphs_.begin(); it != baked_graphs_.end();) {
    if (current.count(it->first) == 0) {
      baked_modes_.erase(it->first);
      it = baked_graphs_.erase(it);
    }
    else {
      ++it;
    }
  }
  for (auto it = calibrated_graphs_.begin(); it != calibrated_graphs_.end();) {
    it = (current.count(it->first) == 0) ? calibrated_graphs_.erase(it) : std::next(it);
  }
}

bool AnariScene::is_baked(const Shader *shader) const
{
  const auto it = baked_graphs_.find(shader);
  return it != baked_graphs_.end() && it->second == shader->graph.get();
}

bool AnariScene::is_calibrated(const Shader *shader) const
{
  const auto it = calibrated_graphs_.find(shader);
  return it != calibrated_graphs_.end() && it->second == shader->graph.get();
}

namespace {

/* Points of one mesh, as the kernel takes them: the triangle in `prim`. */
struct SurfaceSamples {
  const Mesh *mesh = nullptr;
  vector<KernelShaderEvalInput> inputs;
};

/* Some points of a few meshes which have the shader, where values are averaged over its
 * surface. Of meshes with few triangles more than the centers: a wall is two triangles. */
vector<SurfaceSamples> shader_surface_samples(const Scene *scene, const Shader *shader)
{
  const size_t max_triangles = 256;
  const size_t max_meshes = 8;
  const float2 points[4] = {make_float2(1.0f / 3.0f, 1.0f / 3.0f),
                            make_float2(0.6f, 0.2f),
                            make_float2(0.2f, 0.6f),
                            make_float2(0.2f, 0.2f)};

  vector<SurfaceSamples> result;
  for (const Geometry *geom : scene->geometry) {
    if (!geom->is_mesh()) {
      continue;
    }
    const Mesh *mesh = static_cast<const Mesh *>(geom);
    const array<Node *> &used_shaders = mesh->get_used_shaders();
    int shader_index = -1;
    for (size_t i = 0; i < used_shaders.size(); i++) {
      if (used_shaders[i] == shader) {
        shader_index = int(i);
        break;
      }
    }
    if (shader_index < 0) {
      continue;
    }

    const array<int> &triangle_shaders = mesh->get_shader();
    vector<uint> triangles;
    for (size_t i = 0; i < mesh->num_triangles(); i++) {
      if (((i < triangle_shaders.size()) ? triangle_shaders[i] : 0) == shader_index) {
        triangles.push_back(uint(i));
      }
    }
    if (triangles.empty()) {
      continue;
    }

    SurfaceSamples samples;
    samples.mesh = mesh;
    const size_t step = max(triangles.size() / max_triangles, size_t(1));
    const int num_points = (triangles.size() * 4 <= max_triangles) ? 4 : 1;
    for (size_t i = 0; i < triangles.size(); i += step) {
      for (int p = 0; p < num_points; p++) {
        KernelShaderEvalInput in;
        in.object = 0;
        in.prim = int(triangles[i]);
        in.u = points[p].x;
        in.v = points[p].y;
        samples.inputs.push_back(in);
      }
    }
    result.push_back(std::move(samples));
    if (result.size() >= max_meshes) {
      break;
    }
  }
  return result;
}

}  // namespace

bool AnariScene::surface_attribute(Scene *scene,
                                   const Shader *shader,
                                   const string &attribute,
                                   vector<float4> &values)
{
  /* The attribute of the mesh a sampler of the shader reads by this name, see `export_mesh`. */
  AttributeStandard standard = ATTR_STD_NONE;
  ustring name;
  int slot = -1;
  if (attribute == "color") {
    standard = ATTR_STD_VERTEX_COLOR;
  }
  else if (sscanf(attribute.c_str(), "attribute%d", &slot) == 1 && slot == 0) {
    standard = ATTR_STD_UV;
  }
  else if (slot > 0) {
    const AnariAttributeSlots &slots = slots_.get(shader);
    if (slot >= slots.num_used || slots.slots[slot] == ustring(BAKE_UV_ATTRIBUTE)) {
      return false;
    }
    name = slots.slots[slot];
  }
  else {
    return false;
  }

  values.clear();
  for (const SurfaceSamples &samples : shader_surface_samples(scene, shader)) {
    const Mesh *mesh = samples.mesh;
    const Attribute *attr = (standard != ATTR_STD_NONE) ? mesh->attributes.find(standard) :
                                                          mesh->attributes.find(name);
    if (attr == nullptr) {
      continue;
    }
    for (const KernelShaderEvalInput &in : samples.inputs) {
      /* The barycentric coordinates of the kernel. */
      const float weights[3] = {1.0f - in.u - in.v, in.u, in.v};
      float4 value = zero_float4();
      bool valid = true;
      for (int c = 0; c < 3 && valid; c++) {
        const int64_t index = mesh_attribute_index(*attr, mesh, size_t(in.prim), c);
        valid = index >= 0;
        if (valid) {
          value += attribute_value(*attr, size_t(index)) * weights[c];
        }
      }
      if (valid) {
        values.push_back(value);
      }
    }
  }
  return !values.empty();
}

void AnariScene::calibrate_base_color(Scene *scene,
                                      AnariMaterialBuilder &builder,
                                      const Shader *shader,
                                      AnariPrincipled &params)
{
  float4 fitted;
  if (params.base_color.type != AnariValue::IMAGE ||
      !builder.average_value(params.base_color, fitted, shader))
  {
    return;
  }

  /* The color at the points the average of the sampler was taken at. */
  const double start_time = time_dt();
  float3 sum = zero_float3();
  size_t count = 0;
  for (const SurfaceSamples &samples : shader_surface_samples(scene, shader)) {
    vector<float3> colors;
    if (bake_evaluate(scene, samples.mesh, samples.inputs, colors)) {
      for (const float3 &color : colors) {
        if (isfinite_safe(color)) {
          sum += color;
          count++;
        }
      }
    }
  }
  if (count == 0) {
    return;
  }

  /* Scale the colors of the sampler to the average, which keeps black as it is and takes
   * contrast away where the shader darkens a texture. Not up by several times, and not a fit
   * whose average is next to nothing or negative: a ratio of such numbers would blow the
   * texture up, its colors are moved to the average instead. */
  const float3 exact = sum / float(count);
  float4 scale = one_float4();
  float4 offset = zero_float4();
  for (int i = 0; i < 3; i++) {
    const float ratio = (fitted[i] > 0.01f) ? exact[i] / fitted[i] : -1.0f;
    if (ratio >= 0.0f && ratio <= 4.0f) {
      scale[i] = max(ratio, 0.02f);
    }
    else {
      offset[i] = exact[i] - fitted[i];
    }
  }
  params.base_color.apply_linear(scale, offset);
  ANARI_TRACE("bake %s: average of %d points %g %g %g, of the sampler %g %g %g, in %.1f ms",
              shader->name.c_str(),
              int(count),
              double(exact.x),
              double(exact.y),
              double(exact.z),
              double(fitted.x),
              double(fitted.y),
              double(fitted.z),
              (time_dt() - start_time) * 1000.0);
}

bool AnariScene::bake_evaluate(Scene *scene,
                               const Mesh *mesh,
                               const vector<KernelShaderEvalInput> &inputs,
                               vector<float3> &colors)
{
  /* The kernel evaluates for an object: any one which has the mesh. */
  int object_index = -1;
  for (size_t i = 0; i < scene->objects.size(); i++) {
    if (scene->objects[i]->get_geometry() == mesh) {
      object_index = int(i);
      break;
    }
  }
  if (object_index < 0 || inputs.empty()) {
    return false;
  }

  const int num_samples = int(inputs.size());
  colors.assign(inputs.size(), zero_float3());

  /* The kernel returns the displacement in object space, the shader output is in world space. */
  const Transform tfm = mesh->transform_applied ? transform_identity() :
                                                  scene->objects[object_index]->get_tfm();

  Progress progress;
  ShaderEval shader_eval(&device_, progress);
  return shader_eval.eval(
      SHADER_EVAL_DISPLACE,
      num_samples,
      3,
      [&](device_vector<KernelShaderEvalInput> &d_input) {
        KernelShaderEvalInput *input = d_input.data();
        for (int i = 0; i < num_samples; i++) {
          input[i] = inputs[i];
          input[i].object = object_index;
          input[i].prim += mesh->prim_offset;
        }
        return num_samples;
      },
      [&](const device_vector<float> &d_output) {
        const float *output = d_output.data();
        for (int i = 0; i < num_samples; i++) {
          const float3 displacement = make_float3(
              output[i * 3 + 0], output[i * 3 + 1], output[i * 3 + 2]);
          colors[i] = max(transform_direction(&tfm, displacement), zero_float3());
        }
      });
}

bool AnariScene::bake_base_color(Scene *scene,
                                 const Mesh *mesh,
                                 const vector<uint> &triangles,
                                 vector<float3> &colors)
{
  const double start_time = time_dt();
  vector<KernelShaderEvalInput> inputs(triangles.size() * 3);
  for (size_t t = 0; t < triangles.size(); t++) {
    for (int corner = 0; corner < 3; corner++) {
      KernelShaderEvalInput &in = inputs[t * 3 + corner];
      in.prim = int(triangles[t]);
      in.u = (corner == 1) ? 1.0f : 0.0f;
      in.v = (corner == 2) ? 1.0f : 0.0f;
    }
  }
  const bool evaluated = bake_evaluate(scene, mesh, inputs, colors);
  ANARI_TRACE("bake %s: %d corners in %.1f ms%s",
              mesh->name.c_str(),
              int(inputs.size()),
              (time_dt() - start_time) * 1000.0,
              evaluated ? "" : ", failed");
  return evaluated;
}

bool AnariScene::bake_base_color_atlas(Scene *scene,
                                       const Mesh *mesh,
                                       const vector<uint> &triangles,
                                       const AnariBakeMode mode,
                                       BakedAtlas &atlas)
{
  const size_t num_triangles = triangles.size();
  if (num_triangles == 0) {
    return false;
  }

  /* The cell size from the longest edge of the triangles in the scene, so that small objects
   * get small images. All cells have the same size. */
  const Attribute *attr_P = mesh->attributes.find(ATTR_STD_POSITION);
  Transform tfm = transform_identity();
  if (!mesh->transform_applied) {
    for (const Object *object : scene->objects) {
      if (object->get_geometry() == mesh) {
        tfm = object->get_tfm();
        break;
      }
    }
  }
  float longest_edge = 0.0f;
  if (attr_P) {
    const packed_float3 *P = attr_P->data<packed_float3>();
    for (const uint triangle : triangles) {
      const Mesh::Triangle t = mesh->get_triangle(triangle);
      const float3 p0 = transform_point(&tfm, P[t.v[0]]);
      const float3 p1 = transform_point(&tfm, P[t.v[1]]);
      const float3 p2 = transform_point(&tfm, P[t.v[2]]);
      longest_edge = max(longest_edge, max(len(p1 - p0), max(len(p2 - p0), len(p2 - p1))));
    }
  }
  int cell = min(int(sqrtf(float(BAKE_ATLAS_TEXELS) / float(num_triangles))),
                 BAKE_ATLAS_MAX_CELL);
  if (bake_texel_size_ > 0.0f) {
    cell = min(cell, int(ceilf(longest_edge / bake_texel_size_)) + 2);
  }
  if (cell < BAKE_ATLAS_MIN_CELL) {
    return false;
  }

  const double start_time = time_dt();
  atlas.cell = cell;
  atlas.columns = int(ceilf(sqrtf(float(num_triangles))));
  const int rows = int((num_triangles + atlas.columns - 1) / atlas.columns);
  atlas.width = atlas.columns * cell;
  atlas.height = rows * cell;

  /* The corners of a triangle are one texel inside of its cell, see `export_mesh`. Texels beyond
   * the edges of the triangle get the color of the nearest point of it, so that filtered
   * lookups along the edges don't read anything else. */
  const float scale = 1.0f / float(cell - 2);
  vector<KernelShaderEvalInput> inputs(num_triangles * cell * cell);
  for (size_t t = 0; t < num_triangles; t++) {
    for (int y = 0; y < cell; y++) {
      for (int x = 0; x < cell; x++) {
        float u = (float(x) - 0.5f) * scale;
        float v = (float(y) - 0.5f) * scale;
        u = max(u, 0.0f);
        v = max(v, 0.0f);
        if (u + v > 1.0f) {
          const float sum = u + v;
          u /= sum;
          v /= sum;
        }
        KernelShaderEvalInput &in = inputs[(t * cell + y) * cell + x];
        in.prim = int(triangles[t]);
        in.u = u;
        in.v = v;
      }
    }
  }

  vector<float3> colors;
  if (!bake_evaluate(scene, mesh, inputs, colors)) {
    return false;
  }

  /* 8-bit sRGB for base colors, which are in [0, 1], float for emission, and 8-bit linear
   * for masks, which are numbers. */
  atlas.mode = mode;
  const bool hdr = (mode == ANARI_BAKE_EMISSION);
  const bool srgb = (mode == ANARI_BAKE_BASE_COLOR);
  if (hdr) {
    atlas.float_pixels.assign(size_t(atlas.width) * atlas.height, make_float4(0, 0, 0, 1));
  }
  else {
    atlas.pixels.assign(size_t(atlas.width) * atlas.height * 4, 255);
  }
  for (size_t t = 0; t < num_triangles; t++) {
    const int cell_x = int(t % atlas.columns) * cell;
    const int cell_y = int(t / atlas.columns) * cell;
    for (int y = 0; y < cell; y++) {
      for (int x = 0; x < cell; x++) {
        const float3 color = colors[(t * cell + y) * cell + x];
        if (hdr) {
          atlas.float_pixels[size_t(cell_y + y) * atlas.width + cell_x + x] = make_float4(
              color.x, color.y, color.z, 1.0f);
          continue;
        }
        uchar *pixel = &atlas.pixels[(size_t(cell_y + y) * atlas.width + cell_x + x) * 4];
        for (int channel = 0; channel < 3; channel++) {
          const float value = saturatef(color[channel]);
          pixel[channel] = uchar(
              saturatef(srgb ? color_linear_to_srgb(value) : value) * 255.0f + 0.5f);
        }
      }
    }
  }

  ANARI_TRACE("bake %s: atlas %dx%d, %d triangles with cells of %d texels, in %.1f ms",
              mesh->name.c_str(),
              atlas.width,
              atlas.height,
              int(num_triangles),
              cell,
              (time_dt() - start_time) * 1000.0);
  return true;
}

AnariPrincipled AnariScene::baked_material(AnariMaterialBuilder &builder,
                                           const Shader *shader,
                                           const AnariValue &baked)
{
  Shader *mutable_shader = const_cast<Shader *>(shader);
  const auto mode = baked_modes_.find(shader);
  if (mode != baked_modes_.end() && mode->second == ANARI_BAKE_MASK) {
    AnariPrincipled a;
    AnariPrincipled b;
    if (builder.evaluate_mask_mix(mutable_shader, a, b)) {
      return AnariPrincipled::mix_masked(a, b, baked);
    }
    return builder.evaluate_surface(mutable_shader);
  }

  AnariPrincipled params = builder.evaluate_surface(mutable_shader);
  ((mode != baked_modes_.end() && mode->second == ANARI_BAKE_EMISSION) ? params.emissive :
                                                                         params.base_color) =
      baked;
  return params;
}

ANARIMaterial AnariScene::new_atlas_material(Scene *scene,
                                             const Shader *shader,
                                             const BakedAtlas &atlas)
{
  ANARIDevice d = anari();

  const bool hdr = (atlas.mode == ANARI_BAKE_EMISSION);
  ANARIArray2D array = anariNewArray2D(
      d,
      nullptr,
      nullptr,
      nullptr,
      hdr ? ANARI_FLOAT32_VEC4 :
            (atlas.mode == ANARI_BAKE_MASK) ? ANARI_UFIXED8_VEC4 : ANARI_UFIXED8_RGBA_SRGB,
      atlas.width,
      atlas.height);
  if (hdr) {
    memcpy(anariMapArray(d, array),
           atlas.float_pixels.data(),
           atlas.float_pixels.size() * sizeof(float4));
  }
  else {
    memcpy(anariMapArray(d, array), atlas.pixels.data(), atlas.pixels.size());
  }
  anariUnmapArray(d, array);

  /* The material of the shader, with samplers of the baked image. */
  AnariValue baked;
  baked.type = AnariValue::ARRAY;
  baked.array = array;
  baked.attribute = string_printf("attribute%d", atlas.uv_slot);

  AnariMaterialBuilder builder(device_, scene, slots_, images_);
  const AnariPrincipled params = baked_material(builder, shader, baked);
  const string subtype = material_subtype_for(params);
  ANARIMaterial material = anariNewMaterial(d, subtype.c_str());
  builder.set_material(material, subtype.c_str(), params);

  anariRelease(d, array);
  return material;
}

static void average_surface_emission(Scene *scene,
                                     AnariMaterialBuilder &builder,
                                     const Shader *shader,
                                     AnariPrincipled &params);

void AnariScene::sync_materials(Scene *scene, AnariMaterialBuilder &builder)
{
  ANARIDevice d = anari();
  recreated_materials_.clear();

  const set<const Shader *> surface_shaders = find_surface_shaders(scene);

  set<const Shader *> current;
  for (Shader *shader : scene->shaders) {
    if (surface_shaders.find(shader) == surface_shaders.end()) {
      continue;
    }
    current.insert(shader);

    const ShaderGraph *graph = shader->graph.get();
    auto it = materials_.find(shader);
    const bool is_new = (it == materials_.end());
    const bool graph_changed = !is_new && material_graphs_[shader] != graph;

    if (!(full_update_ || is_new || graph_changed || modified_shaders_.count(shader))) {
      continue;
    }

    /* The shader graph as text when the device takes it, otherwise the flattened material. */
    const AnariGraphSource *source = graph_source_for(shader);
    if (source) {
      const auto rejected = rejected_graphs_.find(shader);
      if (rejected != rejected_graphs_.end() && rejected->second == source->hash) {
        source = nullptr;
      }
    }
    AnariPrincipled params;
    string subtype;
    if (source) {
      subtype = anari_graph_subtype(source->format);
    }
    else {
      if (is_baked(shader)) {
        /* The baked value is in the vertex colors of the meshes, see `bake_base_color`. Meshes
         * of large triangles have their own materials with baked images. */
        AnariValue baked;
        baked.type = AnariValue::ATTRIBUTE;
        baked.attribute = "color";
        params = baked_material(builder, shader, baked);
      }
      else {
        params = builder.evaluate_surface(shader);
        if (is_calibrated(shader)) {
          calibrate_base_color(scene, builder, shader, params);
        }
        average_surface_emission(scene, builder, shader, params);
      }
      if (light_shell_shaders_.count(shader)) {
        /* The globe of a lamp, see #update_light_shells: what the camera sees of it stays the
         * same with half of it there and twice the emission. */
        params.opacity.apply_linear(make_float4(LIGHT_SHELL_OPACITY), zero_float4());
        const float gain = 1.0f / LIGHT_SHELL_OPACITY;
        params.emissive.apply_linear(make_float4(gain, gain, gain, 1.0f), zero_float4());
      }
      subtype = material_subtype_for(params);
    }

    ANARIMaterial material = is_new ? nullptr : it->second;
    if (material && material_subtypes_[shader] != subtype) {
      /* The subtype of an ANARI object can't change, replace the material. */
      anariRelease(d, material);
      material = nullptr;
      recreated_materials_.insert(shader);
    }
    if (material == nullptr) {
      material = anariNewMaterial(d, subtype.c_str());
      graph_states_.erase(shader);
    }
    if (source) {
      set_graph_material(scene, material, shader, *source);

      /* A device which can't evaluate the text says so in the `valid` property of the material
       * (devices without the property leave the value alone). The flattened material is
       * closer to the shader than whatever the device shows instead. The text is remembered,
       * so that it is not tried again until it changes. */
      int32_t valid = 1;
      if (anariGetProperty(d, material, "valid", ANARI_BOOL, &valid, sizeof(valid), ANARI_WAIT) &&
          !valid)
      {
        ANARI_TRACE("graph %s: rejected by the device, flattened", shader->name.c_str());
        rejected_graphs_[shader] = source->hash;
        source = nullptr;
        anariRelease(d, material);
        if (!is_new) {
          recreated_materials_.insert(shader);
        }
        params = builder.evaluate_surface(shader);
        subtype = material_subtype_for(params);
        material = anariNewMaterial(d, subtype.c_str());
      }
    }
    if (source == nullptr) {
      ANARI_TRACE("flattened material %s: %s", shader->name.c_str(), subtype.c_str());
      builder.set_material(material, subtype.c_str(), params);
      if (emission_sampling_ && params.has_emission && subtype == material_subtype_) {
        /* The device samples the emission with lights of its own. */
        anariSetParameter(
            d, material, "emissionSampling", ANARI_STRING, emission_sampling_name(shader));
        anariCommitParameters(d, material);
      }
      graph_states_.erase(shader);
    }

    materials_[shader] = material;
    material_graphs_[shader] = graph;
    material_subtypes_[shader] = subtype;
  }

  for (auto it = materials_.begin(); it != materials_.end();) {
    if (current.find(it->first) == current.end()) {
      anariRelease(d, it->second);
      material_graphs_.erase(it->first);
      material_subtypes_.erase(it->first);
      graph_states_.erase(it->first);
      rejected_graphs_.erase(it->first);
      slots_.forget(it->first);
      exported_slots_versions_.erase(it->first);
      it = materials_.erase(it);
    }
    else {
      ++it;
    }
  }
  for (auto it = graph_sources_.begin(); it != graph_sources_.end();) {
    const bool used = current.find(it->first) != current.end() ||
                      light_world_shaders_.find(it->first) != light_world_shaders_.end();
    it = used ? std::next(it) : graph_sources_.erase(it);
  }
}

/* --------------------------------------------------------------------
 * Geometry.
 */

void AnariScene::release_geometry(GeometryData &data)
{
  ANARIDevice d = anari();
  for (ANARISurface surface : data.surfaces) {
    anariRelease(d, surface);
  }
  data.surfaces.clear();
  data.surface_shaders.clear();
  for (ANARIMaterial material : data.materials) {
    anariRelease(d, material);
  }
  data.materials.clear();
  if (data.group) {
    anariRelease(d, data.group);
    data.group = nullptr;
  }
  if (data.volume_group) {
    anariRelease(d, data.volume_group);
    data.volume_group = nullptr;
  }
  data.volume_transform = transform_identity();
}

void AnariScene::add_surface(GeometryData &data,
                             ANARIGeometry geometry,
                             const Shader *shader,
                             ANARIMaterial material)
{
  ANARIDevice d = anari();

  anariCommitParameters(d, geometry);

  ANARISurface surface = anariNewSurface(d);
  anariSetParameter(d, surface, "geometry", ANARI_GEOMETRY, &geometry);
  if (material) {
    /* A material of this surface alone. */
    data.materials.push_back(material);
  }
  else {
    material = material_for_shader(shader);
  }
  anariSetParameter(d, surface, "material", ANARI_MATERIAL, &material);
  if (shader) {
    /* Material pass index, read back through the object ID frame channel. */
    const uint32_t id = uint32_t(shader->get_pass_id());
    anariSetParameter(d, surface, "id", ANARI_UINT32, &id);
  }
  anariCommitParameters(d, surface);
  anariRelease(d, geometry);

  data.surfaces.push_back(surface);
  data.surface_shaders.push_back(shader);
}

void AnariScene::sync_geometry(Scene *scene, Geometry *geom, GeometryData &data)
{
  release_geometry(data);

  if (geom->is_volume()) {
    /* Volumes are rendered by Cycles as bounding meshes with a volume shader, the ANARI volume
     * is made from the density grid instead. */
    export_volume(scene, static_cast<Volume *>(geom), data);
    return;
  }

  if (geom->is_mesh()) {
    export_mesh(scene, static_cast<Mesh *>(geom), data);
  }
  else if (geom->is_hair()) {
    export_hair(static_cast<Hair *>(geom), data);
  }
  else if (geom->is_pointcloud()) {
    export_pointcloud(static_cast<PointCloud *>(geom), data);
  }

  if (data.surfaces.empty()) {
    return;
  }

  ANARIDevice d = anari();
  data.group = anariNewGroup(d);
  set_array_1d(d, data.group, "surface", ANARI_SURFACE, data.surfaces);
  anariCommitParameters(d, data.group);
}

static const Shader *geometry_shader(const Geometry *geom, const int index)
{
  const array<Node *> &used_shaders = geom->get_used_shaders();
  if (index >= 0 && size_t(index) < used_shaders.size()) {
    return static_cast<const Shader *>(used_shaders[index]);
  }
  return nullptr;
}

static bool shader_has_surface(const Shader *shader)
{
  /* Shaders without surface (volume only) don't need ANARI surfaces. */
  return shader == nullptr || shader->has_surface || !shader->has_volume;
}

void AnariScene::export_mesh(Scene *scene, Mesh *mesh, GeometryData &data)
{
  ANARIDevice d = anari();

  const size_t num_triangles = mesh->num_triangles();
  const Attribute *attr_P = mesh->attributes.find(ATTR_STD_POSITION);
  if (num_triangles == 0 || attr_P == nullptr) {
    return;
  }

  const packed_float3 *P = attr_P->data<packed_float3>();
  const array<int> &triangles = mesh->get_triangles();
  const array<int> &shader_index = mesh->get_shader();
  const array<bool> &smooth = mesh->get_smooth();

  const Attribute *attr_vN = mesh->attributes.find(ATTR_STD_VERTEX_NORMAL);
  const Attribute *attr_cN = mesh->attributes.find(ATTR_STD_CORNER_NORMAL);
  const Attribute *attr_UV = mesh->attributes.find(ATTR_STD_UV);
  const Attribute *attr_tangent = mesh->attributes.find(ATTR_STD_UV_TANGENT);
  const Attribute *attr_tangent_sign = mesh->attributes.find(ATTR_STD_UV_TANGENT_SIGN);

  /* Default color attribute, referenced by materials as ANARI vertex color. */
  const Attribute *attr_color = mesh->attributes.find(ATTR_STD_VERTEX_COLOR);

  bool any_smooth = false;
  bool any_flat = false;
  for (size_t i = 0; i < num_triangles; i++) {
    if (i < smooth.size() && smooth[i]) {
      any_smooth = true;
    }
    else {
      any_flat = true;
    }
  }
  const bool use_normals = attr_cN || (attr_vN && any_smooth);

  /* A mirroring object transform which Cycles applied to the vertices inverts the winding of
   * the triangles: the Cycles kernel flips their geometric normal. Other renderers take the
   * normal from the winding, so restore it. */
  const bool flip_winding = mesh->transform_applied && mesh->transform_negative_scaled;

  /* Group the triangles by shader. */
  map<int, vector<uint>> shader_triangles;
  for (size_t i = 0; i < num_triangles; i++) {
    const int shader = (i < shader_index.size()) ? shader_index[i] : 0;
    shader_triangles[shader].push_back(uint(i));
  }

  /* Each triangle gets its own vertices, which allows per-corner attributes such as UV maps,
   * split normals and flat shaded faces without the faceVarying extension. */
  for (const auto &it : shader_triangles) {
    const Shader *shader = geometry_shader(mesh, it.first);
    /* A device which takes the Cycles graph renders the volume inside the mesh from it. */
    const bool is_graph_volume = shader && graph_format_ == ANARI_GRAPH_CYCLES_XML &&
                                 graph_source_for(shader) != nullptr;
    if (!shader_has_surface(shader) && !is_graph_volume) {
      continue;
    }

    const vector<uint> &tris = it.second;
    const size_t num_verts = tris.size() * 3;

    /* Generic attributes requested by the material, in the slots of its shader. */
    const Attribute *attr_slots[AnariAttributeSlots::max_slots] = {attr_UV};
    int bake_uv_slot = -1;
    if (shader) {
      const AnariAttributeSlots &slots = slots_.get(shader);
      for (int i = 1; i < slots.num_used; i++) {
        attr_slots[i] = mesh->attributes.find(slots.slots[i]);
      }
      bake_uv_slot = slots.find(ustring(BAKE_UV_ATTRIBUTE));
    }

    /* The base color of the shader where a sampler can't express it: baked into an image
     * with a cell per triangle, or into vertex colors when the triangles are small. */
    BakedAtlas atlas;
    vector<float3> baked_colors;
    bool use_atlas = false;
    bool use_baked_colors = false;
    if (shader && is_baked(shader)) {
      use_atlas = bake_uv_slot > 0 &&
                  bake_base_color_atlas(scene, mesh, tris, baked_modes_[shader], atlas);
      use_baked_colors = !use_atlas && bake_base_color(scene, mesh, tris, baked_colors);
    }
    atlas.uv_slot = bake_uv_slot;

    vector<packed_float3> positions(num_verts);
    vector<packed_float3> normals(use_normals ? num_verts : 0);
    vector<float4> tangents((use_normals && attr_tangent) ? num_verts : 0);
    vector<float4> colors((attr_color || use_baked_colors) ? num_verts : 0);
    vector<float4> slot_values[AnariAttributeSlots::max_slots];
    for (int s = 0; s < AnariAttributeSlots::max_slots; s++) {
      if (attr_slots[s]) {
        slot_values[s].resize(num_verts);
      }
    }
    if (use_atlas) {
      slot_values[bake_uv_slot].assign(num_verts, zero_float4());
    }

    for (size_t t = 0; t < tris.size(); t++) {
      const size_t tri = tris[t];
      const int v[3] = {triangles[tri * 3 + 0], triangles[tri * 3 + 1], triangles[tri * 3 + 2]};
      const float3 p0 = P[v[0]];
      const float3 p1 = P[v[1]];
      const float3 p2 = P[v[2]];
      const bool is_smooth = (tri < smooth.size()) ? smooth[tri] : false;
      const float3 winding_normal = safe_normalize(cross(p1 - p0, p2 - p0));
      const float3 face_normal = flip_winding ? -winding_normal : winding_normal;

      for (int out_corner = 0; out_corner < 3; out_corner++) {
        const size_t out = t * 3 + out_corner;
        const int c = (flip_winding && out_corner > 0) ? 3 - out_corner : out_corner;
        positions[out] = P[v[c]];

        if (use_normals) {
          if (attr_cN) {
            normals[out] = attr_cN->data<packed_normal>()[tri * 3 + c].decode();
          }
          else if (is_smooth) {
            normals[out] = attr_vN->data<packed_normal>()[v[c]].decode();
          }
          else {
            normals[out] = face_normal;
          }

          if (!tangents.empty()) {
            const float3 tangent = attr_tangent->data<packed_float3>()[tri * 3 + c];
            const float sign = attr_tangent_sign ? attr_tangent_sign->data<float>()[tri * 3 + c] :
                                                   1.0f;
            tangents[out] = make_float4(tangent.x, tangent.y, tangent.z, sign);
          }
        }

        if (use_baked_colors) {
          const float3 color = baked_colors[t * 3 + c];
          colors[out] = make_float4(color.x, color.y, color.z, 1.0f);
        }
        else if (!colors.empty()) {
          const int64_t index = mesh_attribute_index(*attr_color, mesh, tri, c);
          colors[out] = (index >= 0) ? attribute_value(*attr_color, index) : one_float4();
        }

        for (int s = 0; s < AnariAttributeSlots::max_slots; s++) {
          if (attr_slots[s] && !(use_atlas && s == bake_uv_slot)) {
            const int64_t index = mesh_attribute_index(*attr_slots[s], mesh, tri, c);
            slot_values[s][out] = (index >= 0) ? attribute_value(*attr_slots[s], index) :
                                                 zero_float4();
          }
        }
        if (use_atlas) {
          /* The corners are one texel inside of the cell of the triangle. */
          const float x = float(int(t % atlas.columns) * atlas.cell) +
                          ((c == 1) ? float(atlas.cell - 1) : 1.0f);
          const float y = float(int(t / atlas.columns) * atlas.cell) +
                          ((c == 2) ? float(atlas.cell - 1) : 1.0f);
          slot_values[bake_uv_slot][out] = make_float4(
              x / float(atlas.width), y / float(atlas.height), 0.0f, 1.0f);
        }
      }
    }

    ANARIGeometry geometry = anariNewGeometry(d, "triangle");
    set_array_1d(d, geometry, "vertex.position", ANARI_FLOAT32_VEC3, positions);
    /* Explicit indices, some back-ends require them. */
    {
      vector<uint3> indices(tris.size());
      for (size_t t = 0; t < tris.size(); t++) {
        indices[t] = make_uint3(uint(t * 3), uint(t * 3 + 1), uint(t * 3 + 2));
      }
      set_array_1d(d, geometry, "primitive.index", ANARI_UINT32_VEC3, indices);
    }
    if (use_normals) {
      set_array_1d(d, geometry, "vertex.normal", ANARI_FLOAT32_VEC3, normals);
    }
    if (!tangents.empty()) {
      set_array_1d(d, geometry, "vertex.tangent", ANARI_FLOAT32_VEC4, tangents);
    }
    if (!colors.empty()) {
      set_array_1d(d, geometry, "vertex.color", ANARI_FLOAT32_VEC4, colors);
    }
    for (int s = 0; s < AnariAttributeSlots::max_slots; s++) {
      if (!slot_values[s].empty()) {
        const string name = string_printf("vertex.attribute%d", s);
        set_array_1d(d, geometry, name.c_str(), ANARI_FLOAT32_VEC4, slot_values[s]);
      }
    }

    add_surface(
        data, geometry, shader, use_atlas ? new_atlas_material(scene, shader, atlas) : nullptr);
  }

  (void)any_flat;
}

void AnariScene::export_hair(Hair *hair, GeometryData &data)
{
  ANARIDevice d = anari();

  const Attribute *attr_P = hair->attributes.find(ATTR_STD_POSITION);
  const Attribute *attr_R = hair->attributes.find(ATTR_STD_RADIUS);
  if (attr_P == nullptr || hair->num_curves() == 0) {
    return;
  }
  const packed_float3 *keys = attr_P->data<packed_float3>();
  const float *radius = attr_R ? attr_R->data<float>() : nullptr;
  const array<int> &curve_shader = hair->get_curve_shader();

  map<int, vector<size_t>> shader_curves;
  for (size_t i = 0; i < hair->num_curves(); i++) {
    const int shader = (i < curve_shader.size()) ? curve_shader[i] : 0;
    shader_curves[shader].push_back(i);
  }

  for (const auto &it : shader_curves) {
    const Shader *shader = geometry_shader(hair, it.first);
    if (!shader_has_surface(shader)) {
      continue;
    }

    /* Attributes of the curves which the material reads, in the slots of its shader: the
     * texture coordinates of the surface a hair grows on, and what the Hair Info node returns.
     * One value per key; an attribute of the curve is repeated for its keys. */
    const Attribute *attr_slots[AnariAttributeSlots::max_slots] = {
        hair->attributes.find(ATTR_STD_UV)};
    if (shader) {
      const AnariAttributeSlots &slots = slots_.get(shader);
      for (int i = 1; i < slots.num_used; i++) {
        attr_slots[i] = hair->attributes.find(slots.slots[i]);
      }
    }
    vector<float4> slot_values[AnariAttributeSlots::max_slots];

    vector<packed_float3> positions;
    vector<float> radii;
    vector<uint> segments;
    for (const size_t curve_index : it.second) {
      const Hair::Curve curve = hair->get_curve(curve_index);
      if (curve.num_keys < 2) {
        continue;
      }
      const uint first = uint(positions.size());
      for (int k = 0; k < curve.num_keys; k++) {
        const int key = curve.first_key + k;
        positions.push_back(keys[key]);
        radii.push_back(radius ? radius[key] : 0.01f);
        for (int s = 0; s < AnariAttributeSlots::max_slots; s++) {
          if (attr_slots[s] == nullptr) {
            continue;
          }
          int64_t index = -1;
          switch (attr_slots[s]->element & ~(ATTR_ELEMENT_IS_BYTE | ATTR_ELEMENT_IS_NORMAL)) {
            case ATTR_ELEMENT_CURVE:
              index = int64_t(curve_index);
              break;
            case ATTR_ELEMENT_CURVE_KEY:
              index = key;
              break;
            case ATTR_ELEMENT_MESH:
            case ATTR_ELEMENT_OBJECT:
              index = 0;
              break;
            default:
              break;
          }
          slot_values[s].push_back((index >= 0) ? attribute_value(*attr_slots[s], size_t(index)) :
                                                  zero_float4());
        }
      }
      for (int k = 0; k < curve.num_keys - 1; k++) {
        segments.push_back(first + uint(k));
      }
    }
    if (segments.empty()) {
      continue;
    }

    ANARIGeometry geometry = anariNewGeometry(d, "curve");
    set_array_1d(d, geometry, "vertex.position", ANARI_FLOAT32_VEC3, positions);
    set_array_1d(d, geometry, "vertex.radius", ANARI_FLOAT32, radii);
    set_array_1d(d, geometry, "primitive.index", ANARI_UINT32, segments);
    for (int s = 0; s < AnariAttributeSlots::max_slots; s++) {
      if (slot_values[s].size() == positions.size()) {
        const string name = string_printf("vertex.attribute%d", s);
        set_array_1d(d, geometry, name.c_str(), ANARI_FLOAT32_VEC4, slot_values[s]);
      }
    }
    add_surface(data, geometry, shader);
  }
}

void AnariScene::export_pointcloud(PointCloud *pointcloud, GeometryData &data)
{
  ANARIDevice d = anari();

  const Attribute *attr_P = pointcloud->attributes.find(ATTR_STD_POSITION);
  const Attribute *attr_R = pointcloud->attributes.find(ATTR_STD_RADIUS);
  const size_t num_points = pointcloud->num_points();
  if (attr_P == nullptr || num_points == 0) {
    return;
  }
  const packed_float3 *points = attr_P->data<packed_float3>();
  const float *radius = attr_R ? attr_R->data<float>() : nullptr;
  const array<int> &point_shader = pointcloud->get_shader();

  map<int, vector<size_t>> shader_points;
  for (size_t i = 0; i < num_points; i++) {
    const int shader = (i < point_shader.size()) ? point_shader[i] : 0;
    shader_points[shader].push_back(i);
  }

  for (const auto &it : shader_points) {
    const Shader *shader = geometry_shader(pointcloud, it.first);
    if (!shader_has_surface(shader)) {
      continue;
    }

    vector<packed_float3> positions(it.second.size());
    vector<float> radii(it.second.size());
    for (size_t i = 0; i < it.second.size(); i++) {
      positions[i] = points[it.second[i]];
      radii[i] = radius ? radius[it.second[i]] : 0.01f;
    }

    ANARIGeometry geometry = anariNewGeometry(d, "sphere");
    set_array_1d(d, geometry, "vertex.position", ANARI_FLOAT32_VEC3, positions);
    set_array_1d(d, geometry, "vertex.radius", ANARI_FLOAT32, radii);
    add_surface(data, geometry, shader);
  }
}

/* --------------------------------------------------------------------
 * Instances.
 */

/* Density grid of a volume, resampled to a dense array in voxel index space. */
struct DenseGrid {
  vector<float> values;
  int3 size = make_int3(0, 0, 0);
  int3 origin = make_int3(0, 0, 0);
  int stride = 1;
  float max_value = 0.0f;
};

#  ifdef WITH_NANOVDB
template<typename BuildT>
static bool resample_nanovdb(const void *buffer, const int max_resolution, DenseGrid &dense)
{
  const nanovdb::NanoGrid<BuildT> *grid = reinterpret_cast<const nanovdb::NanoGrid<BuildT> *>(
      buffer);
  /* Cycles builds NanoVDB grids without statistics, so the index bounding box of the grid is
   * not set. Compute it from the leaf nodes, which are stored contiguously. */
  nanovdb::CoordBBox bbox = grid->indexBBox();
  if (bbox.empty()) {
    const auto &tree = grid->tree();
    const uint32_t num_leaves = tree.nodeCount(0);
    const auto *leaves = tree.template getFirstNode<0>();
    for (uint32_t i = 0; leaves && i < num_leaves; i++) {
      const nanovdb::Coord origin = leaves[i].origin();
      bbox.expand(origin);
      bbox.expand(origin + nanovdb::Coord(leaves[i].dim() - 1));
    }
  }
  ANARI_TRACE("nanovdb grid: valid %d, type %d, bbox %d %d %d - %d %d %d, active voxels %llu",
              int(grid->isValid()),
              int(grid->gridType()),
              bbox.min()[0],
              bbox.min()[1],
              bbox.min()[2],
              bbox.max()[0],
              bbox.max()[1],
              bbox.max()[2],
              (unsigned long long)grid->activeVoxelCount());
  if (bbox.empty()) {
    return false;
  }
  const nanovdb::Coord min = bbox.min();
  const nanovdb::Coord dim = bbox.dim();
  const int max_dim = max(int(dim[0]), max(int(dim[1]), int(dim[2])));
  dense.stride = max(1, int(divide_up(max_dim, max_resolution)));
  dense.origin = make_int3(min[0], min[1], min[2]);
  dense.size = make_int3(divide_up(dim[0], dense.stride),
                         divide_up(dim[1], dense.stride),
                         divide_up(dim[2], dense.stride));
  dense.values.resize(size_t(dense.size.x) * dense.size.y * dense.size.z);

  auto accessor = grid->getAccessor();
  size_t i = 0;
  for (int z = 0; z < dense.size.z; z++) {
    for (int y = 0; y < dense.size.y; y++) {
      for (int x = 0; x < dense.size.x; x++, i++) {
        const nanovdb::Coord ijk(min[0] + x * dense.stride,
                                 min[1] + y * dense.stride,
                                 min[2] + z * dense.stride);
        const float value = float(accessor.getValue(ijk));
        dense.values[i] = value;
        dense.max_value = max(dense.max_value, value);
      }
    }
  }
  return true;
}
#  endif

void AnariScene::export_volume(Scene *scene, Volume *volume, GeometryData &data)
{
  ANARIDevice d = anari();

  const Shader *shader = geometry_shader(volume, 0);
  AnariMaterialBuilder builder(device_, scene, slots_, images_);
  const AnariMaterialBuilder::Volume params = builder.evaluate_volume(
      const_cast<Shader *>(shader));
  ANARI_TRACE("volume %s: shader valid %d, density %f, attribute %s",
              volume->name.c_str(),
              int(params.valid),
              params.density,
              params.density_attribute.c_str());
  if (!params.valid) {
    return;
  }

  /* Density grid. */
  const Attribute *attr = volume->attributes.find(params.density_attribute);
  if (attr == nullptr || !(attr->element & ATTR_ELEMENT_VOXEL)) {
    attr = volume->attributes.find(ATTR_STD_VOLUME_DENSITY);
  }
  if (attr == nullptr || !(attr->element & ATTR_ELEMENT_VOXEL)) {
    LOG_DEBUG << "ANARI: volume " << volume->name << " has no density grid";
    return;
  }
  const int kernel_id = attr->data_voxel().kernel_id();
  if (kernel_id < 0 || size_t(kernel_id) >= scene->dscene.image_textures.size()) {
    return;
  }
  const KernelImageTexture &texture = scene->dscene.image_textures[kernel_id];
  const AnariImage *image = device_.find_image(texture.image_info_id);
  ANARI_TRACE("volume grid: kernel id %d, image %p, type %d",
              kernel_id,
              (const void *)image,
              image ? int(image->data_type) : -1);
  if (image == nullptr || image->pixels == nullptr) {
    return;
  }

  /* Dense resampling, limited to a resolution all devices can handle. */
  DenseGrid dense;
  bool resampled = false;
#  ifdef WITH_NANOVDB
  const int max_resolution = 256;
  switch (image->data_type) {
    case IMAGE_DATA_TYPE_NANOVDB_FLOAT:
      resampled = resample_nanovdb<float>(image->pixels, max_resolution, dense);
      break;
    case IMAGE_DATA_TYPE_NANOVDB_FP16:
      resampled = resample_nanovdb<nanovdb::Fp16>(image->pixels, max_resolution, dense);
      break;
    case IMAGE_DATA_TYPE_NANOVDB_FPN:
      resampled = resample_nanovdb<nanovdb::FpN>(image->pixels, max_resolution, dense);
      break;
    default:
      break;
  }
#  endif
  ANARI_TRACE("volume grid resampled %d: %dx%dx%d stride %d max %f",
              int(resampled),
              dense.size.x,
              dense.size.y,
              dense.size.z,
              dense.stride,
              dense.max_value);
  if (!resampled || !(dense.max_value > 0.0f)) {
    LOG_DEBUG << "ANARI: unsupported or empty density grid in volume " << volume->name;
    return;
  }

  ANARISpatialField field = anariNewSpatialField(d, "structuredRegular");
  ANARIArray3D array = anariNewArray3D(
      d, nullptr, nullptr, nullptr, ANARI_FLOAT32, dense.size.x, dense.size.y, dense.size.z);
  memcpy(anariMapArray(d, array), dense.values.data(), dense.values.size() * sizeof(float));
  anariUnmapArray(d, array);
  anariSetParameter(d, field, "data", ANARI_ARRAY3D, &array);
  anariRelease(d, array);
  /* Index space of the dense grid to object space. A scale and translation (the common case) is
   * expressed by the origin and spacing of the field, anything else by the transform of the
   * instance. */
  const Transform index_to_object =
      (texture.use_transform_3d ? transform_inverse(texture.transform_3d) :
                                  transform_identity()) *
      transform_translate(
          make_float3(float(dense.origin.x), float(dense.origin.y), float(dense.origin.z)));
  const bool axis_aligned = index_to_object.x.y == 0.0f && index_to_object.x.z == 0.0f &&
                            index_to_object.y.x == 0.0f && index_to_object.y.z == 0.0f &&
                            index_to_object.z.x == 0.0f && index_to_object.z.y == 0.0f &&
                            index_to_object.x.x > 0.0f && index_to_object.y.y > 0.0f &&
                            index_to_object.z.z > 0.0f;
  float3 origin = zero_float3();
  float3 spacing = make_float3(float(dense.stride));
  if (axis_aligned) {
    origin = transform_get_column(&index_to_object, 3);
    spacing *= make_float3(index_to_object.x.x, index_to_object.y.y, index_to_object.z.z);
    data.volume_transform = transform_identity();
  }
  else {
    data.volume_transform = index_to_object;
  }
  anariSetParameter(d, field, "origin", ANARI_FLOAT32_VEC3, &origin);
  anariSetParameter(d, field, "spacing", ANARI_FLOAT32_VEC3, &spacing);
  anariSetParameter(d, field, "filter", ANARI_STRING, "linear");
  anariCommitParameters(d, field);

  /* Volume shading. Cycles-ANARI has the Principled Volume of Cycles itself, other devices get
   * a transfer function: a constant albedo and an extinction proportional to the density. */
  ANARIVolume anari_volume = nullptr;
  if (device_.has_subtype(ANARI_VOLUME, "principled")) {
    anari_volume = anariNewVolume(d, "principled");
    anariSetParameter(d, anari_volume, "value", ANARI_SPATIAL_FIELD, &field);
    anariSetParameter(d, anari_volume, "densityScale", ANARI_FLOAT32, &params.density);
    anariSetParameter(d, anari_volume, "color", ANARI_FLOAT32_VEC3, &params.color);
    anariSetParameter(d, anari_volume, "anisotropy", ANARI_FLOAT32, &params.anisotropy);
    anariSetParameter(
        d, anari_volume, "absorptionColor", ANARI_FLOAT32_VEC3, &params.absorption_color);
    anariSetParameter(
        d, anari_volume, "emissionStrength", ANARI_FLOAT32, &params.emission_strength);
    anariSetParameter(
        d, anari_volume, "emissionColor", ANARI_FLOAT32_VEC3, &params.emission_color);
  }
  else if (device_.has_subtype(ANARI_VOLUME, "transferFunction1D")) {
    anari_volume = anariNewVolume(d, "transferFunction1D");
    anariSetParameter(d, anari_volume, "value", ANARI_SPATIAL_FIELD, &field);
    const float value_range[2] = {0.0f, dense.max_value};
    anariSetParameter(d, anari_volume, "valueRange", ANARI_FLOAT32_BOX1, value_range);
    const vector<packed_float3> colors = {params.color, params.color};
    set_array_1d(d, anari_volume, "color", ANARI_FLOAT32_VEC3, colors);
    /* The extinction has to be proportional to the density. The ANARI specification defines
     * the extinction as -ln(1 - opacity) / unitDistance, some devices use the linear
     * opacity / unitDistance instead. A small opacity range makes both agree (within half of
     * the maximum opacity), so the ramp only reaches `max_opacity` at the maximum value. */
    const float max_opacity = 0.01f;
    const vector<float> opacities = {0.0f, max_opacity};
    set_array_1d(d, anari_volume, "opacity", ANARI_FLOAT32, opacities);
    const float extinction = dense.max_value * params.density;
    const float unit_distance = extinction > 0.0f ? max_opacity / extinction : 1.0f;
    anariSetParameter(d, anari_volume, "unitDistance", ANARI_FLOAT32, &unit_distance);
  }
  anariRelease(d, field);
  ANARI_TRACE("volume subtype: principled %d, transferFunction1D %d",
              int(device_.has_subtype(ANARI_VOLUME, "principled")),
              int(device_.has_subtype(ANARI_VOLUME, "transferFunction1D")));
  if (anari_volume == nullptr) {
    LOG_DEBUG << "ANARI: device has no supported volume subtype";
    return;
  }
  if (shader) {
    const uint32_t id = uint32_t(shader->get_pass_id());
    anariSetParameter(d, anari_volume, "id", ANARI_UINT32, &id);
  }
  anariCommitParameters(d, anari_volume);

  data.volume_group = anariNewGroup(d);
  const vector<ANARIVolume> volumes = {anari_volume};
  set_array_1d(d, data.volume_group, "volume", ANARI_VOLUME, volumes);
  anariCommitParameters(d, data.volume_group);
  anariRelease(d, anari_volume);
}

/* An object which casts no shadows is left out on devices without ray visibility, as it would
 * shadow the scene. That is wrong for the globe of a lamp: it is what one sees of the lamp. With
 * the light inside it has to let the light out as well. It stays with half of its opacity,
 * which lets half of the light through, and the light is made twice as strong: every path
 * from the light crosses the globe once, so the scene gets the light it gets in Cycles, and
 * the camera sees the globe with the emission it has there (twice the emission on half of
 * the samples). Only for small objects, and for shaders which nothing else uses. Returns
 * whether anything changed. */
bool AnariScene::update_light_shells(Scene *scene)
{
  set<const Object *> shell_objects;
  set<const Shader *> shell_shaders;
  set<const Object *> shell_lights;

  if (!instance_visibility_) {
    vector<const Object *> lights;
    BoundBox scene_bounds = BoundBox::empty;
    for (const Object *object : scene->objects) {
      const Geometry *geom = object->get_geometry();
      if (geom && geom->is_light()) {
        lights.push_back(object);
      }
      else if (geom && object->bounds.valid()) {
        scene_bounds.grow(object->bounds);
      }
    }
    const float scene_size = scene_bounds.valid() ? reduce_max(scene_bounds.size()) : 0.0f;

    /* The candidates with their lights, and the shaders which other objects use. */
    map<const Object *, vector<const Object *>> candidates;
    set<const Shader *> other_shaders;
    for (const Object *object : scene->objects) {
      const Geometry *geom = object->get_geometry();
      if (geom == nullptr || geom->is_light()) {
        continue;
      }
      vector<const Object *> inside;
      const uint hidden = ~object->get_visibility() &
                          (PATH_RAY_VISIBILITY_CAMERA | PATH_RAY_VISIBILITY_SHADOW);
      if (hidden == PATH_RAY_VISIBILITY_SHADOW && geom->is_mesh() && object->bounds.valid() &&
          reduce_max(object->bounds.size()) < 0.12f * scene_size)
      {
        const BoundBox &bounds = object->bounds;
        for (const Object *light : lights) {
          const float3 p = transform_get_column(&light->get_tfm(), 3);
          if (p.x >= bounds.min.x && p.x <= bounds.max.x && p.y >= bounds.min.y &&
              p.y <= bounds.max.y && p.z >= bounds.min.z && p.z <= bounds.max.z)
          {
            inside.push_back(light);
          }
        }
      }
      if (!inside.empty()) {
        candidates[object] = inside;
      }
      else {
        for (const Node *node : geom->get_used_shaders()) {
          other_shaders.insert(static_cast<const Shader *>(node));
        }
      }
    }

    for (const auto &candidate : candidates) {
      bool shared = false;
      for (const Node *node : candidate.first->get_geometry()->get_used_shaders()) {
        shared |= other_shaders.count(static_cast<const Shader *>(node)) != 0;
      }
      if (shared) {
        continue;
      }
      shell_objects.insert(candidate.first);
      for (const Node *node : candidate.first->get_geometry()->get_used_shaders()) {
        shell_shaders.insert(static_cast<const Shader *>(node));
      }
      shell_lights.insert(candidate.second.begin(), candidate.second.end());
      ANARI_TRACE("object %s: casts no shadows around %d lights, kept half transparent",
                  candidate.first->name.c_str(),
                  int(candidate.second.size()));
    }
  }

  /* Materials of shaders which became or stopped being the one of a globe. */
  for (const Shader *shader : shell_shaders) {
    if (light_shell_shaders_.count(shader) == 0) {
      modified_shaders_.insert(shader);
    }
  }
  for (const Shader *shader : light_shell_shaders_) {
    if (shell_shaders.count(shader) == 0) {
      modified_shaders_.insert(shader);
    }
  }

  const bool changed = shell_objects != light_shell_objects_ ||
                       shell_shaders != light_shell_shaders_ ||
                       shell_lights != light_shell_lights_;
  light_shell_objects_ = std::move(shell_objects);
  light_shell_shaders_ = std::move(shell_shaders);
  light_shell_lights_ = std::move(shell_lights);
  return changed;
}

void AnariScene::sync_instances(Scene *scene)
{
  ANARIDevice d = anari();

  for (ANARIInstance instance : instances_) {
    anariRelease(d, instance);
  }
  instances_.clear();

  const bool instance_visibility = instance_visibility_;

  /* For objects which cast no shadows on devices without ray visibility, see below: where the
   * lights are, and how large the scene is. */
  vector<float3> light_positions;
  BoundBox scene_bounds = BoundBox::empty;
  if (!instance_visibility) {
    for (const Object *object : scene->objects) {
      const Geometry *geom = object->get_geometry();
      if (geom == nullptr) {
        continue;
      }
      if (geom->is_light() || light_cards_.count(object)) {
        light_positions.push_back(geom->is_light() ? transform_get_column(&object->get_tfm(), 3) :
                                                     object->bounds.center());
      }
      else if (object->bounds.valid()) {
        scene_bounds.grow(object->bounds);
      }
    }
  }
  const float scene_size = scene_bounds.valid() ? reduce_max(scene_bounds.size()) : 0.0f;

  for (const Object *object : scene->objects) {
    const Geometry *geom = object->get_geometry();
    if (geom == nullptr || geom->is_light()) {
      continue;
    }
    auto it = geometries_.find(geom);
    if (it == geometries_.end()) {
      continue;
    }
    if ((object->get_visibility() & PATH_RAY_VISIBILITY_ALL) == 0) {
      continue;
    }

    /* Devices without ray visibility have an object in all rays or in none. An object which is
     * hidden from the camera would show up in the image, and one which casts no shadow would
     * shadow the scene: windows and blinds are set up like that to let the light in. Leaving
     * such objects out is the smaller error, unless they light the scene. Small emissive ones
     * were exported as lights. */
    if (!instance_visibility) {
      const uint hidden = ~object->get_visibility() &
                          (PATH_RAY_VISIBILITY_CAMERA | PATH_RAY_VISIBILITY_SHADOW);
      if (hidden != 0) {
        if (light_cards_.count(object)) {
          continue;
        }
        if (hidden & PATH_RAY_VISIBILITY_SHADOW) {
          /* Also when it emits: the light it keeps out is usually more than its own. Unless
           * it is small and has no light inside, like the globe of a lamp which glows by
           * itself: the shadow it casts then is the one of any small object, and without it
           * the lamp has neither its globe nor its light. */
          /* An object which emits may be larger: it is a lamp, not a window pane. */
          bool emissive = false;
          for (const Node *node : geom->get_used_shaders()) {
            const Shader *shader = static_cast<const Shader *>(node);
            emissive |= (shader && shader->emission_sampling != EMISSION_SAMPLING_NONE);
          }
          const float size = object->bounds.valid() ? reduce_max(object->bounds.size()) : 0.0f;
          bool keep = (hidden & PATH_RAY_VISIBILITY_CAMERA) == 0 && object->bounds.valid() &&
                      size < (emissive ? 0.12f : 0.04f) * scene_size;
          const bool is_light_shell = light_shell_objects_.count(object) != 0;
          for (size_t i = 0; keep && !is_light_shell && i < light_positions.size(); i++) {
            const float3 p = light_positions[i];
            const BoundBox &bounds = object->bounds;
            keep = !(p.x >= bounds.min.x && p.x <= bounds.max.x && p.y >= bounds.min.y &&
                     p.y <= bounds.max.y && p.z >= bounds.min.z && p.z <= bounds.max.z);
          }
          if (!keep) {
            ANARI_TRACE("object %s: casts no shadows in Cycles, left out (size %g of a scene of "
                        "%g%s)",
                        object->name.c_str(),
                        double(size),
                        double(scene_size),
                        emissive ? ", emits" : "");
            continue;
          }
          ANARI_TRACE("object %s: casts no shadows in Cycles, kept as it is small and has no "
                      "light inside",
                      object->name.c_str());
        }
        else {
          bool emissive = false;
          for (const Node *node : geom->get_used_shaders()) {
            const Shader *shader = static_cast<const Shader *>(node);
            emissive |= (shader && shader->emission_sampling != EMISSION_SAMPLING_NONE);
          }
          if (!emissive) {
            continue;
          }
        }
      }
    }

    if (it->second.volume_group) {
      /* Volume grids are looked up in object space, also when Cycles applied the object
       * transform to the bounding mesh. */
      ANARI_TRACE("volume instance of object %s", object->name.c_str());
      ANARIInstance instance = anariNewInstance(d, "transform");
      anariSetParameter(d, instance, "group", ANARI_GROUP, &it->second.volume_group);
      const Transform tfm = object->get_tfm() * it->second.volume_transform;
      float m[16];
      transform_to_mat4(tfm, m);
      anariSetParameter(d, instance, "transform", ANARI_FLOAT32_MAT4, m);
      const uint32_t id = uint32_t(object->get_pass_id());
      anariSetParameter(d, instance, "id", ANARI_UINT32, &id);
      anariCommitParameters(d, instance);
      instances_.push_back(instance);
    }
    if (it->second.group == nullptr) {
      continue;
    }

    ANARIInstance instance = anariNewInstance(d, "transform");
    anariSetParameter(d, instance, "group", ANARI_GROUP, &it->second.group);

    /* Transforms of single user meshes are applied to the vertices by Cycles. */
    const Transform tfm = geom->transform_applied ? transform_identity() : object->get_tfm();
    float m[16];
    transform_to_mat4(tfm, m);
    anariSetParameter(d, instance, "transform", ANARI_FLOAT32_MAT4, m);

    /* Object pass index, read back through the instance ID frame channel. */
    const uint32_t id = uint32_t(object->get_pass_id());
    anariSetParameter(d, instance, "id", ANARI_UINT32, &id);

    /* The outputs of the Object Info node, for devices which evaluate shader graphs: the color
     * of the object as the uniform `color` attribute of the instance, and the kernel's random
     * number of the object. The other devices have no use for them, and `color` is also the
     * attribute which carries baked base colors on the geometry. */
    if (instance_object_random_) {
      const float3 color = object->get_color();
      const float4 color4 = make_float4(color.x, color.y, color.z, object->get_alpha());
      anariSetParameter(d, instance, "color", ANARI_FLOAT32_VEC4, &color4);
      const float random = float(object->get_random_id()) * (1.0f / float(0xFFFFFFFF));
      anariSetParameter(d, instance, "objectRandom", ANARI_FLOAT32, &random);
    }

    /* Ray visibility of the object, for devices which have it: scenes light rooms through
     * emissive planes which are invisible to camera and shadow rays. */
    if (instance_visibility) {
      const uint visibility = object->get_visibility();
      const std::pair<const char *, uint> flags[] = {
          {"visibility.camera", PATH_RAY_VISIBILITY_CAMERA},
          {"visibility.diffuse", PATH_RAY_VISIBILITY_DIFFUSE},
          {"visibility.glossy", PATH_RAY_VISIBILITY_GLOSSY},
          {"visibility.transmission", PATH_RAY_VISIBILITY_TRANSMIT},
          {"visibility.volumeScatter", PATH_RAY_VISIBILITY_VOLUME_SCATTER},
          {"visibility.shadow", PATH_RAY_VISIBILITY_SHADOW},
      };
      for (const auto &flag : flags) {
        const bool visible = (visibility & flag.second) != 0;
        anariSetParameter(d, instance, flag.first, ANARI_BOOL, &visible);
      }
    }

    anariCommitParameters(d, instance);
    instances_.push_back(instance);
  }
}

void AnariScene::commit_world()
{
  ANARIDevice d = anari();

  vector<ANARIInstance> instances = instances_;
  instances.insert(instances.end(), light_instances_.begin(), light_instances_.end());
  if (instances.empty()) {
    anariUnsetParameter(d, world_, "instance");
  }
  else {
    set_array_1d(d, world_, "instance", ANARI_INSTANCE, instances);
  }

  vector<ANARILight> lights = lights_;
  if (background_light_) {
    lights.push_back(background_light_);
  }
  if (background_sun_light_) {
    lights.push_back(background_sun_light_);
  }
  if (lights.empty()) {
    anariUnsetParameter(d, world_, "light");
  }
  else {
    set_array_1d(d, world_, "light", ANARI_LIGHT, lights);
  }

  anariCommitParameters(d, world_);
}

/* --------------------------------------------------------------------
 * Lights.
 */

void AnariScene::release_lights()
{
  ANARIDevice d = anari();
  for (ANARILight light : lights_) {
    anariRelease(d, light);
  }
  lights_.clear();
  for (ANARIInstance instance : light_instances_) {
    anariRelease(d, instance);
  }
  light_instances_.clear();
}

static void set_light_color(ANARIDevice d, ANARILight light, const float3 value, const char *name)
{
  /* Split the colored strength into a normalized color and a scalar intensity. */
  const float scale = max(max(value.x, value.y), value.z);
  const float3 color = (scale > 0.0f) ? value / scale : one_float3();
  anariSetParameter(d, light, "color", ANARI_FLOAT32_VEC3, &color);
  anariSetParameter(d, light, name, ANARI_FLOAT32, &scale);
}

void AnariScene::add_emissive_quad(const float3 corner,
                                   const float3 edge1,
                                   const float3 edge2,
                                   const float3 radiance)
{
  ANARIDevice d = anari();

  const vector<packed_float3> positions = {
      corner, corner + edge1, corner + edge1 + edge2, corner + edge2};
  const vector<uint3> indices = {make_uint3(0, 1, 2), make_uint3(0, 2, 3)};

  ANARIGeometry geometry = anariNewGeometry(d, "triangle");
  set_array_1d(d, geometry, "vertex.position", ANARI_FLOAT32_VEC3, positions);
  set_array_1d(d, geometry, "primitive.index", ANARI_UINT32_VEC3, indices);
  anariCommitParameters(d, geometry);

  ANARIMaterial material = anariNewMaterial(d, material_subtype_.c_str());
  AnariPrincipled params;
  params.has_bsdf = true;
  params.has_emission = true;
  params.base_color = AnariValue::from_float3(zero_float3());
  params.specular = AnariValue::from_float(0.0f);
  params.emissive = AnariValue::from_float3(radiance);
  AnariMaterialBuilder builder(device_, nullptr, slots_, images_);
  builder.set_material(material, material_subtype_.c_str(), params);

  ANARISurface surface = anariNewSurface(d);
  anariSetParameter(d, surface, "geometry", ANARI_GEOMETRY, &geometry);
  anariSetParameter(d, surface, "material", ANARI_MATERIAL, &material);
  anariCommitParameters(d, surface);

  ANARIGroup group = anariNewGroup(d);
  const vector<ANARISurface> surfaces = {surface};
  set_array_1d(d, group, "surface", ANARI_SURFACE, surfaces);
  anariCommitParameters(d, group);

  ANARIInstance instance = anariNewInstance(d, "transform");
  anariSetParameter(d, instance, "group", ANARI_GROUP, &group);
  anariCommitParameters(d, instance);

  anariRelease(d, group);
  anariRelease(d, surface);
  anariRelease(d, material);
  anariRelease(d, geometry);

  light_instances_.push_back(instance);
}

/* Soft falloff of point and spot lights with a radius (Cycles vendor extension). */
void AnariScene::set_light_soft_falloff(ANARILight anari_light, const PointLight *light)
{
  if (!device_.has_extension("ANARI_CYCLES_LIGHT_SOFT_FALLOFF")) {
    return;
  }
  const bool soft_falloff = !light->get_is_sphere();
  anariSetParameter(anari(), anari_light, "softFalloff", ANARI_BOOL, &soft_falloff);
}

void AnariScene::add_light(Scene *scene,
                           const Object *object,
                           const Light *light,
                           const float3 strength,
                           const AnariGraphSource *source,
                           const vector<float> &falloff_distances,
                           const vector<packed_float3> &falloff_colors)
{
  ANARIDevice d = anari();
  const Transform &tfm = object->get_tfm();
  const float3 position = transform_get_translation(&tfm);
  const float3 direction = safe_normalize(-transform_get_column(&tfm, 2));

  ANARILight anari_light = nullptr;

  switch (light->get_light_type()) {
    case LIGHT_POINT: {
      const PointLight *point = static_cast<const PointLight *>(light);
      /* Cycles evaluates point lights as `strength / (4 * pi)` radiant intensity. */
      anari_light = anariNewLight(d, "point");
      anariSetParameter(d, anari_light, "position", ANARI_FLOAT32_VEC3, &position);
      const float radius = point->get_radius();
      anariSetParameter(d, anari_light, "radius", ANARI_FLOAT32, &radius);
      set_light_soft_falloff(anari_light, point);
      /* Unnormalized lights emit `strength / pi` radiance from the sphere surface. */
      const float3 intensity = light->get_normalize() ? strength * (M_1_PI_F * 0.25f) :
                               (radius > 0.0f)        ? strength * sqr(radius) :
                                                        strength * M_1_PI_F;
      set_light_color(d, anari_light, intensity, "intensity");
      break;
    }
    case LIGHT_SPOT: {
      const SpotLight *spot = static_cast<const SpotLight *>(light);
      const float half_angle = spot->get_angle() * 0.5f;
      const float cos_half = cosf(half_angle);
      const float cos_inner = cos_half + (1.0f - cos_half) * spot->get_smooth();
      const float inner_angle = acosf(min(cos_inner, 1.0f));

      const bool supported = device_.has_subtype(ANARI_LIGHT, "spot");
      anari_light = anariNewLight(d, supported ? "spot" : "point");
      anariSetParameter(d, anari_light, "position", ANARI_FLOAT32_VEC3, &position);
      const float radius = spot->get_radius();
      anariSetParameter(d, anari_light, "radius", ANARI_FLOAT32, &radius);
      set_light_soft_falloff(anari_light, spot);
      if (supported) {
        anariSetParameter(d, anari_light, "direction", ANARI_FLOAT32_VEC3, &direction);
        const float opening_angle = spot->get_angle();
        const float falloff_angle = max(half_angle - inner_angle, 0.0f);
        anariSetParameter(d, anari_light, "openingAngle", ANARI_FLOAT32, &opening_angle);
        anariSetParameter(d, anari_light, "falloffAngle", ANARI_FLOAT32, &falloff_angle);
      }
      const float3 intensity = strength * (M_1_PI_F * 0.25f);
      set_light_color(d, anari_light, intensity, "intensity");
      break;
    }
    case LIGHT_SUN: {
      const SunLight *sun = static_cast<const SunLight *>(light);
      anari_light = anariNewLight(d, "directional");
      anariSetParameter(d, anari_light, "direction", ANARI_FLOAT32_VEC3, &direction);
      const float angle = sun->get_angle();
      anariSetParameter(d, anari_light, "angularDiameter", ANARI_FLOAT32, &angle);
      /* Sun strength is irradiance in W/m^2. */
      set_light_color(d, anari_light, strength, "irradiance");
      break;
    }
    case LIGHT_AREA: {
      const AreaLight *area = static_cast<const AreaLight *>(light);
      if (area->get_is_portal()) {
        break;
      }
      float3 edge_u = transform_get_column(&tfm, 0) * area->get_sizeu();
      float3 edge_v = transform_get_column(&tfm, 1) * area->get_sizev();
      if (area->get_ellipse()) {
        /* Approximate ellipses by a rectangle of the same area. */
        const float scale = sqrtf(M_PI_4_F);
        edge_u *= scale;
        edge_v *= scale;
      }
      const float surface_area = len(edge_u) * len(edge_v);
      if (surface_area == 0.0f) {
        break;
      }
      /* Lambertian emitter: radiance = power / (pi * area). */
      const float3 radiance = light->get_normalize() ? strength * M_1_PI_F / surface_area :
                                                       strength * M_1_PI_F;
      /* Emitting towards -Z of the light object: corner + edges with normal cross(e1, e2). */
      const float3 corner = position - 0.5f * edge_u - 0.5f * edge_v;

      if (device_.has_subtype(ANARI_LIGHT, "quad")) {
        anari_light = anariNewLight(d, "quad");
        anariSetParameter(d, anari_light, "position", ANARI_FLOAT32_VEC3, &corner);
        anariSetParameter(d, anari_light, "edge1", ANARI_FLOAT32_VEC3, &edge_v);
        anariSetParameter(d, anari_light, "edge2", ANARI_FLOAT32_VEC3, &edge_u);
        anariSetParameter(d, anari_light, "side", ANARI_STRING, "front");
        set_light_color(d, anari_light, radiance, "radiance");
      }
      else {
        /* Fall back to emissive geometry. */
        add_emissive_quad(corner, edge_v, edge_u, radiance);
      }
      break;
    }
    case LIGHT_BACKGROUND:
    case LIGHT_TRIANGLE:
    default:
      break;
  }

  if (anari_light) {
    /* Lights are hidden from the camera unless the object is visible to camera rays. */
    const bool visible = (object->get_visibility() & PATH_RAY_VISIBILITY_CAMERA) != 0;
    anariSetParameter(d, anari_light, "visible", ANARI_BOOL, &visible);
    /* Which kinds of rays the light contributes to, for devices which have it: a light which
     * glossy rays don't see lights the room without showing up in the mirrors. */
    if (light_visibility_) {
      const uint visibility = object->get_visibility();
      const std::pair<const char *, uint> flags[] = {
          {"visibility.diffuse", PATH_RAY_VISIBILITY_DIFFUSE},
          {"visibility.glossy", PATH_RAY_VISIBILITY_GLOSSY},
          {"visibility.transmission", PATH_RAY_VISIBILITY_TRANSMIT},
          {"visibility.volumeScatter", PATH_RAY_VISIBILITY_VOLUME_SCATTER},
      };
      for (const auto &flag : flags) {
        const bool contributes = (visibility & flag.second) != 0;
        anariSetParameter(d, anari_light, flag.first, ANARI_BOOL, &contributes);
      }
    }
    /* Up to which bounce the light contributes: fill lights which only light what the camera
     * sees directly have 0. */
    if (light_max_bounce_) {
      const int max_bounces = light->get_max_bounces();
      anariSetParameter(d, anari_light, "maxBounce", ANARI_INT32, &max_bounces);
    }
    if (light_cast_shadow_) {
      const bool cast_shadow = light->get_cast_shadow();
      anariSetParameter(d, anari_light, "castShadow", ANARI_BOOL, &cast_shadow);
    }
    if (source) {
      set_graph_source(scene, anari_light, "source", *source);
    }
    if (!falloff_distances.empty()) {
      set_array_1d(d, anari_light, "falloff.distance", ANARI_FLOAT32, falloff_distances);
      set_array_1d(d, anari_light, "falloff.color", ANARI_FLOAT32_VEC3, falloff_colors);
    }
    anariCommitParameters(d, anari_light);
    lights_.push_back(anari_light);
  }
}

/* Whether the shader of a light changes the falloff of the light with the distance. */
static bool light_shader_has_falloff(const Shader *shader)
{
  if (shader->graph == nullptr) {
    return false;
  }
  for (const ShaderNode *node : shader->graph->nodes) {
    if (node->type == LightFalloffNode::get_node_type()) {
      return true;
    }
    if (node->type == LightPathNode::get_node_type()) {
      for (const ShaderOutput *output : node->outputs) {
        if (output->name() == "Ray Length" && !output->links.empty()) {
          return true;
        }
      }
    }
  }
  return false;
}

/* Distances at which an emitter illuminates the scene: a few of the distances from its position
 * to the objects, evenly spread over all of them. For a one sided emitter over the objects in
 * front of it, and only those further away than the emitter is large. */
static vector<float> scene_point_distances(const Scene *scene,
                                           const float3 position,
                                           const float3 direction,
                                           const bool one_sided,
                                           const float min_distance)
{
  /* Points on the surfaces: some of the vertices of every mesh, or the center of the bounds of
   * other geometry. */
  vector<float> distances;
  auto add_point = [&](const float3 point) {
    const float3 to_point = point - position;
    const float distance = len(to_point);
    if (distance >= min_distance && (!one_sided || dot(to_point, direction) > 0.0f)) {
      distances.push_back(distance);
    }
  };
  const size_t max_points_per_object = 256;
  for (const Object *object : scene->objects) {
    const Geometry *geom = object->get_geometry();
    if (geom == nullptr || geom->is_light() || !object->bounds.valid()) {
      continue;
    }
    const Attribute *attr_P = geom->is_mesh() ? geom->attributes.find(ATTR_STD_POSITION) :
                                                nullptr;
    const size_t num_verts = geom->is_mesh() ? static_cast<const Mesh *>(geom)->num_verts() : 0;
    if (attr_P == nullptr || num_verts == 0) {
      add_point(object->bounds.center());
      continue;
    }
    const Transform tfm = geom->transform_applied ? transform_identity() : object->get_tfm();
    const packed_float3 *P = attr_P->data<packed_float3>();
    const size_t step = max(num_verts / max_points_per_object, size_t(1));
    for (size_t i = 0; i < num_verts; i += step) {
      add_point(transform_point(&tfm, P[i]));
    }
  }
  if (distances.empty()) {
    return {1.0f};
  }

  const size_t num = min(distances.size(), size_t(16));
  std::sort(distances.begin(), distances.end());
  vector<float> result(num);
  for (size_t i = 0; i < num; i++) {
    result[i] = distances[(2 * i + 1) * distances.size() / (2 * num)];
  }
  return result;
}

/* For area and spot lights the distances to the objects in front of the light. */
static vector<float> light_object_distances(const Scene *scene,
                                            const Object *light_object,
                                            const Light *light)
{
  if (light->get_light_type() == LIGHT_SUN) {
    /* Distant lights are infinitely far away. */
    return {1e10f};
  }

  const Transform &tfm = light_object->get_tfm();
  const float3 position = transform_get_translation(&tfm);
  const float3 direction = safe_normalize(-transform_get_column(&tfm, 2));
  const bool one_sided = light->get_light_type() == LIGHT_AREA ||
                         light->get_light_type() == LIGHT_SPOT;
  return scene_point_distances(scene, position, direction, one_sided, 0.0f);
}

/* The emission of an emissive surface whose shader fades it with the distance, like the shader
 * of a light does (a lamp shade with a Light Falloff node, so that it does not burn out the
 * ceiling next to it), or with the direction of view (a Layer Weight node, for a shade which is
 * brightest seen face on): the emission is scaled to its average over the distances to the
 * scene and over the hemisphere, so that the surface emits the same flux. */
static void average_surface_emission(Scene *scene,
                                     AnariMaterialBuilder &builder,
                                     const Shader *shader,
                                     AnariPrincipled &params)
{
  if (!params.has_emission || shader->graph == nullptr) {
    return;
  }
  const bool has_falloff = light_shader_has_falloff(shader);
  bool has_view = false;
  for (const ShaderNode *node : shader->graph->nodes) {
    has_view = has_view || node->type == LayerWeightNode::get_node_type() ||
               node->type == FresnelNode::get_node_type();
  }
  if (!has_falloff && !has_view) {
    return;
  }

  /* Distances from the first objects which have the shader to the scene, not to the emitter
   * itself. */
  vector<float> distances;
  if (has_falloff) {
    int num_emitters = 0;
    for (const Object *object : scene->objects) {
      const Geometry *geom = object->get_geometry();
      if (geom == nullptr || geom->is_light() || !object->bounds.valid()) {
        continue;
      }
      bool has_shader = false;
      for (const Node *node : geom->get_used_shaders()) {
        has_shader = has_shader || node == shader;
      }
      if (!has_shader) {
        continue;
      }
      const float radius = 0.5f * len(object->bounds.size());
      for (const float distance :
           scene_point_distances(scene, object->bounds.center(), zero_float3(), false, radius))
      {
        distances.push_back(distance);
      }
      if (++num_emitters >= 4) {
        break;
      }
    }
  }
  if (distances.empty()) {
    distances.push_back(-1.0f);
  }
  /* Directions of view over the hemisphere, weighted with the flux of a Lambertian emitter. */
  vector<float> cosines;
  const int num_cosines = has_view ? 8 : 1;
  for (int i = 0; i < num_cosines; i++) {
    cosines.push_back(has_view ? (float(i) + 0.5f) / float(num_cosines) : -1.0f);
  }

  /* The size of a value: a constant, or the output transform of a sampler. Along the distances
   * and directions only the strength changes, which scales all of it. */
  const auto magnitude = [](const AnariValue &value) {
    if (value.is_constant()) {
      return reduce_add(fabs(make_float3(value.constant)));
    }
    float sum = reduce_add(fabs(make_float3(value.out_offset)));
    for (int i = 0; i < 3; i++) {
      sum += reduce_add(fabs(make_float3(value.out_row[i])));
    }
    return sum;
  };
  const float surface_magnitude = magnitude(params.emissive);
  if (surface_magnitude <= 0.0f) {
    return;
  }

  float sum = 0.0f;
  float weight_sum = 0.0f;
  for (const float distance : distances) {
    for (const float cosine : cosines) {
      builder.set_light_ray_length(distance);
      builder.set_view_cosine(cosine);
      const AnariPrincipled faded = builder.evaluate_surface(const_cast<Shader *>(shader));
      const float weight = ((distance > 0.0f) ? 1.0f / max(sqr(distance), 1e-4f) : 1.0f) *
                           ((cosine > 0.0f) ? cosine : 1.0f);
      sum += weight * (faded.has_emission ? magnitude(faded.emissive) : 0.0f);
      weight_sum += weight;
    }
  }
  builder.set_light_ray_length(-1.0f);
  builder.set_view_cosine(-1.0f);

  const float scale = sum / (weight_sum * surface_magnitude);
  ANARI_TRACE("emission %s: scaled by %g, the average over %d distances and %d directions",
              shader->name.c_str(),
              double(scale),
              int(distances.size()),
              int(cosines.size()));
  params.emissive.apply_linear(make_float4(scale, scale, scale, 1.0f), zero_float4());
}

/* The constant emission of the shader of a light. ANARI lights have a fixed inverse square
 * falloff: a shader which makes its own falloff from the ray length is evaluated for the
 * distances to the objects of the scene and averaged, weighted with the irradiance the objects
 * receive. The scene then gets about the same light in total, distributed differently. */
static bool light_shader_emission(Scene *scene,
                                  AnariMaterialBuilder &builder,
                                  const Object *object,
                                  const Light *light,
                                  const Shader *shader,
                                  float3 &emission)
{
  vector<float> distances;
  if (light_shader_has_falloff(shader)) {
    distances = light_object_distances(scene, object, light);
  }
  else {
    distances.push_back(-1.0f);
  }

  float3 sum = zero_float3();
  float weight_sum = 0.0f;
  for (const float distance : distances) {
    builder.set_light_ray_length(distance);
    const AnariPrincipled params = builder.evaluate_surface(const_cast<Shader *>(shader));
    if (params.has_emission && params.emissive.is_constant()) {
      const float weight = (distance > 0.0f) ? 1.0f / max(sqr(distance), 1e-4f) : 1.0f;
      sum += weight * make_float3(params.emissive.constant);
      weight_sum += weight;
    }
  }
  builder.set_light_ray_length(-1.0f);

  if (weight_sum == 0.0f) {
    return false;
  }
  emission = sum / weight_sum;
  return true;
}

/* The emission of the shader of a light over the distance, for a device which takes it as a ramp
 * (`falloff.distance` and `falloff.color` on the light): a shader which fades the light out with
 * the ray length, or a Light Falloff node. The shader is evaluated from the light to the far end
 * of the scene, more densely where the emission bends. The colors are relative to `emission`,
 * the largest of them. False when the emission does not change with the distance, or is not a
 * constant at a distance (a texture). */
static bool light_shader_falloff(Scene *scene,
                                 AnariMaterialBuilder &builder,
                                 const Object *object,
                                 const Shader *shader,
                                 float3 &emission,
                                 vector<float> &distances,
                                 vector<packed_float3> &colors)
{
  /* How far the scene reaches from the light. */
  const float3 position = transform_get_translation(&object->get_tfm());
  float reach = 0.0f;
  for (const Object *other : scene->objects) {
    const Geometry *geom = other->get_geometry();
    if (geom == nullptr || geom->is_light() || !other->bounds.valid()) {
      continue;
    }
    for (int corner = 0; corner < 8; corner++) {
      const float3 p = make_float3((corner & 1) ? other->bounds.max.x : other->bounds.min.x,
                                   (corner & 2) ? other->bounds.max.y : other->bounds.min.y,
                                   (corner & 4) ? other->bounds.max.z : other->bounds.min.z);
      reach = max(reach, len(p - position));
    }
  }
  if (!(reach > 0.0f)) {
    return false;
  }

  bool constant = true;
  const auto evaluate = [&](const float distance) {
    builder.set_light_ray_length(distance);
    const AnariPrincipled params = builder.evaluate_surface(const_cast<Shader *>(shader));
    if (!params.has_emission) {
      return zero_float3();
    }
    constant = constant && params.emissive.is_constant();
    return make_float3(params.emissive.constant);
  };

  struct Sample {
    float distance;
    float3 value;
    /* The emission half way to the next sample, and how far a line misses it. */
    float3 middle;
    float error;
  };
  const auto measure = [&](Sample &a, const Sample &b) {
    a.middle = evaluate(0.5f * (a.distance + b.distance));
    a.error = reduce_max(fabs(a.middle - 0.5f * (a.value + b.value)));
  };
  const int num_first = 17;
  const size_t num_most = 48;
  vector<Sample> samples;
  for (int i = 0; i < num_first; i++) {
    const float distance = reach * float(i) / float(num_first - 1);
    samples.push_back({distance, evaluate(distance), zero_float3(), 0.0f});
  }
  for (size_t i = 0; i + 1 < samples.size(); i++) {
    measure(samples[i], samples[i + 1]);
  }
  float largest = 0.0f;
  for (const Sample &sample : samples) {
    largest = max(largest, max(reduce_max(sample.value), reduce_max(sample.middle)));
  }
  while (constant && largest > 0.0f && samples.size() < num_most) {
    size_t worst = 0;
    for (size_t i = 0; i + 1 < samples.size(); i++) {
      if (samples[i].error > samples[worst].error) {
        worst = i;
      }
    }
    if (samples[worst].error < 0.005f * largest) {
      break;
    }
    const Sample middle = {0.5f * (samples[worst].distance + samples[worst + 1].distance),
                           samples[worst].middle,
                           zero_float3(),
                           0.0f};
    samples.insert(samples.begin() + worst + 1, middle);
    measure(samples[worst], samples[worst + 1]);
    measure(samples[worst + 1], samples[worst + 2]);
    largest = max(largest,
                  max(reduce_max(samples[worst].middle), reduce_max(samples[worst + 1].middle)));
  }
  builder.set_light_ray_length(-1.0f);

  if (!constant || !(largest > 0.0f)) {
    return false;
  }
  float smallest = largest;
  for (const Sample &sample : samples) {
    smallest = min(smallest, reduce_min(sample.value));
  }
  if (largest - smallest < 1e-4f * largest && samples.size() == size_t(num_first)) {
    /* The same at every distance. */
    return false;
  }

  emission = make_float3(largest);
  distances.clear();
  colors.clear();
  for (const Sample &sample : samples) {
    distances.push_back(sample.distance);
    colors.push_back(max(sample.value, zero_float3()) / largest);
  }
  return true;
}

void AnariScene::sync_lights(Scene *scene, AnariMaterialBuilder &builder)
{
  release_lights();

  for (const Object *object : scene->objects) {
    const Geometry *geom = object->get_geometry();
    if (geom == nullptr || !geom->is_light()) {
      continue;
    }
    const Light *light = static_cast<const Light *>(geom);
    if (!light->get_is_enabled()) {
      continue;
    }

    float3 strength = light->get_strength();

    /* The node tree of the light as a graph when the device takes it, otherwise its constant
     * emission. */
    const Shader *shader = light->get_shader();
    const AnariGraphSource *source = nullptr;
    vector<float> falloff_distances;
    vector<packed_float3> falloff_colors;
    if (shader && shader != scene->default_light) {
      source = light_world_graphs_ ? graph_source_for(shader) : nullptr;
      if (source == nullptr) {
        float3 emission;
        if (light_falloff_ && light->get_light_type() != LIGHT_SUN &&
            light_shader_has_falloff(shader) &&
            light_shader_falloff(
                scene, builder, object, shader, emission, falloff_distances, falloff_colors))
        {
          /* The device fades the light as the shader does. */
          strength *= emission;
        }
        else if (light_shader_emission(scene, builder, object, light, shader, emission)) {
          strength *= emission;
        }
      }
    }

    if (light_shell_lights_.count(object)) {
      /* Inside of a globe which lets half of the light through. */
      strength *= 1.0f / (1.0f - LIGHT_SHELL_OPACITY);
    }

    ANARI_TRACE("light %s: strength %g %g %g%s%s",
                object->name.c_str(),
                double(strength.x),
                double(strength.y),
                double(strength.z),
                source ? ", shader as graph" : "",
                falloff_distances.empty() ?
                    "" :
                    string_printf(", falloff over %g in %d steps",
                                  double(falloff_distances.back()),
                                  int(falloff_distances.size()))
                        .c_str());
    if (is_zero(strength)) {
      continue;
    }
    if (!light_visibility_ && (object->get_visibility() & PATH_RAY_VISIBILITY_DIFFUSE) == 0) {
      /* A light for reflections only. Without ray visibility on the device it would light
       * everything: leaving it out is the smaller error. */
      continue;
    }
    add_light(scene, object, light, strength, source, falloff_distances, falloff_colors);
  }

  light_cards_.clear();
  if (!instance_visibility_ && device_.has_subtype(ANARI_LIGHT, "quad")) {
    for (const Object *object : scene->objects) {
      if (add_light_card(builder, object)) {
        light_cards_.insert(object);
      }
    }
  }
}

/* Scenes are lit through emissive planes which are hidden from camera rays, shadow rays or both:
 * in a window, or next to the camera. A device without ray visibility would show such a plane,
 * and it would keep the sun out of the room. A quad light has the same emission and shadows
 * nothing. Only for meshes of a few triangles with one constant emission. */
bool AnariScene::add_light_card(AnariMaterialBuilder &builder, const Object *object)
{
  const Geometry *geom = object->get_geometry();
  if (geom == nullptr || !geom->is_mesh()) {
    return false;
  }
  const uint visibility = object->get_visibility();
  if ((visibility & PATH_RAY_VISIBILITY_ALL) == 0 ||
      (~visibility & (PATH_RAY_VISIBILITY_CAMERA | PATH_RAY_VISIBILITY_SHADOW)) == 0)
  {
    return false;
  }

  const Mesh *mesh = static_cast<const Mesh *>(geom);
  const size_t num_triangles = mesh->num_triangles();
  const array<Node *> &used_shaders = mesh->get_used_shaders();
  if (num_triangles == 0 || num_triangles > 64 || used_shaders.size() != 1) {
    return false;
  }
  Shader *shader = static_cast<Shader *>(used_shaders[0]);
  if (shader == nullptr || shader->emission_sampling == EMISSION_SAMPLING_NONE) {
    return false;
  }
  const AnariPrincipled params = builder.evaluate_surface(shader);
  /* A textured emission is replaced by its average: the light is the same, a backdrop loses its
   * picture. Not with a textured opacity, which would cut the plane out. */
  float4 emission;
  if (!params.has_emission || !params.opacity.is_constant() ||
      !builder.average_value(params.emissive, emission))
  {
    return false;
  }
  const float3 radiance = max(make_float3(emission), zero_float3()) * params.opacity.constant.x;
  if (is_zero(radiance)) {
    return false;
  }

  ANARIDevice d = anari();
  const Transform tfm = geom->transform_applied ? transform_identity() : object->get_tfm();
  const Attribute *attr_P = mesh->attributes.find(ATTR_STD_POSITION);
  if (attr_P == nullptr) {
    return false;
  }
  const packed_float3 *verts = attr_P->data<packed_float3>();
  /* Not shown to the camera: a light is not clipped by the camera, and such planes are also
   * placed nearer than the clip start. */
  const bool visible = false;

  auto add_quad = [&](const float3 corner, const float3 edge1, const float3 edge2, const float3 L) {
    if (len(cross(edge1, edge2)) == 0.0f) {
      return;
    }
    ANARILight light = anariNewLight(d, "quad");
    anariSetParameter(d, light, "position", ANARI_FLOAT32_VEC3, &corner);
    anariSetParameter(d, light, "edge1", ANARI_FLOAT32_VEC3, &edge1);
    anariSetParameter(d, light, "edge2", ANARI_FLOAT32_VEC3, &edge2);
    anariSetParameter(d, light, "side", ANARI_STRING, "both");
    anariSetParameter(d, light, "visible", ANARI_BOOL, &visible);
    set_light_color(d, light, L, "radiance");
    anariCommitParameters(d, light);
    lights_.push_back(light);
  };

  for (size_t i = 0; i < num_triangles; i++) {
    const Mesh::Triangle triangle = mesh->get_triangle(i);
    const float3 a = transform_point(&tfm, verts[triangle.v[0]]);
    const float3 b = transform_point(&tfm, verts[triangle.v[1]]);
    const float3 c = transform_point(&tfm, verts[triangle.v[2]]);

    /* Two triangles of a parallelogram are one quad. */
    if (i + 1 < num_triangles) {
      const Mesh::Triangle next = mesh->get_triangle(i + 1);
      if (next.v[0] == triangle.v[0] && next.v[1] == triangle.v[2]) {
        const float3 e = transform_point(&tfm, verts[next.v[2]]);
        if (len(a + c - b - e) < 1e-3f * (len(b - a) + len(e - a))) {
          add_quad(a, b - a, e - a, radiance);
          i++;
          continue;
        }
      }
    }
    /* A single triangle: the parallelogram of twice its area, with half the radiance. */
    add_quad(a, b - a, c - a, 0.5f * radiance);
  }

  ANARI_TRACE("light card %s: %d triangles", object->name.c_str(), int(num_triangles));
  return true;
}

/* --------------------------------------------------------------------
 * Background.
 */

/* The sun of a sky texture is too small and too bright for an environment map. Cycles samples it
 * on its own and leaves it out of the map; here it becomes a directional light. Its irradiance is
 * the integral of the world shader over the sun disc, with the sun minus without it, so that
 * everything the shader does with the sky texture is included. */
static bool bake_background_sun(Device *device,
                                Scene *scene,
                                float3 &irradiance,
                                float3 &sun_direction,
                                float &angular_diameter)
{
  KernelBackground &kbackground = scene->dscene.data.background;
  if (!kbackground.use_sun_guiding) {
    /* No sun, or several: they stay in the map. */
    return false;
  }

  sun_direction = safe_normalize(make_float3(kbackground.sun));
  const float half_angle = kbackground.sun.w;
  angular_diameter = 2.0f * half_angle;

  /* A grid in spherical coordinates around the sun: theta from the zenith, phi around it. The
   * map coordinates are u = (phi - pi) / (-2 pi) and v = 1 - theta / pi. */
  const int grid = 96;
  const float theta_center = safe_acosf(sun_direction.z);
  const float phi_center = atan2f(sun_direction.y, sun_direction.x);
  const float theta_extent = 1.3f * half_angle;
  const float phi_extent = min(M_PI_F, theta_extent / max(sinf(theta_center), 1e-3f));
  const int num_samples = grid * grid;

  vector<float> weights(num_samples);
  auto fill_input = [&](device_vector<KernelShaderEvalInput> &d_input) {
    KernelShaderEvalInput *input = d_input.data();
    for (int i = 0; i < grid; i++) {
      const float theta = theta_center + ((float(i) + 0.5f) / float(grid) * 2.0f - 1.0f) * theta_extent;
      for (int j = 0; j < grid; j++) {
        const float phi = phi_center + ((float(j) + 0.5f) / float(grid) * 2.0f - 1.0f) * phi_extent;
        float u = (phi - M_PI_F) / -M_2PI_F;
        u -= floorf(u);

        KernelShaderEvalInput in;
        in.object = 16384;
        in.prim = 8192;
        in.u = u;
        in.v = 1.0f - theta / M_PI_F;
        input[j + i * grid] = in;
        /* Solid angle of the cell, zero beyond the poles. */
        weights[j + i * grid] = (theta > 0.0f && theta < M_PI_F) ?
                                    sinf(theta) * sqr(2.0f / float(grid)) * theta_extent *
                                        phi_extent :
                                    0.0f;
      }
    }
    return num_samples;
  };

  Progress progress;
  ShaderEval shader_eval(device, progress);
  float3 sums[2] = {zero_float3(), zero_float3()};
  bool evaluated = true;

  for (int with_sun = 0; with_sun < 2 && evaluated; with_sun++) {
    /* The kernel leaves the sun out while this is set. */
    kbackground.use_sun_guiding = with_sun ? 0 : 1;
    device->const_copy_to("data", &scene->dscene.data, sizeof(scene->dscene.data));

    evaluated = shader_eval.eval(SHADER_EVAL_BACKGROUND,
                                 num_samples,
                                 3,
                                 fill_input,
                                 [&](device_vector<float> &d_output) {
                                   const float *output = d_output.data();
                                   for (int i = 0; i < num_samples; i++) {
                                     const float3 value = make_float3(
                                         output[i * 3], output[i * 3 + 1], output[i * 3 + 2]);
                                     if (isfinite_safe(value)) {
                                       sums[with_sun] += value * weights[i];
                                     }
                                   }
                                 });
  }

  kbackground.use_sun_guiding = 1;
  device->const_copy_to("data", &scene->dscene.data, sizeof(scene->dscene.data));

  irradiance = max(sums[1] - sums[0], zero_float3());
  return evaluated && !is_zero(irradiance);
}

/* Evaluate the world shader for the directions of an equirectangular map, the way Cycles builds
 * the importance map of its background light. Several samples per pixel, for small bright
 * features. Also returns the average over the sphere. With `camera` the world is evaluated as
 * camera rays see it, instead of as the light it is. */
static bool bake_background(Device *device,
                            Scene *scene,
                            const int width,
                            const int height,
                            const int supersample,
                            const bool camera,
                            vector<float3> &pixels,
                            float3 &average)
{
  /* Needs to be up to date for attribute access. */
  device->const_copy_to("data", &scene->dscene.data, sizeof(scene->dscene.data));

  const int sample_width = width * supersample;
  const int sample_height = height * supersample;
  const float sample_weight = 1.0f / float(supersample * supersample);
  pixels.assign(size_t(width) * size_t(height), zero_float3());

  /* In bands of rows, to bound the memory of the samples. */
  const int band_rows = 64;
  Progress progress;
  ShaderEval shader_eval(device, progress);

  for (int band_y = 0; band_y < height; band_y += band_rows) {
    const int rows = min(band_rows, height - band_y) * supersample;
    const int first_row = band_y * supersample;
    const int num_samples = rows * sample_width;

    const bool evaluated = shader_eval.eval(
        SHADER_EVAL_BACKGROUND,
        num_samples,
        3,
        [&](device_vector<KernelShaderEvalInput> &d_input) {
          KernelShaderEvalInput *input = d_input.data();
          for (int y = 0; y < rows; y++) {
            for (int x = 0; x < sample_width; x++) {
              KernelShaderEvalInput in;
              /* The resolution, for the ray differentials. A negative width selects the
               * camera rays, see #kernel_background_evaluate. */
              in.object = camera ? -sample_width : sample_width;
              in.prim = sample_height;
              in.u = (float(x) + 0.5f) / float(sample_width);
              in.v = (float(first_row + y) + 0.5f) / float(sample_height);
              input[x + y * sample_width] = in;
            }
          }
          return num_samples;
        },
        [&](device_vector<float> &d_output) {
          const float *output = d_output.data();
          for (int y = 0; y < rows; y++) {
            float3 *row = &pixels[size_t(band_y + y / supersample) * size_t(width)];
            for (int x = 0; x < sample_width; x++) {
              const float *rgb = output + (size_t(x) + size_t(y) * sample_width) * 3;
              const float3 value = make_float3(rgb[0], rgb[1], rgb[2]);
              if (isfinite_safe(value)) {
                row[x / supersample] += value * sample_weight;
              }
            }
          }
        });
    if (!evaluated) {
      return false;
    }
  }

  average = zero_float3();
  if (pixels.empty()) {
    return true;
  }

  /* Average weighted by the solid angle of the rows. */
  double sum[3] = {0.0, 0.0, 0.0};
  double weights = 0.0;
  for (int y = 0; y < height; y++) {
    const double weight = sinf(M_PI_F * (float(y) + 0.5f) / float(height));
    for (int x = 0; x < width; x++) {
      const float3 value = pixels[size_t(y) * size_t(width) + x];
      sum[0] += weight * value.x;
      sum[1] += weight * value.y;
      sum[2] += weight * value.z;
    }
    weights += weight * width;
  }
  average = make_float3(float(sum[0] / weights), float(sum[1] / weights), float(sum[2] / weights));
  return true;
}

/* Whether the world shader can make a difference between camera rays and the others. Nodes
 * which are not linked to the output are gone when the graph is compiled. */
static bool world_shader_has_light_path(const Shader *shader)
{
  if (shader == nullptr || shader->graph == nullptr) {
    return false;
  }
  for (const ShaderNode *node : shader->graph->nodes) {
    if (node->type == LightPathNode::get_node_type()) {
      return true;
    }
  }
  return false;
}

/* Bilinear lookup in an equirectangular map of rows from the bottom up, as #bake_background
 * makes it. */
static float3 equirectangular_lookup(const vector<float3> &pixels,
                                     const int width,
                                     const int height,
                                     const float3 direction)
{
  const float u = (atan2f(direction.y, direction.x) - M_PI_F) / -M_2PI_F;
  const float v = 1.0f - safe_acosf(direction.z) / M_PI_F;
  const float x = u * float(width) - 0.5f;
  const float y = clamp(v * float(height) - 0.5f, 0.0f, float(height - 1));
  const int ix = int(floorf(x));
  const int iy = min(int(y), height - 1);
  const float fx = x - float(ix);
  const float fy = y - float(iy);
  const int x0 = ((ix % width) + width) % width;
  const int x1 = (x0 + 1) % width;
  const int y1 = min(iy + 1, height - 1);
  const float3 *row0 = &pixels[size_t(iy) * size_t(width)];
  const float3 *row1 = &pixels[size_t(y1) * size_t(width)];
  return mix(mix(row0[x0], row0[x1], fx), mix(row1[x0], row1[x1], fx), fy);
}

void AnariScene::update_backdrop(const AnariFrameWindow &window)
{
  ANARIDevice d = anari();
  const CameraState &state = camera_state_;

  if (state.type == CAMERA_PANORAMA || window.width <= 0 || window.height <= 0) {
    /* The image is stretched over the frame, which only fits a planar projection. */
    anariSetParameter(d, renderer_, "background", ANARI_FLOAT32_VEC4, &background_color_);
    anariCommitParameters(d, renderer_);
    return;
  }

  /* The device stretches the image over the frame. In the viewport it is made again whenever
   * the view changes: keep it small there. */
  const int step = backdrop_final_ ? 1 : max((window.width + 959) / 960, 1);
  const int width = max(window.width / step, 1);
  const int height = max(window.height / step, 1);

  ANARIArray2D image = anariNewArray2D(
      d, nullptr, nullptr, nullptr, ANARI_FLOAT32_VEC4, width, height);
  float4 *pixels = static_cast<float4 *>(anariMapArray(d, image));

  const bool perspective = state.type == CAMERA_PERSPECTIVE;
  const float3 right = safe_normalize(cross(state.direction, state.up));
  parallel_for(0, height, [&](const int y) {
    const float Y = frame_y0_ + (frame_y1_ - frame_y0_) * (float(y) + 0.5f) / float(height);
    for (int x = 0; x < width; x++) {
      const float X = frame_x0_ + (frame_x1_ - frame_x0_) * (float(x) + 0.5f) / float(width);
      const float3 direction = perspective ?
                                   safe_normalize(state.direction + right * X + state.up * Y) :
                                   state.direction;
      const float3 color = equirectangular_lookup(
          backdrop_pixels_, backdrop_width_, backdrop_height_, direction);
      pixels[size_t(y) * size_t(width) + x] = make_float4(color.x, color.y, color.z, 1.0f);
    }
  });

  anariUnmapArray(d, image);
  anariSetParameter(d, renderer_, "background", ANARI_ARRAY2D, &image);
  anariCommitParameters(d, renderer_);
  anariRelease(d, image);
}

void AnariScene::sync_background(Scene *scene, AnariMaterialBuilder &builder)
{
  ANARIDevice d = anari();

  if (background_light_) {
    anariRelease(d, background_light_);
    background_light_ = nullptr;
  }
  if (background_sun_light_) {
    anariRelease(d, background_sun_light_);
    background_sun_light_ = nullptr;
  }

  Background *background = scene->background;
  Shader *shader = background->get_shader(scene);

  const bool film_transparent = background->get_transparent();
  const bool camera_visible = (background->get_visibility() & PATH_RAY_VISIBILITY_CAMERA) != 0;

  backdrop_pixels_.clear();
  backdrop_modified_ = true;
  backdrop_final_ = scene->params.background;

  if (light_world_graphs_ && graph_source_for(shader)) {
    /* The device evaluates the world shader itself, see #sync_renderer. The alpha of the
     * background color tells it whether camera rays see the world. */
    background_color_ = make_float4(
        0.0f, 0.0f, 0.0f, (film_transparent || !camera_visible) ? 0.0f : 1.0f);
    ambient_radiance_ = zero_float3();
    return;
  }

  const AnariMaterialBuilder::Background world = builder.evaluate_background(shader);

  /* What camera rays see of the world is not always what lights the scene: the shader can tell
   * them apart with the Light Path node (a darker sky behind the characters than the one which
   * lights them), or the world is a backdrop which diffuse rays don't see. An environment light
   * can't be both, so the world is evaluated once more for camera rays and shown as the
   * background image of the renderer, see #update_backdrop. */
  const bool lights_scene = (background->get_visibility() & PATH_RAY_VISIBILITY_DIFFUSE) != 0;
  if (background_image_ && camera_visible && !film_transparent &&
      (world_shader_has_light_path(shader) || (!lights_scene && (world.image || !world.exact))))
  {
    backdrop_width_ = scene->params.background ? 2048 : 1024;
    backdrop_height_ = backdrop_width_ / 2;
    float3 average = zero_float3();
    const double start_time = time_dt();
    if (!bake_background(&device_,
                         scene,
                         backdrop_width_,
                         backdrop_height_,
                         1,
                         true,
                         backdrop_pixels_,
                         average))
    {
      backdrop_pixels_.clear();
    }
    ANARI_TRACE("background: backdrop for camera rays %dx%d in %.1f ms, average %g %g %g",
                backdrop_width_,
                backdrop_height_,
                (time_dt() - start_time) * 1000.0,
                double(average.x),
                double(average.y),
                double(average.z));
  }
  const bool has_backdrop = !backdrop_pixels_.empty();

  /* A world shader which is more than a color or an environment image (a sky texture, mixed
   * backgrounds, color corrections) is evaluated for all directions with the CPU kernels of
   * Cycles, and the result is the environment image. */
  vector<float3> baked;
  int baked_width = 0;
  int baked_height = 0;
  float3 baked_average = zero_float3();
  if (!world.exact) {
    /* A smaller map in the viewport, where the world is baked on every edit. */
    baked_width = scene->params.background ? 2048 : 1024;
    baked_height = baked_width / 2;
    const double start_time = time_dt();
    if (bake_background(
            &device_, scene, baked_width, baked_height, 2, false, baked, baked_average))
    {
      if (anari_trace_enabled()) {
        float3 maximum = zero_float3();
        for (const float3 &value : baked) {
          maximum = max(maximum, value);
        }
        ANARI_TRACE("background: baked %dx%d in %.1f ms, average %g %g %g, maximum %g %g %g",
                    baked_width,
                    baked_height,
                    (time_dt() - start_time) * 1000.0,
                    double(baked_average.x),
                    double(baked_average.y),
                    double(baked_average.z),
                    double(maximum.x),
                    double(maximum.y),
                    double(maximum.z));
      }
    }
    else {
      baked.clear();
      LOG_WARNING << "ANARI: the world shader could not be evaluated, using a constant background";
    }

    /* The sun of a sky texture as a light of its own. */
    float3 sun_irradiance;
    float3 sun_direction;
    float sun_angle;
    if (!baked.empty() &&
        bake_background_sun(&device_, scene, sun_irradiance, sun_direction, sun_angle))
    {
      ANARI_TRACE("background: sun irradiance %g %g %g, diameter %g",
                  double(sun_irradiance.x),
                  double(sun_irradiance.y),
                  double(sun_irradiance.z),
                  double(sun_angle));
      const float3 direction = -sun_direction;
      background_sun_light_ = anariNewLight(d, "directional");
      anariSetParameter(d, background_sun_light_, "direction", ANARI_FLOAT32_VEC3, &direction);
      anariSetParameter(d, background_sun_light_, "angularDiameter", ANARI_FLOAT32, &sun_angle);
      set_light_color(d, background_sun_light_, sun_irradiance, "irradiance");
      const bool visible = camera_visible && !film_transparent && !has_backdrop;
      anariSetParameter(d, background_sun_light_, "visible", ANARI_BOOL, &visible);
      anariCommitParameters(d, background_sun_light_);
    }
  }

  const float3 constant = baked.empty() ? world.color * world.strength : baked_average;
  background_color_ = (film_transparent || !camera_visible) ?
                          zero_float4() :
                          make_float4(constant.x, constant.y, constant.z, 1.0f);
  ambient_radiance_ = zero_float3();

  if ((background->get_visibility() & PATH_RAY_VISIBILITY_DIFFUSE) == 0) {
    /* A world which diffuse rays don't see is a backdrop: the camera sees it, but it does not
     * light the scene. ANARI has no ray visibility for lights, so there is no environment
     * light then, only the background color. */
    if (background_sun_light_) {
      anariRelease(d, background_sun_light_);
      background_sun_light_ = nullptr;
    }
    return;
  }

  if (!device_.has_subtype(ANARI_LIGHT, "hdri")) {
    /* No environment lighting, approximate with ambient light. */
    ambient_radiance_ = constant;
    return;
  }

  ANARIArray2D radiance = nullptr;
  float scale = world.strength;
  float3 direction = make_float3(1.0f, 0.0f, 0.0f);
  float3 up = make_float3(0.0f, 0.0f, 1.0f);

  if (!baked.empty()) {
    /* Rows from the bottom up and +X in the center, like the environment images of Cycles. */
    radiance = anariNewArray2D(
        d, nullptr, nullptr, nullptr, ANARI_FLOAT32_VEC3, baked_width, baked_height);
    packed_float3 *dst = static_cast<packed_float3 *>(anariMapArray(d, radiance));
    for (size_t i = 0; i < baked.size(); i++) {
      dst[i] = baked[i];
    }
    anariUnmapArray(d, radiance);
    scale = 1.0f;
  }
  else if (world.image) {
    /* Environment map, converted to linear float RGB. */
    const int kernel_id = world.image->handle.kernel_id();
    const AnariImage *image = nullptr;
    if (kernel_id >= 0 && size_t(kernel_id) < scene->dscene.image_textures.size()) {
      image = device_.find_image(scene->dscene.image_textures[kernel_id].image_info_id);
    }
    if (image && image->pixels) {
      Progress progress;
      const ImageMetaData metadata = world.image->handle.metadata(progress);
      const bool srgb = metadata.colorspace == u_colorspace_scene_linear_srgb;
      const size_t num_pixels = size_t(image->width) * size_t(image->height);
      radiance = anariNewArray2D(
          d, nullptr, nullptr, nullptr, ANARI_FLOAT32_VEC3, image->width, image->height);
      packed_float3 *dst = static_cast<packed_float3 *>(anariMapArray(d, radiance));
      for (size_t i = 0; i < num_pixels; i++) {
        float3 c;
        switch (image->data_type) {
          case IMAGE_DATA_TYPE_FLOAT4:
            c = make_float3(static_cast<const float4 *>(image->pixels)[i]);
            break;
          case IMAGE_DATA_TYPE_FLOAT:
            c = make_float3(static_cast<const float *>(image->pixels)[i]);
            break;
          case IMAGE_DATA_TYPE_HALF4: {
            const half *h = static_cast<const half *>(image->pixels) + i * 4;
            c = make_float3(half_to_float(h[0]), half_to_float(h[1]), half_to_float(h[2]));
            break;
          }
          case IMAGE_DATA_TYPE_HALF:
            c = make_float3(half_to_float(static_cast<const half *>(image->pixels)[i]));
            break;
          case IMAGE_DATA_TYPE_BYTE4: {
            const uchar *b = static_cast<const uchar *>(image->pixels) + i * 4;
            c = make_float3(b[0], b[1], b[2]) * (1.0f / 255.0f);
            if (srgb) {
              c = color_srgb_to_linear_v3(c);
            }
            break;
          }
          case IMAGE_DATA_TYPE_BYTE: {
            float f = static_cast<const uchar *>(image->pixels)[i] * (1.0f / 255.0f);
            if (srgb) {
              f = color_srgb_to_linear(f);
            }
            c = make_float3(f);
            break;
          }
          case IMAGE_DATA_TYPE_USHORT4: {
            const uint16_t *u = static_cast<const uint16_t *>(image->pixels) + i * 4;
            c = make_float3(u[0], u[1], u[2]) * (1.0f / 65535.0f);
            break;
          }
          default:
            c = zero_float3();
            break;
        }
        dst[i] = c * world.color;
      }
      anariUnmapArray(d, radiance);

      /* The texture lookup direction is `mapping * world_direction`, the center of the
       * equirectangular image is +X and up is +Z. */
      const Transform inverse = transform_inverse(world.mapping);
      direction = safe_normalize(transform_direction(&inverse, make_float3(1.0f, 0.0f, 0.0f)));
      up = safe_normalize(transform_direction(&inverse, make_float3(0.0f, 0.0f, 1.0f)));
    }
  }

  if (radiance == nullptr) {
    if (is_zero(constant)) {
      return;
    }
    /* Constant environment as a uniform map. Not too small: importance sampling of very coarse
     * maps is biased in some back-ends (Mitsuba's envmap is ~5% too bright at 4x2). */
    const int map_width = 64, map_height = 32;
    radiance = anariNewArray2D(
        d, nullptr, nullptr, nullptr, ANARI_FLOAT32_VEC3, map_width, map_height);
    packed_float3 *dst = static_cast<packed_float3 *>(anariMapArray(d, radiance));
    for (int i = 0; i < map_width * map_height; i++) {
      dst[i] = world.color;
    }
    anariUnmapArray(d, radiance);
  }

  background_light_ = anariNewLight(d, "hdri");
  anariSetParameter(d, background_light_, "radiance", ANARI_ARRAY2D, &radiance);
  anariSetParameter(d, background_light_, "scale", ANARI_FLOAT32, &scale);
  anariSetParameter(d, background_light_, "direction", ANARI_FLOAT32_VEC3, &direction);
  anariSetParameter(d, background_light_, "up", ANARI_FLOAT32_VEC3, &up);
  /* With a backdrop the light only lights. */
  const bool visible = camera_visible && !film_transparent && !has_backdrop;
  anariSetParameter(d, background_light_, "visible", ANARI_BOOL, &visible);
  anariCommitParameters(d, background_light_);
  anariRelease(d, radiance);
}

/* --------------------------------------------------------------------
 * Renderer.
 */

void AnariScene::sync_renderer(Scene *scene)
{
  ANARIDevice d = anari();

  if (backdrop_pixels_.empty()) {
    anariSetParameter(d, renderer_, "background", ANARI_FLOAT32_VEC4, &background_color_);
  }
  else {
    /* The background is an image for the camera of the frame, see #update_frame. */
    backdrop_modified_ = true;
  }

  if (light_world_graphs_) {
    /* The world shader as a graph, which replaces the background color and the hdri light. */
    const Shader *shader = scene->background->get_shader(scene);
    const AnariGraphSource *source = shader ? graph_source_for(shader) : nullptr;
    background_samplers_.clear();
    /* The ray visibility of the world: one which only the camera sees is a backdrop. Camera rays
     * are decided by the alpha of the background color. */
    const uint visibility = scene->background->get_visibility();
    const std::pair<const char *, uint> flags[] = {
        {"backgroundVisibility.diffuse", PATH_RAY_VISIBILITY_DIFFUSE},
        {"backgroundVisibility.glossy", PATH_RAY_VISIBILITY_GLOSSY},
        {"backgroundVisibility.transmission", PATH_RAY_VISIBILITY_TRANSMIT},
        {"backgroundVisibility.volumeScatter", PATH_RAY_VISIBILITY_VOLUME_SCATTER},
    };
    for (const auto &flag : flags) {
      const bool visible = (visibility & flag.second) != 0;
      anariSetParameter(d, renderer_, flag.first, ANARI_BOOL, &visible);
    }
    if (source) {
      set_graph_source(scene, renderer_, "backgroundSource", *source);
      for (const AnariGraphImage &image : source->images) {
        background_samplers_.push_back(images_.get_sampler(scene, image.node));
      }
    }
    else {
      anariUnsetParameter(d, renderer_, "backgroundSource");
    }
  }

  const float ambient = max(max(ambient_radiance_.x, ambient_radiance_.y), ambient_radiance_.z);
  const float3 ambient_color = (ambient > 0.0f) ? ambient_radiance_ / ambient : one_float3();
  anariSetParameter(d, renderer_, "ambientRadiance", ANARI_FLOAT32, &ambient);
  anariSetParameter(d, renderer_, "ambientColor", ANARI_FLOAT32_VEC3, &ambient_color);

  const int pixel_samples = 1;
  anariSetParameter(d, renderer_, "pixelSamples", ANARI_INT32, &pixel_samples);

  const int max_depth = max(scene->integrator->get_max_bounce(), 1);
  anariSetParameter(d, renderer_, "maxRayDepth", ANARI_INT32, &max_depth);

  /* The denoiser of the device itself is off unless the user asks for it: the denoising settings
   * of the render are applied by Cycles on this side. barney does not list the parameter of
   * its renderer. */
  const bool denoise = device_.use_device_denoiser();
  /* The capture device of Vela describes the scene it mirrors, not the back-end it renders
   * with: it keeps every parameter and passes it on, and a back-end ignores what it does not
   * know. The settings of the renderer are sent to it without asking. */
  const bool capture_device = device_.library_name() == "vsr";
  if (device_.library_name() == "barney" || capture_device ||
      device_.has_parameter(
          ANARI_RENDERER, device_.renderer_subtype().c_str(), "denoise", ANARI_BOOL))
  {
    anariSetParameter(d, renderer_, "denoise", ANARI_BOOL, &denoise);
  }
  if (device_.library_name() == "cycles") {
    sync_renderer_cycles(scene);
  }
  else if (device_.library_name() == "visrtx") {
    /* The default filter accumulates tone mapped samples, which darkens everything lit by
     * small bright sources: a room lit through an emissive plane renders at a third. */
    anariSetParameter(d, renderer_, "fireflyFilterMode", ANARI_STRING, "none");
  }

  if (device_.library_name() != "cycles") {
    /* The limits of the path length per kind of bounce and the clamping decide how bright an
     * interior is, and they are not in the ANARI standard. A device which has them declares
     * the parameters, with the names of the Cycles device. */
    const Integrator *integrator = scene->integrator;
    const char *subtype = device_.renderer_subtype().c_str();
    /* Fast GI replaces the bounces after its limit by ambient occlusion of the world: in an
     * interior that is no light at all. The nearest a device without it can do is to end the
     * paths there. Transmission bounces and the first glossy bounce are not counted. */
    int max_diffuse_bounce = integrator->get_max_diffuse_bounce();
    int max_glossy_bounce = integrator->get_max_glossy_bounce();
    const int ao_bounces = integrator->get_ao_bounces();
    if (ao_bounces > 0) {
      max_diffuse_bounce = min(max_diffuse_bounce, ao_bounces - 1);
      max_glossy_bounce = min(max_glossy_bounce, ao_bounces);
    }
    const std::pair<const char *, int> limits[] = {
        {"maxBounce", integrator->get_max_bounce()},
        {"maxDiffuseBounce", max_diffuse_bounce},
        {"maxGlossyBounce", max_glossy_bounce},
        {"maxTransmissionBounce", integrator->get_max_transmission_bounce()},
        {"maxVolumeBounce", integrator->get_max_volume_bounce()},
        {"maxTransparencyBounce", integrator->get_transparent_max_bounce()},
    };
    for (const auto &limit : limits) {
      if (capture_device ||
          device_.has_parameter(ANARI_RENDERER, subtype, limit.first, ANARI_INT32))
      {
        anariSetParameter(d, renderer_, limit.first, ANARI_INT32, &limit.second);
      }
    }
    const std::pair<const char *, float> clamps[] = {
        {"clampDirect", integrator->get_sample_clamp_direct()},
        {"clampIndirect", integrator->get_sample_clamp_indirect()},
    };
    for (const auto &clamp : clamps) {
      if (capture_device ||
          device_.has_parameter(ANARI_RENDERER, subtype, clamp.first, ANARI_FLOAT32))
      {
        anariSetParameter(d, renderer_, clamp.first, ANARI_FLOAT32, &clamp.second);
      }
    }
  }

  anariCommitParameters(d, renderer_);
}

/* The Cycles ANARI device exposes most of the integrator settings, pass them so it renders
 * the same as Cycles itself. Sampling, denoising, exposure and resolution scaling are handled
 * by the Cycles session on this side. */
void AnariScene::sync_renderer_cycles(Scene *scene)
{
  ANARIDevice d = anari();
  const Integrator *integrator = scene->integrator;
  const Film *film = scene->film;

  auto set_bool = [&](const char *name, const bool value) {
    anariSetParameter(d, renderer_, name, ANARI_BOOL, &value);
  };
  auto set_int = [&](const char *name, const int value) {
    anariSetParameter(d, renderer_, name, ANARI_INT32, &value);
  };
  auto set_float = [&](const char *name, const float value) {
    anariSetParameter(d, renderer_, name, ANARI_FLOAT32, &value);
  };

  set_bool("denoise", device_.use_device_denoiser());
  set_bool("adaptiveSampling", false);
  set_bool("interactiveScaling", false);

  set_int("maxBounce", integrator->get_max_bounce());
  set_int("maxDiffuseBounce", integrator->get_max_diffuse_bounce());
  set_int("maxGlossyBounce", integrator->get_max_glossy_bounce());
  set_int("maxTransmissionBounce", integrator->get_max_transmission_bounce());
  set_int("maxVolumeBounce", integrator->get_max_volume_bounce());
  set_int("maxTransparencyBounce", integrator->get_transparent_max_bounce());
  set_float("clampDirect", integrator->get_sample_clamp_direct());
  set_float("clampIndirect", integrator->get_sample_clamp_indirect());
  set_bool("lightTree", integrator->get_use_light_tree());
  set_float("lightSamplingThreshold", integrator->get_light_sampling_threshold());
  set_bool("causticsReflective", integrator->get_caustics_reflective());
  set_bool("causticsRefractive", integrator->get_caustics_refractive());
  set_float("filterGlossy", integrator->get_filter_glossy());
  set_int("aoBounces", integrator->get_ao_bounces());
  set_float("aoFactor", integrator->get_ao_factor());
  set_float("aoDistance", integrator->get_ao_distance());

  const char *filter = "box";
  switch (film->get_filter_type()) {
    case FILTER_GAUSSIAN:
      filter = "gaussian";
      break;
    case FILTER_BLACKMAN_HARRIS:
      filter = "blackmanHarris";
      break;
    default:
      break;
  }
  anariSetParameter(d, renderer_, "pixelFilter", ANARI_STRING, filter);
  set_float("pixelFilterWidth", film->get_filter_width());
}

/* --------------------------------------------------------------------
 * Camera.
 */

void AnariScene::sync_camera(Scene *scene)
{
  Camera *camera = scene->camera;
  CameraState state;

  state.type = camera->get_camera_type();
  state.panorama_type = camera->get_panorama_type();

  const Transform &matrix = camera->get_matrix();
  state.position = transform_point(&matrix, zero_float3());
  state.direction = safe_normalize(transform_direction(&matrix, make_float3(0.0f, 0.0f, 1.0f)));
  state.up = safe_normalize(transform_direction(&matrix, make_float3(0.0f, 1.0f, 0.0f)));

  /* Extent of the full image in camera space, from the raster to camera transform which
   * includes the view plane, border and viewport camera offsets. */
  const float full_width = float(camera->get_full_width());
  const float full_height = float(camera->get_full_height());
  const float3 p0 = transform_perspective(&camera->full_rastertocamera, zero_float3());
  const float3 p1 = transform_perspective(&camera->full_rastertocamera,
                                          make_float3(full_width, full_height, 0.0f));

  if (state.type == CAMERA_PERSPECTIVE) {
    state.x0 = p0.x / p0.z;
    state.y0 = p0.y / p0.z;
    state.x1 = p1.x / p1.z;
    state.y1 = p1.y / p1.z;
    state.aperture_radius = camera->get_aperturesize();
    state.focus_distance = camera->get_focaldistance();
  }
  else if (state.type == CAMERA_ORTHOGRAPHIC) {
    state.x0 = p0.x;
    state.y0 = p0.y;
    state.x1 = p1.x;
    state.y1 = p1.y;
  }

  state.near_clip = camera->get_nearclip();
  state.far_clip = camera->get_farclip();

  if (!(state == camera_state_)) {
    camera_state_ = state;
    camera_modified_ = true;
  }
}

bool AnariScene::update_frame(ANARIFrame frame, const AnariFrameWindow &window)
{
  ANARIDevice d = anari();
  bool changed = false;

  const CameraState &state = camera_state_;
  const char *subtype = (state.type == CAMERA_ORTHOGRAPHIC) ? "orthographic" :
                        (state.type == CAMERA_PANORAMA)     ? "omnidirectional" :
                                                              "perspective";

  if (camera_ == nullptr || camera_subtype_ != subtype) {
    if (camera_) {
      anariRelease(d, camera_);
    }
    camera_ = anariNewCamera(d, subtype);
    camera_subtype_ = subtype;
    camera_modified_ = true;
    frame_objects_set_ = false;
  }

  if (!frame_objects_set_) {
    anariSetParameter(d, frame, "world", ANARI_WORLD, &world_);
    anariSetParameter(d, frame, "renderer", ANARI_RENDERER, &renderer_);
    anariSetParameter(d, frame, "camera", ANARI_CAMERA, &camera_);
    frame_objects_set_ = true;
    changed = true;
  }

  if (camera_modified_ || window != frame_window_) {
    /* Camera space extent of the frame window. */
    const float X0 = state.x0 + (state.x1 - state.x0) * window.x0;
    const float X1 = state.x0 + (state.x1 - state.x0) * window.x1;
    const float Y0 = state.y0 + (state.y1 - state.y0) * window.y0;
    const float Y1 = state.y0 + (state.y1 - state.y0) * window.y1;
    frame_x0_ = X0;
    frame_x1_ = X1;
    frame_y0_ = Y0;
    frame_y1_ = Y1;

    anariSetParameter(d, camera_, "position", ANARI_FLOAT32_VEC3, &state.position);
    anariSetParameter(d, camera_, "direction", ANARI_FLOAT32_VEC3, &state.direction);
    anariSetParameter(d, camera_, "up", ANARI_FLOAT32_VEC3, &state.up);

    if (state.type == CAMERA_PERSPECTIVE || state.type == CAMERA_ORTHOGRAPHIC) {
      /* Symmetric frustum covering the window, the window itself is selected with the image
       * region. The frustum has the aspect ratio of the window, so that the region is as wide
       * as it is high: several devices take the aspect ratio from the size of the frame instead
       * of the camera parameter, which is the same thing then. */
      const float window_aspect = max(X1 - X0, 1e-6f) / max(Y1 - Y0, 1e-6f);
      const float ty = max(
          max(max(fabsf(Y0), fabsf(Y1)), max(fabsf(X0), fabsf(X1)) / window_aspect), 1e-6f);
      const float tx = ty * window_aspect;
      const float aspect = window_aspect;
      anariSetParameter(d, camera_, "aspect", ANARI_FLOAT32, &aspect);

      if (state.type == CAMERA_PERSPECTIVE) {
        const float fovy = 2.0f * atanf(ty);
        anariSetParameter(d, camera_, "fovy", ANARI_FLOAT32, &fovy);
        if (state.aperture_radius > 0.0f) {
          anariSetParameter(
              d, camera_, "apertureRadius", ANARI_FLOAT32, &state.aperture_radius);
          anariSetParameter(d, camera_, "focusDistance", ANARI_FLOAT32, &state.focus_distance);
        }
        else {
          anariUnsetParameter(d, camera_, "apertureRadius");
          anariUnsetParameter(d, camera_, "focusDistance");
        }
      }
      else {
        const float height = 2.0f * ty;
        anariSetParameter(d, camera_, "height", ANARI_FLOAT32, &height);
      }

      const float region[4] = {
          (X0 / tx + 1.0f) * 0.5f, (Y0 / ty + 1.0f) * 0.5f, (X1 / tx + 1.0f) * 0.5f, (Y1 / ty + 1.0f) * 0.5f};
      const bool full_region = fabsf(region[0]) < 1e-5f && fabsf(region[1]) < 1e-5f &&
                               fabsf(region[2] - 1.0f) < 1e-5f && fabsf(region[3] - 1.0f) < 1e-5f;
      if (full_region) {
        anariUnsetParameter(d, camera_, "imageRegion");
      }
      else {
        anariSetParameter(d, camera_, "imageRegion", ANARI_FLOAT32_BOX2, region);
      }

      if (state.near_clip > 0.0f) {
        anariSetParameter(d, camera_, "near", ANARI_FLOAT32, &state.near_clip);
      }
      else {
        anariUnsetParameter(d, camera_, "near");
      }
      if (state.far_clip < FLT_MAX) {
        anariSetParameter(d, camera_, "far", ANARI_FLOAT32, &state.far_clip);
      }
      else {
        anariUnsetParameter(d, camera_, "far");
      }
    }
    else {
      const float region[4] = {window.x0, window.y0, window.x1, window.y1};
      anariSetParameter(d, camera_, "imageRegion", ANARI_FLOAT32_BOX2, region);
      anariSetParameter(d, camera_, "layout", ANARI_STRING, "equirectangular");
    }

    anariCommitParameters(d, camera_);
    camera_modified_ = false;
    frame_window_ = window;
    changed = true;
    backdrop_modified_ = true;
  }

  if (backdrop_modified_) {
    if (!backdrop_pixels_.empty()) {
      update_backdrop(window);
      changed = true;
    }
    backdrop_modified_ = false;
  }

  return changed;
}

float AnariScene::depth_to_camera_z(const float t, const int x, const int y) const
{
  if (camera_state_.type != CAMERA_PERSPECTIVE || frame_window_.width <= 0 ||
      frame_window_.height <= 0)
  {
    return t;
  }
  const float u = (x + 0.5f) / frame_window_.width;
  const float v = (y + 0.5f) / frame_window_.height;
  const float X = frame_x0_ + (frame_x1_ - frame_x0_) * u;
  const float Y = frame_y0_ + (frame_y1_ - frame_y0_) * v;
  /* Cosine of the angle between the ray and the camera axis. */
  return t / sqrtf(X * X + Y * Y + 1.0f);
}

CCL_NAMESPACE_END

#endif /* WITH_ANARI */
