# Use from any C++17 project:  add_subdirectory(path/to/meshstate)  then link "meshstate".
cmake_minimum_required(VERSION 3.13)
project(meshstate CXX)
add_library(meshstate meshstate.cpp replica.cpp models.cpp sync.cpp)
target_include_directories(meshstate PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(meshstate PUBLIC cxx_std_17)
