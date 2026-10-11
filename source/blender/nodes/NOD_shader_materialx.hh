/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

/** \file
 * \ingroup nodes
 *
 * MaterialX export of shader node trees for render engines which take the document as text and
 * the images from Blender instead of from files. The headers of MaterialX are not needed to use
 * this.
 */

#include <string>
#include <utility>
#include <vector>

namespace blender {

struct Depsgraph;
struct Image;
struct Material;

namespace nodes::materialx {

struct InlineDocument {
  /** The document as XML. */
  std::string xml;
  /** Name of the `surfacematerial` node of the document. */
  std::string material_name;
  /**
   * Images of the document. The name is `<node name>/file`, and the `file` input of that image
   * node has the same string as its value: no file with that name exists.
   */
  std::vector<std::pair<std::string, Image *>> images;
};

/**
 * Export the node tree of the material. The active UV map is a `texcoord` node, other UV maps
 * are `geompropvalue` nodes with the name of the UV map.
 *
 * \return false when the material has no node tree, or when the tree has a linked node which
 * the export does not implement: the document would use a default value in its place.
 */
bool export_to_materialx_inline(const Depsgraph *depsgraph,
                                const Material *material,
                                InlineDocument &r_document);

}  // namespace nodes::materialx
}  // namespace blender
