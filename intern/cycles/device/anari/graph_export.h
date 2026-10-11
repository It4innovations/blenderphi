/* SPDX-FileCopyrightText: 2011-2026 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

#pragma once

#ifdef WITH_ANARI

#  include "util/string.h"
#  include "util/types.h"
#  include "util/vector.h"

CCL_NAMESPACE_BEGIN

class AnariAttributeSlots;
class ImageSlotTextureNode;
class Shader;
class ShaderGraph;

/* Devices which evaluate shader graphs themselves get the graph of a material as text instead of
 * a flattened `physicallyBased` material. Each format is a material subtype of the device. */
enum AnariGraphFormat {
  ANARI_GRAPH_NONE,
  /* Cycles shader graph as XML, material subtype `cyclesXml`. */
  ANARI_GRAPH_CYCLES_XML,
  /* MaterialX document, material subtype `materialx`. */
  ANARI_GRAPH_MATERIALX,
  /* MoonRay scene objects as RDLA, material subtype `rdla`. */
  ANARI_GRAPH_RDLA,
};

/* Material subtype which takes the graphs of the format. */
const char *anari_graph_subtype(AnariGraphFormat format);

/* Image used by a graph: the text refers to it by the name of the material parameter which
 * carries its sampler. */
struct AnariGraphImage {
  string parameter;
  ImageSlotTextureNode *node = nullptr;
};

/* Text form of the shader graph of one material. */
struct AnariGraphSource {
  AnariGraphFormat format = ANARI_GRAPH_NONE;
  string text;
  /* Hash of the text, an unchanged text is not sent to the device again. */
  uint64_t hash = 0;
  /* Name of the root object in the text, for formats which need one. */
  string material_name;
  vector<AnariGraphImage> images;
  /* Graph the text was written from, the image nodes belong to it. */
  const ShaderGraph *graph = nullptr;
  /* Why the graph could not be exported, for the trace of the scene export. */
  string failure;
  /* What the export approximated or left out, one sentence each. */
  vector<string> notes;

  /* For a surface which emits, and a device which samples such a surface with lights of its
   * own (the material parameter `emissionSampling`): the root object in the text which only
   * emits for the rays those lights are not seen by, and what the lights emit: a color, times
   * the map of this name in the text unless it is empty. Without an emission the name of the
   * root is empty. */
  string sampled_material_name;
  float3 emission_color = zero_float3();
  string emission_map;
};

uint64_t anari_graph_hash(const string &text);

/* Write the surface graph of the shader as XML for the Cycles of the device. The graph must not
 * be finalized yet: the device finalizes it itself. For the shader of a surface, attributes are
 * replaced by the generic ANARI attributes assigned in `slots`; light and world shaders are
 * written as they are. Returns false when the graph can't be exported, the material is flattened
 * then. */
bool anari_graph_write_cycles_xml(const Shader *shader,
                                  AnariAttributeSlots &slots,
                                  AnariGraphSource &source,
                                  bool is_surface = true);

/* Write the surface graph of the shader as MoonRay scene objects in RDLA: one DwaBaseMaterial
 * whose attributes are bound to maps of MoonRay and moonshine for the value nodes, and to the
 * AnariBumpNormalMap of openmoonray-anari for bump. A node without a counterpart is replaced by
 * the closest thing there is, which `source.notes` says. Returns false only when there is no
 * surface shader, the material is flattened then. */
bool anari_graph_write_rdla(const Shader *shader,
                            AnariAttributeSlots &slots,
                            AnariGraphSource &source);

/* Use the MaterialX document the scene synchronization stored in the shader, see
 * #Shader::materialx. Returns false when there is none. */
bool anari_graph_from_materialx(const Shader *shader, AnariGraphSource &source);

CCL_NAMESPACE_END

#endif /* WITH_ANARI */
