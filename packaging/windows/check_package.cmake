# Package check for CTest (R-121): installs the build into a scratch prefix,
# twice, and proves the result runs on a machine without MSYS2; then makes
# the zip and unzips it into that prefix, as an update does (decision 0038).
#   cmake -DBUILD_DIR=<build> -DPREFIX=<scratch> -DOBJDUMP=<objdump>
#         -DMINGW_PREFIX=<e.g. C:/msys64/ucrt64> -DVERSION=<project version>
#         -P check_package.cmake
cmake_minimum_required(VERSION 3.28)

foreach(variable BUILD_DIR PREFIX OBJDUMP MINGW_PREFIX VERSION)
    if(NOT ${variable})
        message(FATAL_ERROR "${variable} must be set")
    endif()
endforeach()

include("${CMAKE_CURRENT_LIST_DIR}/desktop_dlls.cmake")

function(install_package)
    execute_process(COMMAND "${CMAKE_COMMAND}" --install "${BUILD_DIR}" --prefix "${PREFIX}" --component VeriArchive
        RESULT_VARIABLE installed OUTPUT_QUIET)
    if(NOT installed EQUAL 0)
        message(FATAL_ERROR "cmake --install failed")
    endif()
endfunction()

file(REMOVE_RECURSE "${PREFIX}")
install_package()

# A second install into the same prefix updates the first in place, as when a
# portable folder is updated: bin/ already holds the DLLs. A DLL there that
# differs from the MinGW tree's copy, as one from an older install would, is
# replaced. SQLite's DLL is imported by the executables and by no image loader,
# so only the executables' own dependencies bring it in.
file(READ "${PREFIX}/share/licenses/THIRD-PARTY.txt" first_listing)
file(GLOB sqlite_dlls RELATIVE "${PREFIX}/bin" "${PREFIX}/bin/libsqlite3*.dll")
if(NOT sqlite_dlls)
    message(FATAL_ERROR "the package has no SQLite DLL in bin/")
endif()
foreach(dll IN LISTS sqlite_dlls)
    file(WRITE "${PREFIX}/bin/${dll}" "an older copy left by an earlier install")
endforeach()
install_package()
foreach(dll IN LISTS sqlite_dlls)
    file(SHA256 "${PREFIX}/bin/${dll}" installed_hash)
    file(SHA256 "${MINGW_PREFIX}/bin/${dll}" mingw_hash)
    if(NOT installed_hash STREQUAL mingw_hash)
        message(FATAL_ERROR "installing again kept the earlier bin/${dll} instead of the MinGW tree's copy")
    endif()
endforeach()
file(READ "${PREFIX}/share/licenses/THIRD-PARTY.txt" second_listing)
if(NOT second_listing STREQUAL first_listing)
    message(FATAL_ERROR "installing again changed the list of bundled files:\n${second_listing}")
endif()

# Every import of every packaged binary is in bin/ or part of Windows; nothing
# imports the MSYS2 POSIX runtime.
file(GLOB binaries "${PREFIX}/bin/*.exe" "${PREFIX}/bin/*.dll")
foreach(binary IN LISTS binaries)
    execute_process(COMMAND "${OBJDUMP}" -p "${binary}" OUTPUT_VARIABLE headers)
    string(REGEX MATCHALL "DLL Name: [^\r\n]+" imports "${headers}")
    foreach(import IN LISTS imports)
        string(REPLACE "DLL Name: " "" dll "${import}")
        string(TOLOWER "${dll}" lower)
        if(lower STREQUAL "msys-2.0.dll")
            message(FATAL_ERROR "${binary} imports msys-2.0.dll (R-121)")
        endif()
        if(lower MATCHES "^api-ms-|^ext-ms-")
            continue()
        endif()
        veriarchive_desktop_only("${dll}" desktop_only)
        if(NOT desktop_only AND NOT EXISTS "${PREFIX}/bin/${dll}" AND NOT EXISTS "$ENV{SystemRoot}/System32/${dll}")
            message(FATAL_ERROR "${binary} imports ${dll}, which is neither bundled nor part of Windows")
        endif()
    endforeach()
endforeach()

foreach(required
        "share/glib-2.0/schemas/gschemas.compiled"
        "lib/gdk-pixbuf-2.0/2.10.0/loaders.cache"
        "share/icons/Adwaita/index.theme"
        "share/licenses/THIRD-PARTY.txt"
        "share/licenses/gtk4"
        "share/licenses/glib2"
        "share/locale/zh_TW/LC_MESSAGES/gtk40.mo"
        "share/doc/VeriArchive/NOTICE.txt"
        "share/doc/VeriArchive/user-guide.md"
        "share/doc/VeriArchive/user-guide.zh-TW.md")
    if(NOT EXISTS "${PREFIX}/${required}")
        message(FATAL_ERROR "missing from the package: ${required}")
    endif()
endforeach()

# The installed guides show their screenshots: every picture they link to
# (images/<language>/...) is installed beside them.
foreach(guide user-guide.md user-guide.zh-TW.md)
    file(READ "${PREFIX}/share/doc/VeriArchive/${guide}" guide_text)
    # "(images/...)" without the "](" before it: a "]" in a list item would
    # keep CMake from splitting the list.
    string(REGEX MATCHALL "[(]images/[^)]+[)]" picture_links "${guide_text}")
    if(NOT picture_links)
        message(FATAL_ERROR "the installed ${guide} links to no pictures")
    endif()
    foreach(link IN LISTS picture_links)
        string(REGEX REPLACE "^[(](.+)[)]$" "\\1" picture "${link}")
        if(NOT EXISTS "${PREFIX}/share/doc/VeriArchive/${picture}")
            message(FATAL_ERROR "the installed ${guide} shows ${picture}, which is missing from the package")
        endif()
    endforeach()
endforeach()

# Run the packaged programs with only Windows on PATH.
set(clean_path "PATH=$ENV{SystemRoot}/System32;$ENV{SystemRoot}")
execute_process(COMMAND "${CMAKE_COMMAND}" -E env "${clean_path}" VERIARCHIVE_LANG=en
        "${PREFIX}/bin/veriarchive-cli.exe"
    RESULT_VARIABLE cli OUTPUT_VARIABLE usage)
if(NOT cli EQUAL 0 OR NOT usage MATCHES "Usage: veriarchive-cli")
    message(FATAL_ERROR "the packaged CLI does not start without MSYS2 (exit ${cli})")
endif()
# No command after the option: the usage, in Chinese, with the usage exit code.
execute_process(COMMAND "${CMAKE_COMMAND}" -E env "${clean_path}" "${PREFIX}/bin/veriarchive-cli.exe" --lang zh-TW
    ENCODING UTF-8
    RESULT_VARIABLE cli OUTPUT_VARIABLE chinese_usage)
if(NOT cli EQUAL 10 OR NOT chinese_usage MATCHES "用法：veriarchive-cli")
    message(FATAL_ERROR "the packaged CLI does not show Traditional Chinese (exit ${cli})\n${chinese_usage}")
endif()
if(EXISTS "${PREFIX}/bin/veriarchive-gui.exe" AND NOT EXISTS "$ENV{SystemRoot}/System32/opengl32.dll")
    message(STATUS "GUI start not checked: this Windows has no opengl32.dll (Server Core)")
elseif(EXISTS "${PREFIX}/bin/veriarchive-gui.exe")
    # Both interface languages; in Chinese GTK also loads its own zh_TW texts.
    foreach(language en zh-TW)
        execute_process(COMMAND "${CMAKE_COMMAND}" -E env "${clean_path}" VERIARCHIVE_GUI_SMOKE_TEST=1
                "VERIARCHIVE_LANG=${language}" "VERIARCHIVE_REPOSITORY=${PREFIX}/no-repository"
                "${PREFIX}/bin/veriarchive-gui.exe"
            RESULT_VARIABLE gui ERROR_VARIABLE gui_errors)
        if(NOT gui EQUAL 0 OR gui_errors MATCHES "CRITICAL|WARNING")
            message(FATAL_ERROR "the packaged GUI (${language}) does not start without MSYS2 (exit ${gui})\n${gui_errors}")
        endif()
    endforeach()
endif()

# The zip (decision 0038), made as cpack makes it: its name carries the
# version, and it holds no folder of its own, only bin, lib and share. So
# unzipped into the program folder, a zip of any version replaces the
# programs there. Here it is unzipped into the installed prefix, which by now
# holds an older program, a config file and a repository (sample data): the
# program is replaced, the config file and the repository are kept, and no
# other folder appears.
set(zip_folder "${PREFIX}-zip")
file(REMOVE_RECURSE "${zip_folder}")
execute_process(COMMAND "${CMAKE_CPACK_COMMAND}" -B "${zip_folder}"
    WORKING_DIRECTORY "${BUILD_DIR}"
    RESULT_VARIABLE packed OUTPUT_VARIABLE pack_output ERROR_VARIABLE pack_output)
set(zip "${zip_folder}/VeriArchive-${VERSION}-windows-x86_64.zip")
if(NOT packed EQUAL 0 OR NOT EXISTS "${zip}")
    message(FATAL_ERROR "cpack did not make ${zip} (exit ${packed})\n${pack_output}")
endif()
execute_process(COMMAND "${CMAKE_COMMAND}" -E tar tf "${zip}"
    RESULT_VARIABLE listed OUTPUT_VARIABLE zip_listing)
string(STRIP "${zip_listing}" zip_listing)
string(REPLACE ";" "\\;" zip_entries "${zip_listing}")
string(REPLACE "\n" ";" zip_entries "${zip_entries}")
if(NOT listed EQUAL 0 OR NOT "bin/veriarchive-cli.exe" IN_LIST zip_entries)
    list(SUBLIST zip_entries 0 5 first_entries)
    list(JOIN first_entries "\n" first_entries)
    message(FATAL_ERROR "the zip does not hold bin/veriarchive-cli.exe at its root (exit ${listed}); it begins "
        "with:\n${first_entries}")
endif()
foreach(entry IN LISTS zip_entries)
    if(NOT entry MATCHES "^(bin|lib|share)(/|$)")
        message(FATAL_ERROR "the zip holds ${entry} outside bin, lib and share: unzipped into the program "
            "folder, a new version would not replace the programs there")
    endif()
endforeach()
file(SHA256 "${PREFIX}/bin/veriarchive-cli.exe" packaged_cli)
file(WRITE "${PREFIX}/bin/veriarchive-cli.exe" "an older version of the program (sample data)")
set(sample_config "{\"repository\": \"data\", \"note\": \"sample config of the package test\"}\n")
file(WRITE "${PREFIX}/bin/config.json" "${sample_config}")
file(WRITE "${PREFIX}/bin/data/SAMPLE-repository.txt" "Sample data: stands for the folder's repository.\n")
execute_process(COMMAND "${CMAKE_COMMAND}" -E tar xf "${zip}"
    WORKING_DIRECTORY "${PREFIX}" RESULT_VARIABLE unzipped)
file(SHA256 "${PREFIX}/bin/veriarchive-cli.exe" updated_cli)
file(READ "${PREFIX}/bin/config.json" kept_config)
file(GLOB top_level RELATIVE "${PREFIX}" "${PREFIX}/*")
if(NOT unzipped EQUAL 0 OR NOT updated_cli STREQUAL packaged_cli OR NOT kept_config STREQUAL sample_config
        OR NOT EXISTS "${PREFIX}/bin/data/SAMPLE-repository.txt" OR NOT top_level STREQUAL "bin;lib;share")
    message(FATAL_ERROR "unzipping the zip into the program folder (exit ${unzipped}) did not update it in "
        "place: program replaced ${updated_cli} (expected ${packaged_cli}), folders ${top_level}, config:\n"
        "${kept_config}")
endif()
file(REMOVE_RECURSE "${zip_folder}")
message(STATUS "Windows package is self-contained: ${PREFIX}")
