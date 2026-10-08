/* SPDX-FileCopyrightText: 2011-2026 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

#pragma once

#ifdef WITH_ANARI

#  include <anari/anari.h>

#  include "device/anari/device_impl.h"
#  include "device/anari/material.h"

#  include "util/map.h"
#  include "util/set.h"
#  include "util/transform.h"
#  include "util/types.h"
#  include "util/vector.h"

CCL_NAMESPACE_BEGIN

class Geometry;
class Hair;
class Light;
class Mesh;
class Object;
class PointCloud;
class PointLight;
class Volume;
class Scene;
class Shader;
class ShaderGraph;

/* Mirror of the Cycles scene as ANARI objects.
 *
 * The scene is exported after the Cycles scene device update, so that all the processing done by
 * the managers (subdivision, displacement, attribute requests, image loading, shader graph
 * optimization) is reflected in the exported data. */
class AnariScene {
 public:
  explicit AnariScene(AnariDevice &device);
  ~AnariScene();

  /* Record which scene nodes were modified, called before the scene device update. */
  void prepare_update(Scene *scene);
  /* Synchronize the ANARI objects, called after the scene device update. */
  void update(Scene *scene);

  /* Set world, renderer and camera on the frame and update the camera for the frame window.
   * Returns true when the frame needs to be committed. */
  bool update_frame(ANARIFrame frame, const AnariFrameWindow &window);

  /* Convert the distance along the ray stored in the ANARI depth channel to the Cycles depth
   * (distance along the camera axis) for the given pixel of the frame. */
  float depth_to_camera_z(float t, int x, int y) const;

 private:
  struct GeometryData {
    ANARIGroup group = nullptr;
    vector<ANARISurface> surfaces;
    vector<const Shader *> surface_shaders;
    /* Volume in its own group, its spatial field is in voxel index space: the instance
     * transform has to be the object transform times this one. */
    ANARIGroup volume_group = nullptr;
    Transform volume_transform = transform_identity();
  };

  struct CameraState {
    int type = 0;
    int panorama_type = 0;
    float3 position = zero_float3();
    float3 direction = make_float3(0.0f, 0.0f, -1.0f);
    float3 up = make_float3(0.0f, 1.0f, 0.0f);
    /* Extent of the full image in camera space, at distance 1 for perspective cameras. */
    float x0 = -1.0f, x1 = 1.0f, y0 = -1.0f, y1 = 1.0f;
    float aperture_radius = 0.0f;
    float focus_distance = 1.0f;
    float near_clip = 0.0f;
    float far_clip = FLT_MAX;

    bool operator==(const CameraState &other) const;
  };

  ANARIDevice anari() const
  {
    return device_.anari_device();
  }

  void release_geometry(GeometryData &data);
  void release_lights();

  string material_subtype_for(const AnariPrincipled &params) const;
  void sync_materials(Scene *scene, AnariMaterialBuilder &builder);
  void sync_geometry(Scene *scene, Geometry *geom, GeometryData &data);
  void export_mesh(Mesh *mesh, GeometryData &data);
  void export_hair(Hair *hair, GeometryData &data);
  void export_pointcloud(PointCloud *pointcloud, GeometryData &data);
  void add_surface(GeometryData &data, ANARIGeometry geometry, const Shader *shader);
  ANARIMaterial material_for_shader(const Shader *shader);

  void sync_lights(Scene *scene, AnariMaterialBuilder &builder);
  void add_light(Scene *scene, const Object *object, const Light *light, float3 strength);
  void export_volume(Scene *scene, Volume *volume, GeometryData &data);
  void set_light_soft_falloff(ANARILight anari_light, const PointLight *light);
  void add_emissive_quad(const float3 corner,
                         const float3 edge1,
                         const float3 edge2,
                         const float3 radiance);
  void sync_background(Scene *scene, AnariMaterialBuilder &builder);
  void sync_renderer(Scene *scene);
  void sync_renderer_cycles(Scene *scene);
  void sync_instances(Scene *scene);
  void sync_camera(Scene *scene);
  void commit_world();

  AnariDevice &device_;

  ANARIWorld world_ = nullptr;
  ANARIRenderer renderer_ = nullptr;
  ANARICamera camera_ = nullptr;
  string camera_subtype_;
  string material_subtype_;

  map<const Geometry *, GeometryData> geometries_;
  map<const Shader *, ANARIMaterial> materials_;
  map<const Shader *, const ShaderGraph *> material_graphs_;
  map<const Shader *, string> material_subtypes_;
  /* Shaders whose material object was replaced in this update. */
  set<const Shader *> recreated_materials_;
  ANARIMaterial default_material_ = nullptr;

  vector<ANARILight> lights_;
  ANARILight background_light_ = nullptr;
  /* Emissive geometry used for lights the device does not support. */
  vector<ANARIInstance> light_instances_;
  vector<ANARIInstance> instances_;

  AnariAttributeSlots slots_;
  int exported_slots_version_ = 0;
  AnariImageCache images_;

  /* Renderer background and ambient light. */
  float4 background_color_ = zero_float4();
  float3 ambient_radiance_ = zero_float3();

  /* Change tracking. */
  bool full_update_ = true;
  set<const Geometry *> modified_geometry_;
  set<const Shader *> modified_shaders_;
  bool objects_modified_ = false;
  bool lights_modified_ = false;
  bool background_modified_ = false;
  bool renderer_modified_ = false;
  size_t num_objects_ = 0;
  const ShaderGraph *background_graph_ = nullptr;

  /* Camera. */
  CameraState camera_state_;
  bool camera_modified_ = true;
  AnariFrameWindow frame_window_;
  /* Camera space extent of the current frame window. */
  float frame_x0_ = -1.0f, frame_x1_ = 1.0f, frame_y0_ = -1.0f, frame_y1_ = 1.0f;
  bool frame_objects_set_ = false;
};

CCL_NAMESPACE_END

#endif /* WITH_ANARI */
