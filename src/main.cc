#include "http.h"
#include <iostream>

namespace ks {
namespace {

struct Result : public FetchDataBase {
  CURL_INFOS();
};

struct Options {
  int num_redirs = 3;
  bool redirs = true;
  CURL_OPTIONS(
      (CURLOPT_MAXREDIRS)num_redirs,
      (CURLOPT_FOLLOWLOCATION)redirs);
};

//Promise<void> test(const HttpClient& client) {
//  auto result = co_await client.get<Result, Options>("https://google.com",
//      nullptr,
//      Options{});
//  std::cout << result->content.data() << std::endl;
//}

}  // namespace

int main(int argc, char** argv) {
  HttpClient client;
  auto result = client
                    .get<Result, Options>("https://google.com", nullptr,
                                           Options{})
                    .get();

  //test(client).wait();
  std::cout << result->content.data() << std::endl;

  return 0;
}
}  // namespace ks

int main(int argc, char** argv) { return ks::main(argc, argv); }
