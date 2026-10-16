cmake_minimum_required(VERSION 3.28)

project(VeriArchive VERSION 1.0.0 LANGUAGES C CXX)

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
# From CMake 4.3, FindSQLite3 names its target SQLite3::SQLite3 and marks the
# old name, SQLite::SQLite3, deprecated. Older versions (3.28) define only the
# old name, so the new one is made an alias of it.
if(NOT TARGET SQLite3::SQLite3)
    add_library(SQLite3::SQLite3 ALIAS SQLite::SQLite3)
endif()
find_package(OpenSSL 3.0 REQUIRED COMPONENTS Crypto)
find_package(BLAKE3 1.5 CONFIG QUIET)
if(NOT BLAKE3_FOUND)
    # Official BLAKE3 C implementation with SIMD dispatch (§B15), pinned by hash.
    include(FetchContent)
    FetchContent_Declare(blake3
        URL https://github.com/BLAKE3-team/BLAKE3/archive/refs/tags/1.8.2.tar.gz
        URL_HASH SHA256=6b51aefe515969785da02e87befafc7fdc7a065cd3458cf1141f29267749e81f
        SOURCE_SUBDIR c)
    # BLAKE3 1.8.2 asks for CMake 3.9, for which CMake 3.31 and later warn
    # "Compatibility with CMake < 3.10 will be removed". BLAKE3 alone is
    # configured with the policies of CMake 3.10 instead (CMake 4.0 and later
    # read CMAKE_POLICY_VERSION_MINIMUM), so that warning does not arise and no
    # warning is turned off. BLAKE3 uses neither policy 3.10 added (CMP0070,
    # CMP0071), so it is built as before. Turning deprecation warnings off
    # around it would not do: CMAKE_WARN_DEPRECATED does not govern this one,
    # and FetchContent_MakeAvailable runs with the diagnostics (CMake 4.4
    # cmake_diagnostic) in force where FetchContent was included.
    set(CMAKE_POLICY_VERSION_MINIMUM 3.10)
    FetchContent_MakeAvailable(blake3)
    unset(CMAKE_POLICY_VERSION_MINIMUM)
endif()
find_package(PkgConfig REQUIRED)
pkg_check_modules(GLIB REQUIRED IMPORTED_TARGET glib-2.0>=2.72)
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/src/core/repository/schema.sql" VERIARCHIVE_SCHEMA_SQL)
configure_file(src/core/repository/schema_sql.h.in generated/schema_sql.h @ONLY)
# configure_file depends on its template only: CMake runs again, and the
# header takes the schema, also when schema.sql alone changes.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS src/core/repository/schema.sql)

add_library(veriarchive_common
    src/common/archive_path.cpp
    src/common/canonical_json.cpp
    src/common/hex.cpp
    src/common/shell_argument.cpp
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
    src/core/engine/abandon_registration.cpp
    src/core/engine/abandon_relocation.cpp
    src/core/engine/abandon_verification.cpp
    src/core/engine/add_chunk_hashes.cpp
    src/core/engine/archive_status.cpp
    src/core/engine/compare_archives.cpp
    src/core/engine/candidates.cpp
    src/core/engine/comparison.cpp
    src/core/engine/item_rules.cpp
    src/core/engine/rebuild_index.cpp
    src/core/engine/recorded_manifest.cpp
    src/core/engine/register_engine.cpp
    src/core/engine/registration_attempt.cpp
    src/core/engine/relocate_engine.cpp
    src/core/engine/retire_archive.cpp
    src/core/engine/sha256sums.cpp
    src/core/engine/transfer.cpp
    src/core/engine/verify_engine.cpp
    src/core/engine/volume_watch.cpp
    src/core/hash/hash_pipeline.cpp
    src/core/hash/hashers.cpp
    src/core/identity/filesystem_id.cpp
    src/core/identity/identity_text.cpp
    src/core/identity/matcher.cpp
    src/core/identity/volume_json.cpp
    src/core/manifest/entry.cpp
    src/core/manifest/manifest.cpp
    src/core/manifest/manifest_reader.cpp
    src/core/manifest/manifest_writer.cpp
    src/core/merkle/merkle.cpp
    src/core/repository/archive_store.cpp
    src/core/repository/repository.cpp
    src/core/repository/sqlite_raii.cpp
    src/core/scan/scanner.cpp
    src/core/write_file.cpp)
add_library(VeriArchive::core ALIAS veriarchive_core)
target_include_directories(veriarchive_core PUBLIC src PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_compile_features(veriarchive_core PUBLIC cxx_std_23)
target_link_libraries(veriarchive_core
    PUBLIC VeriArchive::common SQLite3::SQLite3
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
        src/platform/windows/interrupt_win.cpp
        src/platform/windows/long_path.cpp
        src/platform/windows/media_enumerator_win.cpp
        src/platform/windows/platform_services_win.cpp
        src/platform/windows/process_table_win.cpp
        src/platform/windows/root_kind.cpp
        src/platform/windows/subst_drive.cpp
        src/platform/windows/win_util.cpp
    )
    # user32: the hidden window that hears of the session's end (interrupt_win.cpp).
    target_link_libraries(veriarchive_platform PRIVATE mpr user32)
elseif(UNIX AND NOT APPLE)
    target_sources(veriarchive_platform PRIVATE
        src/platform/linux/file_writer_linux.cpp
        src/platform/linux/fs_access_linux.cpp
        src/platform/linux/interrupt_linux.cpp
        src/platform/linux/media_enumerator_linux.cpp
        src/platform/linux/mountinfo.cpp
        src/platform/linux/platform_services_linux.cpp
        src/platform/linux/posix_util.cpp
        src/platform/linux/process_table_linux.cpp
        src/platform/linux/vpd.cpp
    )
else()
    message(FATAL_ERROR "VeriArchive currently supports Windows and Linux only")
endif()

# Composition-root helpers shared by the CLI and the GUI (R-05).
add_library(veriarchive_app
    src/app/adding_chunk_hashes.cpp
    src/app/history.cpp
    src/app/local_time.cpp
    src/app/plain_words.cpp
    src/app/progress_estimate.cpp
    src/app/unfinished_operations.cpp
    src/app/session.cpp
    src/app/text.cpp
    src/app/text_zh_tw.cpp
    src/app/typed_path.cpp
    src/app/verify_report.cpp)
add_library(VeriArchive::app ALIAS veriarchive_app)
target_compile_features(veriarchive_app PUBLIC cxx_std_23)
target_link_libraries(veriarchive_app PUBLIC VeriArchive::platform)

add_executable(veriarchive_cli
    src/cli/archive_commands.cpp
    src/cli/cli.cpp
    src/cli/cli_support.cpp
    src/cli/export_commands.cpp
    src/cli/media_commands.cpp
    src/cli/register_commands.cpp
    src/cli/verify_commands.cpp
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
target_compile_definitions(veriarchive_cli PRIVATE VERIARCHIVE_VERSION="${PROJECT_VERSION}")
target_link_libraries(veriarchive_cli PRIVATE VeriArchive::app)

# GTK4 GUI (M5, decision 0016). Built when GTK4 is available.
pkg_check_modules(GTK4 IMPORTED_TARGET gtk4>=4.10)
option(VERIARCHIVE_BUILD_GUI "Build the GTK4 GUI" ${GTK4_FOUND})
if(VERIARCHIVE_BUILD_GUI)
    add_executable(veriarchive_gui
        src/gui/archive_list_view.cpp
        src/gui/compare_view.cpp
        src/gui/displayed_identity.cpp
        src/gui/gui_app.cpp
        src/gui/gui_support.cpp
        src/gui/history_view.cpp
        src/gui/main.cpp
        src/gui/screenshots.cpp
        src/gui/register_view.cpp
        src/gui/settings_view.cpp
        src/gui/verify_view.cpp)
    set_target_properties(veriarchive_gui PROPERTIES OUTPUT_NAME veriarchive-gui WIN32_EXECUTABLE TRUE)
    target_compile_features(veriarchive_gui PRIVATE cxx_std_23)
    target_compile_definitions(veriarchive_gui PRIVATE
        VERIARCHIVE_VERSION="${PROJECT_VERSION}"
        VERIARCHIVE_WITHOUT_SESSION_BUS=$<BOOL:${WIN32}>)
    # libintl: the GUI resets GTK's translation cache when the language changes.
    find_package(Intl REQUIRED)
    target_link_libraries(veriarchive_gui PRIVATE VeriArchive::app PkgConfig::GTK4 Intl::Intl)
    if(WIN32)
        target_sources(veriarchive_gui PRIVATE src/platform/windows/veriarchive.rc)
    endif()
endif()

# Installation and packaging (cmake --install --component VeriArchive, cpack).
# Only the VeriArchive component is packaged, so install rules of fetched
# dependencies (BLAKE3 headers and libraries) stay out. On Windows the install
# tree is made self-contained: DLLs, GTK resources and licenses (R-121).
install(TARGETS veriarchive_cli RUNTIME DESTINATION bin COMPONENT VeriArchive)
if(VERIARCHIVE_BUILD_GUI)
    install(TARGETS veriarchive_gui RUNTIME DESTINATION bin COMPONENT VeriArchive)
endif()
install(FILES docs/user-guide.md docs/user-guide.zh-TW.md
    DESTINATION share/doc/VeriArchive COMPONENT VeriArchive)
# The guides show their screenshots from images/ beside them.
install(DIRECTORY docs/images DESTINATION share/doc/VeriArchive COMPONENT VeriArchive)
# The licenses of the code compiled into the programs (decision 0038):
# nlohmann/json (MIT) and BLAKE3 (CC0 1.0, or Apache 2.0 with or without the
# LLVM exceptions). packaging/licenses holds the texts of the versions fetched
# above, 3.12.0 and 1.8.2; a new version brings its own.
install(DIRECTORY packaging/licenses/ DESTINATION share/licenses COMPONENT VeriArchive)
if(WIN32)
    get_filename_component(VERIARCHIVE_MINGW_PREFIX "${CMAKE_CXX_COMPILER}" DIRECTORY)
    get_filename_component(VERIARCHIVE_MINGW_PREFIX "${VERIARCHIVE_MINGW_PREFIX}" DIRECTORY)
    find_program(VERIARCHIVE_OBJDUMP objdump HINTS "${VERIARCHIVE_MINGW_PREFIX}/bin" REQUIRED)
    install(FILES packaging/windows/NOTICE.txt DESTINATION share/doc/VeriArchive COMPONENT VeriArchive)
    # The package's own short README, in English and Chinese, beside the
    # guides: what the program is, how to start it, where the records are
    # (decision 0038). The repository's README.md is for developers.
    install(FILES packaging/windows/package-readme.md DESTINATION share/doc/VeriArchive
        RENAME README.md COMPONENT VeriArchive)
    install(FILES packaging/windows/package-readme.zh-TW.md DESTINATION share/doc/VeriArchive
        RENAME README.zh-TW.md COMPONENT VeriArchive)
    install(CODE "set(VERIARCHIVE_MINGW_PREFIX \"${VERIARCHIVE_MINGW_PREFIX}\")
set(VERIARCHIVE_OBJDUMP \"${VERIARCHIVE_OBJDUMP}\")
set(VERIARCHIVE_RUNTIME_SCAN_DIR \"${CMAKE_CURRENT_BINARY_DIR}/runtime-scan\")" COMPONENT VeriArchive)
    install(SCRIPT packaging/windows/install_runtime.cmake COMPONENT VeriArchive)
endif()

set(CPACK_PACKAGE_NAME "VeriArchive")
set(CPACK_PACKAGE_VENDOR "VeriArchive")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Integrity verification for archive media")
set(CPACK_GENERATOR ZIP)
if(WIN32)
    set(CPACK_PACKAGE_FILE_NAME "VeriArchive-${PROJECT_VERSION}-windows-x86_64")
else()
    set(CPACK_PACKAGE_FILE_NAME "VeriArchive-${PROJECT_VERSION}-linux-x86_64")
endif()
# The zip holds no top-level folder: bin, lib and share sit at its root, so
# that unzipping a new version into the program folder replaces the programs
# there whatever its version number; the zip's name keeps the version
# (decision 0038).
set(CPACK_INCLUDE_TOPLEVEL_DIRECTORY OFF)
# CPack installs this project's VeriArchive component alone (build folder,
# project, component, subfolder). Its component mode would do the same, but
# stages the files two folders deeper (<component>/<package file name>/),
# where paths in deep build folders reach 260 characters, more than
# glib-compile-schemas (install_runtime.cmake) can open.
set(CPACK_INSTALL_CMAKE_PROJECTS "${CMAKE_BINARY_DIR};${PROJECT_NAME};VeriArchive;/")
include(CPack)

if(BUILD_TESTING)
    add_executable(veriarchive_tests
        tests/unit/test_main.cpp
        tests/unit/abandon_registration_test.cpp
        tests/unit/add_chunk_hashes_test.cpp
        tests/unit/archive_path_test.cpp
        tests/unit/archive_status_test.cpp
        tests/unit/compare_archives_test.cpp
        tests/unit/archive_store_test.cpp
        tests/unit/canonical_json_test.cpp
        tests/unit/config_test.cpp
        tests/unit/displayed_identity_test.cpp
        tests/unit/file_writer_test.cpp
        tests/unit/fs_access_test.cpp
        tests/unit/hash_pipeline_test.cpp
        tests/unit/hash_test.cpp
        tests/unit/identity_test.cpp
        tests/unit/interrupt_test.cpp
        tests/unit/local_time_test.cpp
        tests/unit/progress_estimate_test.cpp
        tests/unit/manifest_test.cpp
        tests/unit/merkle_test.cpp
        tests/unit/plain_words_test.cpp
        tests/unit/platform_test.cpp
        tests/unit/unicode_test.cpp
        tests/unit/vpd_test.cpp
        tests/unit/verify_engine_test.cpp
        tests/unit/rebuild_index_test.cpp
        tests/unit/unfinished_operations_test.cpp
        tests/unit/register_engine_test.cpp
        tests/unit/relocate_test.cpp
        tests/unit/repository_test.cpp
        tests/unit/retire_archive_test.cpp
        tests/unit/scanner_test.cpp
        tests/unit/session_test.cpp
        tests/unit/sha256sums_test.cpp
        tests/unit/subst_drive_test.cpp
        tests/unit/text_test.cpp
        tests/unit/transfer_test.cpp
        tests/unit/typed_path_test.cpp)
    # The platform layer's own names for failed calls (Win32 errors, errno),
    # and a sparse sample file made as each system makes one (R-48).
    if(WIN32)
        target_sources(veriarchive_tests PRIVATE tests/unit/platform_errors_win.cpp tests/unit/sparse_file_win.cpp)
    elseif(UNIX AND NOT APPLE)
        target_sources(veriarchive_tests PRIVATE tests/unit/platform_errors_linux.cpp
            tests/unit/sparse_file_linux.cpp)
    endif()
    target_compile_features(veriarchive_tests PRIVATE cxx_std_23)
    target_link_libraries(veriarchive_tests PRIVATE VeriArchive::platform VeriArchive::app)
    if(WIN32)
        # interrupt_test.cpp sends the session's end to the hidden window.
        target_link_libraries(veriarchive_tests PRIVATE user32)
    endif()
    target_compile_definitions(veriarchive_tests PRIVATE
        VERIARCHIVE_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/golden")
    if(NOT (UNIX AND NOT APPLE))
        # Pure C++ parser; tested on every OS although only Linux uses it.
        target_sources(veriarchive_tests PRIVATE src/platform/linux/mountinfo.cpp src/platform/linux/vpd.cpp)
    endif()
    if(NOT WIN32)
        # Pure C++ path handling; tested on every OS although only Windows uses it.
        target_sources(veriarchive_tests PRIVATE src/platform/windows/long_path.cpp src/platform/windows/root_kind.cpp
            src/platform/windows/subst_drive.cpp)
    endif()
    # The GUI's choice of the media identity it shows is plain C++ without
    # GTK, so it is tested also where the GUI is not built.
    target_sources(veriarchive_tests PRIVATE src/gui/displayed_identity.cpp)
    add_test(NAME veriarchive_unit COMMAND veriarchive_tests)
    if(VERIARCHIVE_BUILD_GUI AND WIN32 AND NOT EXISTS "$ENV{SystemRoot}/System32/opengl32.dll")
        # GTK4 needs OpenGL, which Windows Server Core (CI containers) lacks.
        message(STATUS "GUI smoke tests skipped: this Windows has no opengl32.dll (Server Core)")
    elseif(VERIARCHIVE_BUILD_GUI AND WIN32)
        # Builds every GUI view without showing the window (decision 0016), then
        # rebuilds it in the other language and back (decision 0017). APPDATA
        # keeps the config and the failure log out of the developer's profile.
        foreach(language en zh-TW)
            add_test(NAME veriarchive_gui_smoke_${language} COMMAND veriarchive_gui)
            set_tests_properties(veriarchive_gui_smoke_${language} PROPERTIES
                ENVIRONMENT "VERIARCHIVE_GUI_SMOKE_TEST=1;VERIARCHIVE_LANG=${language};VERIARCHIVE_REPOSITORY=${CMAKE_CURRENT_BINARY_DIR}/no-repository;APPDATA=${CMAKE_CURRENT_BINARY_DIR}/gui-smoke-appdata"
                FAIL_REGULAR_EXPRESSION "CRITICAL|WARNING")
        endforeach()
    endif()
    if(WIN32)
        # R-121: the installed package runs with only Windows on PATH, also
        # after a second install over the first; its zip unzips into the
        # program folder (decision 0038).
        add_test(NAME veriarchive_package
            COMMAND ${CMAKE_COMMAND}
                -DBUILD_DIR=${CMAKE_BINARY_DIR}
                -DPREFIX=${CMAKE_CURRENT_BINARY_DIR}/package-test
                -DOBJDUMP=${VERIARCHIVE_OBJDUMP}
                -DMINGW_PREFIX=${VERIARCHIVE_MINGW_PREFIX}
                -DVERSION=${PROJECT_VERSION}
                -P ${CMAKE_CURRENT_SOURCE_DIR}/packaging/windows/check_package.cmake)
    endif()
    # The sqlite3 program, where there is one (MSYS2's sqlite3 package has it),
    # makes a repository of an older schema version for the end-to-end test
    # of the upgrade (decision 0032); without it, that part is left out.
    find_program(VERIARCHIVE_SQLITE3 sqlite3)
    add_test(NAME veriarchive_cli_e2e
        COMMAND ${CMAKE_COMMAND}
            -DCLI=$<TARGET_FILE:veriarchive_cli>
            -DWORK=${CMAKE_CURRENT_BINARY_DIR}/e2e
            -DVERSION=${PROJECT_VERSION}
            -DSQLITE3=${VERIARCHIVE_SQLITE3}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/cli_e2e.cmake)
    # The test chooses the language through its config file and --lang.
    set_tests_properties(veriarchive_cli_e2e PROPERTIES ENVIRONMENT_MODIFICATION "VERIARCHIVE_LANG=unset:")
endif()

if(MSVC)
    foreach(target veriarchive_common veriarchive_core veriarchive_platform veriarchive_app veriarchive_cli veriarchive_gui veriarchive_tests)
        if(TARGET ${target})
            target_compile_options(${target} PRIVATE /W4 /permissive-)
        endif()
    endforeach()
else()
    foreach(target veriarchive_common veriarchive_core veriarchive_platform veriarchive_app veriarchive_cli veriarchive_gui veriarchive_tests)
        if(TARGET ${target})
            target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
        endif()
    endforeach()
endif()