cmake_minimum_required(VERSION 3.10)
project(Monitor)

set(CMAKE_CXX_STANDARD 17)

add_executable(Monitor
    Monitor.cpp
)

find_package(nlohmann_json 3.9.1 REQUIRED)
target_link_libraries(Monitor PRIVATE nlohmann_json::nlohmann_json)