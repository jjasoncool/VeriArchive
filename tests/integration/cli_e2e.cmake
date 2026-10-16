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
    set(last_errors "${errors}" PARENT_SCOPE)
endfunction()

# The root_hint in the header (line 1) of the manifest at `manifest`.
function(read_root_hint manifest out)
    file(TO_CMAKE_PATH "${manifest}" manifest)
    file(READ "${manifest}" manifest_start LIMIT 65536)
    string(FIND "${manifest_start}" "\n" header_end)
    string(SUBSTRING "${manifest_start}" 0 ${header_end} header)
    string(JSON root_hint GET "${header}" root_hint)
    set(${out} "${root_hint}" PARENT_SCOPE)
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

# A malformed label is a usage error, not an operator decision.
run_cli("10" register "${ROOT}" --label OS-1)

# Registering the same root again needs an operator decision (R-97).
run_cli("12" register "${ROOT}")

# A clean verify passes; MATCH_WEAK (no serial, e.g. in containers) warns.
run_cli("0;2" verify --full "${ROOT}" --json)
string(JSON result GET "${last_output}" result)
if(NOT result MATCHES "^PASSED")
    message(FATAL_ERROR "clean verify returned ${result}")
endif()
string(JSON clean_root GET "${last_output}" root)
# A root typed with a separator at its end, as shells complete a folder name,
# is that folder: it verifies, and the run names the folder as above.
run_cli("0;2" verify --full "${ROOT}/" --json)
string(JSON result GET "${last_output}" result)
string(JSON separator_root GET "${last_output}" root)
if(NOT result MATCHES "^PASSED" OR NOT separator_root STREQUAL clean_root)
    message(FATAL_ERROR "verify of ${ROOT}/ returned ${result} at ${separator_root}, expected ${clean_root}")
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
# Command-line errors follow --lang and the config file too.
run_cli("10" --lang en archive list --label)
if(NOT last_errors STREQUAL "error: option --label needs a value\n")
    message(FATAL_ERROR "a parse error ignores --lang en:\n${last_errors}")
endif()
run_cli("10" archive list --label)
if(NOT last_errors MATCHES "選項 --label 需要一個值")
    message(FATAL_ERROR "a parse error ignores the configured language:\n${last_errors}")
endif()
# A path the user typed is shown as typed, even when it looks like a status word.
run_cli("10" register Missing)
if(NOT last_errors MATCHES "不是資料夾：Missing")
    message(FATAL_ERROR "a typed path was translated:\n${last_errors}")
endif()
# Pinning the repository kept the language in the config file.
file(READ "${WORK}/config.json" config)
string(JSON language GET "${config}" language)
if(NOT language STREQUAL "zh-TW")
    message(FATAL_ERROR "the config file lost its language: ${config}")
endif()

# R-67: the sha256sum export lists every file. Like `archive show`, the export
# commands accept the first 8 or more characters of the uuid.
string(SUBSTRING "${archive_uuid}" 0 8 archive_prefix)
run_cli("10" export-sha256sums "abc" --out "${WORK}/sums.txt")
run_cli("0" export-sha256sums "${archive_prefix}" --out "${WORK}/sums.txt")
file(READ "${WORK}/sums.txt" sums)
string(REGEX MATCHALL "\n" newlines "${sums}")
list(LENGTH newlines sum_lines)
if(NOT sum_lines EQUAL 4)
    message(FATAL_ERROR "expected 4 sha256sum lines, got ${sum_lines}")
endif()

# A backup: a copy of the tree registered as an archive of its own. Another
# root on the same volume is allowed while the roots do not overlap (decision
# 0001). Comparing the two archives reads only their manifests (decision 0020).
# Its folder is typed with a separator at its end, which the archive's
# root_hint does not keep.
set(COPY "${WORK}/copy")
file(COPY "${ROOT}/" DESTINATION "${COPY}")
run_cli("0" register "${COPY}/" --json)
string(JSON copy_uuid GET "${last_output}" archive_uuid)
string(JSON copy_manifest GET "${last_output}" manifest)
read_root_hint("${copy_manifest}" copy_hint)
if(copy_hint MATCHES "[/\\\\]$")
    message(FATAL_ERROR "root_hint of a registration of ${COPY}/ is ${copy_hint}")
endif()
string(SUBSTRING "${copy_uuid}" 0 8 copy_prefix)
run_cli("0" --lang en archive compare "${archive_uuid}" "${copy_uuid}")
if(NOT last_output MATCHES "^COMPARE IDENTICAL  [^\n]*\\(${archive_uuid}\\) r1  vs  [^\n]*\\(${copy_uuid}\\) r1\n")
    message(FATAL_ERROR "compare of a copy is not IDENTICAL:\n${last_output}")
endif()
run_cli("0" archive compare "${archive_uuid}" "${copy_uuid}" --json)
string(JSON result GET "${last_output}" result)
string(JSON same GET "${last_output}" same)
string(JSON difference_count LENGTH "${last_output}" differences)
if(NOT result STREQUAL "IDENTICAL" OR NOT same EQUAL 5 OR NOT difference_count EQUAL 0)
    message(FATAL_ERROR "compare --json of a copy:\n${last_output}")
endif()

# One file changed, one deleted, one added in the copy and registered again:
# the comparison lists each, and DIFFERENT exits with 3.
file(WRITE "${COPY}/readme.txt" "hello, world\n")
file(REMOVE "${COPY}/中文/資料.txt")
file(WRITE "${COPY}/extra.txt" "extra\n")
run_cli("0" register "${COPY}" --new-revision)
# Both archives named by their first 8 characters, like `archive show`.
run_cli("3" archive compare "${archive_prefix}" "${copy_prefix}" --json)
string(JSON result GET "${last_output}" result)
string(JSON revision_b GET "${last_output}" archive_b revision)
string(JSON modified GET "${last_output}" counts Modified)
string(JSON missing GET "${last_output}" counts Missing)
string(JSON added GET "${last_output}" counts New)
string(JSON difference_count LENGTH "${last_output}" differences)
if(NOT result STREQUAL "DIFFERENT" OR NOT revision_b EQUAL 2 OR NOT modified EQUAL 1 OR NOT missing EQUAL 1
        OR NOT added EQUAL 1 OR NOT difference_count EQUAL 3)
    message(FATAL_ERROR "compare --json after changes in the copy:\n${last_output}")
endif()
run_cli("3" --lang en archive compare "${archive_prefix}" "${copy_prefix}")
if(NOT last_output MATCHES "^COMPARE DIFFERENT  "
        OR NOT last_output MATCHES "\n  Modified  readme\\.txt  \\(size [0-9]+ -> [0-9]+\\)\n"
        OR NOT last_output MATCHES "\n  Missing  中文/資料\\.txt\n"
        OR NOT last_output MATCHES "\n  New  extra\\.txt\n")
    message(FATAL_ERROR "compare text after changes in the copy:\n${last_output}")
endif()
# Two archives are needed.
run_cli("10" archive compare "${archive_uuid}")

# An unfinished registration given up on purpose (decision 0025). The first
# registration of a folder fails before it is recorded: its manifest cannot be
# written while a file stands where the manifests folder belongs (sample
# data). Its plan and saved progress stay, so the folder takes no other
# registration until the registration is resumed or abandoned.
set(BROKEN "${WORK}/abandoned")
file(WRITE "${BROKEN}/a.txt" "abandoned sample a\n")
file(WRITE "${BROKEN}/b.txt" "abandoned sample b\n")
file(RENAME "${WORK}/repo/manifests" "${WORK}/repo/manifests.away")
file(WRITE "${WORK}/repo/manifests" "not a folder (sample data)\n")
run_cli("11" register "${BROKEN}")
file(REMOVE "${WORK}/repo/manifests")
file(RENAME "${WORK}/repo/manifests.away" "${WORK}/repo/manifests")
run_cli("12" register "${BROKEN}")
set(refusal "${last_errors}")
# Typed with a separator at its end, the folder has the same unfinished registration.
run_cli("12" register "${BROKEN}/")
if(NOT last_errors STREQUAL refusal)
    message(FATAL_ERROR "register of ${BROKEN}/ while its registration is unfinished:\n${last_errors}")
endif()
run_cli("0" archive list --json)
string(JSON archive_count LENGTH "${last_output}" archives)
math(EXPR last_archive "${archive_count} - 1")
set(broken_uuid "")
foreach(index RANGE ${last_archive})
    string(JSON status GET "${last_output}" archives ${index} status)
    if(status STREQUAL "REGISTRATION_INCOMPLETE")
        string(JSON broken_uuid GET "${last_output}" archives ${index} archive_uuid)
    endif()
endforeach()
if(broken_uuid STREQUAL "")
    message(FATAL_ERROR "the failed registration is not listed:\n${last_output}")
endif()
# The refusal names both ways out: --resume, and abandoning it.
if(NOT refusal MATCHES "--resume" OR NOT refusal MATCHES "archive abandon-registration ${broken_uuid}")
    message(FATAL_ERROR "register of a folder whose registration is unfinished:\n${refusal}")
endif()
string(SUBSTRING "${broken_uuid}" 0 8 broken_prefix)
# Without --confirm it says what would be lost, and changes nothing (exit 12).
run_cli("12" --lang en archive abandon-registration "${broken_prefix}")
if(NOT last_output MATCHES "Unfinished registration of ARCH-[0-9]+ \\(${broken_uuid}\\), revision 1\n"
        OR NOT last_output MATCHES "\n  saved progress: 2 files, [0-9]+ bytes\n"
        OR NOT last_output MATCHES "The archive has no revision yet"
        OR NOT last_errors MATCHES "--confirm")
    message(FATAL_ERROR "abandon-registration without --confirm:\n${last_output}\n${last_errors}")
endif()
run_cli("12" archive abandon-registration "${broken_prefix}" --json)
string(JSON first_registration GET "${last_output}" registration first_registration)
string(JSON saved_files GET "${last_output}" registration saved_progress files)
if(NOT first_registration OR NOT saved_files EQUAL 2)
    message(FATAL_ERROR "abandon-registration --json without --confirm:\n${last_output}")
endif()
run_cli("0" archive abandon-registration "${broken_prefix}" --confirm --json)
string(JSON archive_removed GET "${last_output}" archive_removed)
string(JSON saved_files GET "${last_output}" saved_progress files)
if(NOT archive_removed OR NOT saved_files EQUAL 2)
    message(FATAL_ERROR "abandon-registration --confirm:\n${last_output}")
endif()
run_cli("0" archive list --json)
if(last_output MATCHES "${broken_uuid}")
    message(FATAL_ERROR "the abandoned registration's archive is still listed:\n${last_output}")
endif()
# The folder takes a registration again, which leaves nothing to abandon.
run_cli("0" register "${BROKEN}" --json)
string(JSON registered_uuid GET "${last_output}" archive_uuid)
run_cli("12" archive abandon-registration "${registered_uuid}")
if(NOT last_errors MATCHES "沒有未完成的註冊")
    message(FATAL_ERROR "abandon-registration of a registered archive:\n${last_errors}")
endif()
run_cli("10" archive abandon-registration "${broken_prefix}")

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
# People see local time with its offset from UTC; --json keeps UTC.
if(NOT last_output MATCHES "[0-9]+-[0-9]+-[0-9]+ [0-9]+:[0-9]+:[0-9]+（UTC")
    message(FATAL_ERROR "archive list does not show local time with its offset:\n${last_output}")
endif()
run_cli("0" --lang en history "${archive_uuid}")
if(NOT last_output MATCHES "[0-9]+-[0-9]+-[0-9]+ [0-9]+:[0-9]+:[0-9]+ \\(UTC")
    message(FATAL_ERROR "history does not show local time with its offset:\n${last_output}")
endif()
run_cli("0" history "${archive_uuid}" --json)
string(JSON finished GET "${last_output}" runs 0 finished)
if(NOT finished MATCHES "^[0-9]+-[0-9]+-[0-9]+T[0-9]+:[0-9]+:[0-9]+Z$")
    message(FATAL_ERROR "--json history is not in UTC: ${finished}")
endif()

# Decision 0027 (Windows): a folder reached through a drive letter made with
# subst is registered and verified through the letter, and recorded by its
# real path on its real volume. The probe takes a free letter from S: to V:
# and removes it right after the two commands, before anything is checked;
# then `subst` must no longer list it.
if(CMAKE_HOST_WIN32)
    set(SUBSTITUTED "${WORK}/subst")
    file(WRITE "${SUBSTITUTED}/photos/sample.txt" "subst sample\n")
    file(TO_NATIVE_PATH "${SUBSTITUTED}" substituted_native)
    # The CLI's exit code, stdout and stderr in <prefix>_code, _output and
    # _errors; never stops the script, so the letter is always removed.
    function(probe_cli prefix)
        execute_process(COMMAND "${CLI}" ${GLOBAL} ${ARGN}
            ENCODING UTF-8 TIMEOUT 120
            RESULT_VARIABLE code OUTPUT_VARIABLE output ERROR_VARIABLE errors)
        set(${prefix}_code "${code}" PARENT_SCOPE)
        set(${prefix}_output "${output}" PARENT_SCOPE)
        set(${prefix}_errors "${errors}" PARENT_SCOPE)
    endfunction()
    set(letter "")
    foreach(candidate S T U V)
        if(NOT EXISTS "${candidate}:/")
            execute_process(COMMAND subst "${candidate}:" "${substituted_native}"
                RESULT_VARIABLE made OUTPUT_QUIET ERROR_QUIET)
            if(made EQUAL 0)
                set(letter "${candidate}")
                break()
            endif()
        endif()
    endforeach()
    if(letter STREQUAL "")
        message(STATUS "subst probe skipped: no letter from S: to V: could be made with subst")
    else()
        probe_cli(through_register register "${letter}:\\photos" --json)
        probe_cli(through_verify verify --full "${letter}:\\photos" --json)
        execute_process(COMMAND subst "${letter}:" /D RESULT_VARIABLE removed OUTPUT_QUIET ERROR_QUIET)
        execute_process(COMMAND subst OUTPUT_VARIABLE substs ERROR_QUIET)
        string(FIND "${substs}" "${letter}:\\: =>" left_behind)
        if(NOT removed EQUAL 0 OR NOT left_behind EQUAL -1)
            message(FATAL_ERROR "the subst probe could not remove ${letter}: (exit ${removed}):\n${substs}")
        endif()

        if(NOT through_register_code EQUAL 0)
            message(FATAL_ERROR "register through ${letter}: exit ${through_register_code}\n"
                "${through_register_output}\n${through_register_errors}")
        endif()
        string(JSON subst_uuid GET "${through_register_output}" archive_uuid)
        string(JSON subst_manifest GET "${through_register_output}" manifest)
        if(NOT through_verify_code MATCHES "^[02]$")
            message(FATAL_ERROR "verify through ${letter}: exit ${through_verify_code}\n"
                "${through_verify_output}\n${through_verify_errors}")
        endif()
        string(JSON verified_uuid GET "${through_verify_output}" archive_uuid)
        string(JSON verified_root GET "${through_verify_output}" root)
        string(TOLOWER "${verified_root}" verified_root)
        string(TOLOWER "${substituted_native}\\photos" real_root)
        if(NOT verified_uuid STREQUAL subst_uuid OR NOT verified_root STREQUAL real_root)
            message(FATAL_ERROR "verify through ${letter}: found ${verified_uuid} at ${verified_root}, "
                "expected ${subst_uuid} at ${real_root}")
        endif()
        # The manifest's root_hint is the real path, not the letter.
        file(TO_CMAKE_PATH "${subst_manifest}" subst_manifest)
        file(READ "${subst_manifest}" manifest_start LIMIT 65536)
        string(FIND "${manifest_start}" "\n" header_end)
        string(SUBSTRING "${manifest_start}" 0 ${header_end} header)
        string(JSON root_hint GET "${header}" root_hint)
        string(TOLOWER "${root_hint}" root_hint)
        if(NOT root_hint STREQUAL real_root)
            message(FATAL_ERROR "root_hint of a registration through ${letter}: is ${root_hint}")
        endif()
        # The real volume: the root's place on it, and the same archive by the real path.
        get_filename_component(work_name "${WORK}" NAME)
        run_cli("0" archive show "${subst_uuid}" --json)
        string(JSON subst_subpath GET "${last_output}" root_subpath)
        if(NOT subst_subpath MATCHES "/${work_name}/subst/photos$")
            message(FATAL_ERROR "root_subpath of a registration through ${letter}: is ${subst_subpath}")
        endif()
        run_cli("0;2" verify --full "${SUBSTITUTED}/photos" --json)
        string(JSON verified_uuid GET "${last_output}" archive_uuid)
        if(NOT verified_uuid STREQUAL subst_uuid)
            message(FATAL_ERROR "the real path of a folder registered through ${letter}: is another archive")
        endif()
        run_cli("12" register "${SUBSTITUTED}/photos")
    endif()
endif()

# Export -> import into a second repository (R-87).
run_cli("0" export "${archive_prefix}" --out "${WORK}/export")
file(WRITE "${WORK}/config2.json" "{}\n")
set(GLOBAL --config "${WORK}/config2.json")
run_cli("0" --repository "${WORK}/repo2" repo init)
run_cli("0" import "${WORK}/export/r0001.vam")
run_cli("0" import "${WORK}/export/r0001.vam")

message(STATUS "veriarchive-cli end-to-end test passed")
