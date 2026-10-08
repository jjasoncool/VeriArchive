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
find_package(SQLite3 3.40 REQUIRED)
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/src/core/repository/schema.sql" VERIARCHIVE_SCHEMA_SQL)
configure_file(src/core/repository/schema_sql.h.in generated/schema_sql.h @ONLY)

add_library(veriarchive_common
    src/common/archive_path.cpp
    src/common/canonical_json.cpp
    src/common/hex.cpp
    src/common/config.cpp
    src/common/utf8.cpp)
add_library(VeriArchive::common ALIAS veriarchive_common)
target_include_directories(veriarchive_common PUBLIC src)
target_compile_features(veriarchive_common PUBLIC cxx_std_23)
target_link_libraries(veriarchive_common PUBLIC nlohmann_json::nlohmann_json)

add_library(veriarchive_core
    src/core/repository/repository.cpp
    src/core/repository/sqlite_raii.cpp
    src/core/write_file.cpp)
add_library(VeriArchive::core ALIAS veriarchive_core)
target_include_directories(veriarchive_core PUBLIC src PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_compile_features(veriarchive_core PUBLIC cxx_std_23)
target_link_libraries(veriarchive_core PUBLIC VeriArchive::common SQLite::SQLite3)

add_library(veriarchive_platform)
add_library(VeriArchive::platform ALIAS veriarchive_platform)
target_include_directories(veriarchive_platform PUBLIC src)
target_compile_features(veriarchive_platform PUBLIC cxx_std_23)
target_link_libraries(veriarchive_platform PUBLIC VeriArchive::core)

if(WIN32)
    target_sources(veriarchive_platform PRIVATE
        src/platform/windows/file_writer_win.cpp
        src/platform/windows/platform_services_win.cpp
        src/platform/windows/win_util.cpp
    )
    target_link_libraries(veriarchive_platform PRIVATE mpr)
elseif(UNIX AND NOT APPLE)
    target_sources(veriarchive_platform PRIVATE
        src/platform/linux/file_writer_linux.cpp
        src/platform/linux/platform_services_linux.cpp
        src/platform/linux/posix_util.cpp
    )
else()
    message(FATAL_ERROR "VeriArchive currently supports Windows and Linux only")
endif()

add_executable(veriarchive_cli src/cli/cli.cpp)
set_target_properties(veriarchive_cli PROPERTIES OUTPUT_NAME veriarchive-cli)
if(WIN32)
    target_sources(veriarchive_cli PRIVATE src/cli/main_win.cpp src/platform/windows/veriarchive.rc)
    target_link_options(veriarchive_cli PRIVATE -municode)
endif()
if(UNIX AND NOT APPLE)
    target_sources(veriarchive_cli PRIVATE src/cli/main_linux.cpp)
endif()
target_compile_features(veriarchive_cli PRIVATE cxx_std_23)
target_link_libraries(veriarchive_cli PRIVATE VeriArchive::platform)

if(BUILD_TESTING)
    add_executable(veriarchive_tests
        tests/unit/test_main.cpp
        tests/unit/archive_path_test.cpp
        tests/unit/canonical_json_test.cpp
        tests/unit/config_test.cpp
        tests/unit/file_writer_test.cpp
        tests/unit/repository_test.cpp)
    target_compile_features(veriarchive_tests PRIVATE cxx_std_23)
    target_link_libraries(veriarchive_tests PRIVATE VeriArchive::platform)
    add_test(NAME veriarchive_m1 COMMAND veriarchive_tests)
endif()

if(MSVC)
    foreach(target veriarchive_common veriarchive_core veriarchive_platform veriarchive_cli veriarchive_tests)
        if(TARGET ${target})
            target_compile_options(${target} PRIVATE /W4 /permissive-)
        endif()
    endforeach()
else()
    foreach(target veriarchive_common veriarchive_core veriarchive_platform veriarchive_cli veriarchive_tests)
        if(TARGET ${target})
            target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
        endif()
    endforeach()
endif()