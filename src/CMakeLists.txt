add_executable(${PROJECT_NAME} main.cc util/arena.cc util/semaphore.cc http.cc)

set_property(TARGET ${PROJECT_NAME} PROPERTY CXX_STANDARD 20)

add_subdirectory(ext)

add_dependencies(${PROJECT_NAME} curl_)

target_include_directories(${PROJECT_NAME} PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(${PROJECT_NAME} curl ssl spdlog crypto)

add_executable(promise_test util/promise_test.cc util/arena.cc util/semaphore.cc)
set_property(TARGET promise_test PROPERTY CXX_STANDARD 20)
add_dependencies(promise_test gtest_)
set(EXT_INSTALL "${CMAKE_BINARY_DIR}/install")
target_link_directories(promise_test PRIVATE ${EXT_INSTALL}/lib64)
target_include_directories(promise_test PRIVATE ${EXT_INSTALL}/include)
target_include_directories(promise_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
include(GoogleTest)
target_link_libraries(promise_test gtest gtest_main)
gtest_discover_tests(promise_test)
#target_link_libraries(${PROJECT_NAME} CURL::CURL LibreSSL::LibreSSL SPDLog::SPDLog)
