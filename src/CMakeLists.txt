add_executable(${PROJECT_NAME} main.cc util/arena.cc util/semaphore.cc http.cc)

set_property(TARGET ${PROJECT_NAME} PROPERTY CXX_STANDARD 20)

add_subdirectory(ext)

add_dependencies(${PROJECT_NAME} curl_)

target_include_directories(${PROJECT_NAME} PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(${PROJECT_NAME} curl ssl spdlog crypto)
#target_link_libraries(${PROJECT_NAME} CURL::CURL LibreSSL::LibreSSL SPDLog::SPDLog)
