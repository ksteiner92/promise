set(CURL_SRC "${CMAKE_BINARY_DIR}/src/ext/curl")
set(CURL_BIN_DIR "${CMAKE_BINARY_DIR}/bin/ext/curl")

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
  BUILD_COMMAND ${CMAKE_MAKE_PROGRAM}
  INSTALL_COMMAND ${CMAKE_MAKE_PROGRAM} install
)
add_dependencies(curl_ libressl_)

#set(CURL_INCLUDE_DIR ${EXT_INSTALL}/include
#  CACHE PATH "CURL header directory" FORCE
#)
#set(CURL_LIBRARY ${EXT_INSTALL}/lib64
#  CACHE PATH "CURL library location" FORCE
#)

#add_library(curl STATIC IMPORTED)
#set_target_properties(curl PROPERTIES IMPORTED_LOCATION ${EXT_INSTALL}/lib64/libcurl.a)
