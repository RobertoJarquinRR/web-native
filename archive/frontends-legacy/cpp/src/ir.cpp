#include "ir.hpp"

#include <cmath>
#include <string>

#include "util.hpp"

namespace wn {

namespace {

constexpr const char* kNl = "\r\n";

// Python's RectOp.to_dict() uses round(float) -> int (ties to even).
long long python_round(double value) {
  return static_cast<long long>(std::nearbyint(value));
}

void append_ms(std::string& out, double value) { out += json_ms(value); }

}  // namespace

std::string ir_to_json(int viewport_width, int viewport_height,
                       const std::vector<RectOp>& ops) {
  std::string out;
  out.reserve(96 + ops.size() * 140);
  out += "{";
  out += kNl;
  out += "  \"viewport\": {";
  out += kNl;
  out += "    \"width\": ";
  out += json_int(viewport_width);
  out += ",";
  out += kNl;
  out += "    \"height\": ";
  out += json_int(viewport_height);
  out += kNl;
  out += "  },";
  out += kNl;
  if (ops.empty()) {
    out += "  \"ops\": []";
    out += kNl;
  } else {
    out += "  \"ops\": [";
    out += kNl;
    for (std::size_t i = 0; i < ops.size(); ++i) {
      const RectOp& op = ops[i];
      out += "    {";
      out += kNl;
      out += "      \"type\": \"rect\",";
      out += kNl;
      out += "      \"x\": ";
      out += json_int(python_round(op.x));
      out += ",";
      out += kNl;
      out += "      \"y\": ";
      out += json_int(python_round(op.y));
      out += ",";
      out += kNl;
      out += "      \"w\": ";
      out += json_int(python_round(op.w));
      out += ",";
      out += kNl;
      out += "      \"h\": ";
      out += json_int(python_round(op.h));
      out += ",";
      out += kNl;
      out += "      \"fill\": \"";
      out += op.fill;
      out += "\"";
      out += kNl;
      out += "    }";
      if (i + 1 != ops.size()) {
        out += ",";
      }
      out += kNl;
    }
    out += "  ]";
    out += kNl;
  }
  out += "}";
  out += kNl;
  return out;
}

std::string timings_to_json(const Timings& timings) {
  std::string out;
  out += "{";
  out += kNl;
  const auto field = [&](const char* name, double value, bool comma) {
    out += "  \"";
    out += name;
    out += "\": ";
    append_ms(out, value);
    if (comma) {
      out += ",";
    }
    out += kNl;
  };
  field("html", timings.html, true);
  field("css", timings.css, true);
  field("cascade", timings.cascade, true);
  field("fonts", timings.fonts, true);
  field("layout", timings.layout, true);
  field("text+ir", timings.text_ir, true);
  field("pipeline", timings.pipeline, true);
  out += "  \"ops\": ";
  out += json_int(static_cast<long long>(timings.ops));
  out += ",";
  out += kNl;
  out += "  \"peak_rss_kb\": ";
  out += json_int(timings.peak_rss_kb);
  out += kNl;
  out += "}";
  out += kNl;
  return out;
}

std::string timings_to_line(const Timings& timings) {
  std::string out = "{\"html\": ";
  append_ms(out, timings.html);
  out += ", \"css\": ";
  append_ms(out, timings.css);
  out += ", \"cascade\": ";
  append_ms(out, timings.cascade);
  out += ", \"fonts\": ";
  append_ms(out, timings.fonts);
  out += ", \"layout\": ";
  append_ms(out, timings.layout);
  out += ", \"text+ir\": ";
  append_ms(out, timings.text_ir);
  out += ", \"pipeline\": ";
  append_ms(out, timings.pipeline);
  out += ", \"ops\": ";
  out += json_int(static_cast<long long>(timings.ops));
  out += ", \"peak_rss_kb\": ";
  out += json_int(timings.peak_rss_kb);
  out += "}";
  return out;
}

}  // namespace wn
