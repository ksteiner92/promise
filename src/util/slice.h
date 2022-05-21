#ifndef UTILS_SLICE_H_
#define UTILS_SLICE_H_

#include <cassert>
#include <cstddef>
#include <cstring>
#include <string>
#include <string_view>

namespace ks {

class Slice : public std::string_view {
 public:
  // Create an empty slice.
  Slice() : std::string_view("", 0) {}

  Slice(std::string_view s) : std::string_view(s.data(), s.size()) {}

  // Create a slice that refers to d[0,n-1].
  Slice(const char* d, size_t n) : std::string_view(d, n) {}

  // Create a slice that refers to the contents of "s"
  Slice(const std::string& s) : std::string_view(s.data(), s.size()) {}

  // Create a slice that refers to s[0,strlen(s)-1]
  Slice(const char* s) : std::string_view(s) {}

  // Intentionally copyable.
  Slice(const Slice&) = default;
  Slice& operator=(const Slice&) = default;

  // Return a string that contains the copy of the referenced data.
  std::string ToString() const { return std::string(data(), size()); }

  Slice range(size_t start, size_t end) const {
    return Slice(substr(start, end - start));
  }

  Slice ltrim() {
    std::string_view::remove_prefix(std::distance(
        cbegin(), std::find_if(cbegin(), cend(),
                               [](int c) { return !std::isspace(c); })));
    return *this;
  }

  Slice rtrim() {
    std::string_view::remove_suffix(std::distance(
        crbegin(), std::find_if(crbegin(), crend(),
                                [](int c) { return !std::isspace(c); })));
    return *this;
  }

  void remove_suffix(Slice suffix) {
    if (ends_with(suffix)) {
      std::string_view::remove_suffix(suffix.size());
    }
  }

  void remove_prefix(Slice prefix) {
    if (starts_with(prefix)) {
      std::string_view::remove_prefix(prefix.size());
    }
  }

  Slice trim() {
    ltrim();
    rtrim();
    return *this;
  }
};

}  // namespace ks

#endif  // UTILS_SLICE_H_
