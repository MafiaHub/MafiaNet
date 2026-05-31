cmake_minimum_required(VERSION 3.21)
project(mafianet_consumer_example LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Consumed from vcpkg (manifest mode). The same find_package call works whether
# the active triplet is static or dynamic, thanks to the stable alias target.
find_package(MafiaNet CONFIG REQUIRED)

add_executable(consumer main.cpp)
target_link_libraries(consumer PRIVATE MafiaNet::MafiaNet)
