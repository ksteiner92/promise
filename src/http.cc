#include "http.h"
#include "spdlog/spdlog.h"
#include "util/error.h"
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/resource.h>
#include <sys/sysinfo.h>
#include <fcntl.h>
#include <memory>
#include <mutex>
#include <syscall.h>
#include <utility>

namespace ks {
namespace {

/**
 * @brief CHKs if failed and prints curl multi error message
 *
 * @param code Curl multi return code.
 */
inline void CHKCurlm(CURLMcode code) {
  CHK(code == CURLM_OK, curl_multi_strerror(code));
}

/**
 * @brief Calculates the available memory. Available memory is the amount of
 * memory that is available for allocation to new or existing processes.
 * Available memory is then an estimation of how much memory is available for
 * use without swapping. This function uses /proc/meminfo to obtain this
 * information.
 *
 * @return The available memory in bytes.
 */
static size_t GetAvailableMemory() {
  static const char* kSearch = "MemAvailable:";
  static const size_t kSearchOffset = strlen(kSearch);
  size_t available_mem = 0;
  int fd = open("/proc/meminfo", O_RDONLY);
  if (fd < 0) return 0;
  std::string msg;
  size_t start = 0;
  size_t start_search = 0;
  size_t end = 0;
  for (;;) {
    char buffer[100];
    int rc = read(fd, buffer, 100);
    if (rc <= 0) break;
    msg.append(buffer, rc);
    Slice chunk(msg.data(), msg.size());
    start_search = chunk.find(kSearch, start_search);
    if (start_search != chunk.size()) {
      start = start_search + kSearchOffset;
    }
    if (start) end = chunk.find('\n', start);
    if (end) {
      Slice avail = chunk.range(start, end);
      avail.ltrim();
      avail = avail.range(0, avail.rfind(' '));
      SliceToInt(avail, &available_mem);
      available_mem *= 1024;
      break;
    }
  }
  if (fd >= 0) close(fd);
  return available_mem;
}

}  // namespace

HttpClient::BufferPool::BufferPool() {
  struct sysinfo mem_info;
  sysinfo(&mem_info);
  tot_mem_ = mem_info.totalram * mem_info.mem_unit;
}

std::shared_ptr<Buffer<char>> HttpClient::BufferPool::Acquire() {
  std::shared_ptr<Buffer<char>> entry;
  if (!pool_.empty()) {
    std::scoped_lock<std::mutex> lock(mutex_);
    entry = std::move(pool_.top());
    if (!entry) [[unlikely]] {
      throw std::runtime_error("Buffer entry should not be null");
    }
    pool_.pop();
  } else {
    entry = std::make_shared<Buffer<char>>();
    // Reserve 1kB per default.
    entry->reserve(1 * 1024);
  }
  return entry;
}

void HttpClient::BufferPool::Release(std::shared_ptr<Buffer<char>>&& entry) {
  if (entry.use_count() != 1) [[unlikely]] {
    throw std::runtime_error(
        "Cannot release pool entry because it is still in use. Reference ");
  }
  if (static_cast<double>(GetAvailableMemory()) / tot_mem_ <
      kMemoryUsageThreshold) {
    // Keep the memory alive
    std::scoped_lock<std::mutex> lock(mutex_);
    entry->clear();
    pool_.emplace(std::move(entry));
  }
}

HttpClient::HttpClient(const HttpClientOptions& options) {
  if (options.max_requests_in_flight > 0) {
    sem_ = std::make_unique<Semaphore>(options.max_requests_in_flight);
  }
  // Create the pipes to communicate with the fetch loop
  int pipes[2];
  pipe(pipes);
  schedule_read_fd_ = pipes[0];
  schedule_write_fd_ = pipes[1];
  loop_thread_ =
      std::thread([this, options] { FetchLoop(options, schedule_read_fd_); });
}

HttpClient::~HttpClient() {
  void* EOFNULL = NULL;
  // Signal done by scheduling a null pointer
  CHK(write(schedule_write_fd_, &EOFNULL, sizeof(EOFNULL)));
  close(schedule_write_fd_);
  // Wait for fetch loop to be done
  loop_thread_.join();
}

int HttpClient::SocketCallback(CURL* curl, curl_socket_t socket, int action,
                               EventContext* context, void* socketp) {
  CHK(context && context->curlm);
  struct epoll_event event;
  bzero(&event, sizeof(event));
  event.data.fd = socket;
  if (action == CURL_POLL_REMOVE) {
    if (epoll_ctl(context->epoll_fd, EPOLL_CTL_DEL, socket, &event)) {
      spdlog::error("Error when removing socket: {}", strerror(errno));
    }
    return curl_multi_assign(context->curlm, socket, nullptr);
  } else {
    event.events = ((action & CURL_POLL_IN) ? EPOLLIN : 0) |
                   ((action & CURL_POLL_OUT) ? EPOLLOUT : 0) |
                   ((action & CURL_POLL_INOUT) ? (EPOLLOUT | EPOLLIN) : 0);
    int operation = EPOLL_CTL_MOD;
    if (!socketp) {
      CURLMcode ret;
      if ((ret = curl_multi_assign(context->curlm, socket, context->curlm)) !=
          CURLM_OK) {
        return ret;
      }
      operation = EPOLL_CTL_ADD;
    }
    if (epoll_ctl(context->epoll_fd, operation, socket, &event)) {
      spdlog::error("Error when {} socket: {}",
                    (operation == EPOLL_CTL_MOD ? "modifying" : "adding"),
                    strerror(errno));
    }
  }
  return CURLM_OK;
}

int HttpClient::TimerCallback(CURLM* curlm, long timeout_ms,
                              EventContext* context) {
  CHK(context && context->curlm);
  if (timeout_ms == 0) timeout_ms = 1;
  context->timeout = timeout_ms;
  return CURLM_OK;
}

size_t HttpClient::HeaderCallback(char* buffer, size_t sz, size_t nmemb,
                                  RequestContext* context) {
  CHK(context);
  size_t realsize = sz * nmemb;
  if (context->header_cb && context->num_header_lines) {
    Slice header_line(buffer, realsize);
    header_line.remove_suffix("\r\n");
    header_line.trim();
    size_t sep = header_line.find(':');
    // Not a valid header parameter pair, skip.
    if (sep == header_line.size()) return realsize;
    Slice name = header_line.range(0, sep);
    name.rtrim();
    Slice value = header_line.range(sep + 1, header_line.size());
    value.ltrim();
    context->buffer->reserve(context->buffer->size() + header_line.size());
    const char* buf_start = context->buffer->data();
    memcpy(context->buffer->end(), name.data(), name.size());
    auto name_range =
        std::make_pair(context->buffer->end() - buf_start, name.size());
    context->buffer->set_end(context->buffer->end() + name.size());
    memcpy(context->buffer->end(), value.data(), value.size());
    auto value_range =
        std::make_pair(context->buffer->end() - buf_start, value.size());
    context->buffer->set_end(context->buffer->end() + value.size());
    context->header_params.emplace_back(name_range, value_range);
    if (!context->header_cb(std::make_pair(name, value))) {
      context->aborted = true;
      // According to the docs:
      // This callback function must return the number of bytes actually taken
      // care of. If that amount differs from the amount passed in to your
      // function, it'll signal an error to the library. This will cause the
      // transfer to get aborted and the libcurl function in progress will
      // return CURLE_WRITE_ERROR.
      // Therefore, we abort.
      return realsize + 1;
    }
  }
  ++context->num_header_lines;
  return realsize;
}

size_t HttpClient::WriteCallback(void* contents, size_t sz, size_t nmemb,
                                 RequestContext* context) {
  CHK(context);
  size_t realsize = sz * nmemb;
  context->buffer->reserve(context->buffer->size() + realsize);
  memcpy(context->buffer->end(), contents, realsize);
  context->buffer->set_end(context->buffer->end() + realsize);
  return realsize;
}

void HttpClient::FetchLoop(HttpClientOptions options, int schedule_read_fd) {
  // Set this thread to highest IO priority
  const int kIOPriority = 0;
  int tid = syscall(SYS_gettid);
  int priority = (2 << 13) + kIOPriority;  // 2 << 13 sets class
  CHK(syscall(SYS_ioprio_set, 1, tid, priority) == 0, strerror(errno));

  curl_global_init(CURL_GLOBAL_DEFAULT);
  std::shared_ptr<CURLM> curlm = std::shared_ptr<CURLM>(
      curl_multi_init(), [](CURLM* ptr) { curl_multi_cleanup(ptr); });
  auto buffer_pool = std::make_shared<BufferPool>();
  std::list<CURL*> in_flight;
  EventContext context{.curlm = curlm.get(),
                       .epoll_fd = epoll_create1(0),
                       .buffer_pool = buffer_pool};
  CHK(context.epoll_fd != -1, strerror(errno));
  CHKCurlm(CurlMultiOptSetter(curlm.get(), options));
  CHKCurlm(
      curl_multi_setopt(curlm.get(), CURLMOPT_SOCKETFUNCTION, SocketCallback));
  CHKCurlm(curl_multi_setopt(curlm.get(), CURLMOPT_SOCKETDATA, &context));
  CHKCurlm(
      curl_multi_setopt(curlm.get(), CURLMOPT_TIMERFUNCTION, TimerCallback));
  CHKCurlm(curl_multi_setopt(curlm.get(), CURLMOPT_TIMERDATA, &context));
  {
    epoll_event ev;
    bzero(&ev, sizeof(ev));
    ev.events = EPOLLIN;
    ev.data.fd = schedule_read_fd;
    CHK(!epoll_ctl(context.epoll_fd, EPOLL_CTL_ADD, schedule_read_fd, &ev),
        strerror(errno));
  }
  const long kManualTimeoutResolution = 2000;
  const size_t kMaxEvents = 10;
  const size_t kNumCPUs = sysconf(_SC_NPROCESSORS_ONLN);
  const size_t kNumKeepInFlight = 2 * kNumCPUs;
  long timeout, curl_timeout;
  bool done = false, received_request = false;
  int msgs_left, num_events, running;
  CURLMsg* msg;
  RequestContext* crawl_context;
  CURL* curl;
  CURLMcode res;
  struct epoll_event events[kMaxEvents];
  if ((res = curl_multi_socket_action(curlm.get(), CURL_SOCKET_TIMEOUT, 0,
                                      &running)) != CURLM_OK) {
    spdlog::error("Curl error: {}", curl_multi_strerror(res));
  }
  for (;;) {
    // Timeout calculation from
    // https://github.com/seomoz/linkscape/blob/idina-crawl/liblinkscape/http_fetch.cc:[885-887]
    curl_timeout = (context.timeout != -1) ? context.timeout : 2000;
    timeout = std::min(kManualTimeoutResolution, curl_timeout);
    num_events = epoll_wait(context.epoll_fd, events, kMaxEvents, timeout);
    if (num_events > 0) {
      received_request = false;
      for (int i = 0; i < num_events; ++i) {
        int fd = events[i].data.fd;
        uint32_t revents = events[i].events;
        // New fetch scheduled
        if (fd == schedule_read_fd) {
          received_request = true;
          if (read(schedule_read_fd, &curl, sizeof(void*)) != sizeof(void*)) {
            spdlog::error("File descriptor error: {}", strerror(errno));
            continue;
          }
          // If we reveived null, we got the signal to stop
          if (!curl) {
            close(schedule_read_fd);
            for (CURL* curl : in_flight) curl_easy_cleanup(curl);
            done = true;
            // If it is a valid curl handle, acquire a buffer and add the handle
            // to curl
          } else {
            curl_easy_getinfo(curl, CURLINFO_PRIVATE, &crawl_context);
            crawl_context->buffer = buffer_pool->Acquire();
            crawl_context->curl_it = in_flight.insert(in_flight.end(), curl);
            curl_multi_add_handle(curlm.get(), curl);
          }
          break;
          // Change socket events
        } else if (revents) {
          int kind = ((revents & EPOLLIN) ? CURL_CSELECT_IN : 0) |
                     ((revents & EPOLLOUT) ? CURL_CSELECT_OUT : 0) |
                     ((revents & EPOLLERR) ? CURL_CSELECT_ERR : 0);
          if ((res = curl_multi_socket_action(curlm.get(), fd, kind,
                                              &running)) != CURLM_OK) {
            spdlog::error("Curl error: {}", curl_multi_strerror(res));
          }
        }
      }
      // Try to keep 2 * number of CPUs in-flight
      if (received_request && in_flight.size() < kNumKeepInFlight) continue;
      // Trigger wakeup
    } else if (num_events == 0) {
      if ((res = curl_multi_socket_action(curlm.get(), CURL_SOCKET_TIMEOUT, 0,
                                          &running)) != CURLM_OK) {
        spdlog::error("Curl error: {}", curl_multi_strerror(res));
      }
    } else {
      spdlog::error("Epoll error: {}", strerror(errno));
    }
    if (done) break;
    // Check for done fetches
    while ((msg = curl_multi_info_read(curlm.get(), &msgs_left))) {
      if (msg->msg != CURLMSG_DONE) continue;
      curl_easy_getinfo(msg->easy_handle, CURLINFO_PRIVATE, &crawl_context);
      in_flight.erase(crawl_context->curl_it);
      curl_multi_remove_handle(curlm.get(), msg->easy_handle);
      crawl_context->cb(&context);
    }
  }
}

}  // namespace ks
