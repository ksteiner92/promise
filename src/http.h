#ifndef HTTP_H_
#define HTTP_H_

#include "util/buffer.h"
#include "util/concepts.h"
#include "util/fields.h"
#include "util/macro_sequence.h"
#include "util/promise.h"
#include "util/semaphore.h"
#include "util/slice.h"
#include <curl/curl.h>
#include <condition_variable>
#include <functional>
#include <list>
#include <memory>
#include <mutex>
#include <optional>
#include <stack>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace ks {
namespace internal {

/**
 * @brief Creates a SFINAE test to check if a type contains a field with
 * specified name
 */
#define CURL_SFINAE_TEST(field)                    \
  template <typename T>                            \
  concept is_##field = Fields<T>&& requires(T v) { \
    {std::declval<decltype(T::field)>()};          \
  };

CURL_SFINAE_TEST(curl_infos);
CURL_SFINAE_TEST(curl_options);
CURL_SFINAE_TEST(curl_multi_options);

/**
 * @brief A template recursive function which fills values into members of the
 * given object with the assigned information type using curl_easy_getinfo.
 *
 * @tparam T The type of the objects containing the fields where values should
 * be assigned
 * @tparam I Template iteration variable
 * @param curl The curl easy object from which the values are retreived
 * @param x The object in which the curl information are stored
 *
 * @return CURLE_OK on successfull completion otherwise a CURLcode error.
 */
template <typename T, unsigned I = 0>
static constexpr CURLcode CurlDeserializeImpl(CURL* curl, T* x) {
  auto information_type = std::get<I>(T::curl_infos);
  using FieldType = std::decay_t<std::tuple_element_t<I, typename T::TieType>>;
  CURLcode ret;
  if constexpr (std::is_same_v<FieldType, Slice> ||
                std::is_same_v<FieldType, std::string>) {
    char* tmp;
    if ((ret = curl_easy_getinfo(curl, information_type, &tmp)) != CURLE_OK) {
      return ret;
    }
    if (tmp) std::get<I>(x->tie()) = FieldType(tmp);
  } else if constexpr (is_curl_infos<FieldType>) {
    if ((ret = CurlDeserializeImpl<FieldType, 0>(
             curl, &std::get<I>(x->tie()))) != CURLE_OK) {
      return ret;
    }
  } else {
    if ((ret = curl_easy_getinfo(curl, information_type,
                                 &std::get<I>(x->tie()))) != CURLE_OK) {
      return ret;
    }
  }
  constexpr size_t kNumFields = std::tuple_size_v<typename T::TieType>;
  if constexpr (I < kNumFields - 1) {
    return CurlDeserializeImpl<T, I + 1>(curl, x);
  } else {
    return CURLE_OK;
  }
}

/**
 * @brief Function which sets an option on a curl easy object.
 *
 * @tparam OptionType Curl easy option type
 * @tparam T Type holding the option information
 * @param curl Curl easy object
 * @param option Easy option type
 * @param t Object holding the curl easy option value
 *
 * @return Returns the result of the curl_easy_setopt call
 */
template <typename OptionType, typename T>
static constexpr CURLcode SetEasyOpt(CURL* curl, const OptionType& option,
                                     const T& t) {
  using FieldType = std::decay_t<T>;
  if constexpr (std::is_same_v<FieldType, Slice>) {
    return curl_easy_setopt(curl, option, t.ToString().c_str());
  } else if constexpr (std::is_same_v<FieldType, std::string>) {
    return curl_easy_setopt(curl, option, t.c_str());
  } else {
    return curl_easy_setopt(curl, option, t);
  }
}

/**
 * @brief Function which sets an option on a curl multi object.
 *
 * @tparam OptionType Curl multi option type
 * @tparam T Type holding the multi option information
 * @param curl Curl multi object
 * @param option Multi option type
 * @param t Object holding the multi option value
 *
 * @return Returns the result of the curl_multi_setopt call
 */
template <typename OptionType, typename T>
static constexpr CURLMcode SetMultiOpt(CURLM* curlm, const OptionType& option,
                                       const T& t) {
  using FieldType = std::decay_t<T>;
  if constexpr (std::is_same_v<FieldType, Slice>) {
    return curl_multi_setopt(curlm, option, t.ToString().c_str());
  } else if constexpr (std::is_same_v<FieldType, std::string>) {
    return curl_multi_setopt(curlm, option, t.c_str());
  } else {
    return curl_multi_setopt(curlm, option, t);
  }
}

/**
 * @brief A template recursive function which sets curl easy options on a given
 * curl object.
 *
 * @tparam T The type of the objects containing the curl easy option values
 * @tparam I Template iteration variable
 * @param curl The curl easy object from which the values are retreived
 * @param x The option object
 *
 * @return CURLE_OK on successfull completion otherwise a CURLcode error.
 */
template <typename T, unsigned I = 0>
static constexpr CURLcode CurlEasyOptSetterImpl(CURL* curl, const T& x) {
  auto option_type = std::get<I>(T::curl_options);
  using FieldType = std::decay_t<std::tuple_element_t<I, typename T::TieType>>;
  auto val = std::get<I>(x.tie());
  CURLcode ret;
  if constexpr (is_optional<FieldType>::value) {
    if (val.has_value()) {
      if ((ret = SetEasyOpt(curl, option_type, *val)) != CURLE_OK) {
        return ret;
      }
    }
  } else {
    if ((ret = SetEasyOpt(curl, option_type, val)) != CURLE_OK) {
      return ret;
    }
  }
  constexpr size_t kNumFields = std::tuple_size_v<typename T::TieType>;
  if constexpr (I < kNumFields - 1) {
    return CurlEasyOptSetterImpl<T, I + 1>(curl, x);
  } else {
    return CURLE_OK;
  }
}

/**
 * @brief A template recursive function which sets curl multi options on a given
 * curl multi object.
 *
 * @tparam T The type of the objects containing the curl multi option values
 * @tparam I Template iteration variable
 * @param curlm The curl multi object from which the values are retreived
 * @param x The option object
 *
 * @return CURLM_OK on successfull completion otherwise a CURLMcode error.
 */
template <typename T, unsigned I = 0>
static constexpr CURLMcode CurlMultiOptSetterImpl(CURLM* curlm, const T& x) {
  auto option_type = std::get<I>(T::curl_multi_options);
  using FieldType = std::decay_t<std::tuple_element_t<I, typename T::TieType>>;
  auto val = std::get<I>(x.tie());
  CURLMcode ret;
  if constexpr (is_optional<FieldType>::value) {
    if (val.has_value()) {
      if ((ret = SetMultiOpt(curlm, option_type, *val)) != CURLM_OK) {
        return ret;
      }
    }
  } else {
    if ((ret = SetMultiOpt(curlm, option_type, val)) != CURLM_OK) {
      return ret;
    }
  }
  constexpr size_t kNumFields = std::tuple_size_v<typename T::TieType>;
  if constexpr (I < kNumFields - 1) {
    return CurlMultiOptSetterImpl<T, I + 1>(curlm, x);
  } else {
    return CURLM_OK;
  }
}

}  // namespace internal

// Dummy curl info type to indicate a nested info object
static constexpr CURLINFO NESTED_CURL_INFOS = CURLINFO_NONE;

// See more curl informations at
// https://curl.se/libcurl/c/curl_easy_getinfo.html
#define _CURL_INFOS(...)                                            \
  static constexpr auto curl_infos = std::make_tuple(__VA_ARGS__);  \
  template <typename T>                                             \
  static constexpr CURLcode CurlDeserializeImpl(CURL* curl, T* x) { \
    (void)curl_infos;                                               \
    if constexpr (std::tuple_size_v < typename T::TieType >> 0) {   \
      return ks::internal::CurlDeserializeImpl<T>(curl, x);         \
    } else {                                                        \
      return CURLE_OK;                                              \
    }                                                               \
  }

#define CURL_INFOS(...)        \
  FIELDS(NAMESOF(__VA_ARGS__)) \
  _CURL_INFOS(REPRSOF(__VA_ARGS__))

// See more curl options at
// https://curl.se/libcurl/c/easy_setopt_options.html
#define _CURL_OPTIONS(...)                                                  \
  static constexpr auto curl_options = std::make_tuple(__VA_ARGS__);        \
  template <typename T>                                                     \
  static constexpr CURLcode CurlEasyOptSetterImpl(CURL* curl, const T& x) { \
    (void)curl_options;                                                     \
    if constexpr (std::tuple_size_v < typename T::TieType >> 0) {           \
      return ks::internal::CurlEasyOptSetterImpl(curl, x);                  \
    } else {                                                                \
      return CURLE_OK;                                                      \
    }                                                                       \
  }

#define CURL_OPTIONS(...)      \
  FIELDS(NAMESOF(__VA_ARGS__)) \
  _CURL_OPTIONS(REPRSOF(__VA_ARGS__))

// See more curl options at
// https://curl.se/libcurl/c/multi_setopt_options.html
#define _CURL_MULTI_OPTIONS(...)                                           \
  static constexpr auto curl_multi_options = std::make_tuple(__VA_ARGS__); \
  template <typename T>                                                    \
  static constexpr CURLMcode CurlMultiOptSetterImpl(CURLM* curlm,          \
                                                    const T& x) {          \
    (void)curl_multi_options;                                              \
    if constexpr (std::tuple_size_v < typename T::TieType >> 0) {          \
      return ks::internal::CurlMultiOptSetterImpl(curlm, x);               \
    } else {                                                               \
      return CURLM_OK;                                                     \
    }                                                                      \
  }

#define CURL_MULTI_OPTIONS(...) \
  FIELDS(NAMESOF(__VA_ARGS__))  \
  _CURL_MULTI_OPTIONS(REPRSOF(__VA_ARGS__))

using HeaderParam = std::pair<Slice, Slice>;

/**
 * @brief Base type for all curl information types. A curl information type is a
 * struct using the CURL_INFOS macro. This base type contains the minimal
 * information which will always be set.
 */
struct FetchDataBase {
  Slice error;
  Slice content;
  std::vector<HeaderParam> header_params;
};

/**
 * @brief An empty curl easy option type
 */
struct EmptyEasyOptions {
  CURL_OPTIONS();
};

/**
 * @brief All supported curl multi options and any additional options for
 * FetchQueue. An option can either be set or unset. If unset, curl behaves as
 * if the corresponding curl_easy_setopt call has never been made.
 * See https://curl.se/libcurl/c/multi_setopt_options.html for possible options.
 */
struct HttpClientOptions {
  std::optional<long> chunk_length_penalty_size;
  std::optional<long> content_length_penalty_size;
  std::optional<long> maxconnects;
  std::optional<long> max_concurrent_streams;
  std::optional<long> max_host_connections;
  std::optional<long> max_pipeline_length;
  std::optional<long> max_total_connections;
  std::optional<bool> pipelining;
  // How many requests the fetch queue should process at once. If set to zero,
  // no restriction is given and could lead to performance problems.
  size_t max_requests_in_flight = 0;

  CURL_MULTI_OPTIONS(
      (CURLMOPT_CHUNK_LENGTH_PENALTY_SIZE)chunk_length_penalty_size,
      (CURLMOPT_CONTENT_LENGTH_PENALTY_SIZE)content_length_penalty_size,
      (CURLMOPT_MAXCONNECTS)maxconnects,
      (CURLMOPT_MAX_CONCURRENT_STREAMS)max_concurrent_streams,
      (CURLMOPT_MAX_HOST_CONNECTIONS)max_host_connections,
      (CURLMOPT_MAX_PIPELINE_LENGTH)max_pipeline_length,
      (CURLMOPT_MAX_TOTAL_CONNECTIONS)max_total_connections,
      (CURLMOPT_PIPELINING)pipelining);
};

class HttpClient;

class HttpRequestBuilder {
  friend HttpClient;

 public:
  template <typename OptionType, typename T>
  HttpRequestBuilder& set(const OptionType& option, const T& t) {
    CurlEasyOptSetter(curl_, option);
    return *this;
  }

 private:
  CURL* curl_;
  HttpRequestBuilder(CURL* curl) : curl_(curl) {}

  /**
   * @brief Function which sets an option on a curl easy object.
   *
   * @tparam OptionType Curl easy option type
   * @tparam T Type holding the option information
   * @param curl Curl easy object
   * @param option Easy option type
   * @param t Object holding the curl easy option value
   *
   * @return Returns the result of the curl_easy_setopt call
   */
  template <typename OptionType, typename T>
  static constexpr CURLcode SetEasyOpt(CURL* curl, const OptionType& option,
                                       const T& t) {
    using FieldType = std::decay_t<T>;
    if constexpr (std::is_same_v<FieldType, Slice>) {
      return curl_easy_setopt(curl, option, t.ToString().c_str());
    } else if constexpr (std::is_same_v<FieldType, std::string>) {
      return curl_easy_setopt(curl, option, t.c_str());
    } else {
      return curl_easy_setopt(curl, option, t);
    }
  }

  template <typename T, typename OptionType>
  static constexpr CURLcode CurlEasyOptSetter(CURL* curl,
                                              const OptionType& option_type,
                                              const T& val) {
    CURLcode ret;
    if constexpr (Optional<T>) {
      if (val.has_value()) {
        if ((ret = SetEasyOpt(curl, option_type, *val)) != CURLE_OK) {
          return ret;
        }
      }
    } else {
      if ((ret = SetEasyOpt(curl, option_type, val)) != CURLE_OK) {
        return ret;
      }
    }
    return CURLE_OK;
  }
};

/**
 * @brief  A http fetching queue based on libcurl multi socket API.
 */
class HttpClient {
 public:
  /**
   * @brief Creates a new :FetchQueue object with optional options.
   *
   * @param options The options for this fetch queue.
   */
  explicit HttpClient(const HttpClientOptions& options = {});

  virtual ~HttpClient();

  /**
   * @brief Pushes a URL into the fetch queue with optional header callback and
   * optional fetch options.
   *
   * @tparam ResultType can be any type which uses the CURL_INFOS macro and is
   * derived from :FetchBase.
   * @tparam OptionsType can be any type which uses the CURL_OPTIONS macro.
   * @param url The URL which should be fetched.
   * @param header_cb The header callback (optional).
   * @param options The options object of type :OptionsType (optional).
   *
   * @return Returns a promise which resolves once the fetch completed and all
   * data are available. The resolved type is a std::shared_ptr of the given
   * :ResultType. The shared pointer contains null if an error occured. Any data
   * in the shared pointer's instance have the same lifespan than the shared
   * pointer itself. Also note the promise resolves into the fetch queue's loop
   * thread and any further usage of the result should be deligated into a
   * different thread.
   */
  template <typename ResultType>
  requires(internal::is_curl_infos<ResultType>&&
               std::is_base_of_v<FetchDataBase, ResultType>)
      Promise<std::shared_ptr<ResultType>> send(Slice url) {
    if (sem_) sem_->wait();
    Promise<EventContext*> promise;
    RequestContext* context = new RequestContext{.cb = promise.deferred()};
    CURL* curl = curl_easy_init();
    if (!curl) {
      context->cb(nullptr);
      return promise.then(
          [](auto) -> std::shared_ptr<ResultType> { return nullptr; });
    }
    curl_easy_setopt(curl, CURLOPT_URL, url.ToString().c_str());
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, HeaderCallback);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, context);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, context);
    curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, context->error);
    curl_easy_setopt(curl, CURLOPT_PRIVATE, context);
    // Pipes the new curl option into the fetch loop thread, which will be
    // picked up from there and added to the multi curl fetching
    if (write(schedule_write_fd_, &curl, sizeof(void*)) != sizeof(void*)) {
      context->cb(nullptr);
      return promise.then(
          [](auto) -> std::shared_ptr<ResultType> { return nullptr; });
    }
    // Callback once the fetch loop is done fetching data for this request.
    return promise.then([this, curl_ptr = curl](EventContext* context)
                            -> std::shared_ptr<ResultType> {
      if (sem_) sem_->post();
      // Put the crawl contex object into a shared pointer to keep the buffer
      // memory alive until the data object is destroyed.
      std::shared_ptr<RequestContext> request_context;
      {
        RequestContext* tmp;
        curl_easy_getinfo(curl_ptr, CURLINFO_PRIVATE, &tmp);
        request_context = std::shared_ptr<RequestContext>(tmp);
      }
      // Put the curl object into a shared pointer for the same reason as
      // the crawl context object.
      std::shared_ptr<CURL> curl(curl_ptr,
                                 [](CURL* ptr) { curl_easy_cleanup(ptr); });
      std::shared_ptr<ResultType> data(
          new ResultType{}, [curl, context, request_context](ResultType* ptr) {
            // When the data object is deleted, the curl object is realeas as
            // well as also the buffer is returned to the pool. This lambda is
            // not called by the fetch loop thread! Therefore, any memory
            // changes here might need synchronization. This is for example the
            // case with the BufferPool::Release call here.
            (void)curl;
            if (auto pool = context->buffer_pool.lock()) {
              pool->Release(std::move(request_context->buffer));
            }
            delete ptr;
          });
      // If aborted is true, the request got stopped after parsing the headers.
      if (!request_context->aborted) {
        data->error = Slice(request_context->error);
      }
      // Create the header parameter slices where the data are stored in the
      // beginning of the context buffer.
      data->header_params.reserve(data->header_params.size());
      char* buf_start = request_context->buffer->data();
      for (const auto& [name, value] : request_context->header_params) {
        data->header_params.emplace_back(
            Slice(buf_start + name.first, name.second),
            Slice(buf_start + value.first, value.second));
      }
      if (!request_context->header_params.empty()) {
        auto last_header_param = request_context->header_params.back();
        buf_start +=
            last_header_param.second.first + last_header_param.second.second;
      }
      // Create the content slice containing all the data after the header.
      size_t offset = buf_start - request_context->buffer->data();
      data->content =
          Slice(buf_start, request_context->buffer->size() - offset);
      // Gather all additional information defined in the ResultType.
      if (CurlDeserialize(curl_ptr, data.get()) != CURLE_OK) {
        return nullptr;
      } else {
        return std::move(data);
      }
    });
  }

  /**
   * @brief Pushes a URL into the fetch queue with optional header callback and
   * optional fetch options.
   *
   * @tparam ResultType can be any type which uses the CURL_INFOS macro and is
   * derived from :FetchBase.
   * @tparam OptionsType can be any type which uses the CURL_OPTIONS macro.
   * @param url The URL which should be fetched.
   * @param header_cb The header callback (optional).
   * @param options The options object of type :OptionsType (optional).
   *
   * @return Returns a promise which resolves once the fetch completed and all
   * data are available. The resolved type is a std::shared_ptr of the given
   * :ResultType. The shared pointer contains null if an error occured. Any data
   * in the shared pointer's instance have the same lifespan than the shared
   * pointer itself. Also note the promise resolves into the fetch queue's loop
   * thread and any further usage of the result should be deligated into a
   * different thread.
   */
  template <typename ResultType, typename OptionsType = EmptyEasyOptions>
  requires(internal::is_curl_infos<ResultType>&& internal::is_curl_options<
           OptionsType>&& std::is_base_of_v<FetchDataBase, ResultType>)
      Promise<std::shared_ptr<ResultType>> get(
          Slice url,
          std::function<bool(const HeaderParam&)>&& header_cb = nullptr,
          OptionsType&& options = OptionsType{}) {
    if (sem_) sem_->wait();
    Promise<EventContext*> promise;
    RequestContext* context = new RequestContext{
        .cb = promise.deferred(), .header_cb = std::move(header_cb)};
    CURL* curl = curl_easy_init();
    if (!curl) {
      context->cb(nullptr);
      return promise.then(
          [](auto) -> std::shared_ptr<ResultType> { return nullptr; });
    }
    CurlEasyOptSetter(curl, options);
    curl_easy_setopt(curl, CURLOPT_URL, url.ToString().c_str());
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, HeaderCallback);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, context);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, context);
    curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, context->error);
    curl_easy_setopt(curl, CURLOPT_PRIVATE, context);
    // Pipes the new curl option into the fetch loop thread, which will be
    // picked up from there and added to the multi curl fetching
    if (write(schedule_write_fd_, &curl, sizeof(void*)) != sizeof(void*)) {
      context->cb(nullptr);
      return promise.then(
          [](auto) -> std::shared_ptr<ResultType> { return nullptr; });
    }
    // Callback once the fetch loop is done fetching data for this request.
    return promise.then([this, curl_ptr = curl](EventContext* context)
                            -> std::shared_ptr<ResultType> {
      if (sem_) sem_->post();
      // Put the crawl contex object into a shared pointer to keep the buffer
      // memory alive until the data object is destroyed.
      std::shared_ptr<RequestContext> request_context;
      {
        RequestContext* tmp;
        curl_easy_getinfo(curl_ptr, CURLINFO_PRIVATE, &tmp);
        request_context = std::shared_ptr<RequestContext>(tmp);
      }
      // Put the curl object into a shared pointer for the same reason as
      // the crawl context object.
      std::shared_ptr<CURL> curl(curl_ptr,
                                 [](CURL* ptr) { curl_easy_cleanup(ptr); });
      std::shared_ptr<ResultType> data(
          new ResultType{}, [curl, context, request_context](ResultType* ptr) {
            // When the data object is deleted, the curl object is realeas as
            // well as also the buffer is returned to the pool. This lambda is
            // not called by the fetch loop thread! Therefore, any memory
            // changes here might need synchronization. This is for example the
            // case with the BufferPool::Release call here.
            (void)curl;
            if (auto pool = context->buffer_pool.lock()) {
              pool->Release(std::move(request_context->buffer));
            }
            delete ptr;
          });
      // If aborted is true, the request got stopped after parsing the headers.
      if (!request_context->aborted) {
        data->error = Slice(request_context->error);
      }
      // Create the header parameter slices where the data are stored in the
      // beginning of the context buffer.
      data->header_params.reserve(data->header_params.size());
      char* buf_start = request_context->buffer->data();
      for (const auto& [name, value] : request_context->header_params) {
        data->header_params.emplace_back(
            Slice(buf_start + name.first, name.second),
            Slice(buf_start + value.first, value.second));
      }
      if (!request_context->header_params.empty()) {
        auto last_header_param = request_context->header_params.back();
        buf_start +=
            last_header_param.second.first + last_header_param.second.second;
      }
      // Create the content slice containing all the data after the header.
      size_t offset = buf_start - request_context->buffer->data();
      data->content =
          Slice(buf_start, request_context->buffer->size() - offset);
      // Gather all additional information defined in the ResultType.
      if (CurlDeserialize(curl_ptr, data.get()) != CURLE_OK) {
        return nullptr;
      } else {
        return std::move(data);
      }
    });
  }

 private :
     /**
      * @brief Sub class responsible for managing the buffer memory for every
      * fetch.
      */
     class BufferPool {
   public:
    BufferPool();

    /**
     * @brief Returns a buffer object, either be reusing existing ones or
     * creating a new one.
     *
     * @return buffer object.
     */
    std::shared_ptr<Buffer<char>> Acquire();

    /**
     * @brief Returns a buffer object to the pool. This function will check that
     * the reference counter of the shared_ptr is 1, if not it will CHK.
     *
     * @param entry The buffer object which should be returned.
     */
    void Release(std::shared_ptr<Buffer<char>>&& entry);

   private:
    std::mutex mutex_;
    size_t tot_mem_;
    const double kMemoryUsageThreshold = 0.2;
    std::stack<std::shared_ptr<Buffer<char>>> pool_;
  };

  /**
   * @brief Contex object containing all required data for the curl multi
   * fetching.
   */
  struct EventContext {
    // The curl multi object.
    CURLM* curlm;
    // The epoll file descriptor the fetch loop is using.
    int epoll_fd;
    // Current timeout set
    long timeout = -1;
    // Weak pointer to the buffer pool.
    std::weak_ptr<BufferPool> buffer_pool;
  };

  using MemoryRange = std::pair<size_t, size_t>;
  using HeaderParamMemoryRange = std::pair<MemoryRange, MemoryRange>;

  /**
   * @brief Context object containing information for every fetch.
   */
  struct RequestContext {
    // Data buffer
    std::shared_ptr<Buffer<char>> buffer;
    // Error buffer.
    char error[CURL_ERROR_SIZE];
    // Callback function which is called once the fetch completed.
    std::function<void(EventContext*)> cb;
    // The header callback which is called for every header parameter received.
    // The current implementation only calls this function if a header line in
    // the form <param name>: <value> is received. That means, the first line
    // which contains the http protocol type and status code is neglected. That
    // might needs to change if an early abort based on the status code is
    // necessary. Alternatively, curl_easy_getinfo can also be used to obtain
    // the status code, which avoids parsing this line by hand.
    std::function<bool(const HeaderParam&)> header_cb = nullptr;
    // A list of the header parameter ranges in the buffer.
    std::vector<HeaderParamMemoryRange> header_params;
    // Number of header lines received.
    size_t num_header_lines = 0;
    // Set to true if header_cb returns false.
    bool aborted = false;
    // An iterator pointing to the in-flight curl object list.
    std::list<CURL*>::const_iterator curl_it;
  };

  int schedule_read_fd_;
  int schedule_write_fd_;
  std::thread loop_thread_;
  std::unique_ptr<Semaphore> sem_;

  template <typename T>
  static const CURLcode CurlDeserialize(CURL* curl, T* x) {
    return T::CurlDeserializeImpl(curl, x);
  }

  template <typename T>
  static const CURLcode CurlEasyOptSetter(CURL* curl, const T& x) {
    return T::CurlEasyOptSetterImpl(curl, x);
  }

  template <typename T>
  static const CURLMcode CurlMultiOptSetter(CURLM* curlm, const T& x) {
    return T::CurlMultiOptSetterImpl(curlm, x);
  }

  /**
   * @brief The fetch loop function called in the fetch loop thread.
   *
   * @param options Options object
   * @param schedule_read_fd pipe file descriptor for receiving new requests.
   */
  static void FetchLoop(HttpClientOptions options, int schedule_read_fd);

  /**
   * @brief Callback called by curl for removing or changing sockets.
   *
   * @param curl Curl object
   * @param socket The effected socket
   * @param action Bitmask containing the updated curl flags
   * @param context Private data
   * @param watcher Socket data
   *
   * @return Returns 0 if successfull.
   */
  static int SocketCallback(CURL* curl, curl_socket_t socket, int action,
                            EventContext* context, void* socketp);

  /**
   * @brief Callback called by curl for setting timeouts.
   *
   * @param curlm Curl mutli object
   * @param timeout_ms The timout in milli seconds
   * @param context Private data
   *
   * @return Returns 0 if successfull.
   */
  static int TimerCallback(CURLM* curlm, long timeout_ms,
                           EventContext* context);

  /**
   * @brief Callback called by curl when new data are available.
   *
   * @param contents Data pointer.
   * @param sz
   * @param nmemb
   * @param context Context object of the fetch for which data are available.
   *
   * @return Returns the size of data processed.
   */
  static size_t WriteCallback(void* contents, size_t sz, size_t nmemb,
                              RequestContext* context);

  /**
   * @brief Callback called by curl when a new header line is received.
   *
   * @param buffer Header line data.
   * @param size
   * @param nitems
   * @param userdata Context object of the fetch for which header data are
   * available.
   *
   * @return Returns the size of data processed or data size +1 if fetching is
   * aborted by the user.
   */
  static size_t HeaderCallback(char* buffer, size_t size, size_t nitems,
                               RequestContext* context);
};

}  // namespace ks

#endif  // HTTP_H_
