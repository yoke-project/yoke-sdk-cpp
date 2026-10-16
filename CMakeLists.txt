# The C++ family: the base, the plugin library and the interface library, over the committed definitions,
# on gRPC's C++ API; the interface library carries TLS 1.3 itself, with OpenSSL.
cmake_minimum_required(VERSION 3.25)
project(yoke-sdk-cpp VERSION 0.5.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(PkgConfig REQUIRED)
pkg_check_modules(GRPCXX REQUIRED IMPORTED_TARGET grpc++)
pkg_check_modules(PROTOBUF REQUIRED IMPORTED_TARGET protobuf)
find_package(Threads REQUIRED)
find_package(OpenSSL 3 REQUIRED)

# The generated definitions, compiled as they were generated.
file(GLOB_RECURSE DEFINITIONS CONFIGURE_DEPENDS definitions/*.cc)
set_source_files_properties(${DEFINITIONS} PROPERTIES COMPILE_OPTIONS "-Wall;-Werror")

add_library(yoke-cpp base/base.cpp plugin/plugin.cpp interface/interface.cpp interface/tls.cpp ${DEFINITIONS})
target_include_directories(yoke-cpp
    PUBLIC $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include> $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/definitions>
           $<INSTALL_INTERFACE:include>
    PRIVATE base interface)
target_link_libraries(yoke-cpp PUBLIC PkgConfig::GRPCXX PkgConfig::PROTOBUF Threads::Threads PRIVATE OpenSSL::SSL)
set_source_files_properties(base/base.cpp plugin/plugin.cpp interface/interface.cpp interface/tls.cpp PROPERTIES COMPILE_OPTIONS "-Wall;-Wextra;-Werror")

# The cases, each program reporting its own in the form the record writer reads.
add_library(yoke-cpp-testing STATIC testing/check.cpp testing/channel.cpp testing/core.cpp)
target_include_directories(yoke-cpp-testing PUBLIC testing)
target_link_libraries(yoke-cpp-testing PUBLIC yoke-cpp)
target_compile_options(yoke-cpp-testing PRIVATE -Wall -Wextra -Werror)

foreach(program base/base_test plugin/plugin_test interface/interface_test interface/routable_test interface/published_test)
    get_filename_component(name ${program} NAME)
    add_executable(${name} ${program}.cpp)
    target_include_directories(${name} PRIVATE base interface)
    target_compile_definitions(${name} PRIVATE SOURCE_DIR="${CMAKE_SOURCE_DIR}")
    target_compile_options(${name} PRIVATE -Wall -Wextra -Werror)
    target_link_libraries(${name} PRIVATE yoke-cpp-testing OpenSSL::SSL)
endforeach()

# The harness the conformance suite drives, and its cases. It speaks to the suite in JSON, through
# nlohmann-json, and to the Core only through the library under test.
find_package(nlohmann_json 3 REQUIRED)
add_library(yoke-cpp-harness STATIC harness/harness.cpp)
target_include_directories(yoke-cpp-harness PUBLIC harness)
target_compile_options(yoke-cpp-harness PRIVATE -Wall -Wextra -Werror)
target_link_libraries(yoke-cpp-harness PUBLIC yoke-cpp nlohmann_json::nlohmann_json Threads::Threads)
add_executable(yoke-cpp-plugin-harness harness/main.cpp)
target_link_libraries(yoke-cpp-plugin-harness PRIVATE yoke-cpp-harness)
add_executable(harness_test harness/harness_test.cpp)
target_compile_definitions(harness_test PRIVATE SOURCE_DIR="${CMAKE_SOURCE_DIR}")
target_compile_options(harness_test PRIVATE -Wall -Wextra -Werror)
target_link_libraries(harness_test PRIVATE yoke-cpp-harness yoke-cpp-testing)

# The interface library's harness, alike: JSON to the suite, and the Core only through the library.
add_library(yoke-cpp-iharness STATIC interface-harness/interface_harness.cpp)
target_include_directories(yoke-cpp-iharness PUBLIC interface-harness)
target_compile_options(yoke-cpp-iharness PRIVATE -Wall -Wextra -Werror)
target_link_libraries(yoke-cpp-iharness PUBLIC yoke-cpp nlohmann_json::nlohmann_json Threads::Threads)
add_executable(yoke-cpp-interface-harness interface-harness/main.cpp)
target_link_libraries(yoke-cpp-interface-harness PRIVATE yoke-cpp-iharness)
add_executable(interface_harness_test interface-harness/interface_harness_test.cpp)
target_include_directories(interface_harness_test PRIVATE base)
target_compile_definitions(interface_harness_test PRIVATE SOURCE_DIR="${CMAKE_SOURCE_DIR}")
target_compile_options(interface_harness_test PRIVATE -Wall -Wextra -Werror)
target_link_libraries(interface_harness_test PRIVATE yoke-cpp-iharness yoke-cpp-testing)

# What a consumer installs: the library, its public headers, a CMake package config `find_package` resolves,
# and a pkg-config file for every build that is not CMake. Of the definitions, the interface contract's
# messages are installed, because the interface library surfaces them; the rest are the library's own.
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
install(TARGETS yoke-cpp EXPORT yoke-sdk-cpp-targets ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(DIRECTORY include/yoke DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(DIRECTORY definitions/yoke/interface/v1 DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/yoke/interface
    FILES_MATCHING PATTERN "*.pb.h" PATTERN "*.grpc.pb.h" EXCLUDE)
install(EXPORT yoke-sdk-cpp-targets NAMESPACE yoke:: DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/yoke-sdk-cpp)
configure_package_config_file(cmake/yoke-sdk-cpp-config.cmake.in ${CMAKE_BINARY_DIR}/yoke-sdk-cpp-config.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/yoke-sdk-cpp)
write_basic_package_version_file(${CMAKE_BINARY_DIR}/yoke-sdk-cpp-config-version.cmake COMPATIBILITY SameMinorVersion)
install(FILES ${CMAKE_BINARY_DIR}/yoke-sdk-cpp-config.cmake ${CMAKE_BINARY_DIR}/yoke-sdk-cpp-config-version.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/yoke-sdk-cpp)
# The pkg-config file finds the prefix from where it was installed, so a prefix given at install holds.
file(RELATIVE_PATH PC_TO_PREFIX "/prefix/${CMAKE_INSTALL_LIBDIR}/pkgconfig" "/prefix")
configure_file(cmake/yoke-sdk-cpp.pc.in ${CMAKE_BINARY_DIR}/yoke-sdk-cpp.pc @ONLY)
install(FILES ${CMAKE_BINARY_DIR}/yoke-sdk-cpp.pc DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig)
