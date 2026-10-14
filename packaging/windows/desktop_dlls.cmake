# DLLs that every desktop edition of Windows has in System32, but that Windows
# Server Core (the GitLab Windows runners' containers) leaves out. GTK4 imports
# them, so they are never bundled; on a build machine without them the package
# is still built, and only the GUI cannot start there.
#
# Included by install_runtime.cmake and check_package.cmake.
set(VERIARCHIVE_DESKTOP_DLLS opengl32.dll glu32.dll dwmapi.dll uxtheme.dll)

# True when `dll` is missing from this Windows but belongs to every desktop one.
function(veriarchive_desktop_only dll result)
    string(TOLOWER "${dll}" lower)
    if(lower IN_LIST VERIARCHIVE_DESKTOP_DLLS AND NOT EXISTS "$ENV{SystemRoot}/System32/${dll}")
        set(${result} TRUE PARENT_SCOPE)
    else()
        set(${result} FALSE PARENT_SCOPE)
    endif()
endfunction()
