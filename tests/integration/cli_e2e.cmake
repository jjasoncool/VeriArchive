# End-to-end test of veriarchive-cli (R-120 integration tests): registers a
# real directory tree, verifies it, damages it, and exercises history,
# export/import, the sha256sum export and the interface language (decision 0017).
#
#   cmake -DCLI=<veriarchive-cli> -DWORK=<scratch dir> -P cli_e2e.cmake
cmake_minimum_required(VERSION 3.28)

if(NOT CLI OR NOT WORK)
    message(FATAL_ERROR "CLI and WORK must be set")
endif()
file(REMOVE_RECURSE "${WORK}")
file(MAKE_DIRECTORY "${WORK}")
# The config chooses Traditional Chinese; --json output stays English anyway.
file(WRITE "${WORK}/config.json" "{\"language\": \"zh-TW\"}\n")
set(GLOBAL --config "${WORK}/config.json")

# Runs the CLI and checks its exit code against the allowed list.
function(run_cli expected_codes)
    execute_process(
        COMMAND "${CLI}" ${GLOBAL} ${ARGN}
        ENCODING UTF-8 # the CLI writes UTF-8 whatever the system code page
        RESULT_VARIABLE code
        OUTPUT_VARIABLE output
        ERROR_VARIABLE errors)
    list(FIND expected_codes "${code}" found)
    if(found EQUAL -1)
        message(FATAL_ERROR "veriarchive-cli ${ARGN}\nexit ${code}, expected one of ${expected_codes}\n${output}\n${errors}")
    endif()
    set(last_output "${output}" PARENT_SCOPE)
endfunction()

# A tree with CJK and emoji names, an empty directory and a nested file.
set(ROOT "${WORK}/archive")
file(WRITE "${ROOT}/readme.txt" "hello\n")
file(WRITE "${ROOT}/中文/資料.txt" "中文內容\n")
file(WRITE "${ROOT}/日本語/ファイル.txt" "日本語\n")
file(WRITE "${ROOT}/emoji 😀/nested/deep.bin" "0123456789")
file(MAKE_DIRECTORY "${ROOT}/empty")

run_cli("0" --repository "${WORK}/repo" repo init)
run_cli("0" register "${ROOT}" --json)
string(JSON archive_uuid GET "${last_output}" archive_uuid)
string(JSON files GET "${last_output}" counts files)
if(NOT files EQUAL 4)
    message(FATAL_ERROR "expected 4 files, got ${files}")
endif()

# Registering the same root again needs an operator decision (R-97).
run_cli("12" register "${ROOT}")

# A clean verify passes; MATCH_WEAK (no serial, e.g. in containers) warns.
run_cli("0;2" verify --full "${ROOT}" --json)
string(JSON result GET "${last_output}" result)
if(NOT result MATCHES "^PASSED")
    message(FATAL_ERROR "clean verify returned ${result}")
endif()
run_cli("0;2" verify --full "${archive_uuid}")
if(NOT last_output MATCHES "完整驗證")
    message(FATAL_ERROR "text output does not follow the configured language:\n${last_output}")
endif()
run_cli("0;2" --lang en verify --full "${archive_uuid}")
if(NOT last_output MATCHES "FULL VERIFY")
    message(FATAL_ERROR "--lang en does not override the configured language:\n${last_output}")
endif()
run_cli("10" --lang fr archive list)
# Pinning the repository kept the language in the config file.
file(READ "${WORK}/config.json" config)
string(JSON language GET "${config}" language)
if(NOT language STREQUAL "zh-TW")
    message(FATAL_ERROR "the config file lost its language: ${config}")
endif()

# R-67: the sha256sum export lists every file.
run_cli("0" export-sha256sums "${archive_uuid}" --out "${WORK}/sums.txt")
file(READ "${WORK}/sums.txt" sums)
string(REGEX MATCHALL "\n" newlines "${sums}")
list(LENGTH newlines sum_lines)
if(NOT sum_lines EQUAL 4)
    message(FATAL_ERROR "expected 4 sha256sum lines, got ${sum_lines}")
endif()

# Damage: one changed byte, one deleted file, one new file -> FAILED (exit 3).
file(WRITE "${ROOT}/readme.txt" "hellp\n")
file(REMOVE "${ROOT}/日本語/ファイル.txt")
file(WRITE "${ROOT}/added.txt" "new\n")
run_cli("3" verify --full "${ROOT}" --json)
string(JSON item_count LENGTH "${last_output}" items)
if(NOT item_count EQUAL 3)
    message(FATAL_ERROR "expected 3 reported items, got ${item_count}:\n${last_output}")
endif()

# History is chained and intact (R-18); listing works.
run_cli("0" repo verify-history)
run_cli("0" archive list)
run_cli("0" history "${archive_uuid}")

# Export -> import into a second repository (R-87).
run_cli("0" export "${archive_uuid}" --out "${WORK}/export")
file(WRITE "${WORK}/config2.json" "{}\n")
set(GLOBAL --config "${WORK}/config2.json")
run_cli("0" --repository "${WORK}/repo2" repo init)
run_cli("0" import "${WORK}/export/r0001.vam")
run_cli("0" import "${WORK}/export/r0001.vam")

message(STATUS "veriarchive-cli end-to-end test passed")
