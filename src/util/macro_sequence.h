#ifndef UTIL_MACRO_SEQUENCE_H_
#define UTIL_MACRO_SEQUENCE_H_

namespace ks {

// Macro recursion comes from http://jhnet.co.uk/articles/cpp_magic.
// Processing of '(Type)Name,...' comes from
// https://stackoverflow.com/questions/41453/how-can-i-add-reflection-to-a-c-application?rq=1.

#define EVAL(...) EVAL128(__VA_ARGS__)
#define EVAL128(...) EVAL64(EVAL64(__VA_ARGS__))
#define EVAL64(...) EVAL32(EVAL32(__VA_ARGS__))
#define EVAL32(...) EVAL16(EVAL16(__VA_ARGS__))
#define EVAL16(...) EVAL8(EVAL8(__VA_ARGS__))
#define EVAL8(...) EVAL4(EVAL4(__VA_ARGS__))
#define EVAL4(...) EVAL2(EVAL2(__VA_ARGS__))
#define EVAL2(...) EVAL1(EVAL1(__VA_ARGS__))
#define EVAL1(...) __VA_ARGS__

#define TAKE(...) __VA_ARGS__
#define DROP(...)
#define HEAD(x, ...) TAKE x
#define WRAP(...) (__VA_ARGS__),
#define DEFER(m) m DROP()
#define MAP(m, first, ...) m(first)__VA_OPT__(,DEFER(_MAP)()(m, __VA_ARGS__))
#define _MAP() MAP
// Map arguments (R1) a, (R2) b, ... to R1, R2, ...
#define _REPROF(...) HEAD(__VA_ARGS__)
#define REPROF(x) _REPROF(WRAP x)
#define REPRSOF(...) __VA_OPT__(EVAL(MAP(REPROF, __VA_ARGS__)))
// Map arguments (R1) a, (R2) b, ... to a, b, ...
#define NAMEOF(x) DROP x
#define NAMESOF(...) __VA_OPT__(EVAL(MAP(NAMEOF, __VA_ARGS__)))

// More elaborate expansions for generating metadata() fields.
#define MAP_PREFIXED(func, prefix, arg, ...) \
  func(prefix arg)__VA_OPT__(,DEFER(_MAP_PREFIXED)()(func, prefix, __VA_ARGS__))
#define _MAP_PREFIXED() MAP_PREFIXED

#define MAP_ARG(func, extra_arg, arg, ...) \
  func(extra_arg, arg)__VA_OPT__(,DEFER(_MAP_ARG)()(func, extra_arg, __VA_ARGS__))
#define _MAP_ARG() MAP_ARG

}  // namespace ks

#endif  // UTIL_MACRO_SEQUENCE_H_
