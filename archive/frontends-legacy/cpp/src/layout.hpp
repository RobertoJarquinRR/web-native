// Layout: block flow + flex row/column, a direct port of the v1 Python
// algorithm in pipeline/frontend/python/webnative/layout.py.
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "cascade.hpp"
#include "html.hpp"

namespace wn {

struct RectOp {
  double x = 0.0;
  double y = 0.0;
  double w = 0.0;
  double h = 0.0;
  std::string fill;
};

struct LayoutBox {
  const Style* style = nullptr;
  double x = 0.0;
  double y = 0.0;
  double width = 0.0;
  double height = 0.0;
  double margin_bottom = 0.0;
  std::string kind = "block";
  std::vector<std::unique_ptr<LayoutBox>> children;

  void translate(double dx, double dy);
};

std::unique_ptr<LayoutBox> layout_tree(const Node& root,
                                       const StyleMap& styles,
                                       double viewport_width);

// Pre-order rects: parent background, then children in document order.
std::vector<RectOp> paint(const LayoutBox& box);

}  // namespace wn
