/* SPDX-FileCopyrightText: 2011-2026 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

#ifdef WITH_ANARI

#  include "device/anari/scene.h"

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
#  include "util/progress.h"
#  include "util/projection.h"

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
  anariRelease(d, default_material_);
  if (camera_) {
    anariRelease(d, camera_);
  }
  anariRelease(d, renderer_);
  anariRelease(d, world_);
}

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

  ANARI_TRACE("update: materials");
  /* Materials first, they assign the attribute slots used by the geometry export. */
  sync_materials(scene, builder);

  const bool slots_changed = slots_.version != exported_slots_version_;
  exported_slots_version_ = slots_.version;

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

      bool need_export = full_update_ || is_new || slots_changed ||
                         modified_geometry_.count(geom) != 0;
      /* Rebuild surfaces when one of their materials was recreated. */
      for (const Node *node : geom->get_used_shaders()) {
        const Shader *shader = static_cast<const Shader *>(node);
        if ((modified_shaders_.count(shader) && materials_.find(shader) == materials_.end()) ||
            recreated_materials_.count(shader))
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
  if (full_update_ || lights_modified_ || objects_modified_) {
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
  ANARI_TRACE("update: done");

  full_update_ = false;
  modified_geometry_.clear();
  modified_shaders_.clear();
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

void AnariScene::sync_materials(Scene *scene, AnariMaterialBuilder &builder)
{
  ANARIDevice d = anari();
  recreated_materials_.clear();

  /* Only shaders of surfaces become materials, light and background shaders are exported with
   * the lights and the world. */
  set<const Shader *> surface_shaders;
  for (const Geometry *geom : scene->geometry) {
    if (geom->is_light()) {
      continue;
    }
    for (const Node *node : geom->get_used_shaders()) {
      surface_shaders.insert(static_cast<const Shader *>(node));
    }
  }

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

    const AnariPrincipled params = builder.evaluate_surface(shader);
    const string subtype = material_subtype_for(params);

    ANARIMaterial material = is_new ? nullptr : it->second;
    if (material && material_subtypes_[shader] != subtype) {
      /* The subtype of an ANARI object can't change, replace the material. */
      anariRelease(d, material);
      material = nullptr;
      recreated_materials_.insert(shader);
    }
    if (material == nullptr) {
      material = anariNewMaterial(d, subtype.c_str());
    }
    builder.set_material(material, subtype.c_str(), params);

    materials_[shader] = material;
    material_graphs_[shader] = graph;
    material_subtypes_[shader] = subtype;
  }

  for (auto it = materials_.begin(); it != materials_.end();) {
    if (current.find(it->first) == current.end()) {
      anariRelease(d, it->second);
      material_graphs_.erase(it->first);
      material_subtypes_.erase(it->first);
      it = materials_.erase(it);
    }
    else {
      ++it;
    }
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

void AnariScene::add_surface(GeometryData &data, ANARIGeometry geometry, const Shader *shader)
{
  ANARIDevice d = anari();

  anariCommitParameters(d, geometry);

  ANARISurface surface = anariNewSurface(d);
  anariSetParameter(d, surface, "geometry", ANARI_GEOMETRY, &geometry);
  ANARIMaterial material = material_for_shader(shader);
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
    export_mesh(static_cast<Mesh *>(geom), data);
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

void AnariScene::export_mesh(Mesh *mesh, GeometryData &data)
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

  /* Generic attributes requested by the materials. */
  const Attribute *attr_slots[AnariAttributeSlots::num_slots] = {attr_UV, nullptr, nullptr, nullptr};
  for (int i = 1; i < slots_.num_used; i++) {
    attr_slots[i] = mesh->attributes.find(slots_.slots[i]);
  }

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
    if (!shader_has_surface(shader)) {
      continue;
    }

    const vector<uint> &tris = it.second;
    const size_t num_verts = tris.size() * 3;

    vector<packed_float3> positions(num_verts);
    vector<packed_float3> normals(use_normals ? num_verts : 0);
    vector<float4> tangents((use_normals && attr_tangent) ? num_verts : 0);
    vector<float4> colors(attr_color ? num_verts : 0);
    vector<float4> slot_values[AnariAttributeSlots::num_slots];
    for (int s = 0; s < AnariAttributeSlots::num_slots; s++) {
      if (attr_slots[s]) {
        slot_values[s].resize(num_verts);
      }
    }

    for (size_t t = 0; t < tris.size(); t++) {
      const size_t tri = tris[t];
      const int v[3] = {triangles[tri * 3 + 0], triangles[tri * 3 + 1], triangles[tri * 3 + 2]};
      const float3 p0 = P[v[0]];
      const float3 p1 = P[v[1]];
      const float3 p2 = P[v[2]];
      const bool is_smooth = (tri < smooth.size()) ? smooth[tri] : false;
      const float3 face_normal = safe_normalize(cross(p1 - p0, p2 - p0));

      for (int c = 0; c < 3; c++) {
        const size_t out = t * 3 + c;
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

        if (!colors.empty()) {
          const int64_t index = mesh_attribute_index(*attr_color, mesh, tri, c);
          colors[out] = (index >= 0) ? attribute_value(*attr_color, index) : one_float4();
        }

        for (int s = 0; s < AnariAttributeSlots::num_slots; s++) {
          if (attr_slots[s]) {
            const int64_t index = mesh_attribute_index(*attr_slots[s], mesh, tri, c);
            slot_values[s][out] = (index >= 0) ? attribute_value(*attr_slots[s], index) :
                                                 zero_float4();
          }
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
    for (int s = 0; s < AnariAttributeSlots::num_slots; s++) {
      if (!slot_values[s].empty()) {
        const string name = string_printf("vertex.attribute%d", s);
        set_array_1d(d, geometry, name.c_str(), ANARI_FLOAT32_VEC4, slot_values[s]);
      }
    }

    add_surface(data, geometry, shader);
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

void AnariScene::sync_instances(Scene *scene)
{
  ANARIDevice d = anari();

  for (ANARIInstance instance : instances_) {
    anariRelease(d, instance);
  }
  instances_.clear();

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

    const float3 color = object->get_color();
    const float4 color4 = make_float4(color.x, color.y, color.z, object->get_alpha());
    anariSetParameter(d, instance, "color", ANARI_FLOAT32_VEC4, &color4);

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

void AnariScene::add_light(Scene * /*scene*/,
                           const Object *object,
                           const Light *light,
                           const float3 strength)
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
    anariCommitParameters(d, anari_light);
    lights_.push_back(anari_light);
  }
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

    /* Constant emission of the light node tree. */
    const Shader *shader = light->get_shader();
    if (shader && shader != scene->default_light) {
      const AnariPrincipled params = builder.evaluate_surface(const_cast<Shader *>(shader));
      if (params.has_emission && params.emissive.is_constant()) {
        strength *= make_float3(params.emissive.constant);
      }
    }

    if (is_zero(strength)) {
      continue;
    }
    add_light(scene, object, light, strength);
  }
}

/* --------------------------------------------------------------------
 * Background.
 */

void AnariScene::sync_background(Scene *scene, AnariMaterialBuilder &builder)
{
  ANARIDevice d = anari();

  if (background_light_) {
    anariRelease(d, background_light_);
    background_light_ = nullptr;
  }

  Background *background = scene->background;
  Shader *shader = background->get_shader(scene);
  const AnariMaterialBuilder::Background world = builder.evaluate_background(shader);

  const bool film_transparent = background->get_transparent();
  const bool camera_visible = (background->get_visibility() & PATH_RAY_VISIBILITY_CAMERA) != 0;

  const float3 constant = world.color * world.strength;
  background_color_ = (film_transparent || !camera_visible) ?
                          zero_float4() :
                          make_float4(constant.x, constant.y, constant.z, 1.0f);
  ambient_radiance_ = zero_float3();

  if (!device_.has_subtype(ANARI_LIGHT, "hdri")) {
    /* No environment lighting, approximate with ambient light. */
    ambient_radiance_ = constant;
    return;
  }

  ANARIArray2D radiance = nullptr;
  float scale = world.strength;
  float3 direction = make_float3(1.0f, 0.0f, 0.0f);
  float3 up = make_float3(0.0f, 0.0f, 1.0f);

  if (world.image) {
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
  const bool visible = camera_visible && !film_transparent;
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

  anariSetParameter(d, renderer_, "background", ANARI_FLOAT32_VEC4, &background_color_);

  const float ambient = max(max(ambient_radiance_.x, ambient_radiance_.y), ambient_radiance_.z);
  const float3 ambient_color = (ambient > 0.0f) ? ambient_radiance_ / ambient : one_float3();
  anariSetParameter(d, renderer_, "ambientRadiance", ANARI_FLOAT32, &ambient);
  anariSetParameter(d, renderer_, "ambientColor", ANARI_FLOAT32_VEC3, &ambient_color);

  const int pixel_samples = 1;
  anariSetParameter(d, renderer_, "pixelSamples", ANARI_INT32, &pixel_samples);

  const int max_depth = max(scene->integrator->get_max_bounce(), 1);
  anariSetParameter(d, renderer_, "maxRayDepth", ANARI_INT32, &max_depth);

  if (device_.library_name() == "barney") {
    /* Denoising is done by Cycles. */
    const bool denoise = false;
    anariSetParameter(d, renderer_, "denoise", ANARI_BOOL, &denoise);
  }
  else if (device_.library_name() == "cycles") {
    sync_renderer_cycles(scene);
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

  set_bool("denoise", false);
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
       * region. */
      const float tx = max(max(fabsf(X0), fabsf(X1)), 1e-6f);
      const float ty = max(max(fabsf(Y0), fabsf(Y1)), 1e-6f);
      const float aspect = tx / ty;
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
      if (state.far_clip < FLT_MAX) {
        anariSetParameter(d, camera_, "far", ANARI_FLOAT32, &state.far_clip);
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
