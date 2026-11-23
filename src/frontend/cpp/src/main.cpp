// frontend-cpp: HTML + CSS -> display-list IR JSON.
//
// CLI (see benchmarks/frontend-parity/SPEC.md):
//   frontend-cpp.exe --html <path> --width <W> --height <H> --out <ir.json>
//                    [--timings <timings.json>]
//
// No browser, WebView, Skia or Electron: Gumbo parses HTML, the hand-written
// CSS/cascade/layout stages produce rect ops, and the IR is serialized to JSON.

#include <cstdint>
#include <filesystem>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <psapi.h>
#endif

#include "cascade.hpp"
#include "css.hpp"
#include "html.hpp"
#include "ir.hpp"
#include "layout.hpp"
#include "util.hpp"

namespace {

using wn::Clock;

struct Options {
  std::filesystem::path html;
  int width = 0;
  int height = 0;
  std::filesystem::path out;
  std::optional<std::filesystem::path> timings;
  bool help = false;
};

const char* kUsage =
    "usage: frontend-cpp --html <path> --width <W> --height <H> "
    "--out <ir.json> [--timings <timings.json>]\n";

int parse_dimension(const std::wstring& text, const char* option) {
  try {
    std::size_t consumed = 0;
    const long long value = std::stoll(text, &consumed);
    if (consumed != text.size() || value < 0 ||
        value > 1000000) {
      throw std::runtime_error("");
    }
    return static_cast<int>(value);
  } catch (...) {
    throw std::runtime_error(std::string("invalid value for ") + option);
  }
}

Options parse_args(int argc, wchar_t** argv) {
  Options options;
  bool have_html = false;
  bool have_width = false;
  bool have_height = false;
  bool have_out = false;

  for (int i = 1; i < argc; ++i) {
    std::wstring argument = argv[i];
    std::wstring key = argument;
    std::optional<std::wstring> inline_value;
    const std::size_t equals = argument.find(L'=');
    if (equals != std::wstring::npos) {
      key = argument.substr(0, equals);
      inline_value = argument.substr(equals + 1);
    }

    const auto take_value = [&]() -> std::wstring {
      if (inline_value) {
        return *inline_value;
      }
      if (i + 1 >= argc) {
        throw std::runtime_error("missing value for " +
                                 wn::utf8_from_wide(key));
      }
      return argv[++i];
    };

    if (key == L"--help" || key == L"-h" || key == L"/?") {
      options.help = true;
    } else if (key == L"--html") {
      options.html = std::filesystem::path(take_value());
      have_html = true;
    } else if (key == L"--width") {
      options.width = parse_dimension(take_value(), "--width");
      have_width = true;
    } else if (key == L"--height") {
      options.height = parse_dimension(take_value(), "--height");
      have_height = true;
    } else if (key == L"--out") {
      options.out = std::filesystem::path(take_value());
      have_out = true;
    } else if (key == L"--timings") {
      options.timings = std::filesystem::path(take_value());
    } else {
      throw std::runtime_error("unknown option: " +
                               wn::utf8_from_wide(argument));
    }
  }

  if (!options.help &&
      (!have_html || !have_width || !have_height || !have_out)) {
    throw std::runtime_error(
        "missing required argument (--html, --width, --height, --out)");
  }
  return options;
}

long long peak_rss_kb() {
#ifdef _WIN32
  PROCESS_MEMORY_COUNTERS counters{};
  if (::K32GetProcessMemoryInfo(::GetCurrentProcess(), &counters,
                                sizeof(counters)) != 0) {
    return static_cast<long long>(counters.PeakWorkingSetSize / 1024);
  }
  return 0;
#else
  return 0;
#endif
}

}  // namespace

#ifdef _WIN32
int wmain(int argc, wchar_t** argv) {
#else
#error "frontend-cpp is built for Windows with MSVC (build.ps1)"
#endif
  try {
    const Options options = parse_args(argc, argv);
    if (options.help) {
      std::cout << kUsage;
      return 0;
    }

    wn::Timings timings;
    const auto pipeline_start = Clock::now();

    auto stage_start = Clock::now();
    const std::string html = wn::read_file(options.html);
    std::unique_ptr<wn::Node> root = wn::parse_html(html);
    const std::vector<std::string> stylesheets =
        wn::stylesheet_texts(*root, options.html.parent_path());
    timings.html = wn::ms_since(stage_start);

    stage_start = Clock::now();
    std::vector<wn::Rule> rules;
    for (const auto& text : stylesheets) {
      std::vector<wn::Rule> parsed = wn::parse_stylesheet(text);
      for (auto& rule : parsed) {
        rule.order = rules.size();
        rules.push_back(std::move(rule));
      }
    }
    timings.css = wn::ms_since(stage_start);

    stage_start = Clock::now();
    const wn::StyleMap styles = wn::compute_styles(*root, rules);
    timings.cascade = wn::ms_since(stage_start);

    // No text in the fixture subset: font resolution is a no-op.
    timings.fonts = 0.0;

    stage_start = Clock::now();
    const std::unique_ptr<wn::LayoutBox> box = wn::layout_tree(
        *root, styles, static_cast<double>(options.width));
    timings.layout = wn::ms_since(stage_start);

    stage_start = Clock::now();
    const std::vector<wn::RectOp> ops = wn::paint(*box);
    const std::string ir =
        wn::ir_to_json(options.width, options.height, ops);
    timings.text_ir = wn::ms_since(stage_start);
    timings.ops = ops.size();
    timings.pipeline = wn::ms_since(pipeline_start);

    wn::write_file(options.out, ir);
    timings.peak_rss_kb = peak_rss_kb();
    if (options.timings) {
      wn::write_file(*options.timings, wn::timings_to_json(timings));
    }
    std::cout << wn::timings_to_line(timings) << "\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "ERROR: " << error.what() << "\n";
    if (std::string_view(error.what()).rfind("missing required", 0) == 0 ||
        std::string_view(error.what()).rfind("unknown option", 0) == 0 ||
        std::string_view(error.what()).rfind("invalid value", 0) == 0 ||
        std::string_view(error.what()).rfind("missing value", 0) == 0) {
      std::cerr << kUsage;
      return 2;
    }
    return 1;
  } catch (...) {
    std::cerr << "ERROR: unknown failure\n";
    return 1;
  }
}
