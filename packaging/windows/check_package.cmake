# Package check for CTest (R-121): installs the build into a scratch prefix and
# proves the result runs on a machine without MSYS2.
#   cmake -DBUILD_DIR=<build> -DPREFIX=<scratch> -DOBJDUMP=<objdump> -P check_package.cmake
cmake_minimum_required(VERSION 3.28)

foreach(variable BUILD_DIR PREFIX OBJDUMP)
    if(NOT ${variable})
        message(FATAL_ERROR "${variable} must be set")
    endif()
endforeach()

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
        if(NOT EXISTS "${PREFIX}/bin/${dll}" AND NOT EXISTS "$ENV{SystemRoot}/System32/${dll}")
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
        "share/doc/VeriArchive/NOTICE.txt")
    if(NOT EXISTS "${PREFIX}/${required}")
        message(FATAL_ERROR "missing from the package: ${required}")
    endif()
endforeach()

# Run the packaged programs with only Windows on PATH.
set(clean_path "PATH=$ENV{SystemRoot}/System32;$ENV{SystemRoot}")
execute_process(COMMAND "${CMAKE_COMMAND}" -E env "${clean_path}" "${PREFIX}/bin/veriarchive-cli.exe"
    RESULT_VARIABLE cli OUTPUT_VARIABLE usage)
if(NOT cli EQUAL 0 OR NOT usage MATCHES "Usage: veriarchive-cli")
    message(FATAL_ERROR "the packaged CLI does not start without MSYS2 (exit ${cli})")
endif()
if(EXISTS "${PREFIX}/bin/veriarchive-gui.exe")
    execute_process(COMMAND "${CMAKE_COMMAND}" -E env "${clean_path}" VERIARCHIVE_GUI_SMOKE_TEST=1
            "VERIARCHIVE_REPOSITORY=${PREFIX}/no-repository" "${PREFIX}/bin/veriarchive-gui.exe"
        RESULT_VARIABLE gui ERROR_VARIABLE gui_errors)
    if(NOT gui EQUAL 0 OR gui_errors MATCHES "CRITICAL|WARNING")
        message(FATAL_ERROR "the packaged GUI does not start without MSYS2 (exit ${gui})\n${gui_errors}")
    endif()
endif()
message(STATUS "Windows package is self-contained: ${PREFIX}")
