cmake_minimum_required(VERSION 3.25)
project(@YOKE_PROGRAM@ LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# This family's library, at the release this template came with: installed, or fetched and built beside
# the Plugin.
include(FetchContent)
FetchContent_Declare(yoke-sdk-cpp
    GIT_REPOSITORY https://github.com/yoke-project/yoke-sdk-cpp.git
    GIT_TAG v0.8.0
    FIND_PACKAGE_ARGS 0.8.0)
FetchContent_MakeAvailable(yoke-sdk-cpp)

add_executable(@YOKE_PROGRAM@ main.cpp)
if(TARGET yoke::yoke-cpp)
    target_link_libraries(@YOKE_PROGRAM@ PRIVATE yoke::yoke-cpp)
else()
    target_link_libraries(@YOKE_PROGRAM@ PRIVATE yoke-cpp)
endif()
