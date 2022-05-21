set(SPDLOG_SRC "${CMAKE_BINARY_DIR}/src/ext/spdlog")
set(SPDLOG_BIN_DIR "${CMAKE_BINARY_DIR}/bin/ext/spdlog")

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

#ExternalProject_Get_Property(libressl_ SOURCE_DIR)
#file(MAKE_DIRECTORY ${SOURCE_DIR})
#set(SPDLog_INCLUDE_DIR ${EXT_INSTALL}/include
#  CACHE PATH "SPDLog header directory" FORCE
#)
#set(SPDLog_LIBRARY ${EXT_INSTALL}/lib64
#  CACHE PATH "SPDLog library location" FORCE
#)
#add_library(libressl STATIC IMPORTED)
#set_target_properties(libressl PROPERTIES IMPORTED_LOCATION
#  ${EXT_INSTALL}/lib64/libssl.a
#  ${EXT_INSTALL}/lib64/libcrypto.a)
