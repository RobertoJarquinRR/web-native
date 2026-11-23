// Display-list IR + timings serialization.  The byte layout mirrors Python's
// json.dumps(..., indent=2) + "\n" (with Windows CRLF, like Path.write_text).
#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "layout.hpp"

namespace wn {

struct Timings {
  double html = 0.0;
  double css = 0.0;
  double cascade = 0.0;
  double fonts = 0.0;
  double layout = 0.0;
  double text_ir = 0.0;
  double pipeline = 0.0;
  std::size_t ops = 0;
  long long peak_rss_kb = 0;
};

std::string ir_to_json(int viewport_width, int viewport_height,
                       const std::vector<RectOp>& ops);
std::string timings_to_json(const Timings& timings);
std::string timings_to_line(const Timings& timings);

}  // namespace wn
