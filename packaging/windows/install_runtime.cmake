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
#   VERIARCHIVE_MINGW_PREFIX  e.g. C:/msys64/ucrt64
#   VERIARCHIVE_OBJDUMP       objdump used to read DLL imports
cmake_minimum_required(VERSION 3.28)
if(POLICY CMP0207)
    cmake_policy(SET CMP0207 NEW) # normalise paths before matching exclude patterns
endif()

include("${CMAKE_CURRENT_LIST_DIR}/desktop_dlls.cmake")

set(prefix "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}")
set(mingw "${VERIARCHIVE_MINGW_PREFIX}")
set(loader_dir "lib/gdk-pixbuf-2.0/2.10.0")

# 1. Every DLL the executables and the image loaders import, from the MinGW tree.
set(CMAKE_GET_RUNTIME_DEPENDENCIES_PLATFORM "windows+pe")
set(CMAKE_GET_RUNTIME_DEPENDENCIES_TOOL "objdump")
set(CMAKE_GET_RUNTIME_DEPENDENCIES_COMMAND "${VERIARCHIVE_OBJDUMP}")
file(GLOB executables "${prefix}/bin/*.exe")
file(GLOB loaders "${mingw}/${loader_dir}/loaders/*.dll")
file(GET_RUNTIME_DEPENDENCIES
    EXECUTABLES ${executables}
    MODULES ${loaders}
    RESOLVED_DEPENDENCIES_VAR resolved
    UNRESOLVED_DEPENDENCIES_VAR unresolved
    DIRECTORIES "${mingw}/bin"
    PRE_EXCLUDE_REGEXES "^api-ms-" "^ext-ms-"
    POST_EXCLUDE_REGEXES "^[A-Za-z]:[/\\\\][Ww][Ii][Nn][Dd][Oo][Ww][Ss][/\\\\]")

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

# 3. GTK settings schemas, compiled for this package only.
file(GLOB schemas "${mingw}/share/glib-2.0/schemas/org.gtk.gtk4.*.gschema.xml")
file(INSTALL ${schemas} DESTINATION "${prefix}/share/glib-2.0/schemas")
execute_process(
    COMMAND "${mingw}/bin/glib-compile-schemas.exe" "${prefix}/share/glib-2.0/schemas"
    RESULT_VARIABLE compiled)
if(NOT compiled EQUAL 0)
    message(FATAL_ERROR "glib-compile-schemas failed")
endif()

# 4. Icon themes used by GTK widgets.
file(INSTALL "${mingw}/share/icons/Adwaita" "${mingw}/share/icons/hicolor"
    DESTINATION "${prefix}/share/icons")

# 5. Traditional Chinese texts of GTK's own widgets and dialogs (decision 0017);
# GTK finds them under share/locale next to bin.
foreach(domain gtk40 glib20 gdk-pixbuf)
    set(catalog "${mingw}/share/locale/zh_TW/LC_MESSAGES/${domain}.mo")
    if(EXISTS "${catalog}")
        file(INSTALL "${catalog}" DESTINATION "${prefix}/share/locale/zh_TW/LC_MESSAGES")
    endif()
endforeach()

# 6. Licenses (LGPL notices, R-121): one folder per MSYS2 package that owns a
# bundled file, plus a list of which file came from which package.
get_filename_component(msys_root "${mingw}/.." ABSOLUTE)
find_program(pacman pacman PATHS "${msys_root}/usr/bin" NO_DEFAULT_PATH)
set(owned_paths "")
foreach(file IN LISTS bundled loaders)
    file(RELATIVE_PATH inside "${msys_root}" "${file}")
    list(APPEND owned_paths "/${inside}")
endforeach()
# One pacman call for all files: "<path> is owned by <package> <version>".
set(owner_report "")
if(pacman)
    execute_process(COMMAND "${pacman}" -Qo ${owned_paths}
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
    list(APPEND owners "${package}")
endforeach()
foreach(package glib2 gtk4)
    list(APPEND owners "${package}") # schemas and icon themes
endforeach()
list(APPEND owners adwaita-icon-theme hicolor-icon-theme)
list(REMOVE_DUPLICATES owners)
foreach(package IN LISTS owners)
    if(EXISTS "${mingw}/share/licenses/${package}")
        file(INSTALL "${mingw}/share/licenses/${package}" DESTINATION "${prefix}/share/licenses")
    endif()
endforeach()
file(WRITE "${prefix}/share/licenses/THIRD-PARTY.txt" "${listing}")
