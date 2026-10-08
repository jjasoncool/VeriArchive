cmake_minimum_required(VERSION 3.28)

project(VeriArchive VERSION 0.1.0 LANGUAGES CXX)

include(CTest)
option(VERIARCHIVE_ALLOW_NONBASELINE_TOOLCHAIN "Allow local, non-release toolchain checks" OFF)
if(WIN32 AND NOT VERIARCHIVE_ALLOW_NONBASELINE_TOOLCHAIN)
    if(NOT CMAKE_CXX_COMPILER MATCHES "[Uu][Cc][Rr][Tt]64")
        message(FATAL_ERROR "Windows baseline requires MSYS2 UCRT64. For local diagnostics only, set VERIARCHIVE_ALLOW_NONBASELINE_TOOLCHAIN=ON.")
    endif()
endif()
find_package(nlohmann_json 3.12 QUIET)
if(NOT nlohmann_json_FOUND)
    include(FetchContent)
    FetchContent_Declare(nlohmann_json
        URL https://github.com/nlohmann/json/releases/download/v3.12.0/json.tar.xz)
    FetchContent_MakeAvailable(nlohmann_json)
endif()

add_library(veriarchive_common
    src/common/archive_path.cpp
    src/common/canonical_json.cpp
    src/common/config.cpp
    src/common/utf8.cpp)
add_library(VeriArchive::common ALIAS veriarchive_common)
target_include_directories(veriarchive_common PUBLIC src)
target_compile_features(veriarchive_common PUBLIC cxx_std_23)
target_link_libraries(veriarchive_common PUBLIC nlohmann_json::nlohmann_json)

add_library(veriarchive_platform)
add_library(VeriArchive::platform ALIAS veriarchive_platform)
target_include_directories(veriarchive_platform PUBLIC src)
target_compile_features(veriarchive_platform PUBLIC cxx_std_23)
target_link_libraries(veriarchive_platform PUBLIC VeriArchive::common)

if(WIN32)
    target_sources(veriarchive_platform PRIVATE
        src/platform/windows/platform_services_win.cpp
    )
    target_link_libraries(veriarchive_platform PRIVATE mpr)
elseif(UNIX AND NOT APPLE)
    target_sources(veriarchive_platform PRIVATE
        src/platform/linux/platform_services_linux.cpp
    )
else()
    message(FATAL_ERROR "VeriArchive currently supports Windows and Linux only")
endif()

if(BUILD_TESTING)
    add_executable(veriarchive_tests
        tests/unit/test_main.cpp
        tests/unit/archive_path_test.cpp
        tests/unit/canonical_json_test.cpp
        tests/unit/config_test.cpp)
    target_compile_features(veriarchive_tests PRIVATE cxx_std_23)
    target_link_libraries(veriarchive_tests PRIVATE VeriArchive::platform)
    add_test(NAME veriarchive_m1 COMMAND veriarchive_tests)
endif()

if(MSVC)
    foreach(target veriarchive_common veriarchive_platform veriarchive_tests)
        if(TARGET ${target})
            target_compile_options(${target} PRIVATE /W4 /permissive-)
        endif()
    endforeach()
else()
    foreach(target veriarchive_common veriarchive_platform veriarchive_tests)
        if(TARGET ${target})
            target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
        endif()
    endforeach()
endif()
