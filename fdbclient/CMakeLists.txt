fdb_find_sources(FDBCLIENT_SRCS)
list(APPEND FDBCLIENT_SRCS sha1/SHA1.cpp)

message(STATUS "FDB version is ${FDB_VERSION}")
message(STATUS "FDB package name is ${FDB_PACKAGE_NAME}")

set(options_srcs ${CMAKE_CURRENT_BINARY_DIR}/FDBOptions.g.cpp)

make_directory(${CMAKE_CURRENT_BINARY_DIR}/include/fdbclient/)
vexillographer_compile(TARGET fdboptions_vex LANG cpp OUT ${CMAKE_CURRENT_BINARY_DIR}/FDBOptions.g
  OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/FDBOptions.g.h ${CMAKE_CURRENT_BINARY_DIR}/FDBOptions.g.cpp)

add_custom_command(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/include/fdbclient/FDBOptions.g.h
  DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/FDBOptions.g.h
  COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_CURRENT_BINARY_DIR}/FDBOptions.g.h ${CMAKE_CURRENT_BINARY_DIR}/include/fdbclient/FDBOptions.g.h)

vexillographer_compile(TARGET fdboptions_c LANG c OUT ${CMAKE_CURRENT_BINARY_DIR}/include/fdbclient/fdb_c_options.g.h
  OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/include/fdbclient/fdb_c_options.g.h)

add_custom_target(fdboptions DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/include/fdbclient/FDBOptions.g.h)
add_dependencies(fdboptions fdboptions_c)

################################################################################
# Build information
################################################################################

configure_file(${CMAKE_CURRENT_SOURCE_DIR}/BuildFlags.h.in ${CMAKE_CURRENT_BINARY_DIR}/include/fdbclient/BuildFlags.h)


set(BUILD_GCP_BACKUP OFF CACHE BOOL "Build Google Cloud credential support (gcp_auth) for blobstore backups")
if(BUILD_GCP_BACKUP)
  if(NOT WITH_AWS_BACKUP)
    message(FATAL_ERROR "BUILD_GCP_BACKUP requires BUILD_AWS_BACKUP=ON (it reuses the libcurl built with the AWS SDK)")
  endif()
  add_compile_definitions(WITH_GCP_BACKUP)
  set(FDBCLIENT_SRCS ${FDBCLIENT_SRCS} GcpTokenProvider.cpp)
endif()

set(BUILD_AZURE_BACKUP OFF CACHE BOOL "Build Azure backup client")
if(BUILD_AZURE_BACKUP)
  add_compile_definitions(BUILD_AZURE_BACKUP)
  set(FDBCLIENT_SRCS
    ${FDBCLIENT_SRCS}
    azure_backup/AzureTokenProvider.cpp
    azure_backup/BackupContainerAzureBlobStore.actor.cpp)

  # The azure-storage-cpplite source location can be overridden (e.g. with an internal
  # mirror) for builds without access to github.com.
  set(AZURE_STORAGE_CPPLITE_REPO "https://github.com/Azure/azure-storage-cpplite.git" CACHE STRING
    "Git repository to fetch azure-storage-cpplite from")
  set(AZURE_STORAGE_CPPLITE_TAG "11e1f98b021446ef340f4886796899a6eb1ad9a5" CACHE STRING
    "Git tag/commit of azure-storage-cpplite to build")

  configure_file(azurestorage.cmake azurestorage-download/CMakeLists.txt)

  execute_process(
    COMMAND ${CMAKE_COMMAND} -G "${CMAKE_GENERATOR}" .
    RESULT_VARIABLE results
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/azurestorage-download
  )

  if(results)
    message(FATAL_ERROR "Configuration step for AzureStorage has Failed. ${results}")
  endif()

  execute_process(
    COMMAND ${CMAKE_COMMAND} --build . --config Release
    RESULT_VARIABLE results
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/azurestorage-download
  )

  if(results)
    message(FATAL_ERROR "Build step for AzureStorage has Failed. ${results}")
  endif()

  add_subdirectory(
    ${CMAKE_CURRENT_BINARY_DIR}/azurestorage-src
    ${CMAKE_CURRENT_BINARY_DIR}/azurestorage-build
  )
endif()


if(WITH_AWS_BACKUP)
  add_compile_definitions(WITH_AWS_BACKUP)
  include(awssdk)

  # The AWS SDK build produces a static libcurl that fdbclient links as the `curl` target.  The
  # Azure and Google SDKs need a libcurl newer than the build image's (7.29 lacks
  # CURLINFO_ACTIVESOCKET and friends), so this CURL config package hands them the same one and a
  # second libcurl never enters the binary.  The headers only exist once awssdk_project has built,
  # hence the pre-created include directory and the dependencies declared by the consumers.
  set(AWSSDK_CURL_ROOT "${CMAKE_CURRENT_BINARY_DIR}/awssdk-build/install/external-install/curl")
  set(AWSSDK_CURL_CONFIG_DIR "${CMAKE_CURRENT_BINARY_DIR}/awssdk-curl")
  file(MAKE_DIRECTORY "${AWSSDK_CURL_ROOT}/include")
  file(WRITE "${AWSSDK_CURL_CONFIG_DIR}/CURLConfig.cmake"
    "add_library(CURL::libcurl STATIC IMPORTED)\n"
    "set_target_properties(CURL::libcurl PROPERTIES IMPORTED_LOCATION \"${AWSSDK_CURL_ROOT}/lib/libcurl.a\" INTERFACE_INCLUDE_DIRECTORIES \"${AWSSDK_CURL_ROOT}/include\")\n"
    "set(CURL_FOUND TRUE)\n"
    "set(CURL_VERSION_STRING \"awssdk\")\n"
    "set(CURL_INCLUDE_DIRS \"${AWSSDK_CURL_ROOT}/include\")\n"
    "set(CURL_LIBRARIES CURL::libcurl)\n")
endif()

if(BUILD_GCP_BACKUP)
  include(gcpsdk)
endif()

if(BUILD_AZURE_BACKUP)
  # azure-core and azure-identity from azure-sdk-for-cpp provide the credential chain for
  # managed identity, workload identity and DefaultAzureCredential (the Azure counterpart of
  # the AWS SDK credential provider used for sdk_auth); azure-storage-cpplite stays the data path.
  set(AZURE_SDK_CPP_REPO "https://github.com/Azure/azure-sdk-for-cpp.git" CACHE STRING
    "Git repository to fetch azure-sdk-for-cpp (azure-core, azure-identity) from")
  set(AZURE_SDK_CPP_TAG "azure-identity_1.13.3" CACHE STRING
    "Git tag/commit of azure-sdk-for-cpp to build")

  configure_file(azuresdk.cmake azuresdk-download/CMakeLists.txt)

  execute_process(
    COMMAND ${CMAKE_COMMAND} -G "${CMAKE_GENERATOR}" .
    RESULT_VARIABLE results
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/azuresdk-download
  )
  if(results)
    message(FATAL_ERROR "Configuration step for azure-sdk-for-cpp has Failed. ${results}")
  endif()

  execute_process(
    COMMAND ${CMAKE_COMMAND} --build . --config Release
    RESULT_VARIABLE results
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/azuresdk-download
  )
  if(results)
    message(FATAL_ERROR "Download step for azure-sdk-for-cpp has Failed. ${results}")
  endif()

  # The SDK packages are configured through cache options; force them so the option() calls
  # inside the SDK cannot override them.
  set(BUILD_TRANSPORT_CURL ON CACHE BOOL "azure-core: build the libcurl HTTP transport" FORCE)
  set(WARNINGS_AS_ERRORS OFF CACHE BOOL "azure-sdk-for-cpp: no -Werror" FORCE)
  set(BUILD_TESTING OFF CACHE BOOL "azure-sdk-for-cpp: no tests" FORCE)
  set(BUILD_SAMPLES OFF CACHE BOOL "azure-sdk-for-cpp: no samples" FORCE)
  set(BUILD_PERFORMANCE_TESTS OFF CACHE BOOL "azure-sdk-for-cpp: no perf tests" FORCE)
  # FDB's own BUILD_DOCUMENTATION (sphinx) must not make the SDK look for Doxygen.
  set(BUILD_DOCUMENTATION OFF)
  # Makes azure-identity link the in-tree azure-core target instead of find_package()ing it.
  set(AZ_ALL_LIBRARIES ON)

  if(WITH_AWS_BACKUP)
    # azure-core's find_package(CURL) picks up the AWS-built libcurl (see above).
    set(CURL_DIR "${AWSSDK_CURL_CONFIG_DIR}")
  endif()

  add_subdirectory(
    ${CMAKE_CURRENT_BINARY_DIR}/azuresdk-src/sdk/core/azure-core
    ${CMAKE_CURRENT_BINARY_DIR}/azuresdk-build/azure-core
  )
  add_subdirectory(
    ${CMAKE_CURRENT_BINARY_DIR}/azuresdk-src/sdk/identity/azure-identity
    ${CMAKE_CURRENT_BINARY_DIR}/azuresdk-build/azure-identity
  )

  if(WITH_AWS_BACKUP)
    unset(CURL_DIR)
    add_dependencies(azure-core awssdk_project)
  endif()
endif()

add_flow_target(STATIC_LIBRARY NAME fdbclient SRCS ${FDBCLIENT_SRCS} ADDL_SRCS ${options_srcs})
target_include_directories(fdbclient PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include" "${CMAKE_CURRENT_BINARY_DIR}/include")
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/versions.h.cmake ${CMAKE_CURRENT_BINARY_DIR}/include/fdbclient/versions.h)
add_dependencies(fdbclient fdboptions)
target_link_libraries(fdbclient PUBLIC fdbrpc msgpack PRIVATE rapidxml)

# Create a separate fdbclient library with sampling enabled. This lets
# fdbserver retain sampling functionality in client code while disabling
# sampling for pure clients.
add_flow_target(STATIC_LIBRARY NAME fdbclient_sampling SRCS ${FDBCLIENT_SRCS} ADDL_SRCS ${options_srcs})
target_include_directories(fdbclient_sampling PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include" "${CMAKE_CURRENT_BINARY_DIR}/include")
add_dependencies(fdbclient_sampling fdboptions)
target_link_libraries(fdbclient_sampling PUBLIC fdbrpc_sampling msgpack PRIVATE rapidxml)
target_compile_definitions(fdbclient_sampling PRIVATE -DENABLE_SAMPLING)
if(WIN32)
  add_dependencies(fdbclient_sampling_actors fdbclient_actors)
endif()

add_flow_target(LINK_TEST NAME fdbclientlinktest SRCS LinkTest.cpp)
target_link_libraries(fdbclientlinktest PRIVATE fdbclient rapidxml) # re-link rapidxml due to private link interface

if(BUILD_AZURE_BACKUP)
  target_link_libraries(fdbclient PRIVATE curl azure-storage-lite azure-identity azure-core)
  target_link_libraries(fdbclient_sampling PRIVATE curl azure-storage-lite azure-identity azure-core)
endif()

if(WITH_AWS_BACKUP)
  target_link_libraries(fdbclient PUBLIC awssdk_target)
  target_link_libraries(fdbclient_sampling PUBLIC awssdk_target)
endif()

if(BUILD_GCP_BACKUP)
  target_link_libraries(fdbclient PUBLIC gcpsdk_target)
  target_link_libraries(fdbclient_sampling PUBLIC gcpsdk_target)
  add_dependencies(fdbclient gcpsdk_project)
  add_dependencies(fdbclient_sampling gcpsdk_project)
endif()
