#ifndef UTIL_PROMISE_ALL_H_
#define UTIL_PROMISE_ALL_H_

#include <iterator>
#include <vector>
#include "promise.h"

namespace ks {

///////////////////////////////////////////////////////////////////////////////
// PromiseForAll
//
// Applies iterated iterated elements to given function that maps each to a
// promise. When all of these are resolved, the returned Promise<std:vector<T>>
// is resolved.  If T is void, we return Promise<void>.
//
// Optional behavior is specified with PromiseForAllOptions.
struct PromiseForAllOptions {
  // If batch_size is specified, mapped promises are invoked in batches, where
  // all the promises in a batch must be resolved before the next batch begins.
  // By default, all promises are invoked in a single batch.  This is
  // particularly used instead of max_parallelism in Idina scenarios that cannot
  // block with wait(), but both options can still be used in tandem.
  size_t batch_size = std::numeric_limits<size_t>::max();

  // If max_parallelism is non-zero, a semaphore wait is use to limit the
  // number of concurrent promises that can be executed.
  size_t max_parallelism = 0;

  // If stride > 1, the promise map function is called with a range of values
  // of up to size stride. By default, the promise map function is called with
  // a single value. This parameter is intended for grouping multiple values
  // into one asynchronous operation.
  size_t stride = 1;

  // Optional arena for allocation of promises
  Arena* arena = nullptr;
};

namespace internal {

template <typename T>
struct ForAllState {
  ForAllState(const PromiseForAllOptions& options_in, size_t count_in) :
      options(options_in), in_flight(options.max_parallelism),
      count_in(count_in) {
  }
  PromiseForAllOptions options;
  Semaphore in_flight;
  std::atomic<size_t> count_in;
  std::mutex mutex;
  std::vector<T> values;
  std::function<void(std::vector<T>)> done;
};

template <>
struct ForAllState<void> {
  ForAllState(const PromiseForAllOptions& options_in, size_t count_in) :
      options(options_in), in_flight(options.max_parallelism),
      count_in(count_in) {
  }
  PromiseForAllOptions options;
  Semaphore in_flight;
  std::atomic<size_t> count_in;
  std::function<void()> done;
};

template<typename Vs, typename F, typename = void>
struct has_iter_args : std::false_type {};
template <typename Vs, typename F>
struct has_iter_args<Vs, F, std::enable_if_t<std::is_invocable_v<F,
    typename Vs::const_iterator, typename Vs::const_iterator>>> :
    std::true_type {};
template<typename Vs, typename F, typename I = typename Vs::const_iterator>
requires (!has_iter_args<Vs, F>::value)
auto call_map(const F& map, I begin, I end) {
  return map(*begin);
}
template<typename Vs, typename F, typename I = typename Vs::const_iterator>
requires (has_iter_args<Vs, F>::value)
auto call_map(const F& map, I begin, I end) {
  return map(begin, end);
}

}  // namespace internal

// PromiseForAll overload that works with iterators across a collection of
// values, each of which is mapped to a promise using the given map function.
// Max paralellism/stride options are handled here. Use overload that takes a
// vector for handling batching (non-blocking, unlike max_parallelism).
template <typename Vs, typename F, typename I = typename Vs::const_iterator>
inline auto PromiseForAll(I begin, I end, const F& map,
    PromiseForAllOptions options = {}) {
  using P = decltype(internal::call_map<Vs, F>(map, begin, end));
  static_assert(internal::is_promise_v<P>, "map must return a promise");
  using T = typename P::type;
  Promise<std::conditional_t<std::is_void_v<T>, void, std::vector<T>>>
      result_promise(options.arena);
  size_t count_out = std::distance(begin, end);
  if (!count_out) {
    if constexpr (std::is_void_v<T>) {
      result_promise.resolve();
    } else {
      result_promise.resolve(std::vector<T>{});
    }
    return result_promise;
  }
  size_t stride = std::max<size_t>(options.stride, 1);
  size_t count_in = count_out / stride + count_out % stride;

  auto* state = options.arena
      ? options.arena->New<internal::ForAllState<T>>(options, count_in)
      : new internal::ForAllState<T>(options, count_in);
  state->done = result_promise.deferred();
  while (count_out) {
    size_t n = std::min(stride, count_out);
    auto next = begin + n;
    count_out -= n;
    if (options.max_parallelism) state->in_flight.wait();
    auto promise = internal::call_map<Vs, F>(map, begin, next);
    if constexpr (std::is_void_v<T>) {
      promise.then([state] {
        if (state->options.max_parallelism) state->in_flight.post();
        if (--state->count_in == 0) {
          state->done();
          if (state->options.arena) {
            //state->~ForAllState<void>();
          } else {
            delete state;
          }
        }
      });
    } else {
      promise.then([state](T&& t) {
        if (state->options.max_parallelism) state->in_flight.post();
        {
          std::lock_guard<std::mutex> lock(state->mutex);
          state->values.push_back(std::forward<T>(t));
        }
        if (--state->count_in == 0) {
          state->done(std::move(state->values));
          if (state->options.arena) {
            //state->~ForAllState<T>();
          } else {
            delete state;
          }
        }
      });
    }
    begin = next;
  }
  assert(begin == end);
  return result_promise;
}

// PromiseForAll overload that takes a vector and optionally handles batching
// using PromiseForAllOptions::batch_size.  Batching is offered in lieu of using
// max_parallelism for cases we cannot block; the tradeoff is that each batch
// of promises must be resolved before the next batch of promises are created.
template <typename Vs, typename F>
inline auto PromiseForAll(const Vs& values, const F& map,
    PromiseForAllOptions options = {}) {
  auto begin = std::begin(values), end = std::end(values);
  using P = decltype(internal::call_map<Vs, F>(map, begin, end));
  static_assert(internal::is_promise_v<P>, "map must return a promise");
  using T = typename P::type;
  if (begin == end) return PromiseForAll<Vs, F>(begin, end, map, options);
  if constexpr (std::is_void_v<T>) {
    Promise<void> start_promise(options.arena), promise(options.arena);
    for (size_t c = std::distance(begin, end), n; c; c -= n, begin += n) {
      n = std::min(c, options.batch_size);
      promise = (start_promise ? promise : start_promise).then(
          [begin, end{begin + n}, map, options] {
        return PromiseForAll<Vs, F>(begin, end, map, options);
      });
    }
    start_promise.resolve();
    return promise;
  } else {
    Promise<std::vector<T>> start_promise(options.arena), promise(options.arena);
    for (size_t c = std::distance(begin, end), n; c; c -= n, begin += n) {
      n = std::min(c, options.batch_size);
      promise = (start_promise ? promise : start_promise).then(
          [begin, end{begin + n}, map, options](std::vector<T>&& accum) {
        return PromiseForAll<Vs, F>(begin, end, map, options).then(
            [accum{std::move(accum)}](std::vector<T>&& out_values) mutable {
          for (auto&& value : out_values) accum.push_back(std::move(value));
          return std::move(accum);
        });
      });
    }
    start_promise.resolve(std::vector<T>{});
    return promise;
  }
}

// PromiseForAll overload that is directly given a collection of promises to
// fan-in.
template <typename Promises>
inline auto PromiseForAll(const Promises& promises,
    PromiseForAllOptions options = {}) {
  return PromiseForAll(promises, [](auto promise) { return promise; }, options);
}

}

#endif // UTIL_PROMISE_ALL_H_
