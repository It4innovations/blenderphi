/* SPDX-FileCopyrightText: 2011-2026 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

#ifdef WITH_ANARI

#  include "device/anari/graph_export.h"
#  include "device/anari/material.h"

#  include "scene/shader.h"
#  include "scene/shader_graph.h"
#  include "scene/shader_nodes.h"

#  include "util/colorspace.h"
#  include "util/log.h"
#  include "util/map.h"
#  include "util/set.h"
#  include "util/transform.h"

CCL_NAMESPACE_BEGIN

const char *anari_graph_subtype(const AnariGraphFormat format)
{
  switch (format) {
    case ANARI_GRAPH_CYCLES_XML:
      return "cyclesXml";
    case ANARI_GRAPH_MATERIALX:
      return "materialx";
    case ANARI_GRAPH_RDLA:
      return "rdla";
    case ANARI_GRAPH_NONE:
      break;
  }
  return "";
}

uint64_t anari_graph_hash(const string &text)
{
  /* FNV-1a. */
  uint64_t hash = 14695981039346656037ull;
  for (const char c : text) {
    hash ^= uint64_t(uchar(c));
    hash *= 1099511628211ull;
  }
  return hash;
}

namespace {

/* The text is built by appending to strings, without an XML library: it is written on every
 * material edit in the viewport. */

void append_escaped(string &out, const string_view text)
{
  for (const char c : text) {
    switch (c) {
      case '&':
        out += "&amp;";
        break;
      case '<':
        out += "&lt;";
        break;
      case '>':
        out += "&gt;";
        break;
      case '"':
        out += "&quot;";
        break;
      default:
        /* Control characters are not valid in XML. */
        out += (uchar(c) < 0x20) ? '_' : c;
        break;
    }
  }
}

void append_float(string &out, const float value)
{
  char buffer[32];
  const int length = snprintf(buffer, sizeof(buffer), "%.9g", double(value));
  out.append(buffer, size_t(max(length, 0)));
}

void append_floats(string &out, const float *values, const int num)
{
  for (int i = 0; i < num; i++) {
    if (i) {
      out += ' ';
    }
    append_float(out, values[i]);
  }
}

/* Colors and vectors have four components in the XML of the device. */
void append_float3(string &out, const float3 value)
{
  const float values[4] = {value.x, value.y, value.z, 0.0f};
  append_floats(out, values, 4);
}

void append_transform(string &out, const Transform &tfm)
{
  const float values[12] = {tfm.x.x,
                            tfm.x.y,
                            tfm.x.z,
                            tfm.x.w,
                            tfm.y.x,
                            tfm.y.y,
                            tfm.y.z,
                            tfm.y.w,
                            tfm.z.x,
                            tfm.z.y,
                            tfm.z.z,
                            tfm.z.w};
  append_floats(out, values, 12);
}

/* Append the value of a socket, returns false for types which have no text form. */
bool append_socket_value(string &out, const Node *node, const SocketType &socket)
{
  switch (socket.type) {
    case SocketType::BOOLEAN:
      out += node->get_bool(socket) ? "true" : "false";
      return true;
    case SocketType::FLOAT:
      append_float(out, node->get_float(socket));
      return true;
    case SocketType::INT:
      out += to_string(node->get_int(socket));
      return true;
    case SocketType::UINT:
      out += to_string(node->get_uint(socket));
      return true;
    case SocketType::UINT64:
      out += to_string(node->get_uint64(socket));
      return true;
    case SocketType::COLOR:
    case SocketType::VECTOR:
    case SocketType::POINT:
    case SocketType::NORMAL:
      append_float3(out, node->get_float3(socket));
      return true;
    case SocketType::POINT2: {
      const float2 value = node->get_float2(socket);
      append_floats(out, &value.x, 2);
      return true;
    }
    case SocketType::STRING:
    case SocketType::ENUM:
      append_escaped(out, node->get_string(socket).string());
      return true;
    case SocketType::TRANSFORM:
      append_transform(out, node->get_transform(socket));
      return true;
    case SocketType::BOOLEAN_ARRAY: {
      const array<bool> &values = node->get_bool_array(socket);
      for (size_t i = 0; i < values.size(); i++) {
        out += (i == 0) ? "" : " ";
        out += values[i] ? '1' : '0';
      }
      return true;
    }
    case SocketType::INT_ARRAY: {
      const array<int> &values = node->get_int_array(socket);
      for (size_t i = 0; i < values.size(); i++) {
        out += (i == 0) ? "" : " ";
        out += to_string(values[i]);
      }
      return true;
    }
    case SocketType::FLOAT_ARRAY: {
      const array<float> &values = node->get_float_array(socket);
      append_floats(out, values.data(), int(values.size()));
      return true;
    }
    case SocketType::COLOR_ARRAY:
    case SocketType::VECTOR_ARRAY:
    case SocketType::POINT_ARRAY:
    case SocketType::NORMAL_ARRAY: {
      const array<packed_float3> &values = node->get_float3_array(socket);
      for (size_t i = 0; i < values.size(); i++) {
        out += (i == 0) ? "" : " ";
        append_float3(out, values[i]);
      }
      return true;
    }
    case SocketType::POINT2_ARRAY: {
      const array<float2> &values = node->get_float2_array(socket);
      for (size_t i = 0; i < values.size(); i++) {
        out += (i == 0) ? "" : " ";
        append_floats(out, &values[i].x, 2);
      }
      return true;
    }
    case SocketType::TRANSFORM_ARRAY: {
      const array<Transform> &values = node->get_transform_array(socket);
      for (size_t i = 0; i < values.size(); i++) {
        out += (i == 0) ? "" : " ";
        append_transform(out, values[i]);
      }
      return true;
    }
    case SocketType::STRING_ARRAY:
    case SocketType::NODE:
    case SocketType::NODE_ARRAY:
    case SocketType::CLOSURE:
    case SocketType::UNDEFINED:
    case SocketType::NUM_TYPES:
      break;
  }
  return false;
}

void append_socket(string &out, const string_view name, const string_view value)
{
  out += "<socket name=\"";
  append_escaped(out, name);
  out += "\" value=\"";
  append_escaped(out, value);
  out += "\"/>";
}

/* Attribute of the device geometry for an ANARI attribute slot. */
string slot_attribute(const int slot)
{
  return string_printf("vertex.attribute%d", slot);
}

class CyclesXmlWriter {
 public:
  CyclesXmlWriter(const Shader *shader,
                  AnariAttributeSlots &slots,
                  AnariGraphSource &source,
                  const bool is_surface)
      : shader_(shader), slots_(slots), source_(source), is_surface_(is_surface)
  {
  }

  bool write()
  {
    ShaderGraph *graph = shader_->graph.get();
    if (graph == nullptr || graph->finalized) {
      source_.failure = "graph already finalized";
      return false;
    }
    OutputNode *output = graph->output();
    /* A shader with only a volume is the homogeneous volume of a mesh, which the device renders
     * like the surface shaders. */
    if (output == nullptr ||
        (output->input("Surface")->link == nullptr && output->input("Volume")->link == nullptr))
    {
      source_.failure = "no surface";
      return false;
    }
    if (!collect(output)) {
      return false;
    }

    /* Names in the order of the graph, so the same graph gives the same text. */
    int index = 0;
    for (ShaderNode *node : graph->nodes) {
      if (node != output && used_.count(node) && !is_replaced(node)) {
        names_[node] = string_printf("n%d", index++);
      }
    }
    names_[output] = "output";

    for (ShaderNode *node : graph->nodes) {
      if (names_.count(node) == 0) {
        continue;
      }
      if (node != output) {
        write_node(node);
      }
      write_links(node, is_surface_ && !graph->simplified);
    }

    string &text = source_.text;
    text.clear();
    text.reserve(64 + nodes_.size() + connects_.size());
    text += "<shader name=\"";
    append_escaped(text, shader_->name.string());
    text += "\">";
    write_shader_sockets(text);
    text += nodes_;
    text += connects_;
    text += "</shader>";

    source_.format = ANARI_GRAPH_CYCLES_XML;
    source_.hash = anari_graph_hash(text);
    source_.graph = graph;
    return true;
  }

 private:
  /* Links which are not written: the copies made for bump mapping, and a displacement Cycles
   * already applied to the vertices of the mesh (the device would add bump mapping from it). */
  bool is_bump_sample(const ShaderInput *input) const
  {
    const ustring name = input->socket_type.name;
    if (input->parent->special_type == SHADER_SPECIAL_TYPE_OUTPUT) {
      return name == "displacement" && shader_->get_displacement_method() == DISPLACE_TRUE;
    }
    return input->parent->special_type == SHADER_SPECIAL_TYPE_BUMP &&
           (name == "sample_x" || name == "sample_y");
  }

  /* Nodes which read a named attribute are replaced by an attribute node for its ANARI slot. */
  bool is_replaced(const ShaderNode *node) const
  {
    return is_surface_ && node->type == UVMapNode::get_node_type() &&
           !static_cast<const UVMapNode *>(node)->get_from_dupli();
  }

  /* Find the nodes the output depends on. The copies made for bump mapping are left out: the
   * device makes its own when it simplifies the graph. */
  bool collect(ShaderNode *node)
  {
    if (!used_.insert(node).second) {
      return true;
    }

    if (node->special_type == SHADER_SPECIAL_TYPE_OSL) {
      source_.failure = "OSL script node";
      return false;
    }
    if (node->special_type == SHADER_SPECIAL_TYPE_IMAGE_SLOT) {
      const bool is_image = node->type == ImageTextureNode::get_node_type();
      if (!is_image && node->type != EnvironmentTextureNode::get_node_type()) {
        source_.failure = node->type->name.string();
        return false;
      }
      if (is_image && static_cast<ImageTextureNode *>(node)->get_tiles().size() > 1) {
        source_.failure = "tiled image";
        return false;
      }
    }

    for (ShaderInput *input : node->inputs) {
      if (input->link && !is_bump_sample(input)) {
        if (!collect(input->link->parent)) {
          return false;
        }
      }
    }
    return true;
  }

  const string &attribute_node(const string &attribute)
  {
    auto it = attribute_nodes_.find(attribute);
    if (it != attribute_nodes_.end()) {
      return it->second;
    }
    const string name = string_printf("a%d", int(attribute_nodes_.size()));
    nodes_ += "<attribute name=\"" + name + "\">";
    append_socket(nodes_, "attribute", attribute);
    nodes_ += "</attribute>";
    return attribute_nodes_.emplace(attribute, name).first->second;
  }

  /* Node and socket a link comes from, with the texture coordinates of the mesh replaced by
   * the ANARI attributes. */
  void link_source(const ShaderOutput *output, string &from_node, string &from_socket)
  {
    const ShaderNode *node = output->parent;
    const ustring name = output->socket_type.name;

    if (is_surface_ && node->type == TextureCoordinateNode::get_node_type() &&
        !static_cast<const TextureCoordinateNode *>(node)->get_from_dupli())
    {
      if (name == "UV") {
        from_node = attribute_node(slot_attribute(0));
        from_socket = "vector";
        return;
      }
      if (name == "generated") {
        const int slot = slots_.slot_for(ustring("generated"));
        if (slot > 0) {
          from_node = attribute_node(slot_attribute(slot));
          from_socket = "vector";
        }
        else {
          from_node = names_[node];
          from_socket = "object";
        }
        return;
      }
    }
    if (is_surface_ && node->type == GeometryNode::get_node_type() &&
        (name == "pointiness" || name == "random_per_island"))
    {
      /* Values Cycles computes for the mesh, the device does not have them. */
      const int slot = slots_.slot_for(name);
      if (slot > 0) {
        from_node = attribute_node(slot_attribute(slot));
        from_socket = "fac";
        return;
      }
    }
    if (is_surface_ && node->type == HairInfoNode::get_node_type()) {
      /* What Cycles keeps as attributes of the curves, the device does not have them. */
      const char *attribute = (name == "intercept") ? "curve_intercept" :
                              (name == "size")      ? "curve_length" :
                              (name == "index")     ? "curve_random" :
                                                      nullptr;
      const int slot = attribute ? slots_.slot_for(ustring(attribute)) : -1;
      if (slot > 0) {
        from_node = attribute_node(slot_attribute(slot));
        from_socket = "fac";
        return;
      }
    }
    if (is_replaced(node)) {
      const int slot = slots_.slot_for(static_cast<const UVMapNode *>(node)->get_attribute());
      from_node = attribute_node(slot_attribute(max(slot, 0)));
      from_socket = "vector";
      return;
    }

    from_node = names_[node];
    from_socket = name.string();
  }

  void append_connect(const string &from_node,
                      const string &from_socket,
                      const string &to_node,
                      const string_view to_socket)
  {
    connects_ += "<connect from_node=\"" + from_node + "\" from_socket=\"";
    append_escaped(connects_, from_socket);
    connects_ += "\" to_node=\"" + to_node + "\" to_socket=\"";
    append_escaped(connects_, to_socket);
    connects_ += "\"/>";
  }

  void write_links(ShaderNode *node, const bool default_links)
  {
    const string &to_node = names_[node];
    string from_node;
    string from_socket;

    for (const ShaderInput *input : node->inputs) {
      string_view to_socket = input->socket_type.name.string();

      if (input->link == nullptr) {
        /* Default texture coordinates, when the graph was not simplified yet: in the viewport
         * that is only done before compiling. The other defaults don't need attributes, the
         * device sets them. */
        if (default_links && (input->flags() & SocketType::LINK_TEXTURE_UV)) {
          append_connect(attribute_node(slot_attribute(0)), "vector", to_node, to_socket);
        }
        else if (default_links && (input->flags() & SocketType::LINK_TEXTURE_GENERATED)) {
          const int slot = slots_.slot_for(ustring("generated"));
          if (slot > 0) {
            append_connect(attribute_node(slot_attribute(slot)), "vector", to_node, to_socket);
          }
        }
        continue;
      }
      if (is_bump_sample(input)) {
        continue;
      }
      if (node->special_type == SHADER_SPECIAL_TYPE_BUMP && to_socket == "sample_center") {
        /* Undo the bump refinement, see #ShaderGraph::refine_bump_nodes. */
        to_socket = "height";
      }

      link_source(input->link, from_node, from_socket);
      append_connect(from_node, from_socket, to_node, to_socket);
    }
  }

  static const ShaderInput *find_input(const ShaderNode *node, const SocketType &socket)
  {
    for (const ShaderInput *input : node->inputs) {
      if (&input->socket_type == &socket) {
        return input;
      }
    }
    return nullptr;
  }

  /* Name of the device attribute for a geometry attribute, empty to keep the name. */
  string geometry_attribute(const ustring name)
  {
    /* Object, instancer and view layer attributes have a prefix, see
     * #blender_attribute_name_add_type. */
    if (name.empty() || uchar(name[0]) < 0x20) {
      return string();
    }
    const int slot = slots_.slot_for(name);
    return (slot >= 0) ? slot_attribute(slot) : string();
  }

  void write_node(ShaderNode *node)
  {
    const NodeType *type = node->type;
    string &out = nodes_;

    out += '<';
    out += type->name.string();
    out += " name=\"" + names_[node] + "\">";

    /* Sockets which are written with another value, or not at all when the value is empty. */
    ustring override_name;
    string override_value;
    const bool is_image = node->special_type == SHADER_SPECIAL_TYPE_IMAGE_SLOT;

    if (type == SkyTextureNode::get_node_type()) {
      /* Simplifying the graph mirrors the rotation of the sun, see
       * #SkyTextureNode::simplify_settings, and the device simplifies it once more. */
      override_name = ustring("sun_rotation");
      char value[32];
      snprintf(value,
               sizeof(value),
               "%.9g",
               double(M_2PI_F - static_cast<SkyTextureNode *>(node)->get_sun_rotation()));
      override_value = value;
    }
    else if (!is_surface_) {
      /* Light and world shaders have no geometry attributes to replace. */
    }
    else if (type == AttributeNode::get_node_type()) {
      override_value = geometry_attribute(static_cast<AttributeNode *>(node)->get_attribute());
      if (!override_value.empty()) {
        override_name = ustring("attribute");
      }
    }
    else if (type == VertexColorNode::get_node_type()) {
      const ustring layer = static_cast<VertexColorNode *>(node)->get_layer_name();
      override_name = ustring("layer_name");
      override_value = layer.empty() ? string() : geometry_attribute(layer);
      if (override_value.empty()) {
        /* The default color attribute is the ANARI vertex color. */
        override_value = "vertex.color";
      }
    }
    else if (type == NormalMapNode::get_node_type() || type == TangentNode::get_node_type()) {
      /* Only the tangents of the default UV map are exported. */
      override_name = ustring("attribute");
    }

    for (const SocketType &socket : type->inputs) {
      if (socket.type == SocketType::CLOSURE || socket.type == SocketType::UNDEFINED ||
          (socket.flags & SocketType::INTERNAL))
      {
        continue;
      }
      if (socket.name == override_name) {
        if (!override_value.empty()) {
          append_socket(out, socket.name.string(), override_value);
        }
        continue;
      }
      if (is_image && (socket.name == "filename" || socket.name == "colorspace" ||
                       socket.name == "alpha_type" || socket.name == "tiles"))
      {
        /* The pixels are sent as loaded by Cycles: already in the color space of the scene and
         * with the alpha associated. These sockets are written below. */
        continue;
      }
      const ShaderInput *input = find_input(node, socket);
      if ((input && input->link) || node->has_default_value(socket)) {
        continue;
      }

      const size_t start = out.size();
      out += "<socket name=\"";
      append_escaped(out, socket.name.string());
      out += "\" value=\"";
      if (append_socket_value(out, node, socket)) {
        out += "\"/>";
      }
      else {
        out.resize(start);
      }
    }

    if (is_image) {
      ImageSlotTextureNode *image = static_cast<ImageSlotTextureNode *>(node);
      /* Images of files only get their handle when the shader is compiled, in the scene update:
       * the sampler is looked up afterwards, through the node. */
      const bool is_image_texture = type == ImageTextureNode::get_node_type();
      const ustring filename = is_image_texture ?
                                   static_cast<ImageTextureNode *>(node)->get_filename() :
                                   static_cast<EnvironmentTextureNode *>(node)->get_filename();
      const ustring colorspace =
          is_image_texture ? static_cast<ImageTextureNode *>(node)->get_colorspace() :
                             static_cast<EnvironmentTextureNode *>(node)->get_colorspace();
      const ImageAlphaType alpha_type =
          is_image_texture ? static_cast<ImageTextureNode *>(node)->get_alpha_type() :
                             static_cast<EnvironmentTextureNode *>(node)->get_alpha_type();

      /* The device must not associate the alpha again. What the node still decides is whether
       * the kernel divides the color by the alpha when the alpha output is used: not for data
       * and channel packed images. */
      append_socket(out,
                    "alpha_type",
                    (alpha_type == IMAGE_ALPHA_CHANNEL_PACKED) ? "channel_packed" :
                    (alpha_type == IMAGE_ALPHA_IGNORE)         ? "ignore" :
                                                                 "associated");
      if (ColorSpaceManager::colorspace_is_data(colorspace)) {
        append_socket(out, "colorspace", u_colorspace_data.string());
      }
      if (!image->handle.empty() || !filename.empty()) {
        /* A file name which is the name of a material parameter refers to its image. */
        AnariGraphImage binding;
        binding.parameter = string_printf("image.%d", int(source_.images.size()));
        binding.node = image;
        append_socket(out, "filename", binding.parameter);
        source_.images.push_back(binding);
      }
    }

    out += "</";
    out += type->name.string();
    out += '>';
  }

  void write_shader_sockets(string &out)
  {
    for (const SocketType &socket : shader_->type->inputs) {
      /* The device never displaces: its mesh is the one of the Cycles scene. Bump mapping from
       * the displacement is the default. */
      if (socket.name == "displacement_method" || socket.name == "pass_id" ||
          shader_->has_default_value(socket))
      {
        continue;
      }
      const size_t start = out.size();
      out += "<socket name=\"";
      append_escaped(out, socket.name.string());
      out += "\" value=\"";
      if (append_socket_value(out, shader_, socket)) {
        out += "\"/>";
      }
      else {
        out.resize(start);
      }
    }
  }

  const Shader *shader_;
  AnariAttributeSlots &slots_;
  AnariGraphSource &source_;
  /* Shader of a surface: the attributes of the mesh are replaced by the ANARI attributes. */
  bool is_surface_;

  set<const ShaderNode *> used_;
  map<const ShaderNode *, string> names_;
  /* Attribute name to the name of the node which reads it. */
  map<string, string> attribute_nodes_;

  string nodes_;
  string connects_;
};

}  // namespace

bool anari_graph_write_cycles_xml(const Shader *shader,
                                  AnariAttributeSlots &slots,
                                  AnariGraphSource &source,
                                  const bool is_surface)
{
  source = AnariGraphSource();
  CyclesXmlWriter writer(shader, slots, source, is_surface);
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
