// Hand-written CSS parser for the benchmark subset: qualified rules,
// declarations (with margin/padding shorthand expansion), simple selectors
// (type/class/id/universal/compound), descendant and child combinators, comma
// lists and specificity.  Unsupported selectors are ignored instead of
// crashing, as allowed by the SPEC.
#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace wn {

struct Declaration {
  std::string name;
  std::string value;
  bool important = false;
};

enum class SimpleKind { Universal, Type, Class, Id, Attribute };

enum class AttributeOp {
  Exists,
  Equals,
  Includes,    // ~=
  DashMatch,   // |=
  Prefix,      // ^=
  Suffix,      // $=
  Substring    // *=
};

struct SimpleSelector {
  SimpleKind kind = SimpleKind::Universal;
  std::string name;
  AttributeOp attribute_op = AttributeOp::Exists;
  std::string value;
  bool case_insensitive = false;
};

struct CompoundSelector {
  std::vector<SimpleSelector> parts;
};

enum class Combinator { Descendant, Child };

struct ComplexSelector {
  std::vector<CompoundSelector> compounds;
  std::vector<Combinator> combinators;  // one fewer than `compounds`
  int ids = 0;
  int classes = 0;
  int types = 0;
  bool supported = true;
};

struct Rule {
  ComplexSelector selector;
  std::vector<Declaration> declarations;
  std::size_t order = 0;
};

std::vector<Rule> parse_stylesheet(std::string_view css);
std::vector<Declaration> parse_declarations(std::string_view text);

// CSS lengths accepted by the Python reference: optional sign, decimal number,
// optional `px`; anything else (auto, %, em, ...) is "auto".
std::optional<double> css_length(std::string_view value);

// #rgb / #rgba / #rrggbb / #rrggbbaa, rgb()/rgba(), a few named colors and
// `transparent`.  Returns lowercase #rrggbb; alpha 0 -> no background.
std::optional<std::string> css_color(std::string_view value);

}  // namespace wn
