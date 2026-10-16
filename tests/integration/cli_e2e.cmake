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

# An unfinished registration given up on purpose (decision 0025), in a
# repository of its own that has recorded no revision yet: a registration
# reads the manifest of every recorded revision before it begins (R-14). The
# first registration of a folder fails before it is recorded: its manifest
# cannot be written while a file stands where the manifests folder belongs
# (sample data). Its plan and saved progress stay, so the folder takes no
# other registration until the registration is resumed or abandoned.
file(WRITE "${WORK}/config3.json" "{\"language\": \"zh-TW\"}\n")
set(GLOBAL --config "${WORK}/config3.json")
run_cli("0" --repository "${WORK}/repo3" repo init)
set(BROKEN "${WORK}/abandoned")
file(WRITE "${BROKEN}/a.txt" "abandoned sample a\n")
file(WRITE "${BROKEN}/b.txt" "abandoned sample b\n")
file(RENAME "${WORK}/repo3/manifests" "${WORK}/repo3/manifests.away")
file(WRITE "${WORK}/repo3/manifests" "not a folder (sample data)\n")
run_cli("11" register "${BROKEN}")
file(REMOVE "${WORK}/repo3/manifests")
file(RENAME "${WORK}/repo3/manifests.away" "${WORK}/repo3/manifests")
run_cli("12" register "${BROKEN}")
set(refusal "${last_errors}")
# Typed with a separator at its end, the folder has the same unfinished registration.
run_cli("12" register "${BROKEN}/")
if(NOT last_errors STREQUAL refusal)
    message(FATAL_ERROR "register of ${BROKEN}/ while its registration is unfinished:\n${last_errors}")
endif()
# Rebuilding the index keeps its saved progress (decision 0026): the abandon
# below still finds the 2 files it saved.
run_cli("0" repo rebuild-index --json)
string(JSON kept GET "${last_output}" saved_progress_kept)
if(NOT kept EQUAL 1)
    message(FATAL_ERROR "rebuild-index and an unfinished registration:\n${last_output}")
endif()
run_cli("0" archive list --json)
string(JSON archive_count LENGTH "${last_output}" archives)
math(EXPR last_archive "${archive_count} - 1")
set(broken_uuid "")
foreach(index RANGE ${last_archive})
    string(JSON status GET "${last_output}" archives ${index} status)
    if(status STREQUAL "REGISTRATION_INCOMPLETE")
        string(JSON broken_uuid GET "${last_output}" archives ${index} archive_uuid)
        string(JSON broken_state GET "${last_output}" archives ${index} unfinished_registration state)
        string(JSON broken_saved GET "${last_output}" archives ${index} unfinished_registration files_done)
    endif()
endforeach()
if(broken_uuid STREQUAL "")
    message(FATAL_ERROR "the failed registration is not listed:\n${last_output}")
endif()
# The unfinished registration in --json, and as one line under its archive
# in the text output (decision 0026).
if(NOT broken_state STREQUAL "stopped" OR NOT broken_saved EQUAL 2)
    message(FATAL_ERROR "archive list --json of an unfinished registration:\n${last_output}")
endif()
run_cli("0" --lang en archive list)
if(NOT last_output MATCHES "\n    Unfinished registration of revision 1 from \"[^\n]*abandoned\": [^\n]*2 of 2 files; stopped; it can be resumed\n")
    message(FATAL_ERROR "archive list of an unfinished registration:\n${last_output}")
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
set(GLOBAL --config "${WORK}/config.json")

# A relocation resumes (decision 0026). The archive of a folder is moved to a
# copy in another folder (sample data). The relocation reads the copy, but its
# manifest cannot take its name while a folder stands there (sample data): it
# stops (exit 11), keeping what it matched, and says so with the way on, in
# either language. Started again without --resume it is refused, naming both
# ways out; `archive show --json` shows it stopped with every file matched;
# --resume records it. A second one is abandoned.
set(MOVE_FROM "${WORK}/move-from")
set(MOVE_TO "${WORK}/move-to")
file(WRITE "${MOVE_FROM}/a.txt" "relocation sample a\n")
file(WRITE "${MOVE_FROM}/sub/b.txt" "relocation sample b\n")
run_cli("0" register "${MOVE_FROM}" --json)
string(JSON moving_uuid GET "${last_output}" archive_uuid)
string(SUBSTRING "${moving_uuid}" 0 8 moving_prefix)
file(COPY "${MOVE_FROM}/" DESTINATION "${MOVE_TO}")
set(BLOCKER "${WORK}/repo/manifests/${moving_uuid}/r0002.vam")
file(MAKE_DIRECTORY "${BLOCKER}")
run_cli("11" archive relocate "${moving_prefix}" "${MOVE_TO}")
if(NOT last_errors MATCHES "無法記錄搬移：.+；搬移保留其已儲存的進度：請以 `archive relocate ${moving_uuid} <root_path> --resume` 繼續")
    message(FATAL_ERROR "relocate whose record cannot be written:\n${last_errors}")
endif()
file(REMOVE_RECURSE "${BLOCKER}")
run_cli("12" --lang en archive relocate "${moving_prefix}" "${MOVE_TO}")
if(NOT last_errors MATCHES "--resume" OR NOT last_errors MATCHES "archive abandon-relocation ${moving_uuid}")
    message(FATAL_ERROR "relocate while a relocation of the archive is unfinished:\n${last_errors}")
endif()
run_cli("0" archive show "${moving_prefix}" --json)
string(JSON relocation_state GET "${last_output}" unfinished_relocation state)
string(JSON matched GET "${last_output}" unfinished_relocation files_done)
string(JSON to_match GET "${last_output}" unfinished_relocation files_total)
if(NOT relocation_state STREQUAL "stopped" OR NOT matched EQUAL 2 OR NOT to_match EQUAL 2)
    message(FATAL_ERROR "archive show --json of an unfinished relocation:\n${last_output}")
endif()
run_cli("0" archive list --json)
if(NOT last_output MATCHES "\"unfinished_relocation\"")
    message(FATAL_ERROR "archive list --json leaves out the unfinished relocation:\n${last_output}")
endif()
# One line under the archive in the text output, as --json judges it (decision 0026).
run_cli("0" archive show "${moving_prefix}")
if(NOT last_output MATCHES "\n  未完成的搬移（版次 1，至「[^\n]*move-to」）：([0-9]+) B / ([0-9]+) B（100%）・2 / 2 個檔案；已停止；可以繼續\n"
        OR NOT CMAKE_MATCH_1 STREQUAL CMAKE_MATCH_2)
    message(FATAL_ERROR "archive show of an unfinished relocation:\n${last_output}")
endif()
run_cli("0" --lang en archive list)
if(NOT last_output MATCHES "\n    Unfinished relocation of revision 1 to \"[^\n]*move-to\": [0-9]+ B of [0-9]+ B [(]100%[)] · 2 of 2 files; stopped; it can be resumed\n")
    message(FATAL_ERROR "archive list of an unfinished relocation:\n${last_output}")
endif()
run_cli("0;2" archive relocate "${moving_prefix}" "${MOVE_TO}" --resume --json)
string(JSON resumed GET "${last_output}" resumed)
string(JSON relocated_revision GET "${last_output}" revision)
if(NOT resumed OR NOT relocated_revision EQUAL 2)
    message(FATAL_ERROR "relocate --resume:\n${last_output}")
endif()
run_cli("0" archive show "${moving_prefix}" --json)
if(last_output MATCHES "unfinished_relocation")
    message(FATAL_ERROR "a recorded relocation is still unfinished:\n${last_output}")
endif()
run_cli("12" archive relocate "${moving_prefix}" "${MOVE_TO}" --resume)
# Abandoned on purpose: a relocation back to the first folder, stopped the same way.
set(BLOCKER "${WORK}/repo/manifests/${moving_uuid}/r0003.vam")
file(MAKE_DIRECTORY "${BLOCKER}")
run_cli("11" --lang en archive relocate "${moving_prefix}" "${MOVE_FROM}")
if(NOT last_errors MATCHES "error: cannot record the relocation: .+; the relocation keeps its saved progress: continue it with `archive relocate ${moving_uuid} <root_path> --resume`\n$")
    message(FATAL_ERROR "relocate whose record cannot be written:\n${last_errors}")
endif()
file(REMOVE_RECURSE "${BLOCKER}")
run_cli("12" --lang en archive abandon-relocation "${moving_prefix}")
if(NOT last_output MATCHES "Unfinished relocation of ARCH-[0-9]+ \\(${moving_uuid}\\), revision 2\n"
        OR NOT last_output MATCHES "\n  saved progress: 2 files, [0-9]+ bytes\n"
        OR NOT last_errors MATCHES "--confirm")
    message(FATAL_ERROR "abandon-relocation without --confirm:\n${last_output}\n${last_errors}")
endif()
run_cli("0" archive abandon-relocation "${moving_prefix}" --confirm --json)
string(JSON saved_files GET "${last_output}" saved_progress files)
if(NOT saved_files EQUAL 2)
    message(FATAL_ERROR "abandon-relocation --confirm:\n${last_output}")
endif()
run_cli("12" archive abandon-relocation "${moving_prefix}")
if(NOT last_errors MATCHES "沒有未完成的搬移")
    message(FATAL_ERROR "abandon-relocation with nothing to abandon:\n${last_errors}")
endif()
run_cli("0;2" archive relocate "${moving_prefix}" "${MOVE_FROM}" --json)
string(JSON resumed GET "${last_output}" resumed)
string(JSON relocated_revision GET "${last_output}" revision)
if(resumed OR NOT relocated_revision EQUAL 3)
    message(FATAL_ERROR "relocate after an abandoned relocation:\n${last_output}")
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
# then `subst` must no longer list the probe's mapping. The letter appears in
# the logon session that runs the test, a person's own on a workstation, so
# the probe runs only when VERIARCHIVE_TEST_SUBST is 1, as in the GitLab
# Windows job.
if(CMAKE_HOST_WIN32 AND NOT "$ENV{VERIARCHIVE_TEST_SUBST}" STREQUAL "1")
    message(STATUS "subst probe skipped: it makes a drive letter in this logon session, "
        "so it runs only when VERIARCHIVE_TEST_SUBST=1")
elseif(CMAKE_HOST_WIN32)
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
        # This probe's mapping must be gone, line for line. The letter itself
        # may be listed again: another test run in this logon session (another
        # build tree) may make it for a folder of its own as soon as it is free.
        string(REPLACE "\r" "" listed "\n${substs}\n")
        string(TOLOWER "${listed}" listed)
        string(TOLOWER "\n${letter}:\\: => ${substituted_native}\n" own_mapping)
        string(FIND "${listed}" "${own_mapping}" left_behind)
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
        # The same archive by the real path. The real path is the root the CLI
        # gives for the folder's path without the letter, not that path itself:
        # WORK may be reached through a drive letter made with subst too (a
        # short path for a deep build tree), which the CLI also resolves.
        run_cli("0;2" verify --full "${SUBSTITUTED}/photos" --json)
        string(JSON real_uuid GET "${last_output}" archive_uuid)
        string(JSON real_root GET "${last_output}" root)
        string(TOLOWER "${real_root}" real_root)
        if(NOT real_uuid STREQUAL subst_uuid)
            message(FATAL_ERROR "the real path of a folder registered through ${letter}: is another archive")
        endif()
        string(TOLOWER "^${letter}:" through_letter)
        string(JSON verified_uuid GET "${through_verify_output}" archive_uuid)
        string(JSON verified_root GET "${through_verify_output}" root)
        string(TOLOWER "${verified_root}" verified_root)
        if(NOT verified_uuid STREQUAL subst_uuid OR NOT verified_root STREQUAL real_root
                OR verified_root MATCHES "${through_letter}")
            message(FATAL_ERROR "verify through ${letter}: found ${verified_uuid} at ${verified_root}, "
                "expected ${subst_uuid} at ${real_root}")
        endif()
        # The manifest's root_hint is the real path, not the letter.
        read_root_hint("${subst_manifest}" root_hint)
        string(TOLOWER "${root_hint}" root_hint)
        if(NOT root_hint STREQUAL real_root OR root_hint MATCHES "${through_letter}")
            message(FATAL_ERROR "root_hint of a registration through ${letter}: is ${root_hint}, "
                "expected ${real_root}")
        endif()
        # The real volume: the root's place on it.
        get_filename_component(work_name "${WORK}" NAME)
        run_cli("0" archive show "${subst_uuid}" --json)
        string(JSON subst_subpath GET "${last_output}" root_subpath)
        if(NOT subst_subpath MATCHES "/${work_name}/subst/photos$")
            message(FATAL_ERROR "root_subpath of a registration through ${letter}: is ${subst_subpath}")
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

# An import leaves the revision to an unfinished registration of it that has
# saved files (decision 0030). The first repository records revision 2 of the
# archive and exports it. In the second, a registration of the same revision
# stops at its record, keeping the files it hashed, while a folder stands
# where its manifest belongs (sample data). The import of revision 2 is then
# refused (exit 12), naming the way on, and records once the registration is
# abandoned.
set(GLOBAL --config "${WORK}/config.json")
run_cli("0;2" register "${ROOT}" --new-revision)
run_cli("0" export "${archive_prefix}" --out "${WORK}/export2")
set(GLOBAL --config "${WORK}/config2.json")
set(BLOCKER "${WORK}/repo2/manifests/${archive_uuid}/r0002.vam")
file(MAKE_DIRECTORY "${BLOCKER}")
run_cli("11" register "${ROOT}" --new-revision)
file(REMOVE_RECURSE "${BLOCKER}")
run_cli("12" --lang en import "${WORK}/export2/r0002.vam")
if(NOT last_errors MATCHES "is creating revision 2; nothing was imported"
        OR NOT last_errors MATCHES "archive abandon-registration ${archive_uuid}")
    message(FATAL_ERROR "import over a registration's saved files:\n${last_errors}")
endif()
run_cli("0" archive abandon-registration "${archive_prefix}" --confirm)
run_cli("0" import "${WORK}/export2/r0002.vam" --json)
string(JSON imported_revision GET "${last_output}" revision)
string(JSON imported_before GET "${last_output}" already_present)
if(NOT imported_revision EQUAL 2 OR imported_before)
    message(FATAL_ERROR "import after the registration was abandoned:\n${last_output}")
endif()

# R-14: what is known of a recorded revision comes from its manifest, never
# from the index's copy of its header. Here the manifest of the archive's
# revision 2 has a header line that is not the one recorded (its revision
# written otherwise, sample data). Named by its uuid, the archive is known:
# its verification FAILS on its manifest (decision 0028, exit 3) without
# reading the media, says why plainly in either language, and is recorded in
# its history. A verification by root stops (exit 11): the root may be that
# archive's, so it names the archive and what is wrong, and says to verify it
# by its uuid to record the failure, then to restore the manifest. The
# archive list lists every archive, the damaged one with what is wrong and
# how to fix it, in text in either language and in --json
# (`manifest_problem`), and exits 2: the listing worked, an archive needs
# attention; `archive show` of it reads the same. A command about another
# archive reads no manifest but that archive's: the copy verifies by its
# uuid, and shows its status and history; so does the damaged archive's
# history, which needs no manifest. Restored, all work again.
set(GLOBAL --config "${WORK}/config.json")
run_cli("0" archive show "${archive_prefix}" --json)
string(JSON archive_label GET "${last_output}" label)
set(RECORDED "${WORK}/repo/manifests/${archive_uuid}/r0002.vam")
# Kept byte for byte: file(WRITE) may write other line ends.
file(COPY_FILE "${RECORDED}" "${RECORDED}.kept")
file(READ "${RECORDED}" recorded_bytes)
string(REPLACE "\"revision\":2," "\"revision\": 2," changed_bytes "${recorded_bytes}")
file(WRITE "${RECORDED}" "${changed_bytes}")
run_cli("2" --lang en archive list)
set(listed "${last_output}")
run_cli("2" archive list)
set(listed_chinese "${last_output}")
run_cli("2" archive list --json)
set(listed_json "${last_output}")
run_cli("2" --lang en archive show "${archive_prefix}")
set(shown "${last_output}")
run_cli("11" --lang en verify --full "${ROOT}")
set(verified "${last_errors}")
run_cli("3" --lang en verify --full "${archive_uuid}" --json)
set(failed_json "${last_output}")
run_cli("3" verify --full "${archive_uuid}")
set(failed_chinese "${last_output}")
run_cli("0;2" verify --full "${copy_uuid}")
run_cli("0" archive show "${copy_prefix}")
run_cli("0" history "${copy_prefix}")
run_cli("0" history "${archive_prefix}" --json)
set(history_json "${last_output}")
file(RENAME "${RECORDED}.kept" "${RECORDED}")
set(changed_manifest "the manifest of revision 2 (manifests/${archive_uuid}/r0002.vam) has changed: its first line differs from what the repository recorded; restore it from a backup of the repository")
set(changed_chinese "版次 2 的清單檔（manifests/${archive_uuid}/r0002.vam）已被變更：其第一行與儲存庫所記錄的不同；請從儲存庫的備份還原該清單檔")
set(root_may_belong "this root may belong to archive ${archive_label} (${archive_uuid}), whose manifest cannot be used: the manifest of revision 2 (manifests/${archive_uuid}/r0002.vam) has changed: its first line differs from what the repository recorded; verify that archive by its uuid (`verify --full ${archive_uuid}`) to record the failure, then restore its manifest from a backup of the repository")
string(FIND "${listed}" "\n    problem: ${changed_manifest}\n" in_list)
string(FIND "${listed_chinese}" "\n    問題：${changed_chinese}\n" in_list_chinese)
string(FIND "${shown}" "\n  problem: ${changed_manifest}\n" in_show)
string(SUBSTRING "${copy_uuid}" 0 8 copy_short)
string(FIND "${listed}" "  ${copy_short}  " copy_listed)
set(problem_kind "")
set(copy_without_problem "NOTFOUND")
set(copy_without_volume "")
string(JSON listed_count LENGTH "${listed_json}" archives)
math(EXPR last_listed "${listed_count} - 1")
foreach(index RANGE ${last_listed})
    string(JSON listed_uuid GET "${listed_json}" archives ${index} archive_uuid)
    if(listed_uuid STREQUAL archive_uuid)
        string(JSON problem_kind GET "${listed_json}" archives ${index} manifest_problem kind)
        string(JSON problem_revision GET "${listed_json}" archives ${index} manifest_problem revision)
        string(JSON problem_manifest GET "${listed_json}" archives ${index} manifest_problem manifest)
        string(JSON problem_message GET "${listed_json}" archives ${index} manifest_problem message)
    elseif(listed_uuid STREQUAL copy_uuid)
        # ERROR_VARIABLE is NOTFOUND when the member is there.
        string(JSON copy_problem ERROR_VARIABLE copy_without_problem GET "${listed_json}" archives ${index}
            manifest_problem)
        string(JSON copy_volume ERROR_VARIABLE copy_without_volume GET "${listed_json}" archives ${index}
            volume_label)
    endif()
endforeach()
string(FIND "${verified}" "${root_may_belong}" in_verify)
string(FIND "${failed_chinese}" "完整驗證 失敗" failed_line)
string(FIND "${failed_chinese}" "媒體：未讀取，因為這個版次的清單檔無法使用" unread_line)
string(FIND "${failed_chinese}" "${changed_chinese}" in_chinese)
string(JSON failed_result GET "${failed_json}" result)
string(JSON failed_read GET "${failed_json}" media_read)
string(JSON failed_root TYPE "${failed_json}" root)
string(JSON failed_identity TYPE "${failed_json}" identity)
string(JSON failed_class GET "${failed_json}" failure_classes 0)
string(JSON failed_warning GET "${failed_json}" warnings 0)
string(JSON run_count LENGTH "${history_json}" runs)
math(EXPR last_run "${run_count} - 1")
string(JSON last_result GET "${history_json}" runs ${last_run} result)
string(JSON last_classes GET "${history_json}" runs ${last_run} failure_classes)
if(changed_bytes STREQUAL recorded_bytes OR in_list EQUAL -1 OR in_list_chinese EQUAL -1 OR in_show EQUAL -1
        OR copy_listed EQUAL -1 OR in_verify EQUAL -1)
    message(FATAL_ERROR "a manifest that is not the one recorded, by root and listed:\n${listed}\n${listed_chinese}\n${shown}\n${verified}")
endif()
if(NOT problem_kind STREQUAL "changed" OR NOT problem_revision EQUAL 2
        OR NOT problem_manifest STREQUAL "manifests/${archive_uuid}/r0002.vam"
        OR NOT problem_message STREQUAL changed_manifest OR copy_without_problem STREQUAL "NOTFOUND"
        OR NOT copy_without_volume STREQUAL "NOTFOUND")
    message(FATAL_ERROR "archive list --json of a manifest that is not the one recorded:\n${listed_json}")
endif()
if(NOT failed_result STREQUAL "FAILED" OR failed_read OR NOT failed_root STREQUAL "NULL"
        OR NOT failed_identity STREQUAL "NULL"
        OR NOT failed_class STREQUAL "ManifestHashMismatch" OR NOT failed_warning STREQUAL "manifest: ${changed_manifest}"
        OR failed_line EQUAL -1 OR unread_line EQUAL -1 OR in_chinese EQUAL -1
        OR NOT last_result STREQUAL "FAILED" OR NOT last_classes STREQUAL "ManifestHashMismatch")
    message(FATAL_ERROR "a manifest that is not the one recorded, by uuid:\n${failed_json}\n${failed_chinese}\n${history_json}")
endif()

# The kind of each manifest problem in --json (decision 0028): the manifest
# gone, and one that cannot be read, a folder standing in its place (sample
# data); the copy, whose manifest is intact, has none.
function(manifest_problem_kind listed uuid out)
    set(kind "")
    string(JSON count LENGTH "${listed}" archives)
    math(EXPR last "${count} - 1")
    foreach(index RANGE ${last})
        string(JSON listed_uuid GET "${listed}" archives ${index} archive_uuid)
        if(listed_uuid STREQUAL uuid)
            string(JSON kind ERROR_VARIABLE no_problem GET "${listed}" archives ${index} manifest_problem kind)
            if(no_problem)
                set(kind "none")
            endif()
        endif()
    endforeach()
    set(${out} "${kind}" PARENT_SCOPE)
endfunction()
file(RENAME "${RECORDED}" "${RECORDED}.away")
run_cli("2" archive list --json)
manifest_problem_kind("${last_output}" "${archive_uuid}" missing_kind)
manifest_problem_kind("${last_output}" "${copy_uuid}" copy_kind)
file(MAKE_DIRECTORY "${RECORDED}")
run_cli("2" archive list --json)
manifest_problem_kind("${last_output}" "${archive_uuid}" unreadable_kind)
file(REMOVE_RECURSE "${RECORDED}")
file(RENAME "${RECORDED}.away" "${RECORDED}")
if(NOT missing_kind STREQUAL "missing" OR NOT unreadable_kind STREQUAL "unreadable" OR NOT copy_kind STREQUAL "none")
    message(FATAL_ERROR "manifest_problem kind of a manifest gone: ${missing_kind}, of a folder in its place: "
        "${unreadable_kind}, of the copy: ${copy_kind}")
endif()
run_cli("0" archive list)
run_cli("0;2" verify --full "${ROOT}")

# The manifest of an earlier revision is read only where matching looks at
# the earlier carriers (decision 0004): with the first manifest of the
# relocated archive moved away (sample data), the archive verifies by its
# uuid and by its root, where its latest carrier matches.
set(EARLIER "${WORK}/repo/manifests/${moving_uuid}/r0001.vam")
file(RENAME "${EARLIER}" "${EARLIER}.away")
run_cli("0;2" verify --full "${moving_uuid}")
run_cli("0;2" verify --full "${MOVE_FROM}")
file(RENAME "${EARLIER}.away" "${EARLIER}")

message(STATUS "veriarchive-cli end-to-end test passed")
