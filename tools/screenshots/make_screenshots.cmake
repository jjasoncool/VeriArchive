# Makes the screenshots of the user guide (see README.md next to this file):
# builds sample data, registers it as two archives with veriarchive-cli, then
# runs veriarchive-gui in its screenshot mode, which tours the views and saves
# one PNG per view.
#
#   cmake -DCLI=<veriarchive-cli> -DGUI=<veriarchive-gui> -DWORK=<scratch dir>
#         -DOUT=<png folder> [-DLANG=zh-TW|en] -P make_screenshots.cmake
cmake_minimum_required(VERSION 3.28)

foreach(required CLI GUI WORK OUT)
    if(NOT ${required})
        message(FATAL_ERROR "${required} must be set (see tools/screenshots/README.md)")
    endif()
endforeach()
if(NOT LANG)
    set(LANG zh-TW)
endif()
get_filename_component(WORK "${WORK}" ABSOLUTE)
get_filename_component(OUT "${OUT}" ABSOLUTE)
string(FIND "${OUT}/" "${WORK}/" out_in_work)
if(out_in_work EQUAL 0)
    message(FATAL_ERROR "OUT must not be inside WORK: WORK is emptied on every run")
endif()

# A portable install (config.json next to the program) would use its own
# repository instead of the sample one, and the pictures would show, and the
# tour would change, the real archives.
get_filename_component(gui_folder "${GUI}" DIRECTORY)
if(EXISTS "${gui_folder}/config.json")
    message(FATAL_ERROR "${gui_folder}/config.json exists: that GUI uses its own repository. "
        "Use a build, or an installed copy without config.json.")
endif()

# WORK is emptied first. So that a mistyped path never costs real files, only
# an empty folder or one this script made (it leaves a marker file) is used.
set(marker "${WORK}/.veriarchive-screenshots")
if(EXISTS "${WORK}")
    file(GLOB existing LIST_DIRECTORIES true "${WORK}/*")
    if(existing AND NOT EXISTS "${marker}")
        message(FATAL_ERROR "${WORK} is not empty and was not made by this script; choose another WORK")
    endif()
    file(REMOVE_RECURSE "${WORK}")
endif()
file(WRITE "${marker}" "Sample data of the VeriArchive screenshots; replaced whenever they are made again.\n")

# Sample data, never real data: the pictures are published with the source.
# Archive A is the reference folder; B is a copy of it with one file changed,
# one deleted and one added, so the comparison has something to show.
set(reference_folder "${WORK}/原始資料")
set(copy_folder "${WORK}/備份資料")
file(WRITE "${reference_folder}/readme.txt" "VeriArchive sample data for the user guide.\n")
file(WRITE "${reference_folder}/會議紀錄.txt" "2026-10-01 專案會議\n- 確認備份流程\n")
file(WRITE "${reference_folder}/專案/計畫書.md" "# 計畫書\n\n範例內容。\n")
file(WRITE "${reference_folder}/專案/budget 預算.csv" "item,amount\n硬碟,3200\n")
file(WRITE "${reference_folder}/photos/2026 旅行/notes.txt" "Photos would be here.\n")
file(MAKE_DIRECTORY "${reference_folder}/空資料夾")
file(COPY "${reference_folder}/" DESTINATION "${copy_folder}")
file(WRITE "${copy_folder}/專案/budget 預算.csv" "item,amount\n硬碟,3200\n外接盒,650\n")
file(REMOVE "${copy_folder}/會議紀錄.txt")
file(WRITE "${copy_folder}/新增的檔案.txt" "Only in the copy.\n")

# The GUI reads the config file of the user config folder, which APPDATA
# (Windows) or XDG_CONFIG_HOME (Linux) moves into WORK. The CLI is given the
# same file with --config. VERIARCHIVE_REPOSITORY would override the
# repository pinned there, so neither program sees it.
file(TO_NATIVE_PATH "${WORK}" work_native)
if(CMAKE_HOST_WIN32)
    set(config "${WORK}/VeriArchive/config.json")
    set(config_folder_variable "APPDATA=${work_native}")
else()
    set(config "${WORK}/veriarchive/config.json")
    set(config_folder_variable "XDG_CONFIG_HOME=${work_native}")
endif()
file(WRITE "${config}" "{}\n")

function(run_cli)
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -E env --unset=VERIARCHIVE_REPOSITORY --unset=VERIARCHIVE_LANG
            "${CLI}" --config "${config}" ${ARGN}
        ENCODING UTF-8 # the CLI writes UTF-8 whatever the system code page
        RESULT_VARIABLE code
        OUTPUT_VARIABLE output
        ERROR_VARIABLE errors)
    if(NOT code EQUAL 0)
        message(FATAL_ERROR "veriarchive-cli ${ARGN}\nexit ${code}\n${output}\n${errors}")
    endif()
    set(last_output "${output}" PARENT_SCOPE)
endfunction()

run_cli(--repository "${WORK}/repository" repo init)
run_cli(register "${reference_folder}" --label ARCH-0001 --json)
string(JSON reference GET "${last_output}" archive_uuid)
run_cli(register "${copy_folder}" --label ARCH-0002 --copy-of "${reference}" --json)
string(JSON copy GET "${last_output}" archive_uuid)

set(pictures 01-settings.png 02-archive-list.png 03-register.png 04-verify.png 05-history.png
    06-compare.png 07-language.png)
file(MAKE_DIRECTORY "${OUT}")
foreach(picture IN LISTS pictures)
    file(REMOVE "${OUT}/${picture}")
endforeach()

# The window appears on the desktop for a minute while the tour runs.
file(TO_NATIVE_PATH "${reference_folder}" reference_folder_native)
message(STATUS "Taking the screenshots in ${LANG} ...")
execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env --unset=VERIARCHIVE_REPOSITORY --unset=VERIARCHIVE_GUI_SMOKE_TEST
        "${config_folder_variable}"
        "VERIARCHIVE_LANG=${LANG}"
        "VERIARCHIVE_GUI_SCREENSHOTS=${OUT}"
        "VERIARCHIVE_SCREENSHOT_REFERENCE=${reference}"
        "VERIARCHIVE_SCREENSHOT_COPY=${copy}"
        "VERIARCHIVE_SCREENSHOT_FOLDER=${reference_folder_native}"
        "${GUI}"
    ENCODING UTF-8
    RESULT_VARIABLE code
    OUTPUT_VARIABLE output
    ERROR_VARIABLE errors)
if(NOT code EQUAL 0)
    message(FATAL_ERROR "veriarchive-gui exit ${code}\n${output}\n${errors}")
endif()
if(errors)
    message(STATUS "veriarchive-gui wrote:\n${errors}")
endif()
foreach(picture IN LISTS pictures)
    if(NOT EXISTS "${OUT}/${picture}")
        message(FATAL_ERROR "veriarchive-gui did not save ${OUT}/${picture}")
    endif()
endforeach()
message(STATUS "Screenshots saved in ${OUT}")
