# Runs at install time (cmake --install / cpack) on Windows. Makes the install
# tree self-contained, so it runs on a machine without MSYS2 (R-121):
#   bin/                         executables and every DLL they need
#   lib/gdk-pixbuf-2.0/...       image loaders GTK uses for icons
#   share/glib-2.0/schemas       compiled GTK settings schemas
#   share/icons                  Adwaita and hicolor icon themes
#   share/locale/zh_TW           GTK's own Traditional Chinese texts
#   share/licenses               licenses of every bundled third-party package
#
# Set by the including CMakeLists.txt through install(CODE):
#   VERIARCHIVE_MINGW_PREFIX      e.g. C:/msys64/ucrt64
#   VERIARCHIVE_OBJDUMP           objdump used to read DLL imports
#   VERIARCHIVE_RUNTIME_SCAN_DIR  scratch folder in the build tree (step 1)
cmake_minimum_required(VERSION 3.28)
if(POLICY CMP0207)
    cmake_policy(SET CMP0207 NEW) # normalise paths before matching exclude patterns
endif()

# cmake_install.cmake includes this script from the source tree, so a build
# tree configured before VERIARCHIVE_RUNTIME_SCAN_DIR existed runs it without
# that folder, and step 1 would make its scratch folder at the root of the
# current drive. Building configures the tree again.
if(NOT VERIARCHIVE_RUNTIME_SCAN_DIR)
    message(FATAL_ERROR "VERIARCHIVE_RUNTIME_SCAN_DIR is not set: this build was configured by an older "
        "version; run cmake --build (it configures again) before installing")
endif()

include("${CMAKE_CURRENT_LIST_DIR}/desktop_dlls.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/compile_schemas.cmake")

set(prefix "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}")
set(mingw "${VERIARCHIVE_MINGW_PREFIX}")
set(loader_dir "lib/gdk-pixbuf-2.0/2.10.0")

# 1. Every DLL the executables and the image loaders import, from the MinGW tree.
#
# Like Windows, file(GET_RUNTIME_DEPENDENCIES) looks for a DLL next to the
# binary that imports it first. A prefix that already holds an earlier install,
# as when a portable folder is updated in place, has the DLLs in bin/ beside
# the executables: they would be found there, while the image loaders find the
# same DLLs in the MinGW tree, a conflict the command refuses; and everything
# found in bin/ would come from the earlier install, never updated. So the
# executables are scanned from a folder that holds nothing else: hard links to
# them where the file system allows, copies elsewhere. A folder per prefix
# keeps installs into different prefixes from the same build apart.
set(CMAKE_GET_RUNTIME_DEPENDENCIES_PLATFORM "windows+pe")
set(CMAKE_GET_RUNTIME_DEPENDENCIES_TOOL "objdump")
set(CMAKE_GET_RUNTIME_DEPENDENCIES_COMMAND "${VERIARCHIVE_OBJDUMP}")
string(SHA256 scan_name "${prefix}")
string(SUBSTRING "${scan_name}" 0 16 scan_name)
set(scan_dir "${VERIARCHIVE_RUNTIME_SCAN_DIR}/${scan_name}")
file(REMOVE_RECURSE "${scan_dir}")
file(MAKE_DIRECTORY "${scan_dir}")
file(GLOB installed_executables "${prefix}/bin/*.exe")
set(executables "")
foreach(executable IN LISTS installed_executables)
    get_filename_component(name "${executable}" NAME)
    file(CREATE_LINK "${executable}" "${scan_dir}/${name}" COPY_ON_ERROR)
    list(APPEND executables "${scan_dir}/${name}")
endforeach()
file(GLOB loaders "${mingw}/${loader_dir}/loaders/*.dll")
file(GET_RUNTIME_DEPENDENCIES
    EXECUTABLES ${executables}
    MODULES ${loaders}
    RESOLVED_DEPENDENCIES_VAR resolved
    UNRESOLVED_DEPENDENCIES_VAR unresolved
    DIRECTORIES "${mingw}/bin"
    PRE_EXCLUDE_REGEXES "^api-ms-" "^ext-ms-"
    POST_EXCLUDE_REGEXES "^[A-Za-z]:[/\\\\][Ww][Ii][Nn][Dd][Oo][Ww][Ss][/\\\\]")
file(REMOVE_RECURSE "${scan_dir}")

# Whatever is not found in the MinGW tree must be part of Windows itself. All
# missing DLLs are reported together.
set(missing "")
foreach(dll IN LISTS unresolved)
    veriarchive_desktop_only("${dll}" desktop_only)
    if(desktop_only)
        message(STATUS "${dll} belongs to desktop Windows but is missing on this machine; not bundled")
    elseif(NOT EXISTS "$ENV{SystemRoot}/System32/${dll}")
        list(APPEND missing "${dll}")
    endif()
endforeach()
if(missing)
    message(FATAL_ERROR "Unresolved runtime dependencies: ${missing}")
endif()

set(bundled "")
foreach(dll IN LISTS resolved)
    get_filename_component(name "${dll}" NAME)
    if(name MATCHES "^msys-2\\.0\\.dll$")
        message(FATAL_ERROR "The package must not depend on the MSYS2 POSIX runtime (R-121): ${dll}")
    endif()
    string(TOLOWER "${dll}" lower)
    string(TOLOWER "${mingw}" lower_mingw)
    if(lower MATCHES "^${lower_mingw}/")
        file(INSTALL "${dll}" DESTINATION "${prefix}/bin")
        list(APPEND bundled "${dll}")
    endif()
endforeach()

# 2. Image loaders; loaders.cache uses paths relative to the install prefix.
file(INSTALL ${loaders} DESTINATION "${prefix}/${loader_dir}/loaders")
file(INSTALL "${mingw}/${loader_dir}/loaders.cache" DESTINATION "${prefix}/${loader_dir}")

# 3. GTK settings schemas, compiled for this package only, in a short folder
# (compile_schemas.cmake). A schema that glib-compile-schemas reports and
# leaves out, though it exits 0, fails the install: GTK stops when a schema it
# reads is missing (decision 0038).
file(GLOB schemas "${mingw}/share/glib-2.0/schemas/org.gtk.gtk4.*.gschema.xml")
file(INSTALL ${schemas} DESTINATION "${prefix}/share/glib-2.0/schemas")
veriarchive_compile_schemas("${mingw}/bin/glib-compile-schemas.exe" "${prefix}/share/glib-2.0/schemas" schema_problem)
if(NOT schema_problem STREQUAL "")
    message(FATAL_ERROR "the GTK settings schemas did not compile: ${schema_problem}")
endif()

# 4. Icon themes used by GTK widgets.
file(INSTALL "${mingw}/share/icons/Adwaita" "${mingw}/share/icons/hicolor"
    DESTINATION "${prefix}/share/icons")

# 5. Traditional Chinese texts of GTK's own widgets and dialogs (decision 0017);
# GTK finds them under share/locale next to bin.
foreach(domain gtk40 glib20 gdk-pixbuf libadwaita)
    set(catalog "${mingw}/share/locale/zh_TW/LC_MESSAGES/${domain}.mo")
    if(EXISTS "${catalog}")
        file(INSTALL "${catalog}" DESTINATION "${prefix}/share/locale/zh_TW/LC_MESSAGES")
    endif()
endforeach()

# 6. Licenses (LGPL notices, R-121): one folder per MSYS2 package that owns a
# bundled file, plus a list of which file came from which package. A package
# that MSYS2 ships without a license file, as GStreamer's three, takes the
# text kept for it in packaging/windows/licenses/<package>. A bundled file
# whose package pacman cannot name, or a package with a license in neither
# place, fails the install: the package would hold that library without its
# license (decision 0038).
get_filename_component(msys_root "${mingw}/.." ABSOLUTE)
find_program(pacman pacman PATHS "${msys_root}/usr/bin" NO_DEFAULT_PATH)
set(owned_paths "")
foreach(file IN LISTS bundled loaders)
    file(RELATIVE_PATH inside "${msys_root}" "${file}")
    list(APPEND owned_paths "/${inside}")
endforeach()
# One pacman call for all files: "<path> is owned by <package> <version>".
# Asked in English whatever the shell's language: pacman answers in the
# language of the locale (in a Traditional Chinese MSYS2 shell, "... 被 ...
# 所擁有"), which the lines below would not read.
set(owner_report "")
if(pacman)
    execute_process(COMMAND "${CMAKE_COMMAND}" -E env LC_ALL=C LANGUAGE=C "${pacman}" -Qo ${owned_paths}
        OUTPUT_VARIABLE owner_report ERROR_QUIET)
endif()
# File name -> package, kept as "name|package" pairs. File names are compared
# as plain strings (names such as libstdc++-6.dll contain regex characters).
string(REPLACE ";" "\\;" owner_report "${owner_report}")
string(REPLACE "\n" ";" owner_lines "${owner_report}")
set(ownership "")
foreach(owner_line IN LISTS owner_lines)
    if(owner_line MATCHES "^(.+) is owned by mingw-w64-ucrt-x86_64-([^ ]+)")
        get_filename_component(owned_name "${CMAKE_MATCH_1}" NAME)
        list(APPEND ownership "${owned_name}|${CMAKE_MATCH_2}")
    endif()
endforeach()
set(owners "")
set(ownerless "")
set(listing "Third-party files bundled with VeriArchive\n\n")
foreach(file IN LISTS bundled loaders)
    get_filename_component(name "${file}" NAME)
    set(package "unknown")
    foreach(pair IN LISTS ownership)
        string(FIND "${pair}" "|" bar)
        string(SUBSTRING "${pair}" 0 ${bar} pair_name)
        if(pair_name STREQUAL name)
            math(EXPR package_start "${bar} + 1")
            string(SUBSTRING "${pair}" ${package_start} -1 package)
            break()
        endif()
    endforeach()
    string(APPEND listing "${name}  ${package}\n")
    if(package STREQUAL "unknown")
        list(APPEND ownerless "${name}")
    else()
        list(APPEND owners "${package}")
    endif()
endforeach()
foreach(package glib2 gtk4)
    list(APPEND owners "${package}") # schemas and icon themes
endforeach()
list(APPEND owners adwaita-icon-theme hicolor-icon-theme)
list(REMOVE_DUPLICATES owners)
set(unlicensed "")
foreach(package IN LISTS owners)
    file(GLOB msys2_license "${mingw}/share/licenses/${package}/*")
    file(GLOB kept_license "${CMAKE_CURRENT_LIST_DIR}/licenses/${package}/*")
    if(msys2_license)
        file(INSTALL "${mingw}/share/licenses/${package}" DESTINATION "${prefix}/share/licenses")
    elseif(kept_license)
        file(INSTALL "${CMAKE_CURRENT_LIST_DIR}/licenses/${package}" DESTINATION "${prefix}/share/licenses")
    else()
        list(APPEND unlicensed "${package}")
    endif()
endforeach()
file(WRITE "${prefix}/share/licenses/THIRD-PARTY.txt" "${listing}")
if(ownerless)
    list(JOIN ownerless ", " ownerless)
    if(NOT pacman)
        string(APPEND ownerless " (pacman is not in ${msys_root}/usr/bin)")
    endif()
    message(FATAL_ERROR "no MSYS2 package is known to own these bundled files, so their licenses cannot be "
        "installed: ${ownerless}")
endif()
if(unlicensed)
    list(JOIN unlicensed ", " unlicensed)
    message(FATAL_ERROR "MSYS2 ships no license file of the packages ${unlicensed}, whose files are bundled: keep "
        "each package's license text in packaging/windows/licenses/<package> (decision 0038)")
endif()
