cmake_minimum_required(VERSION 3.13)

project(azurestorage-download)

include(ExternalProject)
ExternalProject_Add(azurestorage
  GIT_REPOSITORY    ${AZURE_STORAGE_CPPLITE_REPO}
  GIT_TAG           ${AZURE_STORAGE_CPPLITE_TAG} # v0.3.0
  SOURCE_DIR        "${CMAKE_CURRENT_BINARY_DIR}/azurestorage-src"
  BINARY_DIR        "${CMAKE_CURRENT_BINARY_DIR}/azurestorage-build"
  CMAKE_ARGS        "-DCMAKE_BUILD_TYPE=Release"
  CONFIGURE_COMMAND ""
  BUILD_COMMAND     ""
  INSTALL_COMMAND   ""
  TEST_COMMAND      ""
  BUILD_BYPRODUCTS  "${CMAKE_CURRENT_BINARY_DIR}/libazure-storage-lite.a"
)
