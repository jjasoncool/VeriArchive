# The device identity reader, veriarchive-device-identity.exe (decisions
# 0047 and 0052), started without administrator permission. Its manifest asks
# Windows for that permission; a program started with CreateProcess, as
# execute_process starts it, is then refused (ERROR_ELEVATION_REQUIRED),
# never asked about. __COMPAT_LAYER=RunAsInvoker has Windows start it as the
# test runs instead, so that what it does before it opens a device, and where
# a device cannot be opened, is tested without a question to anyone:
#   - arguments it does not take are refused (exit 2), nothing is written;
#   - a result file that is there already is refused (exit 3) and kept as it is;
#   - a result file that a junction on its path leads elsewhere is removed
#     again and refused (exit 3);
#   - for devices 63 and 62 (\\.\PhysicalDrive63 and 62), which no computer
#     here has, it writes the error open-failed of each (exit 1) to a new
#     result file.
# No device that is there is opened. Every file and folder here is sample data.
#   cmake -DREADER=<veriarchive-device-identity.exe> -DWORK=<scratch> -P device_identity_reader.cmake
cmake_minimum_required(VERSION 3.28)

foreach(variable READER WORK)
    if(NOT ${variable})
        message(FATAL_ERROR "${variable} must be set")
    endif()
endforeach()

file(REMOVE_RECURSE "${WORK}")
file(MAKE_DIRECTORY "${WORK}")
file(REAL_PATH "${WORK}" work)
file(TO_NATIVE_PATH "${work}" native_work)

function(run_reader expected_exit)
    execute_process(COMMAND "${CMAKE_COMMAND}" -E env __COMPAT_LAYER=RunAsInvoker "${READER}" ${ARGN}
        RESULT_VARIABLE status OUTPUT_VARIABLE output ERROR_VARIABLE output)
    if(NOT status STREQUAL "${expected_exit}")
        message(FATAL_ERROR "the device identity reader with ${ARGN} exited ${status}, not ${expected_exit}\n${output}")
    endif()
endfunction()

# Arguments it does not take: nothing is written. The result file comes
# first, then one to 64 device numbers, each once.
set(new_result "${native_work}\\SAMPLE-result.json")
run_reader(2)
run_reader(2 "${new_result}")
run_reader(2 63)
run_reader(2 63 "${new_result}")
run_reader(2 "${new_result}" 64)
run_reader(2 "${new_result}" 063)
run_reader(2 "${new_result}" +1)
run_reader(2 "${new_result}" 63 63)
run_reader(2 "${new_result}" "\\\\.\\PhysicalDrive1")
run_reader(2 "${new_result}" 63 extra)
run_reader(2 "SAMPLE-result.json" 63)
run_reader(2 "${native_work}\\SAMPLE-result.txt" 63)
run_reader(2 "${native_work}\\SAMPLE-result:stream.json" 63)
run_reader(2 "${native_work}\\..\\SAMPLE-result.json" 63)
file(GLOB written "${work}/*")
if(written)
    message(FATAL_ERROR "the device identity reader wrote ${written} for arguments it does not take")
endif()

# A result file that is there is kept as it is.
set(sample_text "Sample data: a file that is there before the reader runs.\n")
file(WRITE "${work}/SAMPLE-there.json" "${sample_text}")
run_reader(3 "${native_work}\\SAMPLE-there.json" 63)
file(READ "${work}/SAMPLE-there.json" kept)
if(NOT kept STREQUAL sample_text)
    message(FATAL_ERROR "the device identity reader wrote over a result file that was there:\n${kept}")
endif()

# A junction on the result file's path that leads elsewhere (a sample folder
# beside it): the file it made there is removed again, and no device is
# opened. Making a junction needs no administrator permission.
file(MAKE_DIRECTORY "${work}/SAMPLE-target")
execute_process(COMMAND cmd /c mklink /J "${native_work}\\SAMPLE-link" "${native_work}\\SAMPLE-target"
    RESULT_VARIABLE linked OUTPUT_QUIET ERROR_VARIABLE link_errors)
if(NOT linked EQUAL 0 OR NOT IS_DIRECTORY "${work}/SAMPLE-link")
    message(FATAL_ERROR "could not make the sample junction (exit ${linked}): ${link_errors}")
endif()
run_reader(3 "${native_work}\\SAMPLE-link\\SAMPLE-result.json" 63)
file(GLOB redirected "${work}/SAMPLE-target/*")
if(redirected)
    message(FATAL_ERROR "the device identity reader kept ${redirected}, which a junction led elsewhere")
endif()
# rmdir removes the junction alone, not what it leads to.
execute_process(COMMAND cmd /c rmdir "${native_work}\\SAMPLE-link" RESULT_VARIABLE unlinked)
if(NOT unlinked EQUAL 0 OR EXISTS "${work}/SAMPLE-link")
    message(FATAL_ERROR "could not remove the sample junction (exit ${unlinked})")
endif()

# Devices 63 and 62 are not there: the error of each, in the order given, in a
# new result file.
run_reader(1 "${new_result}" 63 62)
file(READ "${work}/SAMPLE-result.json" result)
if(NOT result MATCHES
   "^{\"devices\":\\[{\"code\":[0-9]+,\"error\":\"open-failed\",\"number\":63},{\"code\":[0-9]+,\"error\":\"open-failed\",\"number\":62}\\]}$")
    message(FATAL_ERROR "the device identity reader wrote, for devices that are not there:\n${result}")
endif()
file(REMOVE_RECURSE "${WORK}")
message(STATUS "the device identity reader refuses what it does not take and tells what it cannot open")
