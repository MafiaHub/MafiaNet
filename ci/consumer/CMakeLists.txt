# A minimal downstream project that uses an installed MafiaNet package, the way
# a consumer does: CXX only (no C language enabled), find_package, link, run.
# CI builds it against `cmake --install` output on Linux, macOS and Windows:
#
#   cmake -S ci/consumer -B build-consumer -DCMAKE_PREFIX_PATH=<install prefix>
#   cmake --build build-consumer
#
# MAFIANET_CONSUMER_STATIC=ON links MafiaNet::MafiaNetStatic (which needs the
# libsodium development files) instead of the shared MafiaNet::MafiaNet.
cmake_minimum_required(VERSION 3.21)
project(MafiaNetConsumer LANGUAGES CXX)

option(MAFIANET_CONSUMER_STATIC "Link MafiaNet::MafiaNetStatic instead of MafiaNet::MafiaNet" OFF)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(MafiaNet REQUIRED)

add_executable(consumer main.cpp)
if(MAFIANET_CONSUMER_STATIC)
  target_link_libraries(consumer PRIVATE MafiaNet::MafiaNetStatic)
else()
  target_link_libraries(consumer PRIVATE MafiaNet::MafiaNet)
endif()
