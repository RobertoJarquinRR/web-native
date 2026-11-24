// Selector matching + cascade -> computed style per node.
// Mirrors pipeline/frontend/python/webnative/cascade.py: specificity key
// (important, inline, ids, classes, types, rule order, declaration index),
// inheritance of the same property set, UA display defaults and the small
// value normalisation used by layout.
#pragma once

#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "css.hpp"
#include "html.hpp"

namespace wn {

struct Style {
  std::string display = "block";           // block | flex | inline | none | ...
  double margin[4] = {0.0, 0.0, 0.0, 0.0};  // top, right, bottom, left
  double padding[4] = {0.0, 0.0, 0.0, 0.0};
  std::optional<double> width;
  std::optional<double> height;
  std::optional<std::string> background;   // nullopt = no background
  std::string color = "#000000";
  double font_size = 16.0;
  std::string font_family = "sans-serif";
  std::string flex_direction = "row";
  std::string justify_content = "flex-start";
  std::string align_items = "stretch";
  double gap = 0.0;
};

using StyleMap = std::unordered_map<const Node*, Style>;

bool selector_matches(const Node& node, const ComplexSelector& selector);

StyleMap compute_styles(const Node& root, const std::vector<Rule>& rules);

}  // namespace wn
