/* SPDX-FileCopyrightText: 2011-2022 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include <MaterialXFormat/XmlIo.h>

#include "node_parser.h"

#include "BKE_lib_id.hh"

#include "DEG_depsgraph.hh"

#include "DNA_material_types.h"

#include "NOD_shader.h"
#include "NOD_shader_materialx.hh"
#include "NOD_shader_nodes_inline.hh"

#include "material.h"

namespace blender::nodes::materialx {

class DefaultMaterialNodeParser : public NodeParser {
 public:
  using NodeParser::NodeParser;

  NodeItem compute() override
  {
    const Material *material = graph_.material;
    NodeItem surface = create_node(
        "open_pbr_surface",
        NodeItem::Type::SurfaceShader,
        {{"base_weight", val(1.0f)},
         {"base_color", val(MaterialX::Color3(material->r, material->g, material->b))},
         {"base_diffuse_roughness", val(material->roughness)},
         {"specular_weight", val(material->spec)},
         {"base_metalness", val(material->metallic)}});

    NodeItem res = create_node(
        "surfacematerial", NodeItem::Type::Material, {{"surfaceshader", surface}});
    return res;
  }

  NodeItem compute_error()
  {
    NodeItem surface = create_node("open_pbr_surface",
                                   NodeItem::Type::SurfaceShader,
                                   {{"base_color", val(MaterialX::Color3(1.0f, 0.0f, 1.0f))}});
    NodeItem res = create_node(
        "surfacematerial", NodeItem::Type::Material, {{"surfaceshader", surface}});
    return res;
  }
};

static MaterialX::DocumentPtr export_document(const Depsgraph *depsgraph,
                                              const Material *material,
                                              const ExportParams &export_params,
                                              bool *r_has_unsupported_nodes)
{
  CLOG_DEBUG(LOG_IO_MATERIALX, "Material: %s", material->id.name);

  MaterialX::DocumentPtr doc = MaterialX::createDocument();
  NodeItem output_item;

  NodeGraph graph(depsgraph, material, export_params, doc);

  bNodeTree *local_tree = bke::node_tree_add_tree(
      nullptr, "Inlined Tree", material->nodetree->idname);
  BLI_SCOPED_DEFER([&]() { BKE_id_free(nullptr, &local_tree->id); });

  InlineShaderNodeTreeParams params;
  inline_shader_node_tree(*material->nodetree, *local_tree, params);

  local_tree->ensure_topology_cache();
  bNode *output_node = ntreeShaderOutputNode(local_tree, SHD_OUTPUT_ALL);
  if (output_node && output_node->typeinfo->materialx_fn) {
    NodeParserData data = {graph, NodeItem::Type::Material, nullptr, graph.empty_node()};
    output_node->typeinfo->materialx_fn(&data, output_node, nullptr);
    output_item = data.result;
  }
  else {
    output_item = DefaultMaterialNodeParser(
                      graph, nullptr, nullptr, NodeItem::Type::Material, nullptr)
                      .compute_error();
  }

  /* This node is expected to have a specific name to link up to USD. */
  graph.set_output_node_name(output_item);

  CLOG_DEBUG(LOG_IO_MATERIALX,
             "Material: %s\n%s",
             material->id.name,
             MaterialX::writeToXmlString(doc).c_str());
  if (r_has_unsupported_nodes) {
    *r_has_unsupported_nodes = graph.has_unsupported_nodes;
  }
  return doc;
}

MaterialX::DocumentPtr export_to_materialx(const Depsgraph *depsgraph,
                                           const Material *material,
                                           const ExportParams &export_params)
{
  return export_document(depsgraph, material, export_params, nullptr);
}

bool export_to_materialx_inline(const Depsgraph *depsgraph,
                                const Material *material,
                                InlineDocument &r_document)
{
  r_document = InlineDocument();
  if (material->nodetree == nullptr) {
    return false;
  }

  /* The images get a placeholder as file name, which is replaced below once the names of the
   * image nodes are known. */
  const std::string placeholder = "image:";
  Vector<Image *> images;

  ExportParams params;
  params.output_node_name = "material";
  params.image_fn = [&](Main * /*bmain*/, Scene * /*scene*/, Image *image, ImageUser * /*iuser*/) {
    images.append(image);
    return placeholder + std::to_string(images.size() - 1);
  };
  /* Makes the active UV map a `texcoord` node. */
  params.new_active_uvmap_name = "st";

  bool has_unsupported_nodes = false;
  MaterialX::DocumentPtr doc = export_document(
      depsgraph, material, params, &has_unsupported_nodes);
  if (has_unsupported_nodes) {
    /* The document would render with default values where the nodes are: leave it to the
     * caller to use another representation of the material. */
    return false;
  }

  for (const MaterialX::NodePtr &node : doc->getNodes()) {
    MaterialX::InputPtr file = node->getInput("file");
    if (!file) {
      continue;
    }
    const std::string value = file->getValueString();
    if (value.compare(0, placeholder.size(), placeholder) != 0) {
      continue;
    }
    const int index = atoi(value.c_str() + placeholder.size());
    if (index < 0 || index >= images.size()) {
      continue;
    }
    const std::string name = node->getName() + "/file";
    file->setValueString(name);
    r_document.images.emplace_back(name, images[index]);
  }

  r_document.xml = MaterialX::writeToXmlString(doc);
  r_document.material_name = params.output_node_name;
  return true;
}

}  // namespace blender::nodes::materialx
