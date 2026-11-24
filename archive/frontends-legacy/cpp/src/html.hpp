// HTML -> Node tree.  The document is parsed with Google's Gumbo (vendored in
// third_party/gumbo), which implements the HTML5 tree-construction algorithm
// and creates the implicit html/head/body nodes, matching html5lib used by the
// Python reference.
#pragma once

#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace wn {

struct Node {
  std::string tag;
  std::vector<std::pair<std::string, std::string>> attrs;
  Node* parent = nullptr;
  std::vector<std::unique_ptr<Node>> children;
  std::string text;  // character data before the first child element

  const std::string* attr(std::string_view name) const {
    for (const auto& [key, value] : attrs) {
      if (key == name) {
        return &value;
      }
    }
    return nullptr;
  }
};

std::unique_ptr<Node> parse_html(const std::string& html);
Node* find_body(Node* root);

// CSS sources in document order: <style> text and local <link rel=stylesheet>.
// `href` is resolved relative to `base_dir` (the HTML file's directory).
std::vector<std::string> stylesheet_texts(
    const Node& root, const std::filesystem::path& base_dir);

}  // namespace wn
