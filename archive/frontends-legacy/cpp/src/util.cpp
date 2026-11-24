#include "util.hpp"

#include <cmath>
#include <fstream>
#include <iterator>
#include <system_error>

#ifdef _WIN32
#include <windows.h>
#endif

namespace wn {

namespace {

std::string narrow_path(const std::filesystem::path& path) {
#ifdef _WIN32
  const std::wstring wide = path.wstring();
  if (wide.empty()) {
    return {};
  }
  const int size = ::WideCharToMultiByte(CP_UTF8, 0, wide.data(),
                                         static_cast<int>(wide.size()),
                                         nullptr, 0, nullptr, nullptr);
  if (size <= 0) {
    return "<path>";
  }
  std::string out(static_cast<std::size_t>(size), '\0');
  ::WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()),
                        out.data(), size, nullptr, nullptr);
  return out;
#else
  return path.string();
#endif
}

}  // namespace

std::string path_utf8(const std::filesystem::path& path) {
  return narrow_path(path);
}

std::string utf8_from_wide(std::wstring_view text) {
#ifdef _WIN32
  if (text.empty()) {
    return {};
  }
  const int size = ::WideCharToMultiByte(CP_UTF8, 0, text.data(),
                                         static_cast<int>(text.size()),
                                         nullptr, 0, nullptr, nullptr);
  if (size <= 0) {
    return {};
  }
  std::string out(static_cast<std::size_t>(size), '\0');
  ::WideCharToMultiByte(CP_UTF8, 0, text.data(),
                        static_cast<int>(text.size()), out.data(), size,
                        nullptr, nullptr);
  return out;
#else
  std::string out;
  out.reserve(text.size());
  for (wchar_t c : text) {
    out.push_back(static_cast<char>(c));
  }
  return out;
#endif
}

std::string read_file(const std::filesystem::path& path) {
  std::ifstream stream(path, std::ios::binary);
  if (!stream) {
    throw std::runtime_error("cannot read file: " + path_utf8(path));
  }
  std::string data((std::istreambuf_iterator<char>(stream)),
                   std::istreambuf_iterator<char>());
  if (stream.bad()) {
    throw std::runtime_error("error reading file: " + path_utf8(path));
  }
  return data;
}

void write_file(const std::filesystem::path& path, std::string_view data) {
  std::error_code ec;
  if (!path.parent_path().empty()) {
    std::filesystem::create_directories(path.parent_path(), ec);
    if (ec) {
      throw std::runtime_error("cannot create directory for: " +
                               path_utf8(path) + " (" + ec.message() + ")");
    }
  }
  std::ofstream stream(path, std::ios::binary | std::ios::trunc);
  if (!stream) {
    throw std::runtime_error("cannot write file: " + path_utf8(path));
  }
  stream.write(data.data(), static_cast<std::streamsize>(data.size()));
  stream.flush();
  if (!stream) {
    throw std::runtime_error("error writing file: " + path_utf8(path));
  }
}

bool is_space(char c) {
  const unsigned char u = static_cast<unsigned char>(c);
  return u == ' ' || u == '\t' || u == '\n' || u == '\r' || u == '\f' ||
         u == '\v';
}

bool is_digit(char c) {
  return c >= '0' && c <= '9';
}

std::string trim(std::string_view text) {
  std::size_t begin = 0;
  std::size_t end = text.size();
  while (begin < end && is_space(text[begin])) {
    ++begin;
  }
  while (end > begin && is_space(text[end - 1])) {
    --end;
  }
  return std::string(text.substr(begin, end - begin));
}

std::string lower_ascii(std::string_view text) {
  std::string out(text);
  for (char& c : out) {
    if (c >= 'A' && c <= 'Z') {
      c = static_cast<char>(c - 'A' + 'a');
    }
  }
  return out;
}

std::vector<std::string> split_whitespace(std::string_view text) {
  std::vector<std::string> parts;
  std::size_t i = 0;
  while (i < text.size()) {
    while (i < text.size() && is_space(text[i])) {
      ++i;
    }
    if (i >= text.size()) {
      break;
    }
    const std::size_t start = i;
    while (i < text.size() && !is_space(text[i])) {
      ++i;
    }
    parts.emplace_back(text.substr(start, i - start));
  }
  return parts;
}

std::string json_int(long long value) {
  return std::to_string(value);
}

std::string json_ms(double milliseconds) {
  // Python does round(x, 3): ties-to-even on the decimal value.  Working in
  // integer thousandths is enough (all stages are far below 2^53).
  long long thousandths =
      static_cast<long long>(std::nearbyint(milliseconds * 1000.0));
  const bool negative = thousandths < 0;
  if (negative) {
    thousandths = -thousandths;
  }
  const long long whole = thousandths / 1000;
  long long frac = thousandths % 1000;
  std::string out;
  if (negative && thousandths != 0) {
    out.push_back('-');
  }
  out += std::to_string(whole);
  if (frac == 0) {
    out += ".0";
    return out;
  }
  std::string fractional = std::to_string(frac);
  fractional.insert(fractional.begin(), 3 - fractional.size(), '0');
  while (!fractional.empty() && fractional.back() == '0') {
    fractional.pop_back();
  }
  out.push_back('.');
  out += fractional;
  return out;
}

}  // namespace wn
