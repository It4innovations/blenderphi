/* SPDX-FileCopyrightText: 2011-2026 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

#pragma once

#ifdef WITH_ANARI

#  include <anari/anari.h>

#  include "device/anari/device_impl.h"
#  include "device/anari/graph_export.h"
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
struct KernelShaderEvalInput;

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

  /* Format in which the device takes the shader graphs of materials, ANARI_GRAPH_NONE when all
   * materials are flattened. */
  AnariGraphFormat graph_format() const
  {
    return graph_format_;
  }

  /* Convert the distance along the ray stored in the ANARI depth channel to the Cycles depth
   * (distance along the camera axis) for the given pixel of the frame. */
  float depth_to_camera_z(float t, int x, int y) const;

 private:
  struct GeometryData {
    ANARIGroup group = nullptr;
    vector<ANARISurface> surfaces;
    vector<const Shader *> surface_shaders;
    /* Materials of surfaces with a baked base color, which belong to the geometry. */
    vector<ANARIMaterial> materials;
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

  /* What was last sent for a material which gets its shader graph as text. */
  struct GraphMaterialState {
    bool sent = false;
    uint64_t hash = 0;
    vector<ANARISampler> samplers;
    /* The sides of the surface whose emission the device was asked to sample. */
    string emission_sampling;
  };

  string material_subtype_for(const AnariPrincipled &params) const;
  void sync_materials(Scene *scene, AnariMaterialBuilder &builder);
  void export_graph_sources(Scene *scene);
  const AnariGraphSource *graph_source_for(const Shader *shader) const;
  void set_graph_material(Scene *scene,
                          ANARIMaterial material,
                          const Shader *shader,
                          const AnariGraphSource &source);
  void update_graph_images(Scene *scene);
  /* Set the shader graph of a light or of the world on its object. */
  void set_graph_source(Scene *scene,
                        ANARIObject object,
                        const char *parameter,
                        const AnariGraphSource &source);
  void sync_geometry(Scene *scene, Geometry *geom, GeometryData &data);
  void export_mesh(Scene *scene, Mesh *mesh, GeometryData &data);
  /* Flattened materials whose base color is computed by nodes a sampler can't express get it
   * as vertex color, evaluated by the Cycles kernel at the corners of the triangles. */
  void prepare_base_color_bakes(Scene *scene);
  bool is_baked(const Shader *shader) const;
  bool bake_evaluate(Scene *scene,
                     const Mesh *mesh,
                     const vector<KernelShaderEvalInput> &inputs,
                     vector<float3> &colors);
  /* At the corners of the triangles, for vertex colors. */
  bool bake_base_color(Scene *scene,
                       const Mesh *mesh,
                       const vector<uint> &triangles,
                       vector<float3> &colors);
  /* Image with one square cell of texels per triangle, for meshes of few triangles where
   * vertex colors have no resolution. */
  struct BakedAtlas {
    int cell = 0;
    int columns = 0;
    int width = 0;
    int height = 0;
    /* What was baked: 8-bit sRGB pixels for a base color, float pixels for an emission and
     * 8-bit linear pixels for a mask. */
    AnariBakeMode mode = ANARI_BAKE_BASE_COLOR;
    /* Attribute slot of the texture coordinates. */
    int uv_slot = -1;
    vector<uchar> pixels;
    vector<float4> float_pixels;
  };
  bool bake_base_color_atlas(Scene *scene,
                             const Mesh *mesh,
                             const vector<uint> &triangles,
                             AnariBakeMode mode,
                             BakedAtlas &atlas);
  /* The flattened material of a baked shader, with the baked value from `baked`: an image
   * or the vertex colors. */
  AnariPrincipled baked_material(AnariMaterialBuilder &builder,
                                 const Shader *shader,
                                 const AnariValue &baked);
  ANARIMaterial new_atlas_material(Scene *scene, const Shader *shader, const BakedAtlas &atlas);
  /* A base color which is an image changed by nodes a sampler can't express keeps the image:
   * baking would blur it. Its average is corrected instead, with the color the kernel
   * evaluates at some triangles of the meshes which have the shader. */
  bool is_calibrated(const Shader *shader) const;
  /* Values of an attribute at some points of the meshes which have the shader, by the name a
   * sampler of the flattened material reads it with. For averages over the surface. */
  bool surface_attribute(Scene *scene,
                         const Shader *shader,
                         const string &attribute,
                         vector<float4> &values);
  void calibrate_base_color(Scene *scene,
                            AnariMaterialBuilder &builder,
                            const Shader *shader,
                            AnariPrincipled &params);
  void export_hair(Hair *hair, GeometryData &data);
  void export_pointcloud(PointCloud *pointcloud, GeometryData &data);
  void add_surface(GeometryData &data,
                   ANARIGeometry geometry,
                   const Shader *shader,
                   ANARIMaterial material = nullptr);
  ANARIMaterial material_for_shader(const Shader *shader);

  void sync_lights(Scene *scene, AnariMaterialBuilder &builder);
  void add_light(Scene *scene,
                 const Object *object,
                 const Light *light,
                 float3 strength,
                 const AnariGraphSource *source,
                 const vector<float> &falloff_distances,
                 const vector<packed_float3> &falloff_colors);
  void export_volume(Scene *scene, Volume *volume, GeometryData &data);
  /* On devices without ray visibility: export a small emissive mesh which is hidden from
   * camera or shadow rays as quad lights. Returns false when the object is not one. */
  bool add_light_card(AnariMaterialBuilder &builder, const Object *object);
  void set_light_soft_falloff(ANARILight anari_light, const PointLight *light);
  void add_emissive_quad(const float3 corner,
                         const float3 edge1,
                         const float3 edge2,
                         const float3 radiance);
  void sync_background(Scene *scene, AnariMaterialBuilder &builder);
  /* Set the background image of the renderer from the backdrop, for the camera and the window
   * of the frame. */
  void update_backdrop(const AnariFrameWindow &window);
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

  /* Format of the shader graphs the device takes, ANARI_GRAPH_NONE to flatten all materials. */
  AnariGraphFormat graph_format_ = ANARI_GRAPH_NONE;
  /* Graphs written before the scene update, for the shaders which can be exported as graph. */
  map<const Shader *, AnariGraphSource> graph_sources_;
  map<const Shader *, GraphMaterialState> graph_states_;
  /* Hash of the graph text the device could not evaluate, these shaders are flattened. */
  map<const Shader *, uint64_t> rejected_graphs_;
  /* Shaders whose base color is linked to their displacement output for baking, with the graph
   * it was done for. */
  map<const Shader *, const ShaderGraph *> baked_graphs_;
  /* Shaders whose base color is linked to their displacement output for the correction of its
   * average, with the graph it was done for. */
  map<const Shader *, const ShaderGraph *> calibrated_graphs_;
  /* Shaders whose baked color has to be evaluated again in this update. */
  set<const Shader *> rebake_shaders_;
  /* What is baked of the baked shaders. */
  map<const Shader *, AnariBakeMode> baked_modes_;
  /* Size of a texel of baked images in the scene. */
  float bake_texel_size_ = 0.0f;
  /* The device also takes the shader graphs of lights and of the world. */
  bool light_world_graphs_ = false;
  /* Light and world shaders which have an entry in `graph_sources_`. */
  set<const Shader *> light_world_shaders_;
  /* Samplers of the world shader graph which were set on the renderer. */
  vector<ANARISampler> background_samplers_;

  vector<ANARILight> lights_;
  ANARILight background_light_ = nullptr;
  /* Sun of a sky texture of a world shader which was baked into the background light. */
  ANARILight background_sun_light_ = nullptr;
  /* The device has the ray visibility flags of instances. */
  bool instance_visibility_ = false;
  /* The device takes the random number of an object for the Object Info node. */
  bool instance_object_random_ = false;
  /* The device has the ray visibility flags of lights. */
  bool light_visibility_ = false;
  /* The device has the bounce limit and the shadow switch of lights. */
  bool light_max_bounce_ = false;
  bool light_cast_shadow_ = false;
  /* The device takes what the shader of a light does to its emission over the distance, as a
   * ramp. */
  bool light_falloff_ = false;
  /* The device samples the emission of a surface with lights of its own when the material asks
   * for it: flattened materials and materials which are a graph. */
  bool emission_sampling_ = false;
  bool emission_sampling_graph_ = false;
  /* Objects which were exported as quad lights instead of geometry. */
  set<const Object *> light_cards_;
  /* Objects which cast no shadows and have a light inside (the globe of a lamp), on devices
   * without ray visibility: they stay, half transparent, and the light is that much stronger.
   * See #update_light_shells. */
  set<const Object *> light_shell_objects_;
  set<const Shader *> light_shell_shaders_;
  set<const Object *> light_shell_lights_;
  bool update_light_shells(Scene *scene);
  /* Emissive geometry used for lights the device does not support. */
  vector<ANARIInstance> light_instances_;
  vector<ANARIInstance> instances_;

  AnariShaderSlots slots_;
  /* Version of the slots of every shader which the geometry was exported with. */
  map<const Shader *, int> exported_slots_versions_;
  AnariImageCache images_;

  /* Renderer background and ambient light. */
  float4 background_color_ = zero_float4();
  float3 ambient_radiance_ = zero_float3();
  /* The world as camera rays see it, where that is not what lights the scene (a world shader
   * with a Light Path node, or a world which only the camera sees): an equirectangular map,
   * which is resampled for the camera into the background image of the renderer. */
  vector<float3> backdrop_pixels_;
  int backdrop_width_ = 0;
  int backdrop_height_ = 0;
  bool backdrop_modified_ = false;
  /* Viewport renders get a smaller background image, it is made again for every view. */
  bool backdrop_final_ = false;
  /* The renderer takes an image as background. */
  bool background_image_ = false;

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
