# Alibaba Cloud Credentials SDK (aliyun/credentials-cpp) for aliyun_auth: the ECS instance RAM role,
# ACK RRSA (OIDC), environment variables and the SDK's default chain.  Its core library
# (darabonba, aliyun/tea-cpp) is fetched at a pinned commit and handed to the SDK's FetchContent so
# the build does not follow that repository's master branch.  aliyunsdk-tea-lost-wakeup.patch fixes a
# lost wake-up in that library's HTTP client (the thread reading a response could block forever once the
# transfer finished; seen on a 2-vCPU ECS as backup_agent never obtaining credentials), so the patch is
# applied right after the clone.
set(ALIYUNSDK_TEA_REPO "https://github.com/aliyun/tea-cpp.git" CACHE STRING "Git repository of tea-cpp (darabonba core)")
set(ALIYUNSDK_TEA_TAG "d1849659055ec4e19bf77c02d04d8ed85308f2c4" CACHE STRING "Git commit of tea-cpp")
set(ALIYUNSDK_REPO "https://github.com/aliyun/credentials-cpp.git" CACHE STRING "Git repository of credentials-cpp")
set(ALIYUNSDK_TAG "0.1.1" CACHE STRING "Git tag of credentials-cpp")

set(ALIYUNSDK_INSTALL "${CMAKE_CURRENT_BINARY_DIR}/aliyunsdk-install")
file(MAKE_DIRECTORY "${ALIYUNSDK_INSTALL}/include")

set(ALIYUNSDK_CXX_FLAGS "")
if(APPLE OR USE_LIBCXX)
  set(ALIYUNSDK_CXX_FLAGS "-stdlib=libc++")
endif()
# tea-cpp's RSA signer (compiled into two providers we do not use) calls EVP_MD_CTX_get0_md, an OpenSSL 3 name for
# OpenSSL 1.1's EVP_MD_CTX_md, which has the same signature.
if(OPENSSL_VERSION VERSION_LESS 3.0)
  set(ALIYUNSDK_CXX_FLAGS "${ALIYUNSDK_CXX_FLAGS} -DEVP_MD_CTX_get0_md=EVP_MD_CTX_md")
endif()

include(ExternalProject)
ExternalProject_Add(aliyunsdk_tea
  GIT_REPOSITORY ${ALIYUNSDK_TEA_REPO}
  GIT_TAG ${ALIYUNSDK_TEA_TAG}
  GIT_CONFIG advice.detachedHead=false
  UPDATE_DISCONNECTED ON
  SOURCE_DIR "${CMAKE_CURRENT_BINARY_DIR}/aliyunsdk-tea-src"
  PATCH_COMMAND git checkout -- . COMMAND git apply --whitespace=nowarn "${CMAKE_CURRENT_LIST_DIR}/aliyunsdk-tea-lost-wakeup.patch"
  CONFIGURE_COMMAND ""
  BUILD_COMMAND ""
  INSTALL_COMMAND "")

ExternalProject_Add(aliyunsdk_project
  GIT_REPOSITORY ${ALIYUNSDK_REPO}
  GIT_TAG ${ALIYUNSDK_TAG}
  GIT_SHALLOW TRUE
  GIT_CONFIG advice.detachedHead=false
  UPDATE_DISCONNECTED ON
  DEPENDS aliyunsdk_tea fdbcurl_project
  SOURCE_DIR "${CMAKE_CURRENT_BINARY_DIR}/aliyunsdk-src"
  BINARY_DIR "${CMAKE_CURRENT_BINARY_DIR}/aliyunsdk-build"
  CMAKE_ARGS -DCMAKE_BUILD_TYPE=Release
  -DBUILD_SHARED_LIBS=OFF
  -DENABLE_UNIT_TESTS=OFF
  -DCMAKE_POSITION_INDEPENDENT_CODE=ON
  -DCMAKE_CXX_STANDARD=17
  -DCMAKE_C_COMPILER=${CMAKE_C_COMPILER}
  -DCMAKE_CXX_COMPILER=${CMAKE_CXX_COMPILER}
  "-DCMAKE_CXX_FLAGS=${ALIYUNSDK_CXX_FLAGS}"
  -DCMAKE_INSTALL_PREFIX=${ALIYUNSDK_INSTALL}
  -DCMAKE_INSTALL_LIBDIR=lib
  -DCURL_DIR=${FDBCURL_CONFIG_DIR}
  -DOPENSSL_ROOT_DIR=${OPENSSL_ROOT_DIR}
  -DFETCHCONTENT_SOURCE_DIR__DARABONBA_CORE=${CMAKE_CURRENT_BINARY_DIR}/aliyunsdk-tea-src
  -DFETCHCONTENT_TRY_FIND_PACKAGE_MODE=NEVER
  BUILD_BYPRODUCTS "${ALIYUNSDK_INSTALL}/lib/libalibabacloud_credentials.a"
  "${ALIYUNSDK_INSTALL}/lib/libdarabonba_core.a")

add_library(aliyunsdk_credentials STATIC IMPORTED)
add_dependencies(aliyunsdk_credentials aliyunsdk_project)
set_target_properties(aliyunsdk_credentials PROPERTIES IMPORTED_LOCATION "${ALIYUNSDK_INSTALL}/lib/libalibabacloud_credentials.a")

add_library(aliyunsdk_darabonba STATIC IMPORTED)
add_dependencies(aliyunsdk_darabonba aliyunsdk_project)
set_target_properties(aliyunsdk_darabonba PROPERTIES IMPORTED_LOCATION "${ALIYUNSDK_INSTALL}/lib/libdarabonba_core.a")

add_library(aliyunsdk_target INTERFACE)
target_include_directories(aliyunsdk_target SYSTEM INTERFACE "${ALIYUNSDK_INSTALL}/include")
target_link_libraries(aliyunsdk_target INTERFACE aliyunsdk_credentials aliyunsdk_darabonba curl OpenSSL::SSL OpenSSL::Crypto uuid Threads::Threads)
