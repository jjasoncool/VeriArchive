# Package check for CTest (R-121): installs the build into a scratch prefix and
# proves the result runs on a machine without MSYS2.
#   cmake -DBUILD_DIR=<build> -DPREFIX=<scratch> -DOBJDUMP=<objdump> -P check_package.cmake
cmake_minimum_required(VERSION 3.28)

foreach(variable BUILD_DIR PREFIX OBJDUMP)
    if(NOT ${variable})
        message(FATAL_ERROR "${variable} must be set")
    endif()
endforeach()

include("${CMAKE_CURRENT_LIST_DIR}/desktop_dlls.cmake")

file(REMOVE_RECURSE "${PREFIX}")
execute_process(COMMAND "${CMAKE_COMMAND}" --install "${BUILD_DIR}" --prefix "${PREFIX}" --component VeriArchive
    RESULT_VARIABLE installed OUTPUT_QUIET)
if(NOT installed EQUAL 0)
    message(FATAL_ERROR "cmake --install failed")
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
        "share/doc/VeriArchive/user-guide.zh-TW.md")
    if(NOT EXISTS "${PREFIX}/${required}")
        message(FATAL_ERROR "missing from the package: ${required}")
    endif()
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
message(STATUS "Windows package is self-contained: ${PREFIX}")
