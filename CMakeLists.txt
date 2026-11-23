# The C++ family: the base, the plugin library and the interface library, over the committed definitions,
# on gRPC's C++ API; the interface library carries TLS 1.3 itself, with OpenSSL.
cmake_minimum_required(VERSION 3.25)
project(yoke-sdk-cpp VERSION 0.12.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(PkgConfig REQUIRED)
pkg_check_modules(GRPCXX REQUIRED IMPORTED_TARGET grpc++)
pkg_check_modules(PROTOBUF REQUIRED IMPORTED_TARGET protobuf)
find_package(Threads REQUIRED)
find_package(OpenSSL 3 REQUIRED)
include(GNUInstallDirs)

# The generated definitions of the two contracts the library speaks, compiled as they were generated. The
# administrative contract's are generated beside them and compiled into nothing: a panel built on the
# library must not link the operations an interface may never issue.
#
# By default they are the committed tree, which compiles only against the protobuf that generated it. A
# builder whose protobuf is another names a tree `ci/definitions.sh regenerate` wrote with its own
# generators, from the same definitions module (`just build-here`); that tree is held to -Wall, not to
# -Werror, since what another generator writes is not this repository's to correct.
set(YOKE_SDK_CPP_DEFINITIONS "${PROJECT_SOURCE_DIR}/definitions" CACHE PATH
    "The generated definitions: the committed tree, or one ci/definitions.sh regenerate wrote")
if(NOT EXISTS "${YOKE_SDK_CPP_DEFINITIONS}/record")
    message(FATAL_ERROR "${YOKE_SDK_CPP_DEFINITIONS} holds no definitions: it has no record")
endif()
file(GLOB_RECURSE DEFINITIONS CONFIGURE_DEPENDS ${YOKE_SDK_CPP_DEFINITIONS}/yoke/plugin/*.cc
     ${YOKE_SDK_CPP_DEFINITIONS}/yoke/interface/*.cc)
if(YOKE_SDK_CPP_DEFINITIONS STREQUAL "${PROJECT_SOURCE_DIR}/definitions")
    set_source_files_properties(${DEFINITIONS} PROPERTIES COMPILE_OPTIONS "-Wall;-Werror")
else()
    set_source_files_properties(${DEFINITIONS} PROPERTIES COMPILE_OPTIONS "-Wall")
endif()

add_library(yoke-cpp base/base.cpp plugin/plugin.cpp interface/interface.cpp interface/tls.cpp ${DEFINITIONS})
# The library is reached as the project it is, and also from the project that fetches it: its own
# directories are PROJECT_SOURCE_DIR and PROJECT_BINARY_DIR, never the top level's.
target_include_directories(yoke-cpp
    PUBLIC $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include> $<BUILD_INTERFACE:${YOKE_SDK_CPP_DEFINITIONS}>
           $<INSTALL_INTERFACE:include>
    PRIVATE base interface)
target_link_libraries(yoke-cpp PUBLIC PkgConfig::GRPCXX PkgConfig::PROTOBUF Threads::Threads PRIVATE OpenSSL::SSL)
set_source_files_properties(base/base.cpp plugin/plugin.cpp interface/interface.cpp interface/tls.cpp PROPERTIES COMPILE_OPTIONS "-Wall;-Wextra;-Werror")

# The QML binding, an optional component of the family: built only where it is asked for, since it needs
# Qt 6, and nothing of the plugin or interface library links Qt. It is a QML module, Yoke.Interface, whose
# plugin carries the interface library with it, so the library is compiled position-independent here.
option(YOKE_SDK_CPP_QML "build the QML binding of the interface library, which needs Qt 6" OFF)
set(YOKE_SDK_CPP_QML_DIR "${CMAKE_INSTALL_LIBDIR}/qt6/qml" CACHE STRING
    "where the QML module is installed, relative to the prefix: a directory on the QML import path")
if(YOKE_SDK_CPP_QML)
    find_package(Qt6 6.4 REQUIRED COMPONENTS Qml)
    if(COMMAND qt_policy)
        qt_policy(SET QTP0001 NEW)
    endif()
    set_target_properties(yoke-cpp PROPERTIES POSITION_INDEPENDENT_CODE ON)
    # The module is its own plugin, a shared object a QML engine loads from the import path alone.
    set(YOKE_SDK_CPP_QML_SOURCES qml/binding.hpp qml/binding.cpp qml/convert.hpp qml/convert.cpp)
    qt_add_qml_module(yoke-cpp-qml
        URI Yoke.Interface
        VERSION ${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR}
        PLUGIN_TARGET yoke-cpp-qml
        OUTPUT_DIRECTORY ${PROJECT_BINARY_DIR}/qml/Yoke/Interface
        SOURCES ${YOKE_SDK_CPP_QML_SOURCES})
    set_target_properties(yoke-cpp-qml PROPERTIES AUTOMOC ON)
    target_include_directories(yoke-cpp-qml PRIVATE qml)
    target_link_libraries(yoke-cpp-qml PRIVATE yoke-cpp Qt6::Qml)
    set_source_files_properties(qml/binding.cpp qml/convert.cpp PROPERTIES COMPILE_OPTIONS "-Wall;-Wextra;-Werror")
endif()

# The harnesses and the cases are this repository's own build; a consumer of the source release turns
# them off, and then needs only what the library links.
option(YOKE_SDK_CPP_TESTING "build the harnesses and the case programs" ON)
if(YOKE_SDK_CPP_TESTING)

# The cases, each program reporting its own in the form the record writer reads.
add_library(yoke-cpp-testing STATIC testing/check.cpp testing/channel.cpp testing/core.cpp)
target_include_directories(yoke-cpp-testing PUBLIC testing)
target_link_libraries(yoke-cpp-testing PUBLIC yoke-cpp)
target_compile_options(yoke-cpp-testing PRIVATE -Wall -Wextra -Werror)

foreach(program base/base_test plugin/plugin_test interface/interface_test interface/routable_test interface/published_test)
    get_filename_component(name ${program} NAME)
    add_executable(${name} ${program}.cpp)
    target_include_directories(${name} PRIVATE base interface)
    target_compile_definitions(${name} PRIVATE SOURCE_DIR="${PROJECT_SOURCE_DIR}")
    target_compile_options(${name} PRIVATE -Wall -Wextra -Werror)
    target_link_libraries(${name} PRIVATE yoke-cpp-testing OpenSSL::SSL)
endforeach()
target_compile_definitions(interface_test PRIVATE YOKE_CPP_LIBRARY="$<TARGET_FILE:yoke-cpp>")

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
target_compile_definitions(harness_test PRIVATE SOURCE_DIR="${PROJECT_SOURCE_DIR}")
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
target_compile_definitions(interface_harness_test PRIVATE SOURCE_DIR="${PROJECT_SOURCE_DIR}")
target_compile_options(interface_harness_test PRIVATE -Wall -Wextra -Werror)
target_link_libraries(interface_harness_test PRIVATE yoke-cpp-iharness yoke-cpp-testing)

# The binding's cases, which compile the binding in and register its types into the case's own engine rather
# than load its plugin, so the interface library is linked once.
if(YOKE_SDK_CPP_QML)
    # A consumer that links nothing but Qt, which a case loads the installed module with.
    add_executable(qml_load qml/qml_load.cpp)
    target_compile_options(qml_load PRIVATE -Wall -Wextra -Werror)
    target_link_libraries(qml_load PRIVATE Qt6::Qml)
    add_executable(qml_test qml/qml_test.cpp ${YOKE_SDK_CPP_QML_SOURCES})
    set_target_properties(qml_test PROPERTIES AUTOMOC ON)
    target_include_directories(qml_test PRIVATE qml)
    target_compile_definitions(qml_test PRIVATE YOKE_CPP_LIBRARY="$<TARGET_FILE:yoke-cpp>")
    target_compile_options(qml_test PRIVATE -Wall -Wextra -Werror)
    target_link_libraries(qml_test PRIVATE yoke-cpp-testing Qt6::Qml)
endif()

endif()

# What a consumer installs: the library, its public headers, a CMake package config `find_package` resolves,
# and a pkg-config file for every build that is not CMake. Of the definitions, the interface contract's
# messages are installed, because the interface library surfaces them; the rest are the library's own.
include(CMakePackageConfigHelpers)
install(TARGETS yoke-cpp EXPORT yoke-sdk-cpp-targets ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(DIRECTORY include/yoke DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(DIRECTORY ${YOKE_SDK_CPP_DEFINITIONS}/yoke/interface/v1 DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/yoke/interface
    FILES_MATCHING PATTERN "*.pb.h" PATTERN "*.grpc.pb.h" EXCLUDE)
install(EXPORT yoke-sdk-cpp-targets NAMESPACE yoke:: DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/yoke-sdk-cpp)
configure_package_config_file(cmake/yoke-sdk-cpp-config.cmake.in ${PROJECT_BINARY_DIR}/yoke-sdk-cpp-config.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/yoke-sdk-cpp PATH_VARS YOKE_SDK_CPP_QML_DIR)
write_basic_package_version_file(${PROJECT_BINARY_DIR}/yoke-sdk-cpp-config-version.cmake COMPATIBILITY SameMinorVersion)
install(FILES ${PROJECT_BINARY_DIR}/yoke-sdk-cpp-config.cmake ${PROJECT_BINARY_DIR}/yoke-sdk-cpp-config-version.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/yoke-sdk-cpp)
# The QML module, where asked for: its plugin, its qmldir and its type description, under the import path.
if(YOKE_SDK_CPP_QML)
    qt_query_qml_module(yoke-cpp-qml QMLDIR qml_qmldir TYPEINFO qml_typeinfo MODULE_RESOURCE_PATH qml_module_path)
    set(qml_into "${YOKE_SDK_CPP_QML_DIR}/Yoke/Interface")
    install(TARGETS yoke-cpp-qml LIBRARY DESTINATION "${qml_into}")
    install(FILES "${qml_qmldir}" "${qml_typeinfo}" DESTINATION "${qml_into}")
endif()
# The pkg-config file finds the prefix from where it was installed, so a prefix given at install holds.
file(RELATIVE_PATH PC_TO_PREFIX "/prefix/${CMAKE_INSTALL_LIBDIR}/pkgconfig" "/prefix")
configure_file(cmake/yoke-sdk-cpp.pc.in ${PROJECT_BINARY_DIR}/yoke-sdk-cpp.pc @ONLY)
install(FILES ${PROJECT_BINARY_DIR}/yoke-sdk-cpp.pc DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig)
