// Small helpers shared by the C++ frontend: file IO, string utilities,
// timing and JSON number formatting (Python-compatible).
#pragma once

#include <chrono>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace wn {

using Clock = std::chrono::steady_clock;

inline double ms_since(const Clock::time_point& start) {
  return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

std::string read_file(const std::filesystem::path& path);
void write_file(const std::filesystem::path& path, std::string_view data);
std::string path_utf8(const std::filesystem::path& path);
std::string utf8_from_wide(std::wstring_view text);

std::string trim(std::string_view text);
std::string lower_ascii(std::string_view text);
bool is_space(char c);
bool is_digit(char c);
std::vector<std::string> split_whitespace(std::string_view text);

// JSON number formatting that mirrors Python:
//  - integers (IR geometry uses Python's round() -> int)
//  - milliseconds rounded to 3 decimals, always with a fractional part.
std::string json_int(long long value);
std::string json_ms(double milliseconds);

}  // namespace wn
