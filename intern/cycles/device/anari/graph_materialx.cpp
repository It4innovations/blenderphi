/* SPDX-FileCopyrightText: 2011-2026 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

#ifdef WITH_ANARI

#  include "device/anari/graph_export.h"

#  include "scene/shader.h"
#  include "scene/shader_graph.h"
#  include "scene/shader_nodes.h"

#  include "util/set.h"

CCL_NAMESPACE_BEGIN

bool anari_graph_from_materialx(const Shader *shader, AnariGraphSource &source)
{
  source = AnariGraphSource();

  const Shader::MaterialXDocument &document = shader->materialx;
  const ShaderGraph *graph = shader->graph.get();
  if (document.source.empty() || graph == nullptr) {
    source.failure = "no MaterialX document: a node is not implemented by the export";
    return false;
  }

  /* The document was written from the Blender node tree, its images are loaded by the image
   * texture nodes of the Cycles graph. Simplifying the graph may have removed some of them. */
  set<const ShaderNode *> nodes;
  for (const ShaderNode *node : graph->nodes) {
    nodes.insert(node);
  }
  for (const auto &image : document.images) {
    AnariGraphImage binding;
    binding.parameter = image.first;
    for (ShaderNode *node : image.second) {
      if (nodes.count(node) && node->special_type == SHADER_SPECIAL_TYPE_IMAGE_SLOT) {
        binding.node = static_cast<ImageSlotTextureNode *>(node);
        break;
      }
    }
    source.images.push_back(binding);
  }

  source.format = ANARI_GRAPH_MATERIALX;
  source.text = document.source;
  source.hash = anari_graph_hash(source.text);
  source.material_name = document.material_name;
  source.graph = graph;
  return true;
}

CCL_NAMESPACE_END

#endif /* WITH_ANARI */
