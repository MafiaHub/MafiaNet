# A parent project that brings MafiaNet in with add_subdirectory and provides libsodium itself, the
# way a package manager build or a superbuild does. Configuring it must not need the bundled
# Findsodium module (find_package(sodium) is disabled):
#
#   MAFIANET_PROVIDER=target             defines a `sodium` imported target before add_subdirectory
#   MAFIANET_PROVIDER=unofficial-sodium  sets MAFIANET_USE_UNOFFICIAL_SODIUM, with the vcpkg-style
#                                        package in unofficial-sodium/ on CMAKE_PREFIX_PATH
#
#   cmake -S ci/sodium-provider -B build-provider -DMAFIANET_PROVIDER=target
#   cmake --build build-provider && ./build-provider/consumer
#
# The consumer is ci/consumer/main.cpp, linked against MafiaNet::MafiaNetStatic.
cmake_minimum_required(VERSION 3.21)
project(MafiaNetSodiumProvider LANGUAGES C CXX)

set(MAFIANET_PROVIDER "target" CACHE STRING "How this project provides libsodium: target or unofficial-sodium")
set(CMAKE_DISABLE_FIND_PACKAGE_sodium ON)

if(MAFIANET_PROVIDER STREQUAL "target")
  find_path(PROVIDED_SODIUM_INCLUDE_DIR sodium.h REQUIRED)
  find_library(PROVIDED_SODIUM_LIBRARY NAMES sodium libsodium REQUIRED)
  add_library(sodium UNKNOWN IMPORTED)
  set_target_properties(sodium PROPERTIES
    INTERFACE_INCLUDE_DIRECTORIES "${PROVIDED_SODIUM_INCLUDE_DIR}"
    IMPORTED_LOCATION "${PROVIDED_SODIUM_LIBRARY}")
elseif(MAFIANET_PROVIDER STREQUAL "unofficial-sodium")
  list(APPEND CMAKE_PREFIX_PATH "${CMAKE_CURRENT_SOURCE_DIR}/unofficial-sodium")
  set(MAFIANET_USE_UNOFFICIAL_SODIUM ON)
else()
  message(FATAL_ERROR "Unknown MAFIANET_PROVIDER: ${MAFIANET_PROVIDER}")
endif()

set(MAFIANET_BUILD_SHARED OFF)
add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../.." mafianet)

add_executable(consumer "${CMAKE_CURRENT_SOURCE_DIR}/../consumer/main.cpp")
target_link_libraries(consumer PRIVATE MafiaNet::MafiaNetStatic)
