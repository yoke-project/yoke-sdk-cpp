# The C++ family: the base and the plugin library, over the committed definitions, on gRPC's C++ API.
cmake_minimum_required(VERSION 3.25)
project(yoke-sdk-cpp VERSION 0.4.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(PkgConfig REQUIRED)
pkg_check_modules(GRPCXX REQUIRED IMPORTED_TARGET grpc++)
pkg_check_modules(PROTOBUF REQUIRED IMPORTED_TARGET protobuf)
find_package(Threads REQUIRED)

# The generated definitions, compiled as they were generated.
file(GLOB_RECURSE DEFINITIONS CONFIGURE_DEPENDS definitions/*.cc)
set_source_files_properties(${DEFINITIONS} PROPERTIES COMPILE_OPTIONS "-Wall;-Werror")

add_library(yoke-cpp base/base.cpp plugin/plugin.cpp ${DEFINITIONS})
target_include_directories(yoke-cpp
    PUBLIC include definitions
    PRIVATE base)
target_link_libraries(yoke-cpp PUBLIC PkgConfig::GRPCXX PkgConfig::PROTOBUF Threads::Threads)
set_source_files_properties(base/base.cpp plugin/plugin.cpp PROPERTIES COMPILE_OPTIONS "-Wall;-Wextra;-Werror")

# The cases, each program reporting its own in the form the record writer reads.
add_library(yoke-cpp-testing STATIC testing/check.cpp testing/channel.cpp)
target_include_directories(yoke-cpp-testing PUBLIC testing)
target_link_libraries(yoke-cpp-testing PUBLIC yoke-cpp)
target_compile_options(yoke-cpp-testing PRIVATE -Wall -Wextra -Werror)

foreach(program base/base_test plugin/plugin_test)
    get_filename_component(name ${program} NAME)
    add_executable(${name} ${program}.cpp)
    target_include_directories(${name} PRIVATE base)
    target_compile_definitions(${name} PRIVATE SOURCE_DIR="${CMAKE_SOURCE_DIR}")
    target_compile_options(${name} PRIVATE -Wall -Wextra -Werror)
    target_link_libraries(${name} PRIVATE yoke-cpp-testing)
endforeach()
