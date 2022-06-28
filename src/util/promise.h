#ifndef UTIL_PROMISE_H_
#define UTIL_PROMISE_H_

////////////////////////////////////////////////////////////////////////////////
// Promise: inspired by Javascript Promises/A+
//
// Customized for Idina:
//   * adapts to existing Class::Reset() idiom
//   * implements maximum parallelism in PromiseForAll, with grouping to support
//     certain AWS batching scenarios
//   * no exceptions (i.e. Promise::reject not implemented)
//
// Overview of public methods and functions:
//   Promise: builder of continuation chains that asynchronously provides values
//     Promise::resolve
//     Promise::deferred
//     Promise::then
//     Promise::get
//     Promise::wait
//   MakePromise:   a collection of overloads that a.) wrap values as resolved
//                  promises or b.) wrap Idina functions and classes that take
//                  callbacks as pending promises.
//   PromiseForAll: given a vector of promises, or a vector of values and
//                  mapping from those values to promises, fan-in to a single
//                  promise of type Promise<std::vector<T>> or Promise<void>.
//
// See further comments for more details.
//
#include "util/arena.h"
#include "util/semaphore.h"
#include <assert.h>
#include <coroutine>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stdlib.h>

namespace ks {

template <typename T>
class Promise;

namespace internal {

template <typename T>
struct resolved_value {
  using type = std::optional<bool>;
};
template <typename T>
requires(!std::is_void_v<T>) struct resolved_value<T> {
  using type = std::optional<T>;
};
template <typename T>
using resolved_value_t = typename resolved_value<T>::type;

template <typename T>
struct continuation {
  using type = std::function<void()>;
};
template <typename T>
requires(!std::is_void_v<T> &&
         !std::is_copy_constructible_v<T>) struct continuation<T> {
  static constexpr bool always_move = true;
  static constexpr bool always_copy = false;
  using type = std::function<void(T)>;
};
template <typename T>
requires(!std::is_void_v<T> && std::is_copy_constructible_v<T> &&
         std::is_scalar_v<T>) struct continuation<T> {
  static constexpr bool always_move = false;
  static constexpr bool always_copy = true;
  using type = std::function<void(T)>;
};
template <typename T>
requires(!std::is_void_v<T> && std::is_copy_constructible_v<T> &&
         !std::is_scalar_v<T> && !std::is_const_v<T> &&
         std::is_reference_v<T>) struct continuation<T> {
  static constexpr bool always_move = false;
  static constexpr bool always_copy = false;
  using type = std::function<void(T&)>;
};
template <typename T>
requires(!std::is_void_v<T> && std::is_copy_constructible_v<T> &&
         !std::is_scalar_v<T> && std::is_const_v<T> &&
         std::is_reference_v<T>) struct continuation<T> {
  static constexpr bool always_move = false;
  static constexpr bool always_copy = false;
  using type = std::function<void(const T&)>;
};
template <typename T>
requires(!std::is_void_v<T> && std::is_copy_constructible_v<T> &&
         !std::is_scalar_v<T> &&
         !std::is_reference_v<T>) struct continuation<T> {
  static constexpr bool always_move = false;
  static constexpr bool always_copy = false;
  using type = std::function<void(T&&)>;
};
template <typename T>
using continuation_t = typename continuation<T>::type;

template <typename F, typename T>
struct is_invocable : std::is_invocable<F> {};
template <typename F, typename T>
requires(!std::is_void_v<T>) struct is_invocable<F, T>
    : std::is_invocable<F, T&&> {
};
template <typename F, typename T>
constexpr bool is_invocable_v = is_invocable<F, T>::value;

static inline std::false_type is_promise(...);
template <typename U>
static inline std::true_type is_promise(const volatile Promise<U>*);
template <typename U>
const bool is_promise_v =
    decltype(is_promise(std::declval<typename std::decay<U>::type*>()))::value;

// template <typename T>
// struct is_promise : std::false_type {};
// template <typename T>
// struct is_promise<Promise<T>*> : std::true_type {};
// template <typename T>
// concept is_promise_v = is_promise<std::decay_t<T>>::value;

template <typename T>
struct unwrap_promise {
  using type = T;
};
template <typename T>
requires is_promise_v<T>
struct unwrap_promise<T> {
  using type = typename T::type;
};
template <typename T>
using unwrap_promise_t = typename unwrap_promise<T>::type;

// template<typename Via, typename T>
// concept Viaable = requires (const T) {
// is_invocable_v<Via>
//   using cb_t = continuation_t<T>;
//   via.operator ()(continuation_t<T>)
//         via([fn{std::move(fn)}, cb{std::move(cb)}] {
//
//         via([fn{std::move(fn)}, cb{std::move(cb)}, t{std::forward<T>(t)}]
// };

template <typename T>
struct PromiseState {
  void init_resolved() requires std::is_void_v<T> { resolved_value = true; }

  template <typename V>
  void init_resolved(V&& v) {
    if constexpr (std::is_lvalue_reference_v<V>) {
      resolved_value.template emplace<T>(T(v));
    } else {
      resolved_value.template emplace<T>(std::forward<V>(v));
    }
  }

  void init_rejected(std::exception_ptr eptr) { exception = eptr; }

  void resolve() requires std::is_void_v<T> {
    if (fn) {
      continuation_t<T> fn;
      std::swap(this->fn, fn);
      fn();
    } else {
      std::scoped_lock<std::mutex> lock(mutex);
      assert(!is_resolved());
      resolved_value = true;
    }
  }

  template <typename V>
  void resolve(V&& v) {
    if (fn) {
      continuation_t<T> fn;
      std::swap(this->fn, fn);
      if constexpr (continuation<T>::always_move) {
        fn(std::move(v));
      } else if constexpr (continuation<T>::always_copy ||
                           std::is_lvalue_reference_v<V>) {
        fn(T(v));
      } else {
        fn(std::forward<V>(v));
      }
    } else {
      std::scoped_lock<std::mutex> lock(mutex);
      assert(!is_resolved());
      if constexpr (std::is_lvalue_reference_v<V>) {
        resolved_value.template emplace<T>(T(v));
      } else {
        resolved_value.template emplace<T>(std::forward<V>(v));
      }
    }
  }

  void reject(std::exception_ptr eptr) {
    if (fn_catch) {
      std::function<void(std::exception_ptr)> fn_catch;
      std::swap(this->fn_catch, fn_catch);
      fn_catch(eptr);
    } else {
      std::scoped_lock<std::mutex> lock(mutex);
      exception = eptr;
    }
  }

  bool is_resolved() const {
    return !is_rejected() && resolved_value.has_value();
  }

  bool is_rejected() const { return !!exception; }

  std::mutex mutex;
  continuation_t<T> fn;
  std::function<void(std::exception_ptr)> fn_catch;
  std::exception_ptr exception;
  resolved_value_t<T> resolved_value;
};

}  // namespace internal

inline Promise<void> MakePromise(Arena* arena = nullptr);
template <typename T>
inline Promise<std::remove_cvref_t<T>> MakePromise(T&& t);
template <typename T>
inline Promise<std::remove_cvref_t<T>> MakePromise(Arena*, T&& t);

/**
 * Promise: building block used to construct a continuation chain of functions
 *          with support for asynchronous callbacks.
 */
template <typename T>
class Promise {
  template <typename U>
  static constexpr bool is_promise_v = internal::is_promise_v<U>;
  template <typename F>
  static constexpr bool is_invocable_v = internal::is_invocable_v<F, T>;
  static_assert(!std::is_reference_v<T>,
                "Promised values cannot be references.");
  static_assert(!is_promise_v<T>, "Promised values cannot be promises.");

 public:
  using type = T;

  Promise(Arena* arena = nullptr) : arena_(arena) {
    if (arena) {
      arena_state_ = arena->New<internal::PromiseState<T>>();
    } else {
      shared_state_ = std::make_shared<internal::PromiseState<T>>();
      arena_state_ = nullptr;
    }
  }

  /**
   *Universal constructors for non-promise values are defined to be resolved.
   */
  template <typename V>
  Promise(V&& v) requires(!std::is_void_v<V>) : Promise() {
    state()->init_resolved(std::forward<V>(v));
  }

  /**
   * Universal constructors for non-promise values are defined to be resolved.
   */
  template <typename V>
  Promise(Arena* arena, V&& v) requires(!std::is_void_v<V>) : Promise(arena) {
    state()->init_resolved(std::forward<V>(v));
  }

  /**
   * Universal constructors are deleted for all other promise specializations.
   */
  template <typename U>
  requires(is_promise_v<U>) Promise(Promise<U>&&)
  = delete;

  /**
   * Universal constructors are deleted for all other promise specializations.
   */
  template <typename U>
  requires(is_promise_v<U>) Promise(Arena*, Promise<U>&&)
  = delete;

  /**
   * Special case: provide a way to initialize with a resolved void promise,
   * as zero-argument construction can't do that for us.
   * For the non-void cases, use the below universal constructors.
   */
  static Promise<void> MakeResolved(
      Arena* arena = nullptr) requires std::is_void_v<T> {
    Promise<void> promise(arena);
    promise.state()->init_resolved();
    return promise;
  }

  static Promise<void> MakeRejected(Arena* arena, std::exception_ptr eptr) {
    Promise<void> promise(arena);
    promise.state()->init_rejected(eptr);
    return promise;
  }

  struct promise_type {
    Promise<T> get_return_object() {
      return {.handle_ =
                  std::coroutine_handle<promise_type>::from_promise(*this)};
    }
    std::suspend_never initial_suspend() { return {}; }
    std::suspend_never final_suspend() noexcept { return {}; }
    void unhandled_exception() {}

    template <typename V>
    requires std::is_void_v<T> std::suspend_always yield_value() {
      promise_->resolve();
      return {};
    }

    template <typename V>
    requires(!std::is_void_v<T>) std::suspend_always yield_value(V&& v) {
      promise_->resolve(v);
      return {};
    }

    template <typename V>
    requires std::is_void_v<T> std::suspend_always return_value() {
      promise_->resolve();
      return {};
    }

    template <typename V>
    requires(!std::is_void_v<T>) std::suspend_always return_value(V&& v) {
      promise_->resolve(v);
      return {};
    }

   private:
    Promise<T>* promise_;
  };

  /**
   * Insinuate a callback to a chained then() with the provided value, if any.
   * Specifically: if there is a continuation, propagate the value now;
   * otherwise, store the value in the promise state for later chaining.
   */
  void resolve() requires(std::is_void_v<T>) { state()->resolve(); }
  template <typename V>
  void resolve(V&& v) {
    state()->resolve(std::forward<V>(v));
  }

  void reject(std::exception_ptr eptr) { state()->reject(eptr); }

  /**
   * Obtain a callback that can be used to resolve later.
   */
  internal::continuation_t<T> deferred() {
    internal::continuation_t<T> fn;
    if constexpr (std::is_void_v<T>) {
      if (arena_state_) {
        fn = [s{arena_state_}] { return s->resolve(); };
      } else {
        fn = [s{shared_state_}] { return s->resolve(); };
      }
    } else {
      if (arena_state_) {
        fn = [s{arena_state_}](T t) { return s->resolve(std::move(t)); };
      } else {
        fn = [s{shared_state_}](T t) { return s->resolve(std::move(t)); };
      }
    }
    return fn;
  }

  /**
   * Returns true if there is a pending continuation attached, convenient when
   * creating chains iteratively.
   */
  explicit operator bool() const { return state()->fn != nullptr; }

  //////////////////////////////////////////////////////////////////////////////
  // then(F: T -> X)
  //
  // Specify a continuation function to be called with this promise is resolved.
  // X can be void, non-void, Promise<void>, or Promise<V> for non-void V
  //
  template <typename F>
  requires(!is_invocable_v<F>) auto then(F&& fn) {
    // If the following assertion fires, a continuation passed into a then()
    // handler takes an argument that cannot be obtained by converting the
    // subject promise type T.
    // For example: Promise<int> f; f.then([](std::string) {}); is an error
    // The clang compiler provides a helpful note that we encourage devs to see
    // by disabling clang-format from wrapping the static_assert message.
    // clang-format off
    static_assert(is_invocable_v<F>, "The conflicting T type and then() function are both given in the following note:");
    // clang-format on
  }

  /**
   * Promise type: void
   * Chain
   */
  template <typename F, typename R = std::invoke_result_t<F>>
  requires(is_invocable_v<F>&& std::is_void_v<T>&& is_promise_v<R>) auto then(
      F&& fn) {
    auto* s = state();
    using ResultPromiseType = std::invoke_result_t<F>;
    using InnerPromiseType = typename ResultPromiseType::type;
    if (s->is_resolved()) {
      // 1/2) T:      resolved void
      //      F:      void -> Promise<V> for void or non-void V
      //      action: eval fn(void) now
      //      return: pending Promise<V>
      try {
        return fn();
      } catch (...) {
        return MakeRejectedPromise<InnerPromiseType>(arena_,
                                                     std::current_exception());
      }
    } else if (s->is_rejected()) {
      return MakeRejectedPromise<InnerPromiseType>(arena_, s->exception);
    } else {
      std::scoped_lock<std::mutex> lock(s->mutex);
      Promise<InnerPromiseType> promise(arena_);
      // Note: s->fn lambda captures are mutable because fn can be a
      //       mutable lambda capture.
      ResultPromiseType next;
      try {
        next = fn();
      } catch (...) {
        s->reject(std::current_exception());
      }
      if (arena_) {
        s->fn = [s = promise.arena_state_, next = std::move(next)]() mutable {
          // next.Catch([s] (std::exception_ptr e) {
          //   s->reject(e);
          // });
          if constexpr (std::is_void_v<InnerPromiseType>) {
            // 3) T:      pending void
            //    F:      void -> Promise<void>
            //    action: eval fn() when T is resolved and with its result,
            //            chain T's resolution
            //    return: pending Promise<void> (an unwrapping action)
            next.then([s] { s->resolve(); });
          } else {
            // 4) T:      pending void
            //    F:      void -> Promise<V> for non-void V
            //    action: eval fn() when T is resolved and with its result,
            //            chain T's resolution
            //    return: pending Promise<V> (an unwrapping action)
            next.then([s](InnerPromiseType&& v) {
              s->resolve(std::forward<InnerPromiseType>(v));
            });
          }
        };
      } else {
        s->fn = [s = promise.shared_state_, next = std::move(next)]() mutable {
          // next.Catch([s] (std::exception_ptr e) {
          //   s->reject(e);
          // });
          if constexpr (std::is_void_v<InnerPromiseType>) {
            next.then([s] { s->resolve(); });
          } else {
            next.then([s](InnerPromiseType&& v) {
              s->resolve(std::forward<InnerPromiseType>(v));
            });
          }
        };
      }
      return promise;
    }
  }

  template <typename F, typename R = std::invoke_result_t<F>>
  requires(is_invocable_v<F>&& std::is_void_v<T> &&
           !is_promise_v<R>) auto then(F&& fn) {
    auto* s = state();
    std::scoped_lock<std::mutex> lock(s->mutex);
    if (s->is_resolved()) {
      if constexpr (std::is_void_v<R>) {
        // 5) T:      resolved void
        //    F:      void -> void
        //    action: eval fn() now
        //    return: resolved Promise<void>
        fn();
        return MakePromise(arena_);
      } else {
        // 6) T:      resolved void
        //    F:      void -> V for non-void V
        //    action: eval v = fn() now
        //    return: resolved Promise<V>(v)
        return MakePromise(arena_, fn());
      }
    } else if (s->is_rejected()) {
      return MakeRejectedPromise<R>(arena_, s->exception);
    } else {
      Promise<R> promise(arena_);
      if (arena_) {
        s->fn = [s = promise.arena_state_, fn = std::move(fn)]() mutable {
          if constexpr (std::is_void_v<R>) {
            // 7) T:      pending void
            //    F:      void -> void
            //    action: eval fn() when T is resolved
            //    return: pending Promise<void> automatically resolved to void
            try {
              fn();
              s->resolve();
            } catch (...) {
              s->reject(std::current_exception());
            }
          } else {
            // 8) T:      pending void
            //    F:      void -> V for non-void V
            //    action: eval v = fn() when T is resolved
            //    return: pending Promise<V> automatically resolved to v
            try {
              s->resolve(fn());
            } catch (...) {
              s->reject(std::current_exception());
            }
          }
        };
      } else {
        s->fn = [s = promise.shared_state_, fn = std::move(fn)]() mutable {
          if constexpr (std::is_void_v<R>) {
            try {
              fn();
              s->resolve();
            } catch (...) {
              s->reject(std::current_exception());
            }
          } else {
            try {
              s->resolve(fn());
            } catch (...) {
              s->reject(std::current_exception());
            }
          }
        };
      }
      return promise;
    }
  }

  /**
   * Return type of promise is not void
   * then lambda return type is a promise
   */
  template <typename F, typename R = std::invoke_result_t<F, T>>
  requires(is_invocable_v<F> && !std::is_void_v<T> &&
           is_promise_v<R>) auto then(F&& fn) {
    // There are a total of 16 cases (four of which are folded into two blocks):
    // T: resolved void | resolved non-void | pending void | pending non-void
    // F: T -> void | non-void | Promise<void> | Promise<non-void>
    auto* s = state();
    if (s->is_resolved()) {
      // 9/10) T:      resolved T
      //       F:      T -> Promise<V> for void or non-void V
      //       action: eval fn(T)
      //       return: pending Promise<V>
      return fn(std::move(*s->resolved_value));
    } else {
      std::scoped_lock<std::mutex> lock(s->mutex);
      using V = typename R::type;
      Promise<V> promise(arena_);
      if (arena_) {
        s->fn = [s = promise.arena_state_, fn = std::move(fn)](T&& t) mutable {
          if constexpr (std::is_void_v<V>) {
            // 11) T:      pending T
            //     F:      T -> Promise<void>
            //     action: eval fn() when T is resolved and with its result,
            //             chain T's resolution
            //     return: pending Promise<void> (an unwrapping action)
            fn(std::forward<T>(t)).then([s] { s->resolve(); });
          } else {
            // 12) T:      pending T
            //     F:      T -> Promise<V> for non-void V
            //     action: eval fn() when T is resolved and with its result,
            //             chain T's resolution
            //     return: pending Promise<V> (an unwrapping action)
            fn(std::forward<T>(t)).then([s](V&& v) mutable {
              s->resolve(std::forward<V>(v));
            });
          }
        };
      } else {
        s->fn = [s = promise.shared_state_, fn = std::move(fn)](T&& t) mutable {
          if constexpr (std::is_void_v<V>) {
            fn(std::forward<T>(t)).then([s] { s->resolve(); });
          } else {
            fn(std::forward<T>(t)).then([s](V&& v) mutable {
              s->resolve(std::forward<V>(v));
            });
          }
        };
      }
      return promise;
    }
  }

  /**
   * then lamba return type is not a promise
   */
  template <typename F, typename R = std::invoke_result_t<F, T>>
  requires(is_invocable_v<F> && !std::is_void_v<T> &&
           !is_promise_v<R>) auto then(F&& fn) {
    auto* s = state();
    if (s->is_resolved()) {
      if constexpr (std::is_void_v<R>) {
        // 13) T:      resolved T
        //     F:      T -> void
        //     action: eval fn(T) now
        //     return: resolved Promise<void>
        fn(*std::move(s->resolved_value));
        return MakePromise(arena_);
      } else {
        // 14) T:      resolved T
        //     F:      T -> V for non-void V
        //     action: eval fn(T) now
        //     return: resolved Promise<V>
        return MakePromise(arena_, fn(*std::move(s->resolved_value)));
      }
    } else {
      std::scoped_lock<std::mutex> lock(s->mutex);
      Promise<R> promise(arena_);
      if (arena_) {
        s->fn = [s = promise.arena_state_, fn = std::move(fn)](T&& t) mutable {
          if constexpr (std::is_void_v<R>) {
            // 15) T:      pending T
            //     F:      T -> void
            //     action: eval fn(T) when T is resolved
            //     return: pending Promise<void> automatically resolved to v
            fn(std::forward<T>(t));
            s->resolve();
          } else {
            // 16) T:      pending T
            //     F:      T -> V for some non-void V
            //     action: eval v = fn(T) when T is resolved
            //     return: pending Promise<V> automatically resolved to v
            s->resolve(fn(std::forward<T>(t)));
          }
        };
      } else {
        s->fn = [s = promise.shared_state_, fn = std::move(fn)](T&& t) mutable {
          if constexpr (std::is_void_v<R>) {
            fn(std::forward<T>(t));
            s->resolve();
          } else {
            s->resolve(fn(std::forward<T>(t)));
          }
        };
      }
      return promise;
    }
  }

  // template <typename F, typename E>
  // requires (std::is_base_of_v<std::exception_ptr, E> &&
  // std::is_invocable_v<F, E>) auto katch(F&& fn){
  //   // There are a total of 16 cases (four of which are folded into two
  //   blocks):
  //   // T: resolved void | resolved non-void | pending void | pending non-void
  //   // F: T -> void | non-void | Promise<void> | Promise<non-void>
  //   auto* s = state();
  //   if constexpr (std::is_void_v<T>) {
  //     /**
  //      * then lambda result is a promise
  //      */
  //     if constexpr (is_promise_v<std::invoke_result_t<F>>) {
  //       if (s->is_resolved()) {
  //         // 1/2) T:      resolved void
  //         //      F:      void -> Promise<V> for void or non-void V
  //         //      action: eval fn(void) now
  //         //      return: pending Promise<V>
  //         return fn();
  //       } else {
  //         std::scoped_lock<std::mutex> lock(s->mutex);
  //         using ResultPromiseType = std::invoke_result_t<F>;
  //         using InnerPromiseType = typename ResultPromiseType::type;
  //         Promise<InnerPromiseType> promise(arena_);
  //         // Note: s->fn lambda captures are mutable because fn can be a
  //         //       mutable lambda capture.
  //         if (arena_) {
  //           s->fn = [s=promise.arena_state_, fn=std::move(fn)]() mutable {
  //             if constexpr (std::is_void_v<InnerPromiseType>) {
  //               // 3) T:      pending void
  //               //    F:      void -> Promise<void>
  //               //    action: eval fn() when T is resolved and with its
  //               result,
  //               //            chain T's resolution
  //               //    return: pending Promise<void> (an unwrapping action)
  //               fn().then([s] { s->resolve(); });
  //             } else {
  //               // 4) T:      pending void
  //               //    F:      void -> Promise<V> for non-void V
  //               //    action: eval fn() when T is resolved and with its
  //               result,
  //               //            chain T's resolution
  //               //    return: pending Promise<V> (an unwrapping action)
  //               fn().then([s](InnerPromiseType&& v) {
  //                 s->resolve(std::forward<InnerPromiseType>(v));
  //               });
  //             }
  //           };
  //         } else {
  //           s->fn = [s=promise.shared_state_, fn=std::move(fn)]() mutable {
  //             if constexpr (std::is_void_v<InnerPromiseType>) {
  //               fn().then([s] { s->resolve(); });
  //             } else {
  //               fn().then([s](InnerPromiseType&& v) {
  //                 s->resolve(std::forward<InnerPromiseType>(v));
  //               });
  //             }
  //           };
  //         }
  //         return promise;
  //       }
  //     /**
  //      * then lambda result is not a promise
  //      */
  //     } else {
  //       std::scoped_lock<std::mutex> lock(s->mutex);
  //       using R = std::invoke_result_t<F>;
  //       if (s->is_resolved()) {
  //         if constexpr (std::is_void_v<R>) {
  //             // 5) T:      resolved void
  //             //    F:      void -> void
  //             //    action: eval fn() now
  //             //    return: resolved Promise<void>
  //           fn();
  //           return MakePromise(arena_);
  //         } else {
  //             // 6) T:      resolved void
  //             //    F:      void -> V for non-void V
  //             //    action: eval v = fn() now
  //             //    return: resolved Promise<V>(v)
  //           return MakePromise(arena_, fn());
  //         }
  //       } else {
  //         Promise<R> promise(arena_);
  //         if (arena_) {
  //           s->fn = [s=promise.arena_state_, fn=std::move(fn)]() mutable {
  //             if constexpr (std::is_void_v<R>) {
  //               // 7) T:      pending void
  //               //    F:      void -> void
  //               //    action: eval fn() when T is resolved
  //               //    return: pending Promise<void> automatically resolved to
  //               void fn(); s->resolve();
  //             } else {
  //               // 8) T:      pending void
  //               //    F:      void -> V for non-void V
  //               //    action: eval v = fn() when T is resolved
  //               //    return: pending Promise<V> automatically resolved to v
  //               s->resolve(fn());
  //             }
  //           };
  //         } else {
  //           s->fn = [s=promise.shared_state_, fn=std::move(fn)]() mutable {
  //             if constexpr (std::is_void_v<R>) {
  //               fn();
  //               s->resolve();
  //             } else {
  //               s->resolve(fn());
  //             }
  //           };
  //         }
  //         return promise;
  //       }
  //     }
  //   /**
  //    * Return type of promise is not void
  //    */
  //   } else {
  //     /**
  //      * then lambda return type is a promise
  //      */
  //     if constexpr (is_promise_v<std::invoke_result_t<F, T&&>>) {
  //       using R = std::invoke_result_t<F, T&&>;
  //       if (s->is_resolved()) {
  //         // 9/10) T:      resolved T
  //         //       F:      T -> Promise<V> for void or non-void V
  //         //       action: eval fn(T)
  //         //       return: pending Promise<V>
  //         return fn(std::move(*s->resolved_value));
  //       } else {
  //         std::scoped_lock<std::mutex> lock(s->mutex);
  //         using V = typename R::type;
  //         Promise<V> promise(arena_);
  //         if (arena_) {
  //           s->fn = [s=promise.arena_state_, fn=std::move(fn)](T&& t) mutable
  //           {
  //             if constexpr (std::is_void_v<V>) {
  //               // 11) T:      pending T
  //               //     F:      T -> Promise<void>
  //               //     action: eval fn() when T is resolved and with its
  //               result,
  //               //             chain T's resolution
  //               //     return: pending Promise<void> (an unwrapping action)
  //               fn(std::forward<T>(t)).then([s] { s->resolve(); });
  //             } else {
  //               // 12) T:      pending T
  //               //     F:      T -> Promise<V> for non-void V
  //               //     action: eval fn() when T is resolved and with its
  //               result,
  //               //             chain T's resolution
  //               //     return: pending Promise<V> (an unwrapping action)
  //               fn(std::forward<T>(t)).then([s](V&& v) mutable {
  //                 s->resolve(std::forward<V>(v));
  //               });
  //             }
  //           };
  //         } else {
  //           s->fn = [s=promise.shared_state_, fn=std::move(fn)](T&& t)
  //           mutable {
  //             if constexpr (std::is_void_v<V>) {
  //               fn(std::forward<T>(t)).then([s] { s->resolve(); });
  //             } else {
  //               fn(std::forward<T>(t)).then([s](V&& v) mutable {
  //                 s->resolve(std::forward<V>(v));
  //               });
  //             }
  //           };
  //         }
  //         return promise;
  //       }
  //     /**
  //      * then lamba return type is not a promise
  //      */
  //     } else {
  //       using R = std::invoke_result_t<F, T&&>;
  //       if (s->is_resolved()) {
  //         if constexpr (std::is_void_v<R>) {
  //           // 13) T:      resolved T
  //           //     F:      T -> void
  //           //     action: eval fn(T) now
  //           //     return: resolved Promise<void>
  //           fn(*std::move(s->resolved_value));
  //           return MakePromise(arena_);
  //         } else {
  //           // 14) T:      resolved T
  //           //     F:      T -> V for non-void V
  //           //     action: eval fn(T) now
  //           //     return: resolved Promise<V>
  //           return MakePromise(arena_, fn(*std::move(s->resolved_value)));
  //         }
  //       } else {
  //         std::scoped_lock<std::mutex> lock(s->mutex);
  //         Promise<R> promise(arena_);
  //         if (arena_) {
  //           s->fn = [s=promise.arena_state_, fn=std::move(fn)](T&& t) mutable
  //           {
  //             if constexpr (std::is_void_v<R>) {
  //               // 15) T:      pending T
  //               //     F:      T -> void
  //               //     action: eval fn(T) when T is resolved
  //               //     return: pending Promise<void> automatically resolved
  //               to v fn(std::forward<T>(t)); s->resolve();
  //             } else {
  //               // 16) T:      pending T
  //               //     F:      T -> V for some non-void V
  //               //     action: eval v = fn(T) when T is resolved
  //               //     return: pending Promise<V> automatically resolved to v
  //               s->resolve(fn(std::forward<T>(t)));
  //             }
  //           };
  //         } else {
  //           s->fn = [s=promise.shared_state_, fn=std::move(fn)](T&& t)
  //           mutable {
  //             if constexpr (std::is_void_v<R>) {
  //               fn(std::forward<T>(t));
  //               s->resolve();
  //             } else {
  //               s->resolve(fn(std::forward<T>(t)));
  //             }
  //           };
  //         }
  //         return promise;
  //       }
  //     }
  //   }
  // }

  template <typename F, typename E>
  requires(std::is_base_of_v<std::exception, E>&&
               std::is_invocable_v<F, E>) auto Catch(F&& fn) {
    // promise.exception_.
    // fn(std::forward());
    // return promise;
  }

  /**
   * Obtain the resolved value of this promise, blocking as needed.
   */
  auto get() {
    auto* s = state();
    if (s->is_resolved()) {
      if constexpr (!std::is_void_v<T>) {
        return *std::move(s->resolved_value);
      }
    } else {
      Semaphore sem;
      if constexpr (std::is_void_v<T>) {
        then([&sem]() mutable { sem.post(); });
        sem.wait();
      } else {
        T result;
        then([&sem, &result](T&& t) mutable {
          result = std::move(t);
          sem.post();
        });
        sem.wait();
        return std::move(result);
      }
    }
  }

  /**
   * Use get() to get the resolved promise (blocking as needed), and create
   * a resolved promise to result. Often used at a tail of a Promise<void>
   * continuation chain as get() on a Promise<void> is somewhat non-idiomatic.
   */
  auto wait() {
    if constexpr (std::is_void_v<T>) {
      get();
      return MakePromise();
    } else {
      return MakePromise(get());
    }
  }

  /**
   * then_via()
   *
   * A form of then() that delegates to via, a function expected to schedule the
   * invocation of the given continuation function fn.
   */
  template <typename Via, typename F>
  requires(is_invocable_v<F>) auto then_via(Via&& via, F&& fn) {
    if constexpr (std::is_void_v<T>) {
      using R = std::invoke_result_t<F>;
      Promise<internal::unwrap_promise_t<R>> promise;
      then([via{std::move(via)}, fn{std::move(fn)}, cb{promise.deferred()}] {
        via([fn{std::move(fn)}, cb{std::move(cb)}] {
          MakePromise().then(std::move(fn)).then(std::move(cb));
        });
      });
      return promise;
    } else {
      using R = std::invoke_result_t<F, T&&>;
      Promise<internal::unwrap_promise_t<R>> promise;
      then([via{std::move(via)}, fn{std::move(fn)},
            cb{promise.deferred()}](T&& t) {
        via([fn{std::move(fn)}, cb{std::move(cb)},
             t{std::forward<T>(t)}]() mutable {
          MakePromise(std::forward<T>(t))
              .then(std::move(fn))
              .then(std::move(cb));
        });
      });
      return promise;
    }
  }

 private:
  template <class U>
  friend class Promise;

  internal::PromiseState<T>* state() const {
    return arena_state_ ? arena_state_ : shared_state_.get();
  }
  Arena* arena_;
  internal::PromiseState<T>* arena_state_;
  std::shared_ptr<internal::PromiseState<T>> shared_state_;
  std::exception_ptr exception_;
  std::coroutine_handle<promise_type> handle_;
};

/**
 * MakePromise
 *
 * This is a collection of overloads addressing several interop scenarios.
 * 1. To wrap a void or non-void values with a synchronously resolved promise.
 *    This is typically used in then() bodies that returns promises
 *    in some of its conditional branches. (Note: then() will unwrap
 *    nested promises: i.e. if the body of then() returns Promise<Promise<T>>,
 *    then() itself return a Promise<T>.)
 * 2. To wrap an asynchronous callback with Promise<void>, where callback is
 *    found as the last argument in a a.) Reset method, b.) constructor, or
 *    c.) function.
 */
inline Promise<void> MakePromise(Arena* arena) {
  // void -> Promise<void>
  // Special case: a default constructor normally gives us an unresolved
  // promise. We'd rather have a resolved promise that also shortcuts locking
  // (knowing that promise initializations can't race continuation chaining).
  return Promise<void>::MakeResolved(arena);
}

// With the exception of std::shared_ptr MakePromise encourages moves.
template <typename T>
inline Promise<std::remove_cvref_t<T>> MakePromise(T&& t) {
  // non-void T -> Promise<T>
  using U = std::remove_reference_t<T>;
  return Promise<U>(std::forward<U>(t));
}

// With the exception of std::shared_ptr MakePromise encourages moves.
template <typename T>
inline Promise<std::remove_cvref_t<T>> MakePromise(Arena* arena, T&& t) {
  // non-void T -> Promise<T>
  using U = std::remove_reference_t<T>;
  return Promise<U>(arena, std::forward<U>(t));
}

template <typename T>
inline Promise<std::shared_ptr<T>> MakePromise(std::shared_ptr<T> p) {
  // std::shared_ptr<T> -> Promise<std::shared_ptr<T>>
  return Promise<std::shared_ptr<T>>(std::move(p));  // this move is okay
}

template <typename T>
inline Promise<std::shared_ptr<T>> MakePromise(Arena* arena,
                                               std::shared_ptr<T> p) {
  // std::shared_ptr<T> -> Promise<std::shared_ptr<T>>
  return Promise<std::shared_ptr<T>>(arena, std::move(p));  // this move is okay
}

template <typename T>
requires std::is_void_v<T>
inline Promise<T> MakeRejectedPromise(Arena* arena, std::exception_ptr e) {
  // void -> Promise<void>
  // Special case: a default constructor normally gives us an unresolved
  // promise. We'd rather have a resolved promise that also shortcuts locking
  // (knowing that promise initializations can't race continuation chaining).
  return Promise<void>::MakeRejected(arena, e);
}

// With the exception of std::shared_ptr MakePromise encourages moves.
template <typename T>
inline Promise<T> MakeRejectedPromise(std::exception_ptr e) {
  // non-void T -> Promise<T>
  using U = std::remove_reference_t<T>;
  Promise<U> promise;
  promise.reject(e);
  return promise;
}

// With the exception of std::shared_ptr MakePromise encourages moves.
template <typename T>
inline Promise<T> MakeRejectedPromise(Arena* arena, std::exception_ptr e) {
  // non-void T -> Promise<T>
  using U = std::remove_reference_t<T>;
  Promise<U> promise(arena);
  promise.reject(e);
  return promise;
}

// MakeViaPromise: wrapper that invokes then_via() from a resolved promise.
template <typename Via, typename F>
auto MakeViaPromise(Via&& via, F&& fn) {
  return MakePromise().then_via(std::move(via), std::move(fn));
}
template <typename Via, typename F>
auto MakeViaPromise(Arena* arena, Via&& via, F&& fn) {
  return MakePromise(arena).then_via(std::move(via), std::move(fn));
}

template <typename F, typename... Args>
requires std::is_function_v<F(Args&&...)> Promise<void> MakePromise(
    F&& fn, Args&&... args) {
  Promise<void> promise;
  fn(std::forward<Args>(args)..., promise.deferred());
  return promise;
}

template <typename T, typename F, typename... Args>
requires std::is_function_v<F(Args&&...)> Promise<T> MakeTypedPromise(
    F&& fn, Args&&... args) {
  Promise<T> promise;
  fn(std::forward<Args>(args)..., promise.deferred());
  return promise;
}

template <typename F, typename... Args>
requires std::is_function_v<F(Args&&...)> Promise<void> MakePromise(
    Arena* arena, F&& fn, Args&&... args) {
  Promise<void> promise(arena);
  fn(std::forward<Args>(args)..., promise.deferred());
  return promise;
}

template <typename T, typename F, typename... Args>
requires std::is_function_v<F(Args&&...)> Promise<T> MakeTypedPromise(
    Arena* arena, F&& fn, Args&&... args) {
  Promise<T> promise(arena);
  fn(std::forward<Args>(args)..., promise.deferred());
  return promise;
}

}  // namespace ks

#endif  // UTIL_PROMISE_H_
