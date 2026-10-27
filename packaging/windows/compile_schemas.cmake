# Compiles the GSettings schemas of a folder (decision 0038). Included by
# install_runtime.cmake, which packages them, and by check_package.cmake.
#
# glib-compile-schemas compiles every *.gschema.xml of a folder into
# gschemas.compiled. It cannot open a path of 260 characters or more, as in
# the folder where cpack stages the package, four folders below a build
# folder; so it compiles a copy of the schema files in a short folder in TEMP,
# and gschemas.compiled is copied back. A file it cannot open or parse it
# names on its output and leaves out, and it still exits 0; it prints nothing
# when it compiled every file, so any message it prints counts as a failure.
#
#   veriarchive_compile_schemas(<glib-compile-schemas> <folder> <result variable>)
#
# sets the result variable to "" when every schema compiled, else to what
# went wrong.
function(veriarchive_compile_schemas compiler folder result)
    set(short "")
    foreach(variable TEMP TMP)
        if(short STREQUAL "" AND NOT "$ENV{${variable}}" STREQUAL "")
            file(TO_CMAKE_PATH "$ENV{${variable}}" short)
        endif()
    endforeach()
    if(short STREQUAL "")
        set(short "${folder}") # no TEMP: compiled where they are
    else()
        # One folder per target folder, so installs into different prefixes
        # do not meet.
        string(SHA256 key "${folder}")
        string(SUBSTRING "${key}" 0 16 key)
        set(short "${short}/veriarchive-schemas-${key}")
        file(REMOVE_RECURSE "${short}")
        file(MAKE_DIRECTORY "${short}")
        file(GLOB schemas "${folder}/*.gschema.xml")
        if(schemas)
            file(COPY ${schemas} DESTINATION "${short}")
        endif()
    endif()
    execute_process(COMMAND "${compiler}" "${short}"
        ENCODING UTF-8
        RESULT_VARIABLE exit_code OUTPUT_VARIABLE messages ERROR_VARIABLE messages)
    string(STRIP "${messages}" messages)
    set(problem "")
    if(NOT exit_code EQUAL 0 OR NOT messages STREQUAL "")
        set(problem "glib-compile-schemas of the schemas of ${folder} (exit ${exit_code}):\n${messages}")
    elseif(NOT short STREQUAL folder)
        file(COPY "${short}/gschemas.compiled" DESTINATION "${folder}")
    endif()
    if(NOT short STREQUAL folder)
        file(REMOVE_RECURSE "${short}")
    endif()
    set(${result} "${problem}" PARENT_SCOPE)
endfunction()
