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

# The device identity reader (decision 0047) is in the package, and as it
# runs with administrator permission, it loads only Windows' own DLLs, none
# from the program's folder (it is linked statically). Windows looks for a DLL
# that is not one of its known DLLs in the program's folder first, so no DLL
# it imports may be in bin/, which the portable zip leaves writable to the
# user. It is never started here: its manifest asks Windows for
# administrator permission.
set(reader "${PREFIX}/bin/veriarchive-device-identity.exe")
if(NOT EXISTS "${reader}")
    message(FATAL_ERROR "missing from the package: bin/veriarchive-device-identity.exe")
endif()
execute_process(COMMAND "${OBJDUMP}" -p "${reader}" OUTPUT_VARIABLE reader_headers)
string(REGEX MATCHALL "DLL Name: [^\r\n]+" reader_imports "${reader_headers}")
if(NOT reader_imports)
    message(FATAL_ERROR "objdump finds no import of ${reader}")
endif()
foreach(import IN LISTS reader_imports)
    string(REPLACE "DLL Name: " "" dll "${import}")
    string(TOLOWER "${dll}" lower)
    if(NOT lower MATCHES "^api-ms-|^ext-ms-" AND NOT EXISTS "$ENV{SystemRoot}/System32/${dll}")
        message(FATAL_ERROR "${reader} imports ${dll}, which is no DLL of Windows")
    endif()
    if(EXISTS "${PREFIX}/bin/${dll}")
        message(FATAL_ERROR "${reader} imports ${dll}, which bin/ also holds: the elevated reader would load that copy")
    endif()
endforeach()

foreach(required
        "share/glib-2.0/schemas/gschemas.compiled"
        "lib/gdk-pixbuf-2.0/2.10.0/loaders.cache"
        "share/icons/Adwaita/index.theme"
        "share/licenses/THIRD-PARTY.txt"
        "share/licenses/gtk4"
        "share/licenses/glib2"
        "share/licenses/libadwaita"
        "share/licenses/nlohmann-json/LICENSE.MIT"
        "share/licenses/blake3/LICENSE_CC0"
        "share/licenses/blake3/LICENSE_A2"
        "share/licenses/blake3/LICENSE_A2LLVM"
        # Linked into the device identity reader (NOTICE.txt).
        "share/licenses/libgcc"
        "share/licenses/libstdc++"
        "share/licenses/libwinpthread/COPYING"
        "share/locale/zh_TW/LC_MESSAGES/gtk40.mo"
        "share/locale/zh_TW/LC_MESSAGES/libadwaita.mo"
        "share/doc/VeriArchive/NOTICE.txt"
        "share/doc/VeriArchive/README.md"
        "share/doc/VeriArchive/README.zh-TW.md"
        "share/doc/VeriArchive/user-guide.md"
        "share/doc/VeriArchive/user-guide.zh-TW.md")
    if(NOT EXISTS "${PREFIX}/${required}")
        message(FATAL_ERROR "missing from the package: ${required}")
    endif()
endforeach()
# Every MSYS2 package that owns a bundled file, as THIRD-PARTY.txt names it
# beside the file's name, and the packages of the icon themes have their
# license in share/licenses/<package>, a folder that holds a file (R-121,
# decision 0038). MSYS2 ships none for some packages, as GStreamer's.
file(STRINGS "${PREFIX}/share/licenses/THIRD-PARTY.txt" bundled_files REGEX "^[^ ]+  [^ ]+$")
if(NOT bundled_files)
    message(FATAL_ERROR "share/licenses/THIRD-PARTY.txt lists no bundled file")
endif()
set(licensed_packages adwaita-icon-theme hicolor-icon-theme)
foreach(bundled_file IN LISTS bundled_files)
    string(REGEX REPLACE "^[^ ]+  " "" package "${bundled_file}")
    list(APPEND licensed_packages "${package}")
endforeach()
list(REMOVE_DUPLICATES licensed_packages)
set(unlicensed "")
foreach(package IN LISTS licensed_packages)
    file(GLOB_RECURSE license_files "${PREFIX}/share/licenses/${package}/*")
    if(NOT license_files)
        list(APPEND unlicensed "${package}")
    endif()
endforeach()
if(unlicensed)
    list(JOIN unlicensed ", " unlicensed)
    message(FATAL_ERROR "the package holds no license of ${unlicensed}, whose files it bundles (THIRD-PARTY.txt)")
endif()
# NOTICE.txt names the code compiled into the programs and where its licenses
# are (decision 0038).
file(READ "${PREFIX}/share/doc/VeriArchive/NOTICE.txt" notice)
foreach(named "nlohmann/json" "share\\licenses\\nlohmann-json\\LICENSE.MIT" "BLAKE3" "share\\licenses\\blake3\\"
        "bin\\veriarchive-device-identity.exe" "share\\licenses\\libwinpthread\\")
    string(FIND "${notice}" "${named}" found)
    if(found EQUAL -1)
        message(FATAL_ERROR "NOTICE.txt does not name ${named}")
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

# The README in either language is the package's own (decision 0038), not
# the repository's README for developers: it points to both guides beside
# it, and everything it links to is in the package.
foreach(readme README.md README.zh-TW.md)
    file(READ "${PREFIX}/share/doc/VeriArchive/${readme}" readme_text)
    # "](" becomes a marker first: a "]" in a list item would keep CMake
    # from splitting the list.
    string(REPLACE "](" "<link>" readme_text "${readme_text}")
    string(REGEX MATCHALL "<link>[^)]+[)]" links "${readme_text}")
    set(targets "")
    foreach(link IN LISTS links)
        string(REGEX REPLACE "^<link>(.+)[)]$" "\\1" target "${link}")
        list(APPEND targets "${target}")
        if(NOT EXISTS "${PREFIX}/share/doc/VeriArchive/${target}")
            message(FATAL_ERROR "the installed ${readme} links to ${target}, which is not in the package beside it")
        endif()
    endforeach()
    if(NOT "user-guide.md" IN_LIST targets OR NOT "user-guide.zh-TW.md" IN_LIST targets)
        message(FATAL_ERROR "the installed ${readme} does not point to both guides beside it: ${targets}")
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

# GTK settings schemas (decision 0038). glib-compile-schemas cannot open a
# schema path of 260 characters or more, and a schema file it cannot open or
# parse it names and leaves out, still exiting 0; packaging compiles a copy
# in a short folder and takes any message for a failure. GTK's schemas, in a
# folder deep enough that every path of them has 260 characters or more,
# compile; with a broken schema beside them (sample data), the compile fails.
include("${CMAKE_CURRENT_LIST_DIR}/compile_schemas.cmake")
set(compiler "${MINGW_PREFIX}/bin/glib-compile-schemas.exe")
set(schema_probe "${PREFIX}-schemas")
file(REMOVE_RECURSE "${schema_probe}")
set(deep_schemas "${schema_probe}")
string(LENGTH "${deep_schemas}" deep_length)
while(deep_length LESS 225)
    string(APPEND deep_schemas "/a-folder-of-sample-schemas-deep-SAMPLE")
    string(LENGTH "${deep_schemas}" deep_length)
endwhile()
file(GLOB gtk_schema_files "${MINGW_PREFIX}/share/glib-2.0/schemas/org.gtk.gtk4.*.gschema.xml")
file(COPY ${gtk_schema_files} DESTINATION "${deep_schemas}")
veriarchive_compile_schemas("${compiler}" "${deep_schemas}" intact_problem)
set(deep_compiled FALSE)
if(EXISTS "${deep_schemas}/gschemas.compiled")
    set(deep_compiled TRUE)
endif()
file(WRITE "${deep_schemas}/org.example.SAMPLE-broken.gschema.xml"
    "<schemalist>\n  <schema id=\"org.example.SAMPLE\" path=\"/org/example/sample/\">\n")
veriarchive_compile_schemas("${compiler}" "${deep_schemas}" broken_problem)
if(NOT intact_problem STREQUAL "" OR NOT deep_compiled OR broken_problem STREQUAL "")
    message(FATAL_ERROR "the schema compile fails on GTK's schemas in a deep folder, or takes a broken schema "
        "for a success:\n${intact_problem}\n${broken_problem}")
endif()
file(REMOVE_RECURSE "${schema_probe}")

# Every schema of GTK's schema files is in the package's gschemas.compiled,
# as GLib itself reads it: gsettings, kept from every other schema folder,
# lists the schemas compiled in the package's alone. This is the zip's
# compile, unzipped over the installed one above.
set(no_data "${PREFIX}-no-data")
file(MAKE_DIRECTORY "${no_data}")
set(isolated "XDG_DATA_DIRS=${no_data}" "XDG_DATA_HOME=${no_data}"
    "GSETTINGS_SCHEMA_DIR=${PREFIX}/share/glib-2.0/schemas")
set(compiled_schemas "")
foreach(listing list-schemas list-relocatable-schemas)
    execute_process(COMMAND "${CMAKE_COMMAND}" -E env ${isolated} "${MINGW_PREFIX}/bin/gsettings.exe" ${listing}
        ENCODING UTF-8 RESULT_VARIABLE listed OUTPUT_VARIABLE schema_listing ERROR_VARIABLE schema_errors)
    if(NOT listed EQUAL 0 AND listing STREQUAL "list-schemas")
        message(FATAL_ERROR "gsettings finds no schema in the package (exit ${listed}):\n${schema_errors}")
    endif()
    string(REPLACE "\r" "" schema_listing "${schema_listing}")
    string(REPLACE "\n" ";" schema_lines "${schema_listing}")
    list(APPEND compiled_schemas ${schema_lines})
endforeach()
file(REMOVE_RECURSE "${no_data}")
file(GLOB gtk_schema_files "${MINGW_PREFIX}/share/glib-2.0/schemas/org.gtk.gtk4.*.gschema.xml")
set(expected_schemas "")
foreach(schema_file IN LISTS gtk_schema_files)
    file(READ "${schema_file}" schema_text)
    string(REGEX MATCHALL "<schema [^>]*id=[\"'][^\"']+[\"']" schema_tags "${schema_text}")
    foreach(tag IN LISTS schema_tags)
        string(REGEX REPLACE ".*id=[\"']([^\"']+)[\"']$" "\\1" schema_id "${tag}")
        list(APPEND expected_schemas "${schema_id}")
    endforeach()
endforeach()
if(NOT expected_schemas)
    message(FATAL_ERROR "found no GTK schema in ${MINGW_PREFIX}/share/glib-2.0/schemas")
endif()
foreach(schema_id IN LISTS expected_schemas)
    if(NOT schema_id IN_LIST compiled_schemas)
        message(FATAL_ERROR "the package's gschemas.compiled lacks the GTK schema ${schema_id}; it holds: "
            "${compiled_schemas}")
    endif()
endforeach()
message(STATUS "Windows package is self-contained: ${PREFIX}")
