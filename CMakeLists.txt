cmake_minimum_required(VERSION 3.28)

project(VeriArchive VERSION 0.1.0 LANGUAGES C CXX)

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
find_package(OpenSSL 3.0 REQUIRED COMPONENTS Crypto)
find_package(BLAKE3 1.5 CONFIG QUIET)
if(NOT BLAKE3_FOUND)
    # Official BLAKE3 C implementation with SIMD dispatch (§B15), pinned by hash.
    include(FetchContent)
    FetchContent_Declare(blake3
        URL https://github.com/BLAKE3-team/BLAKE3/archive/refs/tags/1.8.2.tar.gz
        URL_HASH SHA256=6b51aefe515969785da02e87befafc7fdc7a065cd3458cf1141f29267749e81f
        SOURCE_SUBDIR c)
    FetchContent_MakeAvailable(blake3)
endif()
find_package(PkgConfig REQUIRED)
pkg_check_modules(GLIB REQUIRED IMPORTED_TARGET glib-2.0>=2.72)
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/src/core/repository/schema.sql" VERIARCHIVE_SCHEMA_SQL)
configure_file(src/core/repository/schema_sql.h.in generated/schema_sql.h @ONLY)

add_library(veriarchive_common
    src/common/archive_path.cpp
    src/common/canonical_json.cpp
    src/common/hex.cpp
    src/common/unicode.cpp
    src/common/config.cpp
    src/common/time.cpp
    src/common/utf8.cpp
    src/common/uuid.cpp)
add_library(VeriArchive::common ALIAS veriarchive_common)
target_include_directories(veriarchive_common PUBLIC src)
target_compile_features(veriarchive_common PUBLIC cxx_std_23)
target_link_libraries(veriarchive_common
    PUBLIC nlohmann_json::nlohmann_json
    PRIVATE PkgConfig::GLIB)

add_library(veriarchive_core
    src/core/engine/candidates.cpp
    src/core/engine/register_engine.cpp
    src/core/hash/hash_pipeline.cpp
    src/core/hash/hashers.cpp
    src/core/identity/filesystem_id.cpp
    src/core/identity/matcher.cpp
    src/core/identity/volume_json.cpp
    src/core/manifest/entry.cpp
    src/core/manifest/manifest.cpp
    src/core/manifest/manifest_reader.cpp
    src/core/manifest/manifest_writer.cpp
    src/core/repository/archive_store.cpp
    src/core/repository/repository.cpp
    src/core/repository/sqlite_raii.cpp
    src/core/scan/scanner.cpp
    src/core/write_file.cpp)
add_library(VeriArchive::core ALIAS veriarchive_core)
target_include_directories(veriarchive_core PUBLIC src PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_compile_features(veriarchive_core PUBLIC cxx_std_23)
target_link_libraries(veriarchive_core
    PUBLIC VeriArchive::common SQLite::SQLite3
    PRIVATE BLAKE3::blake3 OpenSSL::Crypto)

add_library(veriarchive_platform)
add_library(VeriArchive::platform ALIAS veriarchive_platform)
target_include_directories(veriarchive_platform PUBLIC src)
target_compile_features(veriarchive_platform PUBLIC cxx_std_23)
target_link_libraries(veriarchive_platform PUBLIC VeriArchive::core)

if(WIN32)
    target_sources(veriarchive_platform PRIVATE
        src/platform/windows/file_writer_win.cpp
        src/platform/windows/fs_access_win.cpp
        src/platform/windows/media_enumerator_win.cpp
        src/platform/windows/platform_services_win.cpp
        src/platform/windows/win_util.cpp
    )
    target_link_libraries(veriarchive_platform PRIVATE mpr)
elseif(UNIX AND NOT APPLE)
    target_sources(veriarchive_platform PRIVATE
        src/platform/linux/file_writer_linux.cpp
        src/platform/linux/fs_access_linux.cpp
        src/platform/linux/media_enumerator_linux.cpp
        src/platform/linux/mountinfo.cpp
        src/platform/linux/platform_services_linux.cpp
        src/platform/linux/posix_util.cpp
    )
else()
    message(FATAL_ERROR "VeriArchive currently supports Windows and Linux only")
endif()

add_executable(veriarchive_cli
    src/cli/cli.cpp
    src/cli/cli_support.cpp
    src/cli/media_commands.cpp
    src/cli/repo_commands.cpp)
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
        tests/unit/archive_store_test.cpp
        tests/unit/canonical_json_test.cpp
        tests/unit/config_test.cpp
        tests/unit/file_writer_test.cpp
        tests/unit/fs_access_test.cpp
        tests/unit/hash_pipeline_test.cpp
        tests/unit/hash_test.cpp
        tests/unit/identity_test.cpp
        tests/unit/manifest_test.cpp
        tests/unit/platform_test.cpp
        tests/unit/unicode_test.cpp
        tests/unit/register_engine_test.cpp
        tests/unit/repository_test.cpp
        tests/unit/scanner_test.cpp)
    target_compile_features(veriarchive_tests PRIVATE cxx_std_23)
    target_link_libraries(veriarchive_tests PRIVATE VeriArchive::platform)
    target_compile_definitions(veriarchive_tests PRIVATE
        VERIARCHIVE_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/golden")
    if(NOT (UNIX AND NOT APPLE))
        # Pure C++ parser; tested on every OS although only Linux uses it.
        target_sources(veriarchive_tests PRIVATE src/platform/linux/mountinfo.cpp)
    endif()
    add_test(NAME veriarchive_unit COMMAND veriarchive_tests)
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