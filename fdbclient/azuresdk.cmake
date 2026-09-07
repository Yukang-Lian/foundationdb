cmake_minimum_required(VERSION 3.13)

project(azuresdk-download)

include(ExternalProject)
ExternalProject_Add(azuresdk
  GIT_REPOSITORY    ${AZURE_SDK_CPP_REPO}
  GIT_TAG           ${AZURE_SDK_CPP_TAG}
  GIT_SHALLOW       TRUE
  SOURCE_DIR        "${CMAKE_CURRENT_BINARY_DIR}/azuresdk-src"
  BINARY_DIR        "${CMAKE_CURRENT_BINARY_DIR}/azuresdk-build"
  CONFIGURE_COMMAND ""
  BUILD_COMMAND     ""
  INSTALL_COMMAND   ""
  TEST_COMMAND      ""
)
