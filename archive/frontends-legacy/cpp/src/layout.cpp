#include "layout.hpp"

#include <algorithm>
#include <optional>
#include <stdexcept>
#include <utility>

namespace wn {

namespace {

std::vector<const Node*> renderable_children(const Node& node,
                                             const StyleMap& styles) {
  std::vector<const Node*> children;
  for (const auto& child : node.children) {
    const auto found = styles.find(child.get());
    if (found == styles.end()) {
      continue;
    }
    if (found->second.display == "block" || found->second.display == "flex") {
      children.push_back(child.get());
    }
  }
  return children;
}

// (start offset, spacing) on the main axis for justify-content.
std::pair<double, double> justify(const std::string& name, double free,
                                  std::size_t count, double gap) {
  if (count <= 1) {
    const double start =
        name == "center" ? free / 2.0
        : (name == "flex-end" || name == "end") ? free
                                                : 0.0;
    return {start, gap};
  }
  if (name == "center") {
    return {free / 2.0, gap};
  }
  if (name == "flex-end" || name == "end") {
    return {free, gap};
  }
  if (name == "space-between") {
    return {0.0, gap + free / static_cast<double>(count - 1)};
  }
  return {0.0, gap};  // flex-start/start and unsupported values
}

std::unique_ptr<LayoutBox> layout_box(const Node& node, const Style& style,
                                      const StyleMap& styles, double origin_x,
                                      double origin_y, double available_width);

// Places flex children and returns the consumed size on the main axis.
double layout_flex_children(LayoutBox& parent_box,
                            const std::vector<const Node*>& children,
                            const StyleMap& styles, double content_x,
                            double content_y, double content_width,
                            std::optional<double> content_height,
                            const Style& parent) {
  if (children.empty()) {
    return 0.0;
  }
  const bool row = parent.flex_direction != "column";
  const double gap = std::max(parent.gap, 0.0);

  std::vector<std::unique_ptr<LayoutBox>> measured;
  measured.reserve(children.size());
  for (const Node* child : children) {
    const Style& child_style = styles.at(child);
    measured.push_back(layout_box(*child, child_style, styles, 0.0, 0.0,
                                  content_width));
  }

  std::vector<double> main_sizes;
  std::vector<double> cross_sizes;
  main_sizes.reserve(measured.size());
  cross_sizes.reserve(measured.size());
  for (const auto& box : measured) {
    main_sizes.push_back(row ? box->width : box->height);
    cross_sizes.push_back(row ? box->height : box->width);
  }

  double extent = 0.0;
  std::optional<double> main_available;
  if (row) {
    extent = content_height.value_or(
        cross_sizes.empty()
            ? 0.0
            : *std::max_element(cross_sizes.begin(), cross_sizes.end()));
    main_available = content_width;
  } else {
    extent = content_width;
    if (content_height) {
      main_available = *content_height;
    } else {
      double total = 0.0;
      for (double size : main_sizes) {
        total += size;
      }
      main_available =
          total + gap * static_cast<double>(measured.size() - 1);
    }
  }

  double used = 0.0;
  for (double size : main_sizes) {
    used += size;
  }
  used += gap * static_cast<double>(measured.size() - 1);
  const double free =
      main_available ? std::max(*main_available - used, 0.0) : 0.0;
  const auto [start, spacing] =
      justify(parent.justify_content, free, measured.size(), gap);

  double cursor = 0.0;
  for (std::size_t i = 0; i < measured.size(); ++i) {
    LayoutBox& box = *measured[i];
    const Style& child_style = styles.at(children[i]);
    double cross_size = cross_sizes[i];
    if (parent.align_items == "stretch" || parent.align_items == "normal") {
      if (row && !child_style.height) {
        box.height = extent;
        cross_size = extent;
      } else if (!row && !child_style.width) {
        box.width = extent;
        cross_size = extent;
      }
    }
    double cross_offset = 0.0;
    if (parent.align_items == "center") {
      cross_offset = (extent - cross_size) / 2.0;
    } else if (parent.align_items == "flex-end" ||
               parent.align_items == "end") {
      cross_offset = extent - cross_size;
    }
    if (row) {
      box.translate(content_x + start + cursor, content_y + cross_offset);
    } else {
      box.translate(content_x + cross_offset, content_y + start + cursor);
    }
    parent_box.children.push_back(std::move(measured[i]));
    cursor += main_sizes[i] + spacing;
  }

  return row ? extent : used;
}

std::unique_ptr<LayoutBox> layout_box(const Node& node, const Style& style,
                                      const StyleMap& styles, double origin_x,
                                      double origin_y,
                                      double available_width) {
  const double* margin = style.margin;
  const double* padding = style.padding;

  const double width =
      style.width ? *style.width
                  : std::max(available_width - margin[1] - margin[3], 0.0);
  const double x = origin_x + margin[3];
  const double y = origin_y + margin[0];
  const double content_x = x + padding[3];
  const double content_y = y + padding[0];
  const double content_width =
      std::max(width - padding[1] - padding[3], 0.0);

  auto box = std::make_unique<LayoutBox>();
  box->style = &style;
  box->x = x;
  box->y = y;
  box->width = width;
  box->height = 0.0;
  box->margin_bottom = margin[2];
  box->kind = (style.display == "block" || style.display == "flex")
                  ? style.display
                  : "block";

  double cursor_y = content_y;
  const std::vector<const Node*> children = renderable_children(node, styles);
  if (style.display == "flex" && !children.empty()) {
    std::optional<double> content_height;
    if (style.height) {
      content_height = std::max(*style.height - padding[0] - padding[2], 0.0);
    }
    cursor_y += layout_flex_children(*box, children, styles, content_x,
                                     cursor_y, content_width, content_height,
                                     style);
  } else {
    for (const Node* child : children) {
      const Style& child_style = styles.at(child);
      auto child_box = layout_box(*child, child_style, styles, content_x,
                                  cursor_y, content_width);
      cursor_y = child_box->y + child_box->height + child_box->margin_bottom;
      box->children.push_back(std::move(child_box));
    }
  }

  const double content_used = cursor_y - content_y;
  box->height =
      style.height ? *style.height
                   : std::max(content_used + padding[0] + padding[2], 0.0);
  return box;
}

void paint_into(const LayoutBox& box, std::vector<RectOp>& ops) {
  if (box.style != nullptr && box.style->background.has_value()) {
    ops.push_back(RectOp{box.x, box.y, box.width, box.height,
                         *box.style->background});
  }
  for (const auto& child : box.children) {
    paint_into(*child, ops);
  }
}

}  // namespace

void LayoutBox::translate(double dx, double dy) {
  x += dx;
  y += dy;
  for (auto& child : children) {
    child->translate(dx, dy);
  }
}

std::unique_ptr<LayoutBox> layout_tree(const Node& root,
                                       const StyleMap& styles,
                                       double viewport_width) {
  Node* body = find_body(const_cast<Node*>(&root));
  const auto found = styles.find(body);
  if (found == styles.end()) {
    throw std::runtime_error("layout: no computed style for body");
  }
  return layout_box(*body, found->second, styles, 0.0, 0.0, viewport_width);
}

std::vector<RectOp> paint(const LayoutBox& box) {
  std::vector<RectOp> ops;
  paint_into(box, ops);
  return ops;
}

}  // namespace wn
