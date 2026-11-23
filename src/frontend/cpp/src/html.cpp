#include "html.hpp"

#include <stdexcept>

#include "gumbo.h"

#include "util.hpp"

namespace wn {

namespace {

std::string tag_name(const GumboElement& element) {
  if (element.tag != GUMBO_TAG_UNKNOWN) {
    const char* name = gumbo_normalized_tagname(element.tag);
    if (name != nullptr && name[0] != '\0') {
      return std::string(name);
    }
  }
  return std::string(element.original_tag.data, element.original_tag.length);
}

std::unique_ptr<Node> convert(const GumboNode* source, Node* parent) {
  auto node = std::make_unique<Node>();
  node->parent = parent;

  const auto* element = &source->v.element;
  node->tag = tag_name(*element);
  const std::size_t attr_count = element->attributes.length;
  node->attrs.reserve(attr_count);
  for (std::size_t i = 0; i < attr_count; ++i) {
    const auto* attr =
        static_cast<const GumboAttribute*>(element->attributes.data[i]);
    const std::string name = attr->name != nullptr ? attr->name : "";
    const std::string value = attr->value != nullptr ? attr->value : "";
    bool duplicate = false;
    for (const auto& [existing, _] : node->attrs) {
      if (existing == name) {
        duplicate = true;
        break;
      }
    }
    if (!duplicate) {
      node->attrs.emplace_back(name, value);
    }
  }

  bool seen_element = false;
  const std::size_t child_count = element->children.length;
  for (std::size_t i = 0; i < child_count; ++i) {
    const auto* child =
        static_cast<const GumboNode*>(element->children.data[i]);
    if (child->type == GUMBO_NODE_ELEMENT ||
        child->type == GUMBO_NODE_TEMPLATE) {
      seen_element = true;
      node->children.push_back(convert(child, node.get()));
    } else if (!seen_element &&
               (child->type == GUMBO_NODE_TEXT ||
                child->type == GUMBO_NODE_WHITESPACE ||
                child->type == GUMBO_NODE_CDATA)) {
      if (child->v.text.text != nullptr) {
        node->text += child->v.text.text;
      }
    }
    // Comments are ignored, as the Python reference does.
  }
  return node;
}

}  // namespace

std::unique_ptr<Node> parse_html(const std::string& html) {
  GumboOutput* output = gumbo_parse(html.c_str());
  if (output == nullptr || output->root == nullptr) {
    if (output != nullptr) {
      gumbo_destroy_output(&kGumboDefaultOptions, output);
    }
    throw std::runtime_error("HTML parser produced no document root");
  }
  std::unique_ptr<Node> root;
  try {
    root = convert(output->root, nullptr);
  } catch (...) {
    gumbo_destroy_output(&kGumboDefaultOptions, output);
    throw;
  }
  gumbo_destroy_output(&kGumboDefaultOptions, output);
  return root;
}

namespace {

Node* find_body_impl(Node* root) {
  if (root == nullptr) {
    return nullptr;
  }
  if (root->tag == "body") {
    return root;
  }
  for (auto& child : root->children) {
    if (Node* found = find_body_impl(child.get())) {
      return found;
    }
  }
  return nullptr;
}

}  // namespace

Node* find_body(Node* root) {
  Node* body = find_body_impl(root);
  return body != nullptr ? body : root;
}

std::vector<std::string> stylesheet_texts(
    const Node& root, const std::filesystem::path& base_dir) {
  std::vector<std::string> texts;

  // Pre-order traversal, matching Node.walk() in the Python reference.
  const auto visit = [&](const auto& self, const Node& node) -> void {
    if (node.tag == "style") {
      texts.push_back(node.text);
    } else if (node.tag == "link") {
      const std::string* href = node.attr("href");
      const std::string* rel = node.attr("rel");
      const std::string rel_value = rel != nullptr ? lower_ascii(*rel) : "";
      bool is_stylesheet = false;
      for (const auto& token : split_whitespace(rel_value)) {
        if (token == "stylesheet") {
          is_stylesheet = true;
          break;
        }
      }
      if (is_stylesheet && href != nullptr && !href->empty() &&
          href->rfind("http:", 0) != 0 && href->rfind("https:", 0) != 0 &&
          href->rfind("//", 0) != 0) {
        const std::filesystem::path path = base_dir / *href;
        std::error_code ec;
        if (!std::filesystem::is_regular_file(path, ec)) {
          throw std::runtime_error("stylesheet not found: " + path_utf8(path));
        }
        texts.push_back(read_file(path));
      }
    }
    for (const auto& child : node.children) {
      self(self, *child);
    }
  };
  visit(visit, root);
  return texts;
}

}  // namespace wn
