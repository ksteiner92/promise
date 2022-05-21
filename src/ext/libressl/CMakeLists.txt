set(SSL_SRC "${CMAKE_BINARY_DIR}/src/ext/libressl")
set(SSL_BIN_DIR "${CMAKE_BINARY_DIR}/bin/ext/libressl")

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

#add_custom_target(LibreSSL)
#add_dependencies(LibreSSL libressl_)
#ExternalProject_Get_Property(libressl_ SOURCE_DIR)
#file(MAKE_DIRECTORY ${SOURCE_DIR})
#set(LibreSSL_INCLUDE_DIR ${EXT_INSTALL}/include
#  CACHE PATH "LibreSSL header directory" FORCE
#)
#set(LibreSSL_LIBRARY ${EXT_INSTALL}/lib64
#  CACHE PATH "LibreSSL library location" FORCE
#)
#add_library(libressl STATIC IMPORTED)
#set_target_properties(libressl PROPERTIES IMPORTED_LOCATION
#  ${EXT_INSTALL}/lib64/libssl.a
#  ${EXT_INSTALL}/lib64/libcrypto.a)
