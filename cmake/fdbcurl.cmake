# A static libcurl for the cloud SDKs (AWS, Azure, Google, Alibaba Cloud) and azure-storage-cpplite.
#
# The build image's libcurl (7.29) is too old for any of them and the one the AWS SDK builds for
# itself (7.52) lacks the URL and multi APIs the Alibaba Cloud SDK needs.  Building one modern
# libcurl here and pointing every SDK at it also keeps a dynamic libcurl (and with it a second
# OpenSSL) out of fdbbackup, so the binary behaves the same on every host.
#
# HTTP(S) only, OpenSSL from the FDB build, no compiled-in CA bundle: with CURL_CA_FALLBACK the
# OpenSSL default paths are used when a consumer sets no CA file, and fdbbackup points them at the
# host's CA bundle through SSL_CERT_FILE (see fdbbackup/backup.actor.cpp).
set(FDBCURL_REPO "https://github.com/curl/curl.git" CACHE STRING "Git repository of curl")
set(FDBCURL_TAG "curl-8_11_1" CACHE STRING "Git tag of curl")

set(FDBCURL_INSTALL "${CMAKE_CURRENT_BINARY_DIR}/fdbcurl-install")
# consumers are configured before curl is built, so the include directory must already exist
file(MAKE_DIRECTORY "${FDBCURL_INSTALL}/include")

include(ExternalProject)
ExternalProject_Add(fdbcurl_project
  GIT_REPOSITORY ${FDBCURL_REPO}
  GIT_TAG ${FDBCURL_TAG}
  GIT_SHALLOW TRUE
  GIT_CONFIG advice.detachedHead=false
  UPDATE_DISCONNECTED ON
  SOURCE_DIR "${CMAKE_CURRENT_BINARY_DIR}/fdbcurl-src"
  BINARY_DIR "${CMAKE_CURRENT_BINARY_DIR}/fdbcurl-build"
  CMAKE_ARGS -DCMAKE_BUILD_TYPE=Release
  -DCMAKE_C_COMPILER=${CMAKE_C_COMPILER}
  -DCMAKE_POSITION_INDEPENDENT_CODE=ON
  -DCMAKE_INSTALL_PREFIX=${FDBCURL_INSTALL}
  -DCMAKE_INSTALL_LIBDIR=lib
  -DBUILD_SHARED_LIBS=OFF
  -DBUILD_STATIC_LIBS=ON
  -DBUILD_CURL_EXE=OFF
  -DBUILD_TESTING=OFF
  -DBUILD_LIBCURL_DOCS=OFF
  -DBUILD_MISC_DOCS=OFF
  -DENABLE_CURL_MANUAL=OFF
  -DHTTP_ONLY=ON
  -DCURL_USE_OPENSSL=ON
  -DOPENSSL_USE_STATIC_LIBS=ON
  -DOPENSSL_ROOT_DIR=${OPENSSL_ROOT_DIR}
  -DCURL_USE_LIBPSL=OFF
  -DUSE_LIBIDN2=OFF
  -DCURL_USE_LIBSSH2=OFF
  -DCURL_USE_LIBSSH=OFF
  -DUSE_NGHTTP2=OFF
  -DCURL_BROTLI=OFF
  -DCURL_ZSTD=OFF
  -DCURL_ZLIB=OFF
  -DCURL_USE_GSSAPI=OFF
  -DCURL_DISABLE_LDAP=ON
  -DCURL_CA_BUNDLE=none
  -DCURL_CA_PATH=none
  -DCURL_CA_FALLBACK=ON
  BUILD_BYPRODUCTS "${FDBCURL_INSTALL}/lib/libcurl.a")

# the `curl` target every consumer inside this project links
add_library(curl STATIC IMPORTED)
add_dependencies(curl fdbcurl_project)
set_target_properties(curl PROPERTIES
  IMPORTED_LOCATION "${FDBCURL_INSTALL}/lib/libcurl.a"
  INTERFACE_INCLUDE_DIRECTORIES "${FDBCURL_INSTALL}/include"
  INTERFACE_LINK_LIBRARIES "${OPENSSL_SSL_LIBRARY};${OPENSSL_CRYPTO_LIBRARY};dl;pthread")

# A CURL config package for the SDKs' find_package(CURL), inside this project (add_subdirectory
# consumers pick it up through CURL_DIR) and in ExternalProject sub-builds (-DCURL_DIR=...).
# Guarded so that a project and its subprojects can each call find_package(CURL).
set(FDBCURL_CONFIG_DIR "${CMAKE_CURRENT_BINARY_DIR}/fdbcurl-config")
file(WRITE "${FDBCURL_CONFIG_DIR}/CURLConfig.cmake"
  "if(NOT TARGET CURL::libcurl)\n"
  "  add_library(CURL::libcurl STATIC IMPORTED)\n"
  "  set_target_properties(CURL::libcurl PROPERTIES IMPORTED_LOCATION \"${FDBCURL_INSTALL}/lib/libcurl.a\" INTERFACE_INCLUDE_DIRECTORIES \"${FDBCURL_INSTALL}/include\" INTERFACE_LINK_LIBRARIES \"${OPENSSL_SSL_LIBRARY};${OPENSSL_CRYPTO_LIBRARY};dl;pthread\" INTERFACE_COMPILE_DEFINITIONS \"CURL_STATICLIB\")\n"
  "endif()\n"
  "set(CURL_FOUND TRUE)\n"
  "set(CURL_VERSION_STRING \"8.11.1\")\n"
  "set(CURL_INCLUDE_DIRS \"${FDBCURL_INSTALL}/include\")\n"
  "set(CURL_INCLUDE_DIR \"${FDBCURL_INSTALL}/include\")\n"
  "set(CURL_LIBRARIES CURL::libcurl)\n"
  "set(CURL_LIBRARY CURL::libcurl)\n")
