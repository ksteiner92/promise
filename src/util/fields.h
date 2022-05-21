#ifndef UTIL_FIELDS_H_
#define UTIL_FIELDS_H_

#include <tuple>
#include <utility>

#include "macro_sequence.h"
#include "slice.h"

namespace ks {

// To declare what fields are on a struct for programatic enumeration, use the
// FIELDS macro.  This will create add methods to create a tie of the fields,
// as well, as return the name of the fields.
#define _FIELDS(...) \
  typedef decltype(std::tie(__VA_ARGS__)) TieType; \
  TieType tie() { \
    return std::tie(__VA_ARGS__); \
  } \
  auto tie() const -> decltype(std::tie(__VA_ARGS__)) { \
    return std::tie(__VA_ARGS__); \
  } \
  static const constexpr char* field_name_list() { \
    return #__VA_ARGS__; \
  } \
  static const constexpr char* field_name_start(int i) { \
    return field_name_list() + \
           fields_internal::FindParam(field_name_list(), 0, i); \
  } \
  static inline Slice field_name(int i) { \
    return Slice(field_name_start(i), \
                 fields_internal::FindNameEnd(field_name_start(i), 0)); \
  } \
  template <class _T> bool operator<(const _T& t) const { \
    return ((_T&)*this).tie() < ((_T&)t).tie(); \
  } \
  template <class _T> bool operator<=(const _T& t) const { \
    return ((_T&)*this).tie() <= ((_T&)t).tie(); \
  } \
  template <class _T> bool operator>(const _T& t) const { \
    return ((_T&)*this).tie() > ((_T&)t).tie(); \
  } \
  template <class _T> bool operator>=(const _T& t) const { \
    return ((_T&)*this).tie() >= ((_T&)t).tie(); \
  } \
  template <class _T> bool operator==(const _T& t) const { \
    return ((_T&)*this).tie() == ((_T&)t).tie(); \
  } \
  template <class _T> bool operator!=(const _T& t) const { \
    return ((_T&)*this).tie() != ((_T&)t).tie(); \
  }
#define FIELDS(...) _FIELDS(__VA_ARGS__)

namespace fields_internal {

static constexpr bool IsIdentifierChar(char c) {
  return
      (c >= '0' && c <= '9') ||
      (c >= 'a' && c <= 'z') ||
      (c >= 'A' && c <= 'Z') ||
      c == '_';
}

// Given a string s like "blah, foobar, cheeseball" this returns the index of
// say, foobar.  Pos is where to start counting from, and how_many is how many
// ',' to pass by before looking for an indentifier.
inline constexpr int FindParam(const char *s, int pos, int how_many) {
  return
      (how_many == 0 && IsIdentifierChar(s[pos])) ?
      pos :
      FindParam(s,
                 pos + 1,
                 how_many - (s[pos] == ','));
}

// Given a position in a string, this finds the end of the current identifier.
inline constexpr int FindNameEnd(const char *s, int pos) {
  return IsIdentifierChar(s[pos]) ? FindNameEnd(s, pos+1) : pos;
}

}  // namespace fields_internal
}  // namespace ks

#endif  // UTIL_FIELDS_H_
