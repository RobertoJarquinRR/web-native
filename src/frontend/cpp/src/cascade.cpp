#include "cascade.hpp"

#include <map>
#include <string_view>
#include <unordered_set>
#include <utility>

#include "util.hpp"

namespace wn {

namespace {

using RawStyle = std::map<std::string, std::string>;

const std::unordered_set<std::string>& inherited_properties() {
  static const std::unordered_set<std::string> kInherited = {
      "color",      "font-family", "font-size", "font-weight",
      "line-height", "text-align",  "white-space"};
  return kInherited;
}

const std::unordered_set<std::string>& non_rendered_tags() {
  static const std::unordered_set<std::string> kTags = {
      "head",     "title",   "meta",    "link",    "style",
      "script",   "base",    "template", "noscript", "source",
      "track"};
  return kTags;
}

const std::unordered_set<std::string>& inline_tags() {
  static const std::unordered_set<std::string> kTags = {
      "a",    "abbr", "b",     "bdi",  "bdo",  "br",   "cite", "code",
      "em",   "i",    "img",   "label", "q",   "s",    "small", "span",
      "strong", "sub", "sup",  "u",    "var",  "wbr"};
  return kTags;
}

bool matches_compound(const Node& node, const CompoundSelector& compound) {
  for (const auto& part : compound.parts) {
    switch (part.kind) {
      case SimpleKind::Universal:
        break;
      case SimpleKind::Type:
        if (node.tag != part.name) {
          return false;
        }
        break;
      case SimpleKind::Id: {
        const std::string* id = node.attr("id");
        if (id == nullptr || *id != part.name) {
          return false;
        }
        break;
      }
      case SimpleKind::Class: {
        const std::string* classes = node.attr("class");
        if (classes == nullptr) {
          return false;
        }
        bool found = false;
        for (const auto& token : split_whitespace(*classes)) {
          if (token == part.name) {
            found = true;
            break;
          }
        }
        if (!found) {
          return false;
        }
        break;
      }
      case SimpleKind::Attribute: {
        const std::string* attribute = node.attr(part.name);
        if (attribute == nullptr) {
          return false;
        }
        std::string actual = part.case_insensitive
                                 ? lower_ascii(*attribute)
                                 : *attribute;
        std::string expected = part.case_insensitive
                                   ? lower_ascii(part.value)
                                   : part.value;
        switch (part.attribute_op) {
          case AttributeOp::Exists:
            break;
          case AttributeOp::Equals:
            if (actual != expected) {
              return false;
            }
            break;
          case AttributeOp::Includes: {
            bool found = false;
            for (const auto& token : split_whitespace(actual)) {
              if (token == expected) {
                found = true;
                break;
              }
            }
            if (!found) {
              return false;
            }
            break;
          }
          case AttributeOp::DashMatch:
            if (actual != expected &&
                !(actual.size() > expected.size() &&
                  actual.compare(0, expected.size(), expected) == 0 &&
                  actual[expected.size()] == '-')) {
              return false;
            }
            break;
          case AttributeOp::Prefix:
            if (actual.rfind(expected, 0) != 0) {
              return false;
            }
            break;
          case AttributeOp::Suffix:
            if (actual.size() < expected.size() ||
                actual.compare(actual.size() - expected.size(), expected.size(),
                               expected) != 0) {
              return false;
            }
            break;
          case AttributeOp::Substring:
            if (actual.find(expected) == std::string::npos) {
              return false;
            }
            break;
        }
        break;
      }
    }
  }
  return true;
}

bool matches_at(const Node& node, const ComplexSelector& selector,
                std::size_t index) {
  if (!matches_compound(node, selector.compounds[index])) {
    return false;
  }
  if (index == 0) {
    return true;
  }
  if (selector.combinators[index - 1] == Combinator::Child) {
    return node.parent != nullptr &&
           matches_at(*node.parent, selector, index - 1);
  }
  for (const Node* ancestor = node.parent; ancestor != nullptr;
       ancestor = ancestor->parent) {
    if (matches_at(*ancestor, selector, index - 1)) {
      return true;
    }
  }
  return false;
}

struct CascadeKey {
  int important = 0;
  int inline_style = 0;
  int ids = 0;
  int classes = 0;
  int types = 0;
  std::size_t order = 0;
  std::size_t declaration = 0;

  bool operator<(const CascadeKey& other) const {
    if (important != other.important) {
      return important < other.important;
    }
    if (inline_style != other.inline_style) {
      return inline_style < other.inline_style;
    }
    if (ids != other.ids) {
      return ids < other.ids;
    }
    if (classes != other.classes) {
      return classes < other.classes;
    }
    if (types != other.types) {
      return types < other.types;
    }
    if (order != other.order) {
      return order < other.order;
    }
    return declaration < other.declaration;
  }
};

struct Winner {
  CascadeKey key;
  std::string value;
};

const std::string* raw_value(const RawStyle& raw, std::string_view name) {
  const auto found = raw.find(std::string(name));
  return found == raw.end() ? nullptr : &found->second;
}

std::string raw_keyword(const RawStyle& raw, std::string_view name,
                        std::string_view fallback) {
  const std::string* value = raw_value(raw, name);
  if (value == nullptr) {
    return std::string(fallback);
  }
  const std::string keyword = lower_ascii(trim(*value));
  return keyword.empty() ? std::string(fallback) : keyword;
}

std::string ua_display(const std::string& tag) {
  if (non_rendered_tags().count(tag) != 0) {
    return "none";
  }
  if (inline_tags().count(tag) != 0) {
    return "inline";
  }
  return "block";
}

Style resolve(const Node& node, const RawStyle& raw) {
  Style style;
  const std::string* display = raw_value(raw, "display");
  style.display = display != nullptr && !display->empty()
                      ? lower_ascii(trim(*display))
                      : ua_display(node.tag);

  const auto side = [&](const char* property) {
    const std::string* value = raw_value(raw, property);
    return value == nullptr ? 0.0 : css_length(*value).value_or(0.0);
  };
  style.margin[0] = side("margin-top");
  style.margin[1] = side("margin-right");
  style.margin[2] = side("margin-bottom");
  style.margin[3] = side("margin-left");
  style.padding[0] = side("padding-top");
  style.padding[1] = side("padding-right");
  style.padding[2] = side("padding-bottom");
  style.padding[3] = side("padding-left");

  if (const std::string* width = raw_value(raw, "width")) {
    style.width = css_length(*width);
  }
  if (const std::string* height = raw_value(raw, "height")) {
    style.height = css_length(*height);
  }

  const std::string* background = raw_value(raw, "background-color");
  if (background == nullptr || background->empty()) {
    background = raw_value(raw, "background");
  }
  if (background != nullptr) {
    style.background = css_color(*background);
  }

  if (const std::string* color = raw_value(raw, "color")) {
    style.color = css_color(*color).value_or("#000000");
  }
  if (const std::string* font_size = raw_value(raw, "font-size")) {
    style.font_size = css_length(*font_size).value_or(16.0);
  }
  if (const std::string* family = raw_value(raw, "font-family")) {
    style.font_family = *family;
  }
  style.flex_direction = raw_keyword(raw, "flex-direction", "row");
  style.justify_content = raw_keyword(raw, "justify-content", "flex-start");
  style.align_items = raw_keyword(raw, "align-items", "stretch");

  // Python: gap = length("gap"); only when that is None:
  //   gap = length("row-gap") or length("column-gap") or 0
  const std::string* gap_text = raw_value(raw, "gap");
  std::optional<double> gap =
      gap_text != nullptr ? css_length(*gap_text) : std::nullopt;
  if (!gap) {
    const std::string* row_gap = raw_value(raw, "row-gap");
    const std::string* column_gap = raw_value(raw, "column-gap");
    const std::optional<double> row =
        row_gap != nullptr ? css_length(*row_gap) : std::nullopt;
    const std::optional<double> column =
        column_gap != nullptr ? css_length(*column_gap) : std::nullopt;
    if (row && *row != 0.0) {
      gap = row;
    } else if (column && *column != 0.0) {
      gap = column;
    } else {
      gap = 0.0;
    }
  }
  style.gap = *gap;

  return style;
}

void offer(std::map<std::string, Winner>& winners, const std::string& name,
           const std::string& value, const CascadeKey& key) {
  auto found = winners.find(name);
  if (found == winners.end()) {
    winners.emplace(name, Winner{key, value});
    return;
  }
  if (!(key < found->second.key)) {
    found->second = Winner{key, value};
  }
}

void compute_rec(const Node& node, const RawStyle& parent_raw,
                 const std::vector<Rule>& rules, StyleMap& styles) {
  RawStyle raw;
  for (const auto& [name, value] : parent_raw) {
    if (inherited_properties().count(name) != 0) {
      raw.emplace(name, value);
    }
  }

  std::map<std::string, Winner> winners;
  for (const auto& rule : rules) {
    if (rule.selector.compounds.empty()) {
      continue;
    }
    if (!matches_at(node, rule.selector, rule.selector.compounds.size() - 1)) {
      continue;
    }
    for (std::size_t i = 0; i < rule.declarations.size(); ++i) {
      const Declaration& declaration = rule.declarations[i];
      CascadeKey key;
      key.important = declaration.important ? 1 : 0;
      key.inline_style = 0;
      key.ids = rule.selector.ids;
      key.classes = rule.selector.classes;
      key.types = rule.selector.types;
      key.order = rule.order;
      key.declaration = i;
      offer(winners, declaration.name, declaration.value, key);
    }
  }

  if (const std::string* inline_style = node.attr("style")) {
    const std::vector<Declaration> declarations =
        parse_declarations(*inline_style);
    for (std::size_t i = 0; i < declarations.size(); ++i) {
      const Declaration& declaration = declarations[i];
      CascadeKey key;
      key.important = declaration.important ? 1 : 0;
      key.inline_style = 1;
      key.order = 0;
      key.declaration = i;
      offer(winners, declaration.name, declaration.value, key);
    }
  }

  for (const auto& [name, winner] : winners) {
    raw[name] = winner.value;
  }

  styles[&node] = resolve(node, raw);
  for (const auto& child : node.children) {
    compute_rec(*child, raw, rules, styles);
  }
}

}  // namespace

bool selector_matches(const Node& node, const ComplexSelector& selector) {
  if (!selector.supported || selector.compounds.empty()) {
    return false;
  }
  return matches_at(node, selector, selector.compounds.size() - 1);
}

StyleMap compute_styles(const Node& root, const std::vector<Rule>& rules) {
  StyleMap styles;
  compute_rec(root, RawStyle{}, rules, styles);
  return styles;
}

}  // namespace wn
