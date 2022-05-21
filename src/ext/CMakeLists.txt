set(FETCHCONTENT_QUIET FALSE)

set(EXT_INSTALL "${CMAKE_BINARY_DIR}/install")

#add_subdirectory(libressl)
#add_subdirectory(curl)
#add_subdirectory(spdlog)

#add_dependencies(LibreSSL::LibreSSL libressl_)
#add_dependencies(SPDLog::SPDLog spdlog_)
#add_dependencies(CURL::CURL curl_)

#find_package(LibreSSL REQUIRED)
#find_package(CURL REQUIRED)
#find_package(SPDLog REQUIRED)

#set(EXT_INSTALL "${CMAKE_BINARY_DIR}/install")
set(SPDLOG_SRC "${CMAKE_BINARY_DIR}/src/ext/spdlog")
set(SPDLOG_BIN_DIR "${CMAKE_BINARY_DIR}/bin/ext/spdlog")
set(SSL_SRC "${CMAKE_BINARY_DIR}/src/ext/libressl")
set(SSL_BIN_DIR "${CMAKE_BINARY_DIR}/bin/ext/libressl")
set(CURL_SRC "${CMAKE_BINARY_DIR}/src/ext/curl")
set(CURL_BIN_DIR "${CMAKE_BINARY_DIR}/bin/ext/curl")
set(GTEST_SRC "${CMAKE_BINARY_DIR}/src/ext/gtest")
set(GTEST_BIN_DIR "${CMAKE_BINARY_DIR}/bin/ext/gtest")

ExternalProject_Add(
  libressl_
  GIT_REPOSITORY https://github.com/libressl-portable/portable.git
  GIT_TAG v${LIBRESSL_VERSION}
  GIT_PROGRESS TRUE
  SOURCE_DIR ${SSL_SRC}
  BINARY_DIR ${SSL_BIN_DIR}
  INSTALL_DIR ${EXT_INSTALL}
  CONFIGURE_COMMAND cd ${SSL_SRC} &&
    ./autogen.sh && cd ${SSL_BIN_DIR} &&
    cmake -G ${CMAKE_GENERATOR} ${SSL_SRC}
    -DCMAKE_INSTALL_PREFIX=${EXT_INSTALL}
  BUILD_COMMAND ${CMAKE_MAKE_PROGRAM}
  INSTALL_COMMAND ${CMAKE_MAKE_PROGRAM} install
)

string(REPLACE "." "_" CURL_GIT_TAG_ ${CURL_VERSION})
ExternalProject_Add(
  curl_
  GIT_REPOSITORY https://github.com/curl/curl.git
  GIT_TAG curl-${CURL_GIT_TAG_}
  GIT_PROGRESS TRUE
  SOURCE_DIR ${CURL_SRC}
  BINARY_DIR ${CURL_BIN_DIR}
  INSTALL_DIR ${EXT_INSTALL}
  CONFIGURE_COMMAND cd ${CURL_BIN_DIR} &&
    cmake -G ${CMAKE_GENERATOR} ${CURL_SRC}
    -DCMAKE_INSTALL_PREFIX=${EXT_INSTALL}
    -DOPENSSL_INCLUDE_DIR=${EXT_INSTALL}/include
    -DOPENSSL_CRYPTO_LIBRARY=${EXT_INSTALL}/lib64/libcrypto.a
    -DOPENSSL_SSL_LIBRARY=${EXT_INSTALL}/lib64/libssl.a
    -DCURL_USE_LIBSSH2=OFF
    -DBUILD_SHARED_LIBS=OFF
    -DCURL_ZLIB=OFF
  BUILD_COMMAND ${CMAKE_MAKE_PROGRAM}
  INSTALL_COMMAND ${CMAKE_MAKE_PROGRAM} install
)

ExternalProject_Add(
  spdlog_
  GIT_REPOSITORY https://github.com/gabime/spdlog.git
  GIT_TAG v${SPDLOG_VERSION}
  GIT_PROGRESS TRUE
  SOURCE_DIR ${SPDLOG_SRC}
  BINARY_DIR ${SPDLOG_BIN_DIR}
  INSTALL_DIR ${EXT_INSTALL}
  CONFIGURE_COMMAND cd ${SPDLOG_BIN_DIR} &&
  cmake -G ${CMAKE_GENERATOR} ${SPDLOG_SRC}
    -DCMAKE_INSTALL_PREFIX=${EXT_INSTALL}
    -DOPENSSL_INCLUDE_DIR=${EXT_INSTALL}/include
  BUILD_COMMAND ${CMAKE_MAKE_PROGRAM}
  INSTALL_COMMAND ${CMAKE_MAKE_PROGRAM} install
)

ExternalProject_Add(
  gtest_
  GIT_REPOSITORY https://github.com/google/googletest.git
  GIT_TAG release-${GTEST_VERSION}
  GIT_PROGRESS TRUE
  SOURCE_DIR ${GTEST_SRC}
  BINARY_DIR ${GTEST_BIN_DIR}
  INSTALL_DIR ${EXT_INSTALL}
  CONFIGURE_COMMAND cd ${GTEST_BIN_DIR} &&
  cmake -G ${CMAKE_GENERATOR} ${GTEST_SRC}
    -DCMAKE_INSTALL_PREFIX=${EXT_INSTALL}
  BUILD_COMMAND ${CMAKE_MAKE_PROGRAM}
  INSTALL_COMMAND ${CMAKE_MAKE_PROGRAM} install
)

#add_dependencies(curl_ libressl_)
#add_dependencies(${PROJECT_NAME} curl_)
#add_dependencies(${PROJECT_NAME} spdlog_)
#add_dependencies(${PROJECT_NAME} gtest_)
#
##include(${EXT_INSTALL}/lib64/cmake/CURL/CURLConfig.cmake)
##find_package(CURL ${CURL_VERSION} REQUIRED HINTS ${EXT_INSTALL}/lib64/cmake/CURL)
##target_link_directories(${PROJECT_NAME} PRIVATE ${CURL_LIBRARIES})
##target_include_directories(${PROJECT_NAME} PRIVATE ${CURL_INCLUDE_DIR})
#
target_link_directories(${PROJECT_NAME} PRIVATE ${EXT_INSTALL}/lib64)
target_include_directories(${PROJECT_NAME} PRIVATE ${EXT_INSTALL}/include)
