# google-cloud-cpp (oauth2 component) with its dependencies, the Google counterpart of awssdk.cmake:
# it resolves application default credentials for blobstore URLs with gcp_auth=1.  Everything is
# built as static libraries with FDB's compiler and standard library, against the libcurl produced
# by the AWS SDK build (the build image's own libcurl is too old), so exactly one libcurl is linked.
project(gcpsdk-download NONE)

set(GCPSDK_COMPILER_FLAGS "")
if(APPLE OR USE_LIBCXX)
  set(GCPSDK_COMPILER_FLAGS "-stdlib=libc++")
endif()

set(GCPSDK_ABSEIL_REPO "https://github.com/abseil/abseil-cpp.git" CACHE STRING "Git repository of abseil-cpp")
set(GCPSDK_ABSEIL_TAG "20250512.1" CACHE STRING "Git tag of abseil-cpp")
set(GCPSDK_JSON_REPO "https://github.com/nlohmann/json.git" CACHE STRING "Git repository of nlohmann/json")
set(GCPSDK_JSON_TAG "v3.11.3" CACHE STRING "Git tag of nlohmann/json")
set(GCPSDK_REPO "https://github.com/googleapis/google-cloud-cpp.git" CACHE STRING "Git repository of google-cloud-cpp")
set(GCPSDK_TAG "v2.47.1" CACHE STRING "Git tag of google-cloud-cpp")

set(GCPSDK_INSTALL "${CMAKE_CURRENT_BINARY_DIR}/gcpsdk-install")
# The headers only exist once the external projects have built; CMake insists that interface
# include directories exist at generate time.
file(MAKE_DIRECTORY "${GCPSDK_INSTALL}/include")

set(GCPSDK_COMMON_ARGS
  -DCMAKE_BUILD_TYPE=Release
  -DCMAKE_INSTALL_PREFIX=${GCPSDK_INSTALL}
  -DBUILD_SHARED_LIBS=OFF
  -DCMAKE_POSITION_INDEPENDENT_CODE=ON
  -DCMAKE_CXX_STANDARD=17
  -DCMAKE_C_COMPILER=${CMAKE_C_COMPILER}
  -DCMAKE_CXX_COMPILER=${CMAKE_CXX_COMPILER}
  -DCMAKE_CXX_FLAGS=${GCPSDK_COMPILER_FLAGS}
  -DCMAKE_PREFIX_PATH=${GCPSDK_INSTALL}
  -DCURL_DIR=${AWSSDK_CURL_CONFIG_DIR}
  -DOPENSSL_ROOT_DIR=${OPENSSL_ROOT_DIR})

# Static libraries needed to link google-cloud-cpp::oauth2, dependents first (derived from the
# installed CMake exports of the pinned versions).
set(GCPSDK_GOOGLE_LIBS google_cloud_cpp_oauth2 google_cloud_cpp_rest_internal google_cloud_cpp_common)
set(GCPSDK_ABSL_LIBS absl_time absl_time_zone absl_civil_time absl_str_format_internal absl_strings absl_int128
  absl_strings_internal absl_string_view absl_base absl_spinlock_wait absl_throw_delegate absl_raw_logging_internal
  absl_log_severity)

set(GCPSDK_GOOGLE_BYPRODUCTS "")
foreach(lib ${GCPSDK_GOOGLE_LIBS})
  list(APPEND GCPSDK_GOOGLE_BYPRODUCTS "${GCPSDK_INSTALL}/lib64/lib${lib}.a")
endforeach()
set(GCPSDK_ABSL_BYPRODUCTS "")
foreach(lib ${GCPSDK_ABSL_LIBS})
  list(APPEND GCPSDK_ABSL_BYPRODUCTS "${GCPSDK_INSTALL}/lib64/lib${lib}.a")
endforeach()

include(ExternalProject)
ExternalProject_Add(gcpsdk_abseil
  GIT_REPOSITORY ${GCPSDK_ABSEIL_REPO}
  GIT_TAG ${GCPSDK_ABSEIL_TAG}
  GIT_SHALLOW TRUE
  GIT_CONFIG advice.detachedHead=false
  UPDATE_DISCONNECTED ON
  SOURCE_DIR "${CMAKE_CURRENT_BINARY_DIR}/gcpsdk-abseil-src"
  BINARY_DIR "${CMAKE_CURRENT_BINARY_DIR}/gcpsdk-abseil-build"
  CMAKE_ARGS ${GCPSDK_COMMON_ARGS} -DABSL_PROPAGATE_CXX_STD=ON -DABSL_BUILD_TESTING=OFF -DABSL_ENABLE_INSTALL=ON
  TEST_COMMAND ""
  BUILD_BYPRODUCTS ${GCPSDK_ABSL_BYPRODUCTS})

ExternalProject_Add(gcpsdk_json
  GIT_REPOSITORY ${GCPSDK_JSON_REPO}
  GIT_TAG ${GCPSDK_JSON_TAG}
  GIT_SHALLOW TRUE
  GIT_CONFIG advice.detachedHead=false
  UPDATE_DISCONNECTED ON
  SOURCE_DIR "${CMAKE_CURRENT_BINARY_DIR}/gcpsdk-json-src"
  BINARY_DIR "${CMAKE_CURRENT_BINARY_DIR}/gcpsdk-json-build"
  CMAKE_ARGS ${GCPSDK_COMMON_ARGS} -DJSON_BuildTests=OFF
  TEST_COMMAND "")

ExternalProject_Add(gcpsdk_project
  GIT_REPOSITORY ${GCPSDK_REPO}
  GIT_TAG ${GCPSDK_TAG}
  GIT_SHALLOW TRUE
  GIT_CONFIG advice.detachedHead=false
  UPDATE_DISCONNECTED ON
  DEPENDS gcpsdk_abseil gcpsdk_json awssdk_project
  SOURCE_DIR "${CMAKE_CURRENT_BINARY_DIR}/gcpsdk-src"
  BINARY_DIR "${CMAKE_CURRENT_BINARY_DIR}/gcpsdk-build"
  CMAKE_ARGS ${GCPSDK_COMMON_ARGS} -DGOOGLE_CLOUD_CPP_ENABLE=oauth2 -DBUILD_TESTING=OFF
    -DGOOGLE_CLOUD_CPP_ENABLE_EXAMPLES=OFF -DGOOGLE_CLOUD_CPP_WITH_MOCKS=OFF
  TEST_COMMAND ""
  BUILD_BYPRODUCTS ${GCPSDK_GOOGLE_BYPRODUCTS})

set(GCPSDK_IMPORTED "")
foreach(lib ${GCPSDK_GOOGLE_LIBS})
  add_library(gcpsdk_${lib} STATIC IMPORTED)
  add_dependencies(gcpsdk_${lib} gcpsdk_project)
  set_target_properties(gcpsdk_${lib} PROPERTIES IMPORTED_LOCATION "${GCPSDK_INSTALL}/lib64/lib${lib}.a")
  list(APPEND GCPSDK_IMPORTED gcpsdk_${lib})
endforeach()
foreach(lib ${GCPSDK_ABSL_LIBS})
  add_library(gcpsdk_${lib} STATIC IMPORTED)
  add_dependencies(gcpsdk_${lib} gcpsdk_abseil)
  set_target_properties(gcpsdk_${lib} PROPERTIES IMPORTED_LOCATION "${GCPSDK_INSTALL}/lib64/lib${lib}.a")
  list(APPEND GCPSDK_IMPORTED gcpsdk_${lib})
endforeach()

add_library(gcpsdk_target INTERFACE)
target_include_directories(gcpsdk_target INTERFACE "${GCPSDK_INSTALL}/include")
target_link_libraries(gcpsdk_target INTERFACE ${GCPSDK_IMPORTED} curl OpenSSL::SSL OpenSSL::Crypto Threads::Threads rt)
