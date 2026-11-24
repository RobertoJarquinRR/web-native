#include "css.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <limits>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "util.hpp"

namespace wn {

namespace {

constexpr std::size_t kNpos = std::string_view::npos;

bool is_ident_start(char c) {
  const unsigned char u = static_cast<unsigned char>(c);
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' ||
         c == '-' || u >= 0x80;
}

bool is_ident_char(char c) {
  const unsigned char u = static_cast<unsigned char>(c);
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
         (c >= '0' && c <= '9') || c == '_' || c == '-' || u >= 0x80;
}

std::string strip_comments(std::string_view text) {
  std::string out;
  out.reserve(text.size());
  std::size_t i = 0;
  char quote = 0;
  while (i < text.size()) {
    const char c = text[i];
    if (quote != 0) {
      out.push_back(c);
      if (c == '\\' && i + 1 < text.size()) {
        out.push_back(text[i + 1]);
        i += 2;
        continue;
      }
      if (c == quote) {
        quote = 0;
      }
      ++i;
      continue;
    }
    if (c == '\'' || c == '"') {
      quote = c;
      out.push_back(c);
      ++i;
      continue;
    }
    if (c == '/' && i + 1 < text.size() && text[i + 1] == '*') {
      i += 2;
      while (i + 1 < text.size() && !(text[i] == '*' && text[i + 1] == '/')) {
        ++i;
      }
      i = (i + 1 < text.size()) ? i + 2 : text.size();
      continue;
    }
    out.push_back(c);
    ++i;
  }
  return out;
}

// First occurrence of any character in `targets` at nesting depth 0, ignoring
// strings, parentheses and square brackets.
std::size_t find_top_level(std::string_view text, std::size_t from,
                           std::string_view targets) {
  int parens = 0;
  int brackets = 0;
  char quote = 0;
  for (std::size_t i = from; i < text.size(); ++i) {
    const char c = text[i];
    if (quote != 0) {
      if (c == '\\') {
        ++i;
      } else if (c == quote) {
        quote = 0;
      }
      continue;
    }
    if (c == '\'' || c == '"') {
      quote = c;
      continue;
    }
    if (c == '(') {
      ++parens;
      continue;
    }
    if (c == ')') {
      if (parens > 0) {
        --parens;
      }
      continue;
    }
    if (c == '[') {
      ++brackets;
      continue;
    }
    if (c == ']') {
      if (brackets > 0) {
        --brackets;
      }
      continue;
    }
    if (parens == 0 && brackets == 0 && targets.find(c) != kNpos) {
      return i;
    }
  }
  return kNpos;
}

std::size_t matching_brace(std::string_view text, std::size_t open) {
  int depth = 0;
  char quote = 0;
  for (std::size_t i = open; i < text.size(); ++i) {
    const char c = text[i];
    if (quote != 0) {
      if (c == '\\') {
        ++i;
      } else if (c == quote) {
        quote = 0;
      }
      continue;
    }
    if (c == '\'' || c == '"') {
      quote = c;
      continue;
    }
    if (c == '{') {
      ++depth;
    } else if (c == '}') {
      --depth;
      if (depth == 0) {
        return i;
      }
    }
  }
  return kNpos;
}

std::vector<std::string> split_top_level(std::string_view text, char separator) {
  std::vector<std::string> parts;
  std::size_t start = 0;
  while (start <= text.size()) {
    const std::size_t found =
        find_top_level(text, start, std::string_view(&separator, 1));
    if (found == kNpos) {
      parts.push_back(trim(text.substr(start)));
      break;
    }
    parts.push_back(trim(text.substr(start, found - start)));
    start = found + 1;
  }
  return parts;
}

bool strip_important(std::string& value) {
  std::size_t end = value.size();
  while (end > 0 && is_space(value[end - 1])) {
    --end;
  }
  constexpr std::string_view kImportant = "important";
  if (end < kImportant.size()) {
    return false;
  }
  const std::size_t word_start = end - kImportant.size();
  if (lower_ascii(std::string_view(value).substr(word_start, kImportant.size())) !=
      kImportant) {
    return false;
  }
  if (word_start > 0 && is_ident_char(value[word_start - 1])) {
    return false;
  }
  std::size_t bang = word_start;
  while (bang > 0 && is_space(value[bang - 1])) {
    --bang;
  }
  if (bang == 0 || value[bang - 1] != '!') {
    return false;
  }
  value = trim(std::string_view(value).substr(0, bang - 1));
  return true;
}

std::vector<Declaration> expand_shorthand(const std::string& name,
                                          const std::string& value,
                                          bool important) {
  std::vector<Declaration> out;
  if (name != "margin" && name != "padding") {
    out.push_back({name, value, important});
    return out;
  }
  const std::vector<std::string> parts = split_whitespace(value);
  if (parts.empty() || parts.size() > 4) {
    return out;  // invalid shorthand: declaration dropped
  }
  const std::string top = parts[0];
  const std::string right = parts.size() > 1 ? parts[1] : parts[0];
  const std::string bottom = parts.size() > 2 ? parts[2] : top;
  const std::string left = parts.size() > 3 ? parts[3] : right;
  out.push_back({name + "-top", top, important});
  out.push_back({name + "-right", right, important});
  out.push_back({name + "-bottom", bottom, important});
  out.push_back({name + "-left", left, important});
  return out;
}

std::size_t find_attr_close(std::string_view text, std::size_t open) {
  char quote = 0;
  for (std::size_t i = open + 1; i < text.size(); ++i) {
    const char c = text[i];
    if (quote != 0) {
      if (c == '\\') {
        ++i;
      } else if (c == quote) {
        quote = 0;
      }
      continue;
    }
    if (c == '\'' || c == '"') {
      quote = c;
    } else if (c == ']') {
      return i;
    }
  }
  return kNpos;
}

bool parse_attribute_selector(std::string_view content, SimpleSelector& out) {
  std::string text = trim(content);
  if (text.empty()) {
    return false;
  }
  std::size_t i = 0;
  const std::size_t n = text.size();
  if (!is_ident_start(text[i])) {
    return false;
  }
  const std::size_t name_start = i;
  while (i < n && is_ident_char(text[i])) {
    ++i;
  }
  out = SimpleSelector{};
  out.kind = SimpleKind::Attribute;
  out.name = lower_ascii(
      std::string_view(text).substr(name_start, i - name_start));
  while (i < n && is_space(text[i])) {
    ++i;
  }
  if (i >= n) {
    out.attribute_op = AttributeOp::Exists;
    return true;
  }

  if (text[i] == '=') {
    out.attribute_op = AttributeOp::Equals;
    ++i;
  } else if (i + 1 < n && text[i + 1] == '=') {
    switch (text[i]) {
      case '~':
        out.attribute_op = AttributeOp::Includes;
        break;
      case '|':
        out.attribute_op = AttributeOp::DashMatch;
        break;
      case '^':
        out.attribute_op = AttributeOp::Prefix;
        break;
      case '$':
        out.attribute_op = AttributeOp::Suffix;
        break;
      case '*':
        out.attribute_op = AttributeOp::Substring;
        break;
      default:
        return false;
    }
    i += 2;
  } else {
    return false;
  }
  while (i < n && is_space(text[i])) {
    ++i;
  }
  if (i >= n) {
    return false;
  }

  std::string value;
  if (text[i] == '\'' || text[i] == '"') {
    const char quote = text[i++];
    while (i < n && text[i] != quote) {
      if (text[i] == '\\' && i + 1 < n) {
        ++i;
      }
      value.push_back(text[i++]);
    }
    if (i >= n || text[i] != quote) {
      return false;
    }
    ++i;
  } else {
    const std::size_t value_start = i;
    while (i < n && !is_space(text[i])) {
      ++i;
    }
    value = std::string(text.substr(value_start, i - value_start));
  }
  out.value = value;

  while (i < n && is_space(text[i])) {
    ++i;
  }
  if (i < n) {
    const char flag = static_cast<char>(
        std::tolower(static_cast<unsigned char>(text[i])));
    if (flag == 'i') {
      out.case_insensitive = true;
      ++i;
    } else if (flag == 's') {
      ++i;
    }
  }
  while (i < n && is_space(text[i])) {
    ++i;
  }
  return i == n;
}

ComplexSelector parse_selector(std::string_view text) {
  ComplexSelector selector;
  std::size_t i = 0;
  const std::size_t n = text.size();

  const auto skip_space = [&]() {
    while (i < n && is_space(text[i])) {
      ++i;
    }
  };

  skip_space();
  if (i >= n) {
    selector.supported = false;
    return selector;
  }

  while (i < n) {
    CompoundSelector compound;
    while (i < n) {
      const char c = text[i];
      if (is_space(c) || c == '>' || c == '+' || c == '~') {
        break;
      }
      if (c == '*') {
        compound.parts.push_back({SimpleKind::Universal, ""});
        ++i;
      } else if (c == '[') {
        const std::size_t close = find_attr_close(text, i);
        if (close == kNpos) {
          selector.supported = false;
          return selector;
        }
        SimpleSelector attribute;
        if (!parse_attribute_selector(
                text.substr(i + 1, close - i - 1), attribute)) {
          selector.supported = false;
          return selector;
        }
        compound.parts.push_back(std::move(attribute));
        i = close + 1;
      } else if (c == '.' || c == '#') {
        const bool is_id = c == '#';
        ++i;
        const std::size_t start = i;
        while (i < n && is_ident_char(text[i])) {
          ++i;
        }
        if (i == start) {
          selector.supported = false;
          return selector;
        }
        compound.parts.push_back(
            {is_id ? SimpleKind::Id : SimpleKind::Class,
             std::string(text.substr(start, i - start))});
      } else if (is_ident_start(c)) {
        const std::size_t start = i;
        while (i < n && is_ident_char(text[i])) {
          ++i;
        }
        compound.parts.push_back(
            {SimpleKind::Type, lower_ascii(text.substr(start, i - start))});
      } else {
        // Attribute selectors, pseudo-classes/elements, namespaces, ...
        // Outside the subset: ignore the whole selector without crashing.
        selector.supported = false;
        return selector;
      }
    }
    if (compound.parts.empty()) {
      selector.supported = false;
      return selector;
    }
    selector.compounds.push_back(std::move(compound));

    std::size_t after_compound = i;
    skip_space();
    if (i >= n) {
      break;
    }
    if (text[i] == '>') {
      selector.combinators.push_back(Combinator::Child);
      ++i;
      skip_space();
    } else if (text[i] == '+' || text[i] == '~') {
      selector.supported = false;
      return selector;
    } else {
      selector.combinators.push_back(Combinator::Descendant);
    }
    if (i == after_compound) {
      // Defensive: should not happen with the branches above.
      selector.supported = false;
      return selector;
    }
  }

  if (selector.compounds.empty() ||
      selector.combinators.size() + 1 != selector.compounds.size()) {
    selector.supported = false;
    return selector;
  }

  for (const auto& compound : selector.compounds) {
    for (const auto& part : compound.parts) {
      switch (part.kind) {
        case SimpleKind::Id:
          ++selector.ids;
          break;
        case SimpleKind::Class:
          ++selector.classes;
          break;
        case SimpleKind::Attribute:
          ++selector.classes;
          break;
        case SimpleKind::Type:
          ++selector.types;
          break;
        case SimpleKind::Universal:
          break;
      }
    }
  }
  return selector;
}

int hex_value(char c) {
  if (c >= '0' && c <= '9') {
    return c - '0';
  }
  if (c >= 'a' && c <= 'f') {
    return c - 'a' + 10;
  }
  if (c >= 'A' && c <= 'F') {
    return c - 'A' + 10;
  }
  return -1;
}

int round_channel(double channel) {
  if (channel < 0.0) {
    channel = 0.0;
  }
  if (channel > 255.0) {
    channel = 255.0;
  }
  return static_cast<int>(std::nearbyint(channel));
}

std::string make_hex(int r, int g, int b) {
  static constexpr char kHex[] = "0123456789abcdef";
  std::string out = "#";
  for (int value : {r, g, b}) {
    out.push_back(kHex[(value >> 4) & 0xf]);
    out.push_back(kHex[value & 0xf]);
  }
  return out;
}

std::optional<std::string> parse_hex_color(std::string_view digits) {
  for (char c : digits) {
    if (hex_value(c) < 0) {
      return std::nullopt;
    }
  }
  int r = 0;
  int g = 0;
  int b = 0;
  int a = 255;
  const auto nibble = [&](std::size_t index) { return hex_value(digits[index]); };
  switch (digits.size()) {
    case 3:
      r = nibble(0) * 17;
      g = nibble(1) * 17;
      b = nibble(2) * 17;
      break;
    case 4:
      r = nibble(0) * 17;
      g = nibble(1) * 17;
      b = nibble(2) * 17;
      a = nibble(3) * 17;
      break;
    case 6:
      r = nibble(0) * 16 + nibble(1);
      g = nibble(2) * 16 + nibble(3);
      b = nibble(4) * 16 + nibble(5);
      break;
    case 8:
      r = nibble(0) * 16 + nibble(1);
      g = nibble(2) * 16 + nibble(3);
      b = nibble(4) * 16 + nibble(5);
      a = nibble(6) * 16 + nibble(7);
      break;
    default:
      return std::nullopt;
  }
  if (a <= 0) {
    return std::nullopt;
  }
  return make_hex(r, g, b);
}

std::optional<double> parse_number(std::string_view text) {
  std::string value = trim(text);
  if (value.empty()) {
    return std::nullopt;
  }
  char* end = nullptr;
  const double parsed = std::strtod(value.c_str(), &end);
  if (end == value.c_str() || *end != '\0') {
    return std::nullopt;
  }
  return parsed;
}

std::optional<double> parse_channel(std::string_view text, bool& percent) {
  std::string value = trim(text);
  percent = !value.empty() && value.back() == '%';
  if (percent) {
    value.pop_back();
  }
  const auto number = parse_number(value);
  if (!number) {
    return std::nullopt;
  }
  return percent ? *number * 255.0 / 100.0 : *number;
}

std::optional<double> parse_alpha(std::string_view text, bool& percent) {
  std::string value = trim(text);
  percent = !value.empty() && value.back() == '%';
  if (percent) {
    value.pop_back();
  }
  const auto number = parse_number(value);
  if (!number) {
    return std::nullopt;
  }
  return percent ? *number / 100.0 : *number;
}

std::optional<std::string> parse_rgb_color(std::string_view inside) {
  std::vector<std::string> parts;
  std::string_view channels = inside;
  std::optional<double> slash_alpha;
  const std::size_t slash = find_top_level(inside, 0, std::string_view("/"));
  if (slash != kNpos) {
    channels = inside.substr(0, slash);
    bool alpha_percent = false;
    slash_alpha = parse_alpha(inside.substr(slash + 1), alpha_percent);
    if (!slash_alpha) {
      return std::nullopt;
    }
  }
  if (channels.find(',') != kNpos) {
    for (const auto& part : split_top_level(channels, ',')) {
      if (!part.empty()) {
        parts.push_back(part);
      }
    }
  } else {
    parts = split_whitespace(channels);
  }
  if (parts.size() < 3) {
    return std::nullopt;
  }
  bool percent = false;
  const auto r = parse_channel(parts[0], percent);
  const auto g = parse_channel(parts[1], percent);
  const auto b = parse_channel(parts[2], percent);
  if (!r || !g || !b) {
    return std::nullopt;
  }
  double alpha = 1.0;
  if (slash_alpha) {
    alpha = *slash_alpha;
  } else if (parts.size() >= 4) {
    bool alpha_percent = false;
    const auto a = parse_alpha(parts[3], alpha_percent);
    if (!a) {
      return std::nullopt;
    }
    alpha = *a;
  }
  if (alpha <= 0.0) {
    return std::nullopt;
  }
  return make_hex(round_channel(*r), round_channel(*g), round_channel(*b));
}

std::optional<std::string> named_color(std::string_view name) {
  static const std::map<std::string, std::pair<int, std::string>> kColors = {
      {"black", {0, "#000000"}},     {"white", {0, "#ffffff"}},
      {"red", {0, "#ff0000"}},       {"green", {0, "#008000"}},
      {"lime", {0, "#00ff00"}},      {"blue", {0, "#0000ff"}},
      {"yellow", {0, "#ffff00"}},    {"cyan", {0, "#00ffff"}},
      {"aqua", {0, "#00ffff"}},      {"magenta", {0, "#ff00ff"}},
      {"fuchsia", {0, "#ff00ff"}},   {"silver", {0, "#c0c0c0"}},
      {"gray", {0, "#808080"}},      {"grey", {0, "#808080"}},
      {"maroon", {0, "#800000"}},    {"olive", {0, "#808000"}},
      {"navy", {0, "#000080"}},      {"teal", {0, "#008080"}},
      {"purple", {0, "#800080"}},    {"orange", {0, "#ffa500"}},
      {"transparent", {0, ""}},
  };
  const auto found = kColors.find(std::string(name));
  if (found == kColors.end()) {
    return std::nullopt;
  }
  if (found->second.second.empty()) {
    return std::nullopt;
  }
  return found->second.second;
}

}  // namespace

std::vector<Declaration> parse_declarations(std::string_view text) {
  const std::string cleaned = strip_comments(text);
  std::vector<Declaration> out;
  std::size_t start = 0;
  while (start <= cleaned.size()) {
    const std::size_t semi =
        find_top_level(cleaned, start, std::string_view(";"));
    const std::size_t end = semi == kNpos ? cleaned.size() : semi;
    const std::string chunk = trim(
        std::string_view(cleaned).substr(start, end - start));
    start = semi == kNpos ? cleaned.size() + 1 : semi + 1;
    if (chunk.empty()) {
      continue;
    }
    const std::size_t colon =
        find_top_level(chunk, 0, std::string_view(":"));
    if (colon == kNpos) {
      continue;
    }
    std::string name = lower_ascii(trim(
        std::string_view(chunk).substr(0, colon)));
    if (name.empty()) {
      continue;
    }
    std::string value = trim(std::string_view(chunk).substr(colon + 1));
    const bool important = strip_important(value);
    std::vector<Declaration> expanded =
        expand_shorthand(name, value, important);
    out.insert(out.end(), expanded.begin(), expanded.end());
  }
  return out;
}

std::vector<Rule> parse_stylesheet(std::string_view css) {
  const std::string cleaned = strip_comments(css);
  std::vector<Rule> rules;
  std::size_t i = 0;
  while (i < cleaned.size()) {
    while (i < cleaned.size() &&
           (is_space(cleaned[i]) || cleaned[i] == ';')) {
      ++i;
    }
    if (i >= cleaned.size()) {
      break;
    }
    if (cleaned[i] == '@') {
      const std::size_t terminator =
          find_top_level(cleaned, i, std::string_view(";{"));
      if (terminator == kNpos) {
        break;
      }
      if (cleaned[terminator] == ';') {
        i = terminator + 1;
      } else {
        const std::size_t close = matching_brace(cleaned, terminator);
        i = close == kNpos ? cleaned.size() : close + 1;
      }
      continue;
    }
    const std::size_t brace =
        find_top_level(cleaned, i, std::string_view("{"));
    if (brace == kNpos) {
      break;
    }
    const std::size_t close = matching_brace(cleaned, brace);
    const std::size_t body_end = close == kNpos ? cleaned.size() : close;
    const std::string prelude =
        trim(std::string_view(cleaned).substr(i, brace - i));
    const std::string body =
        std::string(std::string_view(cleaned).substr(brace + 1,
                                                     body_end - brace - 1));
    const std::vector<Declaration> declarations = parse_declarations(body);
    if (!declarations.empty() && !prelude.empty()) {
      for (const auto& part : split_top_level(prelude, ',')) {
        if (part.empty()) {
          continue;
        }
        ComplexSelector selector = parse_selector(part);
        if (selector.supported) {
          Rule rule;
          rule.selector = std::move(selector);
          rule.declarations = declarations;
          rule.order = rules.size();
          rules.push_back(std::move(rule));
        }
      }
    }
    i = close == kNpos ? cleaned.size() : close + 1;
  }
  return rules;
}

std::optional<double> css_length(std::string_view raw) {
  std::string value = lower_ascii(trim(raw));
  if (value.empty() || value == "auto" || value == "none" ||
      value == "normal") {
    return std::nullopt;
  }
  std::size_t i = 0;
  bool negative = false;
  if (i < value.size() && value[i] == '-') {
    negative = true;
    ++i;
  }
  const std::size_t number_start = i;
  std::size_t digits = 0;
  while (i < value.size() && is_digit(value[i])) {
    ++i;
    ++digits;
  }
  if (i < value.size() && value[i] == '.') {
    ++i;
    while (i < value.size() && is_digit(value[i])) {
      ++i;
      ++digits;
    }
  }
  if (digits == 0) {
    return std::nullopt;
  }
  const std::size_t number_end = i;
  if (value.compare(i, 2, "px") == 0) {
    i += 2;
  }
  if (i != value.size()) {
    return std::nullopt;
  }
  const std::string number = value.substr(number_start, number_end - number_start);
  char* end = nullptr;
  double parsed = std::strtod(number.c_str(), &end);
  if (end == number.c_str() || *end != '\0') {
    return std::nullopt;
  }
  return negative ? -parsed : parsed;
}

std::optional<std::string> css_color(std::string_view raw) {
  const std::string value = lower_ascii(trim(raw));
  if (value.empty()) {
    return std::nullopt;
  }
  if (value[0] == '#') {
    return parse_hex_color(std::string_view(value).substr(1));
  }
  if (value.rfind("rgb(", 0) == 0 || value.rfind("rgba(", 0) == 0) {
    const std::size_t open = value.find('(');
    const std::size_t close = value.rfind(')');
    if (open == std::string::npos || close == std::string::npos ||
        close <= open) {
      return std::nullopt;
    }
    return parse_rgb_color(
        std::string_view(value).substr(open + 1, close - open - 1));
  }
  return named_color(value);
}

}  // namespace wn
