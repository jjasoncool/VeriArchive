# End-to-end test of veriarchive-cli (R-120 integration tests): registers a
# real directory tree, verifies it, damages it, and exercises history,
# export/import, the sha256sum export and the interface language (decision 0017).
#
#   cmake -DCLI=<veriarchive-cli> -DWORK=<scratch dir> -DVERSION=<program version> -P cli_e2e.cmake
cmake_minimum_required(VERSION 3.28)

if(NOT CLI OR NOT WORK OR NOT VERSION)
    message(FATAL_ERROR "CLI, WORK and VERSION must be set")
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
    # GitLab's runners run as root or as an administrator: every command that
    # opens the repository then first warns that VeriArchive runs elevated
    # (R-12). The checks compare what a command says about its own work, so
    # that line is left out of the errors they see, in either language.
    string(REGEX REPLACE "(^|\n)(warning: VeriArchive is running elevated|警告：VeriArchive 正以系統管理員權限執行)[^\n]*\n"
        "\\1" errors "${errors}")
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
# Decision 0037: the program shows its version, with --version (exit 0) in
# either language and in --json, and in repo info, text and --json.
run_cli("0" --lang en --version)
set(version_english "${last_output}")
run_cli("0" --version)
set(version_chinese "${last_output}")
run_cli("0" --version --json)
string(JSON version_json GET "${last_output}" app_version)
run_cli("0" --lang en repo info)
string(FIND "${last_output}" "\nprogram version: ${VERSION}\n" info_version)
run_cli("0" repo info)
string(FIND "${last_output}" "\n程式版本：${VERSION}\n" info_version_chinese)
run_cli("0" repo info --json)
string(JSON info_json_version GET "${last_output}" app_version)
if(NOT version_english STREQUAL "VeriArchive version ${VERSION}\n" OR NOT version_chinese STREQUAL "VeriArchive 版本 ${VERSION}\n"
        OR NOT version_json STREQUAL VERSION OR info_version EQUAL -1 OR info_version_chinese EQUAL -1
        OR NOT info_json_version STREQUAL VERSION)
    message(FATAL_ERROR "the program's version (${VERSION}): ${version_english}${version_chinese}--json ${version_json}, "
        "repo info ${info_version}/${info_version_chinese}, --json ${info_json_version}")
endif()
run_cli("0" register "${ROOT}" --json)
string(JSON archive_uuid GET "${last_output}" archive_uuid)
string(JSON files GET "${last_output}" counts files)
if(NOT files EQUAL 4)
    message(FATAL_ERROR "expected 4 files, got ${files}")
endif()
# One of a thing is said in the singular (decision 0017, amended 2026-10-27).
run_cli("0" --lang en archive show "${archive_uuid}")
if(NOT last_output MATCHES "\n  content: 4 files, 1 empty directory, 0 links, [0-9]+ bytes\n")
    message(FATAL_ERROR "archive show of an archive with one empty directory:\n${last_output}")
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
# R-02: a verify report states the guarantee, and neither it nor the refusal
# of QUICK and DEEP names a version of the program (decision 0017), in either
# language; --json keeps the English sentence in "guarantee".
set(guarantee "VeriArchive detects accidental corruption and unintended change; tamper evidence (signed manifests) is not part of this version.")
string(FIND "${last_output}" "\n  ${guarantee}\n" guarantee_english)
run_cli("0;2" verify --full "${archive_uuid}")
string(FIND "${last_output}" "\n  VeriArchive 能偵測意外損毀與非預期的變更；這個版本尚未提供竄改證據（簽章清單檔）。\n" guarantee_chinese)
run_cli("0;2" verify --full "${archive_uuid}" --json)
string(JSON guarantee_json GET "${last_output}" guarantee)
run_cli("10" --lang en verify --quick "${archive_uuid}")
set(quick_refusal "${last_errors}")
run_cli("10" verify --deep "${archive_uuid}")
set(deep_refusal "${last_errors}")
if(guarantee_english EQUAL -1 OR guarantee_chinese EQUAL -1 OR NOT guarantee_json STREQUAL guarantee
        OR NOT quick_refusal STREQUAL "error: QUICK and DEEP verification are not available in this version; use --full\n"
        OR NOT deep_refusal STREQUAL "錯誤：這個版本尚未提供快速驗證與深度驗證；請使用 --full\n")
    message(FATAL_ERROR "the guarantee or the refusal of QUICK and DEEP: ${guarantee_english} ${guarantee_chinese} "
        "${guarantee_json}\n${quick_refusal}\n${deep_refusal}")
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
# Decision 0017: a usage error follows --json too, as JSON on stdout (error,
# exit_code), whatever --lang or the config file choose: a parse error, a
# --lang that is none, an unknown command, no command, and an unknown option
# of media list, which checks its options as every command does.
function(usage_error_in_json expected)
    run_cli("10" --json ${ARGN})
    string(JSON usage_error ERROR_VARIABLE usage_not_json GET "${last_output}" error)
    string(JSON usage_code ERROR_VARIABLE usage_no_code GET "${last_output}" exit_code)
    if(usage_not_json OR usage_no_code OR NOT usage_code EQUAL 10 OR NOT usage_error STREQUAL expected)
        message(FATAL_ERROR "--json ${ARGN}: not this usage error in JSON: ${expected}\n${last_output}\n${last_errors}")
    endif()
endfunction()
usage_error_in_json("option --out needs a value" export "${archive_uuid}" --out)
usage_error_in_json("--lang takes en or zh-TW" --lang fr archive list)
usage_error_in_json("unknown command bogus-command-SAMPLE" bogus-command-SAMPLE)
usage_error_in_json("a command is needed; veriarchive-cli without arguments shows the commands")
usage_error_in_json("unknown option --bogus" media list --bogus)
run_cli("10" --lang en media list --bogus)
if(NOT last_errors STREQUAL "error: unknown option --bogus\n")
    message(FATAL_ERROR "media list with an unknown option:\n${last_errors}")
endif()
run_cli("10" bogus-command-SAMPLE)
if(NOT last_errors STREQUAL "錯誤：未知的指令 bogus-command-SAMPLE\n" OR NOT last_output MATCHES "^用法：veriarchive-cli ")
    message(FATAL_ERROR "an unknown command:\n${last_output}\n${last_errors}")
endif()
# A path the user typed is shown as typed, even when it looks like a status word.
run_cli("10" register Missing)
if(NOT last_errors MATCHES "找不到資料夾或磁碟區 Missing")
    message(FATAL_ERROR "a typed path was translated:\n${last_errors}")
endif()
# Decision 0029, point 7: `verify --full` takes an archive's uuid, a folder or
# volume that is there, or the start of an archive's uuid (8 characters or
# more); anything else is a folder or volume that was not found (exit 10), in
# either language, never "not a registered archive". Where no volume is
# attached as the drive letter typed (Windows; K:, a sample letter, where no
# volume has it), it says to check that the device is attached.
run_cli("10" --lang en verify --full "${WORK}/no-such-folder-SAMPLE")
if(NOT last_errors STREQUAL "error: the folder or volume ${WORK}/no-such-folder-SAMPLE was not found\n")
    message(FATAL_ERROR "verify of a folder that is not there:\n${last_errors}")
endif()
run_cli("10" verify --full abcdef12)
if(NOT last_errors MATCHES "錯誤：找不到資料夾或磁碟區 abcdef12\n$")
    message(FATAL_ERROR "verify of the start of no archive's uuid:\n${last_errors}")
endif()
# A whole uuid that names no registered archive is not found either (exit 10),
# as its first 8 characters are, never a repository error.
run_cli("10" --lang en verify --full c0ffee00-0000-4000-8000-0000000000aa)
if(NOT last_errors STREQUAL "error: no registered archive c0ffee00-0000-4000-8000-0000000000aa\n")
    message(FATAL_ERROR "verify of a uuid that names no archive:\n${last_errors}")
endif()
string(SUBSTRING "${archive_uuid}" 0 8 uuid_start)
run_cli("0;2" verify --full "${uuid_start}" --json)
string(JSON start_verified GET "${last_output}" archive_uuid)
if(NOT start_verified STREQUAL archive_uuid)
    message(FATAL_ERROR "verify --full of the start of an archive's uuid:\n${last_output}")
endif()
if(CMAKE_HOST_WIN32 AND NOT EXISTS "K:/")
    run_cli("10" --lang en verify --full [[K:\SAMPLE]])
    if(NOT last_errors STREQUAL "error: the folder or volume K:\\SAMPLE was not found: no volume is attached as K:; check that the device that holds it is attached\n")
        message(FATAL_ERROR "verify on a drive letter that no volume has:\n${last_errors}")
    endif()
elseif(CMAKE_HOST_WIN32)
    message(STATUS "drive letter probe skipped: a volume is attached as K:")
endif()
# Decision 0029, point 6: a root that holds the repository, or lies inside it,
# is refused before anything is read (exit 10), in either language, and
# nothing is recorded: WORK holds the repository, and its manifests folder
# lies inside it.
run_cli("0" archive list --json)
string(JSON archives_before_repository LENGTH "${last_output}" archives)
run_cli("10" --lang en register "${WORK}")
string(FIND "${last_errors}" "error: this folder or volume holds VeriArchive's repository " repository_held)
string(FIND "${last_errors}" ", which changes while it is read, so a verification of this archive could never pass; register a folder that does not hold the repository, or move the repository out of it first\n" repository_held_why)
run_cli("10" register "${WORK}/repo/manifests")
string(FIND "${last_errors}" "錯誤：此資料夾位於 VeriArchive 的儲存庫 " repository_inside)
run_cli("0" archive list --json)
string(JSON archives_after_repository LENGTH "${last_output}" archives)
if(repository_held EQUAL -1 OR repository_held_why EQUAL -1 OR repository_inside EQUAL -1
        OR NOT archives_after_repository EQUAL archives_before_repository)
    message(FATAL_ERROR "register of a root that holds the repository, or lies inside it: ${repository_held} "
        "${repository_held_why} ${repository_inside}\n${last_errors}")
endif()
# Another spelling of a folder that holds the repository is refused alike: a
# link to the repository (a junction on Windows, a symbolic link on Linux,
# sample), and on Windows WORK's short (8.3) name where its volume gives one.
set(repository_link "${WORK}/link-to-repository-SAMPLE")
if(CMAKE_HOST_WIN32)
    file(TO_NATIVE_PATH "${repository_link}" repository_link_native)
    file(TO_NATIVE_PATH "${WORK}/repo" repository_native)
    execute_process(COMMAND cmd /c mklink /J "${repository_link_native}" "${repository_native}"
        RESULT_VARIABLE linked OUTPUT_QUIET ERROR_QUIET)
else()
    file(CREATE_LINK "${WORK}/repo" "${repository_link}" RESULT linked SYMBOLIC)
endif()
if(NOT linked EQUAL 0)
    message(FATAL_ERROR "cannot make a link to the sample repository: ${linked}")
endif()
run_cli("10" --lang en register "${repository_link}")
string(FIND "${last_errors}" "error: this folder or volume holds VeriArchive's repository " link_held)
file(REMOVE "${repository_link}")
set(short_held 0)
if(CMAKE_HOST_WIN32)
    # The command prompt tells a path's short name (%~s), from a batch file
    # that takes the path as its argument, whatever characters it holds.
    file(TO_NATIVE_PATH "${WORK}" work_native)
    file(WRITE "${WORK}/short-name-SAMPLE.bat" "@echo off\r\nfor %%I in (\"%~1\") do echo %%~sI\r\n")
    file(TO_NATIVE_PATH "${WORK}/short-name-SAMPLE.bat" short_name_script)
    execute_process(COMMAND cmd /c "${short_name_script}" "${work_native}"
        OUTPUT_VARIABLE work_short OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    file(REMOVE "${WORK}/short-name-SAMPLE.bat")
    if(work_short STREQUAL "" OR work_short STREQUAL work_native)
        message(STATUS "short name probe skipped: the volume gives ${WORK} no short (8.3) name")
    else()
        run_cli("10" --lang en register "${work_short}")
        string(FIND "${last_errors}" "error: this folder or volume holds VeriArchive's repository " short_held)
    endif()
endif()
run_cli("0" archive list --json)
string(JSON archives_after_links LENGTH "${last_output}" archives)
if(link_held EQUAL -1 OR short_held EQUAL -1 OR NOT archives_after_links EQUAL archives_before_repository)
    message(FATAL_ERROR "register of another spelling of a folder that holds the repository: link ${link_held}, "
        "short name '${work_short}' ${short_held}\n${last_errors}")
endif()
# A new root of a relocation that is no folder is a usage error too, named as typed.
run_cli("10" --lang en archive relocate "${archive_uuid}" Missing)
if(NOT last_errors STREQUAL "error: the folder or volume Missing was not found\n")
    message(FATAL_ERROR "relocate to a root that is not there:\n${last_errors}")
endif()
# Decision 0029 (Windows): a root on a network share, or one written in the
# \\?\ form, is refused by register, verify and relocate before anything of
# it is read (exit 10), in either language, and nothing is recorded. The share
# is a sample name, refused from its text alone: nothing contacts it.
if(CMAKE_HOST_WIN32)
    set(SHARE_ROOT [[\\sample-server\sample-share\photos]])
    set(share_words "the archive root is on a network share; VeriArchive registers and verifies folders on volumes attached to this computer")
    run_cli("0" archive list --json)
    string(JSON archives_before LENGTH "${last_output}" archives)
    run_cli("10" --lang en register "${SHARE_ROOT}")
    string(FIND "${last_errors}" "error: ${share_words}\n" found)
    if(found EQUAL -1)
        message(FATAL_ERROR "register of a root on a network share:\n${last_errors}")
    endif()
    run_cli("10" register "${SHARE_ROOT}")
    string(FIND "${last_errors}" "錯誤：封存根目錄位於網路共用資料夾上；VeriArchive 只註冊和驗證這台電腦所連接的磁碟區上的資料夾" found)
    if(found EQUAL -1)
        message(FATAL_ERROR "register of a root on a network share in zh-TW:\n${last_errors}")
    endif()
    run_cli("10" verify --full "${SHARE_ROOT}" --json)
    string(JSON refusal GET "${last_output}" error)
    if(NOT refusal STREQUAL share_words)
        message(FATAL_ERROR "verify of a root on a network share:\n${last_output}")
    endif()
    run_cli("10" --lang en archive relocate "${archive_uuid}" "${SHARE_ROOT}")
    string(FIND "${last_errors}" "error: ${share_words}\n" found)
    if(found EQUAL -1)
        message(FATAL_ERROR "relocate to a root on a network share:\n${last_errors}")
    endif()
    file(TO_NATIVE_PATH "${ROOT}" root_native)
    set(device_prefix [[\\?\]])
    run_cli("10" --lang en register "${device_prefix}${root_native}")
    string(FIND "${last_errors}" [[error: write the archive root without \\?\ in front (for example D:\Photos)]] found)
    if(found EQUAL -1)
        message(FATAL_ERROR "register of a root written in the \\\\?\\ form:\n${last_errors}")
    endif()
    run_cli("10" verify --full "${device_prefix}${root_native}")
    string(FIND "${last_errors}" [[錯誤：封存根目錄前面請不要加上 \\?\（例如寫成 D:\Photos）]] found)
    if(found EQUAL -1)
        message(FATAL_ERROR "verify of a root written in the \\\\?\\ form in zh-TW:\n${last_errors}")
    endif()
    run_cli("0" archive list --json)
    string(JSON archives_after LENGTH "${last_output}" archives)
    if(NOT archives_after EQUAL archives_before)
        message(FATAL_ERROR "a refused root was recorded:\n${last_output}")
    endif()
endif()
# Decision 0029, point 3 (Windows): a volume's GUID path (\\?\Volume{...}\),
# the only name of a volume that has no drive letter or folder path, is taken.
# Through it ROOT is the same archive as through its drive letter: registered
# again only with a decision (R-97), and verified there. A sample folder
# registered through it keeps it as its root_hint and is the archive its drive
# letter names; a copy of it is relocated to through it. The \\.\ form of the
# path is refused, saying how to write it. The GUID path is the one mountvol
# lists for the volume WORK lies on (it only reads); where it lists none (WORK
# on a drive letter made with subst, say), this part is skipped.
if(CMAKE_HOST_WIN32)
    file(TO_NATIVE_PATH "${ROOT}" root_native)
    string(SUBSTRING "${root_native}" 0 3 work_drive)
    execute_process(COMMAND mountvol "${work_drive}" /L
        OUTPUT_VARIABLE volume_name RESULT_VARIABLE listed ERROR_QUIET)
    string(STRIP "${volume_name}" volume_name)
    string(SUBSTRING "${volume_name}" 0 11 volume_head)
    if(NOT listed EQUAL 0 OR NOT volume_head STREQUAL [[\\?\Volume{]])
        message(STATUS "GUID path probe skipped: mountvol lists no GUID path for ${work_drive}")
    else()
        string(SUBSTRING "${root_native}" 3 -1 root_below)
        set(GUID_ROOT "${volume_name}${root_below}")
        run_cli("12" --lang en register "${GUID_ROOT}")
        string(FIND "${last_errors}" "already registered as archive ${archive_uuid}" found)
        if(found EQUAL -1)
            message(FATAL_ERROR "register of ${GUID_ROOT}, the root of an archive through its volume's GUID path:\n${last_errors}")
        endif()
        run_cli("0;2" verify --full "${GUID_ROOT}" --json)
        string(JSON guid_uuid GET "${last_output}" archive_uuid)
        string(JSON guid_verified_root GET "${last_output}" root)
        if(NOT guid_uuid STREQUAL archive_uuid OR NOT guid_verified_root STREQUAL GUID_ROOT)
            message(FATAL_ERROR "verify of ${GUID_ROOT}:\n${last_output}")
        endif()
        set(GUID_FOLDER "${WORK}/guid")
        file(WRITE "${GUID_FOLDER}/sample.txt" "GUID path sample\n")
        file(TO_NATIVE_PATH "${GUID_FOLDER}" guid_folder_native)
        string(SUBSTRING "${guid_folder_native}" 3 -1 guid_folder_below)
        run_cli("0" register "${volume_name}${guid_folder_below}" --json)
        string(JSON guid_folder_uuid GET "${last_output}" archive_uuid)
        string(JSON guid_folder_manifest GET "${last_output}" manifest)
        read_root_hint("${guid_folder_manifest}" guid_hint)
        if(NOT guid_hint STREQUAL "${volume_name}${guid_folder_below}")
            message(FATAL_ERROR "root_hint of a registration through a GUID path is ${guid_hint}")
        endif()
        run_cli("0;2" verify --full "${GUID_FOLDER}" --json)
        string(JSON verified_uuid GET "${last_output}" archive_uuid)
        if(NOT verified_uuid STREQUAL guid_folder_uuid)
            message(FATAL_ERROR "verify of ${GUID_FOLDER} through its drive letter:\n${last_output}")
        endif()
        set(GUID_COPY "${WORK}/guid-copy")
        file(COPY "${GUID_FOLDER}/" DESTINATION "${GUID_COPY}")
        file(TO_NATIVE_PATH "${GUID_COPY}" guid_copy_native)
        string(SUBSTRING "${guid_copy_native}" 3 -1 guid_copy_below)
        run_cli("0;2" archive relocate "${guid_folder_uuid}" "${volume_name}${guid_copy_below}" --json)
        string(JSON relocated_revision GET "${last_output}" revision)
        if(NOT relocated_revision EQUAL 2)
            message(FATAL_ERROR "relocate through a GUID path:\n${last_output}")
        endif()
        set(dot_prefix [[\\.\]])
        string(SUBSTRING "${volume_name}" 4 -1 volume_rest)
        run_cli("10" --lang en register "${dot_prefix}${volume_rest}${guid_folder_below}")
        string(FIND "${last_errors}" [[error: write a volume's GUID path exactly as mountvol lists it, with \\?\ in front]] found)
        if(found EQUAL -1)
            message(FATAL_ERROR "register of a GUID path in the \\\\.\\ form:\n${last_errors}")
        endif()
        run_cli("10" verify --full "${dot_prefix}${volume_rest}${guid_folder_below}")
        string(FIND "${last_errors}" [[錯誤：磁碟區的 GUID 路徑請完全照 mountvol 列出的樣子寫，前面加上 \\?\]] found)
        if(found EQUAL -1)
            message(FATAL_ERROR "verify of a GUID path in the \\\\.\\ form in zh-TW:\n${last_errors}")
        endif()
        # Written with "..", "." or two separators in a row after its GUID, a
        # GUID path names the folder they resolve to, as a path through a
        # drive letter does: ROOT is verified as the archive it is, read and
        # named without them, and a sample folder is registered through "..",
        # its root_hint the folder (Windows reads a \\?\ path as given). No
        # spelling ends with a backslash, which would escape the ';' after it
        # in run_cli's list of arguments.
        file(TO_NATIVE_PATH "${WORK}" work_native)
        string(SUBSTRING "${work_native}" 3 -1 work_below)
        foreach(spelling "${GUID_ROOT}\\..\\archive" "${GUID_ROOT}\\." "${volume_name}${work_below}\\\\archive"
                "${volume_name}\\${root_below}")
            run_cli("0;2" verify --full "${spelling}" --json)
            string(JSON spelled_uuid GET "${last_output}" archive_uuid)
            string(JSON spelled_root GET "${last_output}" root)
            string(JSON spelled_result GET "${last_output}" result)
            if(NOT spelled_uuid STREQUAL archive_uuid OR NOT spelled_root STREQUAL GUID_ROOT
                    OR NOT spelled_result MATCHES "^PASSED")
                message(FATAL_ERROR "verify of ${spelling}:\n${last_output}")
            endif()
        endforeach()
        file(WRITE "${WORK}/guid-dots/sample.txt" "GUID path sample, registered through ..\n")
        run_cli("0" register "${GUID_ROOT}\\..\\guid-dots" --json)
        string(JSON dots_manifest GET "${last_output}" manifest)
        read_root_hint("${dots_manifest}" dots_hint)
        if(NOT dots_hint STREQUAL "${volume_name}${work_below}\\guid-dots")
            message(FATAL_ERROR "root_hint of a registration through a GUID path with .. in it is ${dots_hint}")
        endif()
    endif()
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

# Decision 0005: --copy-of takes the original's uuid or its first 8 or more
# characters, as `archive list` shows them, and records the whole uuid; the
# start of no archive's uuid is a usage error (exit 10) that says where the
# whole uuid is shown. (A copy of the copy's sample folder of its own.)
set(COPY_OF "${WORK}/copy-of-sample")
file(COPY "${ROOT}/" DESTINATION "${COPY_OF}")
run_cli("10" --lang en register "${COPY_OF}" --copy-of abcdef12)
if(NOT last_errors STREQUAL "error: --copy-of abcdef12: no archive's uuid starts with it; give the original's uuid or its first 8 or more characters, as `archive list` shows them (`archive show <uuid>` shows the whole uuid)\n")
    message(FATAL_ERROR "register --copy-of of the start of no archive's uuid:\n${last_errors}")
endif()
run_cli("0" register "${COPY_OF}" --copy-of "${archive_prefix}" --json)
string(JSON copy_of_uuid GET "${last_output}" archive_uuid)
run_cli("0" archive show "${copy_of_uuid}" --json)
string(JSON recorded_copy_of GET "${last_output}" copy_of)
if(NOT recorded_copy_of STREQUAL archive_uuid)
    message(FATAL_ERROR "register --copy-of of the start of an archive's uuid recorded ${recorded_copy_of}")
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
# --resume records it. A second one is abandoned. The way on names the new
# root as it can be typed (decision 0022, amended 2026-10-27): in quotes when
# it holds a space, as the copy's folder does.
set(MOVE_FROM "${WORK}/move-from")
set(MOVE_TO "${WORK}/move to SAMPLE")
# A root as the program writes it in a command: as it is when it holds only
# letters, digits and \ / : . _ -, else in double quotes on Windows and in
# single quotes on Linux. A path the program writes is compared as
# cmake_path(NATIVE_PATH) gives it, with the system's separators and nothing
# else: on Linux file(TO_NATIVE_PATH) writes each space as "\ ", as a makefile
# needs it.
function(shell_argument out path)
    cmake_path(NATIVE_PATH path NORMALIZE native)
    if(native MATCHES "^[A-Za-z0-9\\\\/:._-]+$")
        set(${out} "${native}" PARENT_SCOPE)
    elseif(CMAKE_HOST_WIN32)
        set(${out} "\"${native}\"" PARENT_SCOPE)
    else()
        set(${out} "'${native}'" PARENT_SCOPE)
    endif()
endfunction()
shell_argument(move_to_argument "${MOVE_TO}")
shell_argument(move_from_argument "${MOVE_FROM}")
file(WRITE "${MOVE_FROM}/a.txt" "relocation sample a\n")
file(WRITE "${MOVE_FROM}/sub/b.txt" "relocation sample b\n")
run_cli("0" register "${MOVE_FROM}" --json)
string(JSON moving_uuid GET "${last_output}" archive_uuid)
string(SUBSTRING "${moving_uuid}" 0 8 moving_prefix)
file(COPY "${MOVE_FROM}/" DESTINATION "${MOVE_TO}")
set(BLOCKER "${WORK}/repo/manifests/${moving_uuid}/r0002.vam")
file(MAKE_DIRECTORY "${BLOCKER}")
run_cli("11" archive relocate "${moving_prefix}" "${MOVE_TO}")
string(FIND "${last_errors}" "；搬移保留其已儲存的進度：請以 `archive relocate ${moving_uuid} ${move_to_argument} --resume` 繼續\n" way_on)
if(NOT last_errors MATCHES "錯誤：無法記錄搬移：" OR way_on EQUAL -1)
    message(FATAL_ERROR "relocate whose record cannot be written:\n${last_errors}")
endif()
file(REMOVE_RECURSE "${BLOCKER}")
run_cli("12" --lang en archive relocate "${moving_prefix}" "${MOVE_TO}")
string(FIND "${last_errors}" "; resume it with `archive relocate ${moving_uuid} ${move_to_argument} --resume`, or abandon it with `archive abandon-relocation ${moving_uuid}`\n" ways_out)
if(ways_out EQUAL -1)
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
if(NOT last_output MATCHES "\n  未完成的搬移（版次 1，至「[^\n]*move to SAMPLE」）：([0-9]+) B / ([0-9]+) B（100%）・2 / 2 個檔案；已停止；可以繼續\n"
        OR NOT CMAKE_MATCH_1 STREQUAL CMAKE_MATCH_2)
    message(FATAL_ERROR "archive show of an unfinished relocation:\n${last_output}")
endif()
run_cli("0" --lang en archive list)
if(NOT last_output MATCHES "\n    Unfinished relocation of revision 1 to \"[^\n]*move to SAMPLE\": [0-9]+ B of [0-9]+ B [(]100%[)] · 2 of 2 files; stopped; it can be resumed\n")
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
string(FIND "${last_errors}" "; the relocation keeps its saved progress: continue it with `archive relocate ${moving_uuid} ${move_from_argument} --resume`\n" way_on)
if(NOT last_errors MATCHES "error: cannot record the relocation: " OR way_on EQUAL -1)
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
# Decision 0004: a relocation to a copy that differs (one file changed, sample
# data) is refused (exit 3), saying to copy the archive again from its
# carrier, never `register --new-revision`, in either language; text lists
# each entry that differs with its detail, and --json gives them as `items`.
set(REFUSE_FROM "${WORK}/refuse-from-sample")
set(REFUSE_TO "${WORK}/refuse-to-sample")
file(WRITE "${REFUSE_FROM}/a.txt" "refusal sample a\n")
file(WRITE "${REFUSE_FROM}/b.txt" "refusal sample b\n")
file(COPY "${REFUSE_FROM}/" DESTINATION "${REFUSE_TO}")
file(WRITE "${REFUSE_TO}/a.txt" "refusal sample a, changed\n")
run_cli("0" register "${REFUSE_FROM}" --json)
string(JSON refused_uuid GET "${last_output}" archive_uuid)
run_cli("3" --lang en archive relocate "${refused_uuid}" "${REFUSE_TO}")
set(refused_text "${last_output}")
run_cli("3" archive relocate "${refused_uuid}" "${REFUSE_TO}")
set(refused_chinese "${last_output}")
run_cli("3" archive relocate "${refused_uuid}" "${REFUSE_TO}" --json)
string(JSON refused_error GET "${last_output}" error)
string(JSON refused_items LENGTH "${last_output}" items)
string(JSON refused_path GET "${last_output}" items 0 p)
string(JSON refused_status GET "${last_output}" items 0 status)
if(NOT refused_text MATCHES "^RELOCATION REFUSED: the new carrier does not hold the recorded content; nothing was changed: copy the archive again from its carrier to the new one, then relocate again\n  Modified  a[.]txt  [(][^)\n]+[)]\n$"
        OR NOT refused_chinese MATCHES "^拒絕搬移：新載體上的內容與記錄不符；未做任何變更：請從封存的載體重新複製到新載體，再重新搬移\n  已修改  a[.]txt"
        OR refused_text MATCHES "new-revision" OR refused_chinese MATCHES "new-revision"
        OR NOT refused_error STREQUAL "the new carrier does not hold the recorded content; nothing was changed: copy the archive again from its carrier to the new one, then relocate again"
        OR NOT refused_items EQUAL 1 OR NOT refused_path STREQUAL "a.txt" OR NOT refused_status STREQUAL "Modified")
    message(FATAL_ERROR "a relocation to a copy that differs:\n${refused_text}\n${refused_chinese}\n${last_output}")
endif()

# The history says which runs are a relocation's (decision 0026): the FULL
# verifications that recorded revisions 2 and 3, in either language, and
# --json names the relocation of each, which is the run itself.
run_cli("0" --lang en history "${moving_prefix}")
if(NOT last_output MATCHES "  FULL VERIFY PASSED[_A-Z]*  r2  [0-9]+ bytes  [-0-9a-f]+  relocation from r1\n"
        OR NOT last_output MATCHES "  FULL VERIFY PASSED[_A-Z]*  r3  [0-9]+ bytes  [-0-9a-f]+  relocation from r2\n")
    message(FATAL_ERROR "history of a relocated archive:\n${last_output}")
endif()
run_cli("0" history "${moving_prefix}")
if(NOT last_output MATCHES "  r2  [0-9]+ 位元組  [-0-9a-f]+  從 r1 搬移\n")
    message(FATAL_ERROR "history of a relocated archive in zh-TW:\n${last_output}")
endif()
run_cli("0" history "${moving_prefix}" --json)
string(JSON relocation_runs LENGTH "${last_output}" runs)
string(JSON first_run GET "${last_output}" runs 0 run_id)
string(JSON first_relocation GET "${last_output}" runs 0 relocation run_id)
string(JSON first_from GET "${last_output}" runs 0 relocation from_revision)
string(JSON first_attempt GET "${last_output}" runs 0 relocation interrupted_attempt)
string(JSON second_from GET "${last_output}" runs 1 relocation from_revision)
if(NOT relocation_runs EQUAL 2 OR NOT first_relocation STREQUAL first_run OR NOT first_from EQUAL 1
        OR first_attempt OR NOT second_from EQUAL 2)
    message(FATAL_ERROR "history --json of a relocated archive:\n${last_output}")
endif()

# Damage: one changed byte, one deleted file, one new file, and the empty
# folder made a file (sample data) -> FAILED (exit 3). An item's detail is a
# code in --json and plain words in text, in either language (decision 0017,
# amended 2026-10-27).
file(WRITE "${ROOT}/readme.txt" "hellp\n")
file(REMOVE "${ROOT}/日本語/ファイル.txt")
file(WRITE "${ROOT}/added.txt" "new\n")
file(REMOVE_RECURSE "${ROOT}/empty")
file(WRITE "${ROOT}/empty" "a file now (sample data)\n")
run_cli("3" verify --full "${ROOT}" --json)
string(JSON item_count LENGTH "${last_output}" items)
set(type_change "")
math(EXPR last_item "${item_count} - 1")
foreach(index RANGE ${last_item})
    string(JSON item_status GET "${last_output}" items ${index} status)
    if(item_status STREQUAL "TypeChanged")
        string(JSON type_change GET "${last_output}" items ${index} detail)
    endif()
endforeach()
if(NOT item_count EQUAL 4 OR NOT type_change STREQUAL "d -> f")
    message(FATAL_ERROR "expected 4 reported items, a folder made a file among them:\n${last_output}")
endif()
run_cli("3" --lang en verify --full "${ROOT}")
string(FIND "${last_output}" "\n  TypeChanged  empty  (folder -> file)\n" type_change_words)
string(FIND "${last_output}" "\n  Modified  readme.txt  (content differs)\n" content_words)
run_cli("3" verify --full "${ROOT}")
string(FIND "${last_output}" "\n  類型改變  empty  （資料夾 → 檔案）\n" type_change_chinese)
if(type_change_words EQUAL -1 OR content_words EQUAL -1 OR type_change_chinese EQUAL -1)
    message(FATAL_ERROR "the details of the damaged items in text:\n${last_output}")
endif()

# History is chained and intact (R-18); listing works. The check says what
# it cannot tell, in either language, naming no version of the program
# (decision 0017).
run_cli("0" repo verify-history)
if(NOT last_output MATCHES "\n檢查歷程鏈可偵測已記錄的執行紀錄被修改或刪除，但偵測不到最新幾筆執行紀錄被刪除.*；能偵測這些情況的歷程簽章，這個版本尚未提供。\n")
    message(FATAL_ERROR "verify-history does not say what the chain cannot tell:\n${last_output}")
endif()
run_cli("0" --lang en repo verify-history)
if(NOT last_output MATCHES "\nChecking the chain detects a recorded run that was changed or removed, but not the removal of the newest runs, nor runs changed with the chain computed again; history signatures, which would detect those, are not part of this version[.]\n")
    message(FATAL_ERROR "verify-history does not say what the chain cannot tell:\n${last_output}")
endif()
run_cli("0" archive list)
# People see local time with its offset from UTC; --json keeps UTC.
if(NOT last_output MATCHES "[0-9]+-[0-9]+-[0-9]+ [0-9]+:[0-9]+:[0-9]+（UTC")
    message(FATAL_ERROR "archive list does not show local time with its offset:\n${last_output}")
endif()
run_cli("0" --lang en history "${archive_uuid}")
if(NOT last_output MATCHES "[0-9]+-[0-9]+-[0-9]+ [0-9]+:[0-9]+:[0-9]+ \\(UTC")
    message(FATAL_ERROR "history does not show local time with its offset:\n${last_output}")
endif()
# Verifications are no relocation's: nothing is said beside them.
if(last_output MATCHES "relocation")
    message(FATAL_ERROR "history marks a verification as a relocation's:\n${last_output}")
endif()
run_cli("0" history "${archive_uuid}" --json)
string(JSON finished GET "${last_output}" runs 0 finished)
if(NOT finished MATCHES "^[0-9]+-[0-9]+-[0-9]+T[0-9]+:[0-9]+:[0-9]+Z$")
    message(FATAL_ERROR "--json history is not in UTC: ${finished}")
endif()
string(JSON no_relocation TYPE "${last_output}" runs 0 relocation)
if(NOT no_relocation STREQUAL "NULL")
    message(FATAL_ERROR "--json history gives a verification a relocation:\n${last_output}")
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
# Decision 0037: a repository named with --repository for one command leaves
# the pin's record of the uuid alone, so the next plain command on the pinned
# repository warns of nothing.
set(GLOBAL --config "${WORK}/config.json")
run_cli("0" --repository "${WORK}/repo2" archive list)
run_cli("0" --lang en repo info)
if(NOT last_errors STREQUAL "")
    message(FATAL_ERROR "a command on another repository changed what the pinned one says:\n${last_errors}")
endif()
set(GLOBAL --config "${WORK}/config2.json")

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
# How matching sees the copy's folder: MATCH where its volume shows a device
# serial, MATCH_WEAK where it shows none (as in containers).
run_cli("0;2" verify --full "${COPY}" --json)
string(JSON copy_identity GET "${last_output}" identity result)
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
# Decision 0028, point 4: the copy's folder, plainly the copy's (a MATCH of
# it at its root), verifies meanwhile; with no device serial to compare
# (MATCH_WEAK), a clone of its volume could be the archive's, and it stops,
# naming the archive.
if(copy_identity STREQUAL "MATCH")
    run_cli("0;2" verify --full "${COPY}" --json)
    string(JSON copy_verified GET "${last_output}" archive_uuid)
    if(NOT copy_verified STREQUAL copy_uuid)
        message(FATAL_ERROR "the copy's folder while another archive's manifest cannot be used:\n${last_output}")
    endif()
else()
    run_cli("11" --lang en verify --full "${COPY}")
    string(FIND "${last_errors}" "this root may belong to archive ${archive_label} (${archive_uuid})" copy_stopped)
    if(copy_stopped EQUAL -1)
        message(FATAL_ERROR "the copy's folder (${copy_identity}) while another archive's manifest cannot be used:\n${last_errors}")
    endif()
endif()
run_cli("3" --lang en verify --full "${archive_uuid}" --json)
set(failed_json "${last_output}")
run_cli("3" verify --full "${archive_uuid}")
set(failed_chinese "${last_output}")
# The failure class in plain words in text, as it is in --json; and a
# command's failure without the class that leads it in --json (decision
# 0017, amended 2026-10-27).
string(FIND "${failed_chinese}" "\n  失敗：清單檔不是記錄中的那一個\n" failure_words)
run_cli("3" --lang en export "${archive_uuid}" --out "${WORK}/export-of-a-changed-manifest")
string(FIND "${last_errors}" "error: the manifest of revision 2 (manifests/${archive_uuid}/r0002.vam) has changed: " export_text)
string(FIND "${last_errors}" "ManifestHashMismatch" export_class)
run_cli("3" --lang en export "${archive_uuid}" --out "${WORK}/export-of-a-changed-manifest" --json)
string(JSON export_error GET "${last_output}" error)
if(failure_words EQUAL -1 OR NOT export_text EQUAL 0 OR NOT export_class EQUAL -1
        OR NOT export_error MATCHES "^ManifestHashMismatch: the manifest of revision 2 ")
    message(FATAL_ERROR "a failure class in text and in --json:\n${failed_chinese}\n${last_errors}\n${export_error}")
endif()
run_cli("0;2" verify --full "${copy_uuid}")
run_cli("0" archive show "${copy_prefix}")
run_cli("0" history "${copy_prefix}")
run_cli("0" history "${archive_prefix}" --json)
set(history_json "${last_output}")
run_cli("0" --lang en history "${archive_prefix}")
if(NOT last_output MATCHES "  FULL VERIFY FAILED  r2  [0-9]+ bytes  the manifest is not the one recorded  [-0-9a-f]+\n"
        OR last_output MATCHES "ManifestHashMismatch")
    message(FATAL_ERROR "history of a run that failed on its manifest:\n${last_output}")
endif()
file(RENAME "${RECORDED}.kept" "${RECORDED}")
set(changed_manifest "the manifest of revision 2 (manifests/${archive_uuid}/r0002.vam) has changed: its first line differs from what the repository recorded; put it back with `import <folder>/r0002.vam` from a backup of the repository, an export of revision 2, or the media copy in the archive's .veriarchive folder, where registration wrote one; if none of these holds it, register revision 3 from what the archive's root holds now with `register <root_path> --lost-manifest ${archive_uuid}`; if the device that held the archive is gone as well, retire the archive with `archive retire ${archive_uuid} --confirm`")
set(changed_chinese "版次 2 的清單檔（manifests/${archive_uuid}/r0002.vam）已被變更：其第一行與儲存庫所記錄的不同；請以 `import <folder>/r0002.vam` 從儲存庫的備份、版次 2 的匯出檔，或封存 .veriarchive 資料夾中註冊時寫入的媒體副本放回該清單檔；若這些都沒有，請以 `register <root_path> --lost-manifest ${archive_uuid}` 用封存根目錄目前的內容註冊版次 3；若存放此封存的裝置也已不在，請以 `archive retire ${archive_uuid} --confirm` 停用此封存")
set(root_may_belong "this root may belong to archive ${archive_label} (${archive_uuid}), whose manifest cannot be used: the manifest of revision 2 (manifests/${archive_uuid}/r0002.vam) has changed: its first line differs from what the repository recorded; verify that archive by its uuid (`verify --full ${archive_uuid}`) to record the failure, then put its manifest back with `import <folder>/r0002.vam` from a backup of the repository, an export of revision 2, or the media copy in the archive's .veriarchive folder, where registration wrote one; if none of these holds it, register revision 3 from what the archive's root holds now with `register <root_path> --lost-manifest ${archive_uuid}`; if the device that held the archive is gone as well, retire the archive with `archive retire ${archive_uuid} --confirm`")
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

# `repo rebuild-index` keeps the record of a revision whose manifest it cannot
# read, and the archive at that revision (decision 0028): the manifest of
# revision 2 of the archive is moved away while that of revision 1 is intact
# (sample data). The rebuild exits 2, naming the revision it kept as
# recorded, in either language and in --json. The archive stays at revision 2, never taken
# back to revision 1, and is handled as one whose manifest cannot be used:
# listed with the problem (exit 2), FAILED by its uuid at revision 2 without
# reading the media (exit 3), and its root stops (exit 11), naming it.
# Restored from its export with `import`, which puts back a file that is the
# one recorded and says so, the manifest verifies at revision 2 with no other
# rebuild; imported again, it is already present.
file(RENAME "${RECORDED}" "${RECORDED}.away")
run_cli("2" repo rebuild-index --json)
string(JSON rebuild_kept LENGTH "${last_output}" revisions_kept)
string(JSON kept_revision GET "${last_output}" revisions_kept 0 revision)
string(JSON kept_place GET "${last_output}" revisions_kept 0 manifest)
string(JSON kept_missing GET "${last_output}" revisions_kept 0 missing)
string(JSON kept_left_out LENGTH "${last_output}" revisions_left_out)
run_cli("2" repo rebuild-index)
string(FIND "${last_output}" "\n  保留原紀錄：${archive_label}（${archive_uuid}）：版次 2 的清單檔（manifests/${archive_uuid}/r0002.vam）已遺失\n" kept_chinese)
run_cli("2" --lang en repo rebuild-index)
string(FIND "${last_output}" "\n  kept as recorded: ${archive_label} (${archive_uuid}): the manifest of revision 2 (manifests/${archive_uuid}/r0002.vam) is missing\n" kept_english)
run_cli("2" archive show "${archive_prefix}" --json)
string(JSON skipped_revision GET "${last_output}" revision)
string(JSON skipped_kind GET "${last_output}" manifest_problem kind)
string(JSON skipped_message GET "${last_output}" manifest_problem message)
run_cli("3" verify --full "${archive_uuid}" --json)
string(JSON skipped_result GET "${last_output}" result)
string(JSON skipped_read GET "${last_output}" media_read)
string(JSON skipped_verified_revision GET "${last_output}" revision)
run_cli("11" --lang en verify --full "${ROOT}")
string(FIND "${last_errors}" "this root may belong to archive ${archive_label} (${archive_uuid}), whose manifest cannot be used: the manifest of revision 2 (manifests/${archive_uuid}/r0002.vam) is missing;" skipped_by_root)
run_cli("0" --lang en import "${WORK}/export2/r0002.vam")
string(FIND "${last_output}" "Restored the manifest of ${archive_label} (${archive_uuid}) revision 2: the file is the one the repository recorded." restored_said)
run_cli("0" import "${WORK}/export2/r0002.vam" --json)
string(JSON restored_again GET "${last_output}" restored)
string(JSON present_again GET "${last_output}" already_present)
file(READ "${RECORDED}" restored_bytes HEX)
file(READ "${RECORDED}.away" away_bytes HEX)
file(REMOVE "${RECORDED}.away")
if(restored_said EQUAL -1 OR restored_again OR NOT present_again OR NOT restored_bytes STREQUAL away_bytes)
    message(FATAL_ERROR "import of the export of a revision whose manifest is missing: said ${restored_said}, "
        "again restored ${restored_again} and present ${present_again}")
endif()
if(NOT rebuild_kept EQUAL 1 OR NOT kept_revision EQUAL 2 OR NOT kept_place STREQUAL "manifests/${archive_uuid}/r0002.vam"
        OR NOT kept_missing OR NOT kept_left_out EQUAL 0
        OR kept_chinese EQUAL -1 OR kept_english EQUAL -1 OR NOT skipped_revision EQUAL 2
        OR NOT skipped_kind STREQUAL "missing"
        OR NOT skipped_message STREQUAL "the manifest of revision 2 (manifests/${archive_uuid}/r0002.vam) is missing; put it back with `import <folder>/r0002.vam` from a backup of the repository, an export of revision 2, or the media copy in the archive's .veriarchive folder, where registration wrote one; if none of these holds it, register revision 3 from what the archive's root holds now with `register <root_path> --lost-manifest ${archive_uuid}`; if the device that held the archive is gone as well, retire the archive with `archive retire ${archive_uuid} --confirm`"
        OR NOT skipped_result STREQUAL "FAILED" OR skipped_read OR NOT skipped_verified_revision EQUAL 2
        OR skipped_by_root EQUAL -1)
    message(FATAL_ERROR "an archive whose latest manifest repo rebuild-index could not read: kept ${rebuild_kept}, "
        "revision ${skipped_revision}, kind ${skipped_kind}, ${skipped_message}; by uuid ${skipped_result} at "
        "revision ${skipped_verified_revision}, media read ${skipped_read}; by root:\n${last_errors}")
endif()
run_cli("0" archive list)
run_cli("0;2" verify --full "${ROOT}" --json)
string(JSON restored_revision GET "${last_output}" revision)
if(NOT restored_revision EQUAL 2)
    message(FATAL_ERROR "the restored manifest verifies at revision ${restored_revision}")
endif()

# The manifest of an earlier revision is read only where matching looks at
# the earlier carriers (decision 0004): with the first manifest of the
# relocated archive moved away (sample data), the archive verifies by its
# uuid and by its root, where its latest carrier matches. `archive show`
# reads the manifests of the earlier revisions too, and marks the one that
# is gone (exit 2), in text and in --json. `repo rebuild-index` leaves the
# record of that earlier revision out of the index, naming it (exit 2), in
# text and in --json; `archive show` then lists the other revisions alone.
# Put back, the manifest is indexed again by the next rebuild.
set(EARLIER "${WORK}/repo/manifests/${moving_uuid}/r0001.vam")
set(earlier_place "manifests/${moving_uuid}/r0001.vam")
file(RENAME "${EARLIER}" "${EARLIER}.away")
run_cli("0;2" verify --full "${moving_uuid}")
run_cli("0;2" verify --full "${MOVE_FROM}")
run_cli("2" --lang en archive show "${moving_prefix}")
string(FIND "${last_output}" "\n      problem: the manifest of revision 1 (${earlier_place}) is missing; put it back with `import <folder>/r0001.vam` from a backup of the repository, an export of revision 1, or the media copy in the archive's .veriarchive folder, where registration wrote one; if none of these holds it, leave revision 1 out of the index with `repo rebuild-index`\n" earlier_shown)
run_cli("2" archive show "${moving_prefix}" --json)
string(JSON earlier_kind GET "${last_output}" revisions 0 manifest_problem kind)
string(JSON earlier_problem_revision GET "${last_output}" revisions 0 manifest_problem revision)
run_cli("2" --lang en repo rebuild-index)
string(REGEX MATCH "\n  left out of the index: ARCH-[0-9]+ \\(${moving_uuid}\\): the manifest of revision 1 \\(${earlier_place}\\) is missing\n" left_out_said "${last_output}")
file(RENAME "${EARLIER}.away" "${EARLIER}")
run_cli("0" repo rebuild-index)
file(RENAME "${EARLIER}" "${EARLIER}.away")
run_cli("2" repo rebuild-index --json)
string(JSON left_out_count LENGTH "${last_output}" revisions_left_out)
string(JSON left_out_uuid GET "${last_output}" revisions_left_out 0 archive_uuid)
string(JSON left_out_revision GET "${last_output}" revisions_left_out 0 revision)
string(JSON left_out_missing GET "${last_output}" revisions_left_out 0 missing)
string(JSON left_out_kept LENGTH "${last_output}" revisions_kept)
run_cli("0" archive show "${moving_prefix}" --json)
string(JSON moving_revisions LENGTH "${last_output}" revisions)
file(RENAME "${EARLIER}.away" "${EARLIER}")
run_cli("0" repo rebuild-index)
run_cli("0" archive show "${moving_prefix}" --json)
string(JSON moving_revisions_back LENGTH "${last_output}" revisions)
if(earlier_shown EQUAL -1 OR NOT earlier_kind STREQUAL "missing" OR NOT earlier_problem_revision EQUAL 1
        OR NOT left_out_said OR NOT left_out_count EQUAL 1 OR NOT left_out_uuid STREQUAL moving_uuid
        OR NOT left_out_revision EQUAL 1 OR NOT left_out_missing OR NOT left_out_kept EQUAL 0
        OR NOT moving_revisions EQUAL 2 OR NOT moving_revisions_back EQUAL 3)
    message(FATAL_ERROR "an earlier revision whose manifest is gone: shown ${earlier_shown} (${earlier_kind}), "
        "left out ${left_out_count} (r${left_out_revision}, ${left_out_uuid}), revisions ${moving_revisions} then "
        "${moving_revisions_back}")
endif()

# Chunk lines (decision 0032), in a repository of their own. A new archive
# gets 64 MiB chunks: a pattern file a little over two of them, 128 MiB and 16
# bytes (sample data), has three chunk lines, which `register --json` counts
# and `archive show` gives with the chunk size and the Merkle root of each
# revision, as its manifest says. The golden manifests (sample data) are a
# revision without a chunk size, as V1 wrote them, and one with 64 KiB chunks.
file(WRITE "${WORK}/config4.json" "{\"language\": \"zh-TW\"}\n")
set(GLOBAL --config "${WORK}/config4.json")
run_cli("0" --repository "${WORK}/repo4" repo init)
set(LARGE "${WORK}/large")
# Written a chunk at a time, so that the test holds at most two in memory.
string(REPEAT "0123456789abcdef" 4194304 chunk_content)
file(WRITE "${LARGE}/sample.bin" "${chunk_content}")
file(APPEND "${LARGE}/sample.bin" "${chunk_content}")
file(APPEND "${LARGE}/sample.bin" "0123456789abcdef")
run_cli("0" register "${LARGE}" --json)
string(JSON large_uuid GET "${last_output}" archive_uuid)
string(JSON large_chunks GET "${last_output}" counts chunks)
run_cli("0" archive show "${large_uuid}" --json)
string(JSON large_chunk_size GET "${last_output}" revisions 0 chunk_size)
string(JSON large_chunk_count GET "${last_output}" revisions 0 chunks)
string(JSON large_root GET "${last_output}" revisions 0 merkle_root)
string(LENGTH "${large_root}" large_root_length)
if(NOT large_chunks EQUAL 3 OR NOT large_chunk_size EQUAL 67108864 OR NOT large_chunk_count EQUAL 3
        OR NOT large_root MATCHES "^[0-9a-f]+$" OR NOT large_root_length EQUAL 64)
    message(FATAL_ERROR "chunk lines of a file larger than 64 MiB:\n${last_output}")
endif()
run_cli("0" archive show "${large_uuid}")
if(NOT last_output MATCHES "\n    r1  [^\n]*r0001\\.vam\n      區塊大小 64 MiB，3 個區塊，總碼 ${large_root}\n")
    message(FATAL_ERROR "archive show of a revision with chunk lines:\n${last_output}")
endif()
run_cli("0;2" verify --full "${large_uuid}")
# A FULL verify that adds chunk hashes (decision 0033) is refused for an
# archive whose revision has them, before it reads anything (exit 12), in
# either language and in --json; no run is recorded.
run_cli("0" history "${large_uuid}" --json)
string(JSON runs_before LENGTH "${last_output}" runs)
run_cli("12" --lang en verify --full "${large_uuid}" --add-chunk-hashes)
set(has_them "revision 1 of archive ${large_uuid} has chunk hashes already; a FULL verify compares them, and there are none to add")
if(NOT last_errors MATCHES "${has_them}")
    message(FATAL_ERROR "verify --add-chunk-hashes of an archive with chunk hashes:\n${last_errors}")
endif()
run_cli("12" verify --full "${LARGE}" --add-chunk-hashes)
if(NOT last_errors MATCHES "封存 ${large_uuid} 的版次 1 已有區塊雜湊；完整驗證會比對它們，沒有可加入的區塊雜湊")
    message(FATAL_ERROR "verify --add-chunk-hashes of an archive with chunk hashes, in Chinese:\n${last_errors}")
endif()
run_cli("12" verify --full "${large_uuid}" --add-chunk-hashes --json)
string(JSON refused_error GET "${last_output}" error)
run_cli("0" history "${large_uuid}" --json)
string(JSON runs_after LENGTH "${last_output}" runs)
if(NOT refused_error STREQUAL has_them OR NOT runs_after EQUAL runs_before)
    message(FATAL_ERROR "verify --add-chunk-hashes --json of an archive with chunk hashes:\n${refused_error}\n${last_output}")
endif()
# Nothing to abandon: no verification that adds chunk hashes to it is
# unfinished (exit 12), with or without --confirm, in either language.
run_cli("12" --lang en archive abandon-verification "${large_uuid}")
if(NOT last_errors MATCHES "archive ${large_uuid} has no unfinished verification that adds chunk hashes")
    message(FATAL_ERROR "archive abandon-verification with nothing to abandon:\n${last_errors}")
endif()
run_cli("12" archive abandon-verification "${large_uuid}" --confirm)
if(NOT last_errors MATCHES "封存 ${large_uuid} 沒有加入區塊雜湊的未完成驗證")
    message(FATAL_ERROR "archive abandon-verification --confirm with nothing to abandon, in Chinese:\n${last_errors}")
endif()

# One byte changed in chunk 1, 16000 bytes into it (sample data): a FULL
# verify fails (exit 3) and names that chunk of the three and the byte where
# it begins, in --json and in either language (decision 0032).
string(REPEAT "0123456789abcdef" 1000 before_change)
string(REPEAT "0123456789abcdef" 4193303 after_change)
file(WRITE "${LARGE}/sample.bin" "${chunk_content}")
unset(chunk_content)
file(APPEND "${LARGE}/sample.bin" "${before_change}X123456789abcdef${after_change}")
unset(after_change)
file(APPEND "${LARGE}/sample.bin" "0123456789abcdef")
file(SIZE "${LARGE}/sample.bin" changed_size)
if(NOT changed_size EQUAL 134217744)
    message(FATAL_ERROR "the changed sample is ${changed_size} bytes long")
endif()
run_cli("3" verify --full "${large_uuid}" --json)
string(JSON item_count LENGTH "${last_output}" items)
string(JSON changed_path GET "${last_output}" items 0 p)
string(JSON changed_status GET "${last_output}" items 0 status)
string(JSON changed_detail GET "${last_output}" items 0 detail)
string(JSON differing_count LENGTH "${last_output}" items 0 differing_chunks)
string(JSON differing_chunk GET "${last_output}" items 0 differing_chunks 0)
string(JSON file_chunks GET "${last_output}" items 0 chunks)
string(JSON differing_offset GET "${last_output}" items 0 first_differing_offset)
if(NOT item_count EQUAL 1 OR NOT changed_path STREQUAL "sample.bin" OR NOT changed_status STREQUAL "Modified"
        OR NOT changed_detail STREQUAL "content differs: chunk 1 of 3 differs, from byte 67,108,864"
        OR NOT differing_count EQUAL 1 OR NOT differing_chunk EQUAL 1 OR NOT file_chunks EQUAL 3
        OR NOT differing_offset EQUAL 67108864)
    message(FATAL_ERROR "verify --json of a file with one changed chunk:\n${last_output}")
endif()
run_cli("3" --lang en verify --full "${large_uuid}")
if(NOT last_output MATCHES "\n  Modified  sample\\.bin  \\(content differs: chunk 1 of 3 differs, from byte 67,108,864\\)\n")
    message(FATAL_ERROR "verify of a file with one changed chunk, in English:\n${last_output}")
endif()
run_cli("3" verify --full "${large_uuid}")
if(NOT last_output MATCHES "\n  已修改  sample\\.bin  （內容不同：共 3 個區塊，其中區塊 1 不同，從位元組 67,108,864 開始）\n")
    message(FATAL_ERROR "verify of a file with one changed chunk, in Chinese:\n${last_output}")
endif()
file(REMOVE_RECURSE "${LARGE}")

set(GOLDEN "${CMAKE_CURRENT_LIST_DIR}/../golden")
run_cli("0" import "${GOLDEN}/golden_r0002.vam")
run_cli("0" import "${GOLDEN}/golden_chunks_r0003.vam")
set(golden_uuid "28ff645e-a220-4aac-ae0b-6c328bf1a2b3")
set(golden_root "4f3f8bde47f06d709c9cabfa80f703e95f895b071409fec0509f58d6fb1eccbc")
run_cli("0" archive show "${golden_uuid}" --json)
string(JSON v1_chunk_size GET "${last_output}" revisions 0 chunk_size)
string(JSON v1_root_type TYPE "${last_output}" revisions 0 merkle_root)
string(JSON chunked_size GET "${last_output}" revisions 1 chunk_size)
string(JSON chunked_count GET "${last_output}" revisions 1 chunks)
string(JSON chunked_root GET "${last_output}" revisions 1 merkle_root)
if(NOT v1_chunk_size EQUAL 0 OR NOT v1_root_type STREQUAL "NULL" OR NOT chunked_size EQUAL 65536
        OR NOT chunked_count EQUAL 8 OR NOT chunked_root STREQUAL golden_root)
    message(FATAL_ERROR "archive show --json of revisions with and without chunk lines:\n${last_output}")
endif()
run_cli("0" archive show "${golden_uuid}")
if(NOT last_output MATCHES "\n    r2  [^\n]*r0002\\.vam\n      區塊大小 0：只有整個檔案的雜湊\n    r3  [^\n]*r0003\\.vam\n      區塊大小 64 KiB，8 個區塊，總碼 ${golden_root}\n")
    message(FATAL_ERROR "archive show of revisions with and without chunk lines:\n${last_output}")
endif()
run_cli("0" --lang en archive show "${golden_uuid}")
if(NOT last_output MATCHES "\n      chunk size 0: whole-file hashes only\n    r3  [^\n]*\n      chunk size 64 KiB, 8 chunks, Merkle root ${golden_root}\n")
    message(FATAL_ERROR "archive show in English of revisions with and without chunk lines:\n${last_output}")
endif()

# A registration or relocation made for an older revision never records
# (decision 0030). The archive of a folder (sample data) is registered in the
# first repository and imported into the second. In the second, a relocation
# against revision 1 to a copy of the folder, and then a registration of
# revision 2, each stop at their record while a folder stands where the
# manifest of revision 2 belongs, keeping what they did. The first repository
# records revisions 2 and 3, and the second imports revision 3. Then each
# resume, the folder's registration and a relocation without --resume are
# refused (exit 12), saying that the archive has a later revision and how to
# abandon them, and `archive show` shows each in a state of its own. The
# archive stays at revision 3, which verifies; abandoned, both are gone.
set(GLOBAL --config "${WORK}/config.json")
set(LATER "${WORK}/later")
set(LATER_COPY "${WORK}/later-copy")
file(WRITE "${LATER}/a.txt" "later sample a\n")
file(WRITE "${LATER}/b.txt" "later sample b\n")
file(COPY "${LATER}/" DESTINATION "${LATER_COPY}")
run_cli("0" register "${LATER}" --json)
string(JSON later_uuid GET "${last_output}" archive_uuid)
string(SUBSTRING "${later_uuid}" 0 8 later_prefix)
run_cli("0" export "${later_prefix}" --out "${WORK}/export-later1")
set(GLOBAL --config "${WORK}/config2.json")
run_cli("0" import "${WORK}/export-later1/r0001.vam")
set(BLOCKER "${WORK}/repo2/manifests/${later_uuid}/r0002.vam")
file(MAKE_DIRECTORY "${BLOCKER}")
run_cli("11" archive relocate "${later_prefix}" "${LATER_COPY}")
run_cli("11" register "${LATER}" --new-revision)
file(REMOVE_RECURSE "${BLOCKER}")
set(GLOBAL --config "${WORK}/config.json")
run_cli("0;2" register "${LATER}" --new-revision)
run_cli("0;2" register "${LATER}" --new-revision)
run_cli("0" export "${later_prefix}" --out "${WORK}/export-later3")
set(GLOBAL --config "${WORK}/config2.json")
run_cli("0" import "${WORK}/export-later3/r0003.vam")
set(later_registration "would create revision 2, but the archive now has a later revision (3); the registration can no longer finish: abandon it with `archive abandon-registration ${later_uuid}`")
set(later_relocation "verifies revision 1, but the archive now has a later revision (3); the relocation can no longer finish: abandon it with `archive abandon-relocation ${later_uuid}`, then relocate again")
foreach(refused IN ITEMS "register;${LATER};--resume" "register;${LATER};--new-revision"
        "archive;relocate;${later_prefix};${LATER_COPY};--resume" "archive;relocate;${later_prefix};${LATER_COPY}")
    run_cli("12" --lang en ${refused})
    if(refused MATCHES "^register")
        string(FIND "${last_errors}" "${later_registration}" said)
    else()
        string(FIND "${last_errors}" "${later_relocation}" said)
    endif()
    if(said EQUAL -1)
        message(FATAL_ERROR "${refused} made for an older revision:\n${last_errors}")
    endif()
endforeach()
# Both show in a state of their own, to be abandoned: in --json, and as the
# line under the archive.
run_cli("0" archive show "${later_prefix}" --json)
string(JSON later_revision GET "${last_output}" revision)
string(JSON registration_state GET "${last_output}" unfinished_registration state)
string(JSON relocation_state GET "${last_output}" unfinished_relocation state)
if(NOT later_revision EQUAL 3 OR NOT registration_state STREQUAL "later_revision"
        OR NOT relocation_state STREQUAL "later_revision")
    message(FATAL_ERROR "an archive whose operations were made for an older revision:\n${last_output}")
endif()
run_cli("0" --lang en archive show "${later_prefix}")
if(NOT last_output MATCHES "\n  Unfinished registration of revision 2 from [^\n]*; archive has a later revision; abandon it\n"
        OR NOT last_output MATCHES "\n  Unfinished relocation of revision 1 to [^\n]*; archive has a later revision; abandon it\n")
    message(FATAL_ERROR "archive show of operations made for an older revision:\n${last_output}")
endif()
run_cli("0;2" verify --full "${later_uuid}")
run_cli("0" archive abandon-registration "${later_prefix}" --confirm)
run_cli("0" archive abandon-relocation "${later_prefix}" --confirm)
run_cli("0" archive show "${later_prefix}" --json)
if(last_output MATCHES "unfinished_")
    message(FATAL_ERROR "an abandoned registration or relocation is still unfinished:\n${last_output}")
endif()
set(GLOBAL --config "${WORK}/config.json")

# A media copy restores a manifest as an export does (decision 0028): a folder
# (sample data) registered twice with a media copy, whose manifest of
# revision 2 is then gone from the repository and the index rebuilt. `import`
# of the copy on the media puts it back, saying so in Chinese, and the
# archive verifies at revision 2.
set(MEDIA "${WORK}/media-sample")
file(WRITE "${MEDIA}/a.txt" "media sample a\n")
run_cli("0" register "${MEDIA}" --write-media-copy --json)
string(JSON media_uuid GET "${last_output}" archive_uuid)
string(JSON media_label GET "${last_output}" label)
file(WRITE "${MEDIA}/b.txt" "media sample b\n")
run_cli("0;2" register "${MEDIA}" --new-revision --write-media-copy)
file(REMOVE "${WORK}/repo/manifests/${media_uuid}/r0002.vam")
run_cli("2" repo rebuild-index)
run_cli("0" import "${MEDIA}/.veriarchive/r0002.vam")
string(FIND "${last_output}" "已還原 ${media_label}（${media_uuid}）版次 2 的清單檔：此檔案就是儲存庫所記錄的那一份。" media_restored)
run_cli("0;2" verify --full "${media_uuid}" --json)
string(JSON media_result GET "${last_output}" result)
string(JSON media_revision GET "${last_output}" revision)
if(media_restored EQUAL -1 OR NOT media_result MATCHES "^PASSED" OR NOT media_revision EQUAL 2)
    message(FATAL_ERROR "import of a media copy of a revision whose manifest is missing: ${media_result} at "
        "revision ${media_revision}")
endif()

# A registration over a lost manifest (decision 0028, point 8), of the same
# sample folder at revision 2. While its manifest can be used, it is refused
# (exit 12). With the manifest of revision 2 gone from the repository, `archive
# list` says how to put it back and names the last resort, and the
# registration over it is refused while the folder's .veriarchive holds the
# media copy that `import` puts back, naming the import. With that copy moved
# away, not confirmed, it says what it would register and changes nothing (exit
# 12), in either language and in --json. Confirmed, it registers revision 3
# from the folder, leaving revision 2 out of the index: the archive verifies
# at revision 3, is listed without a problem, and its history chain is intact.
string(SUBSTRING "${media_uuid}" 0 8 media_prefix)
run_cli("12" --lang en register "${MEDIA}" --lost-manifest "${media_prefix}")
string(FIND "${last_errors}" "error: the manifest of revision 2 of archive ${media_uuid} can be used" media_usable)
file(REMOVE "${WORK}/repo/manifests/${media_uuid}/r0002.vam")
run_cli("2" --lang en archive list)
string(FIND "${last_output}" "or the media copy in the archive's .veriarchive folder, where registration wrote one; if none of these holds it, register revision 3 from what the archive's root holds now with `register <root_path> --lost-manifest ${media_uuid}`; if the device that held the archive is gone as well, retire the archive with `archive retire ${media_uuid} --confirm`\n" media_last_resort)
run_cli("12" --lang en register "${MEDIA}" --lost-manifest "${media_prefix}")
string(FIND "${last_errors}" "error: the media copy at this root holds the manifest of revision 2 of archive ${media_uuid}: put it back with `import " media_import_named)
file(RENAME "${MEDIA}/.veriarchive/r0002.vam" "${WORK}/media-copy-r0002.vam")
run_cli("12" --lang en register "${MEDIA}" --lost-manifest "${media_prefix}")
set(media_asked "${last_output}")
set(media_asked_error "${last_errors}")
run_cli("12" register "${MEDIA}" --lost-manifest "${media_prefix}")
set(media_asked_chinese "${last_output}${last_errors}")
run_cli("12" register "${MEDIA}" --lost-manifest "${media_prefix}" --json)
string(JSON asked_revision GET "${last_output}" registration revision)
string(JSON asked_lost GET "${last_output}" registration lost_revision)
string(JSON asked_kind GET "${last_output}" registration manifest_problem kind)
# No other archive's root overlaps this one on a volume that may be a clone,
# nor is this one another archive's very root.
string(JSON asked_clones LENGTH "${last_output}" registration possible_clones)
string(JSON asked_same LENGTH "${last_output}" registration same_root)
run_cli("2" archive show "${media_prefix}" --json)
string(JSON asked_unchanged GET "${last_output}" revision)
run_cli("0" register "${MEDIA}" --lost-manifest "${media_prefix}" --confirm --json)
string(JSON over_revision GET "${last_output}" revision)
string(JSON over_lost GET "${last_output}" lost_manifest revision)
string(JSON over_left_out GET "${last_output}" lost_manifest left_out)
run_cli("0" archive list)
run_cli("0" archive show "${media_prefix}" --json)
string(JSON media_revisions LENGTH "${last_output}" revisions)
string(JSON media_first GET "${last_output}" revisions 0 revision)
string(JSON media_latest GET "${last_output}" revisions 1 revision)
run_cli("0;2" verify --full "${media_uuid}" --json)
string(JSON over_verified GET "${last_output}" result)
string(JSON over_verified_revision GET "${last_output}" revision)
run_cli("0" repo verify-history)
if(media_usable EQUAL -1 OR media_last_resort EQUAL -1 OR media_import_named EQUAL -1
        OR NOT media_asked MATCHES "^Lost manifest of ARCH-[0-9]+ \\(${media_uuid}\\), revision 2\n  the manifest of revision 2 \\(manifests/${media_uuid}/r0002[.]vam\\) is missing\n  root: [^\n]+\nRegistering revision 3 from this root takes what it holds now as the archive's new baseline, without comparing it with revision 2, whose manifest is lost: damage since revision 2 was registered would not be found[.]"
        OR NOT media_asked_error STREQUAL "error: run the command again with --confirm to register revision 3\n"
        OR NOT media_asked_chinese MATCHES "^無法取回的清單檔：ARCH-[0-9]+（${media_uuid}），版次 2\n"
        OR NOT media_asked_chinese MATCHES "錯誤：若要註冊版次 3，請加上 --confirm 再執行一次\n$"
        OR NOT asked_revision EQUAL 3 OR NOT asked_lost EQUAL 2 OR NOT asked_kind STREQUAL "missing"
        OR NOT asked_clones EQUAL 0 OR NOT asked_same EQUAL 0 OR NOT asked_unchanged EQUAL 2 OR NOT over_revision EQUAL 3 OR NOT over_lost EQUAL 2 OR NOT over_left_out
        OR NOT media_revisions EQUAL 2 OR NOT media_first EQUAL 1 OR NOT media_latest EQUAL 3
        OR NOT over_verified MATCHES "^PASSED" OR NOT over_verified_revision EQUAL 3)
    message(FATAL_ERROR "a registration over a lost manifest: usable ${media_usable}, last resort ${media_last_resort}, "
        "import named ${media_import_named}, asked r${asked_revision} over r${asked_lost} (${asked_kind}), archive at "
        "r${asked_unchanged}; registered r${over_revision} over r${over_lost}, left out ${over_left_out}, revisions "
        "${media_revisions} (r${media_first}, r${media_latest}), verified ${over_verified} at r${over_verified_revision}"
        "\n${media_asked}\n${media_asked_error}\n${media_asked_chinese}")
endif()

# Decision 0028, point 9: the device is gone as well. A sample folder,
# registered, whose manifest is then gone from the repository: `archive list`
# exits 2, and registering another sample folder stops on it (exit 11).
# `archive retire` without --confirm says what retiring it means and changes
# nothing (exit 12), in either language and in --json. With --confirm it
# retires the archive: `archive list` and `archive show` exit 0 and show it
# retired with its manifest lost (`manifest_lost` in --json), the other folder
# registers, and a registration over the lost manifest is refused (exit 12),
# as a retired archive takes no new revision.
set(GONE "${WORK}/gone-sample")
set(AFTER_GONE "${WORK}/after-gone-sample")
file(WRITE "${GONE}/a.txt" "gone sample a\n")
file(WRITE "${AFTER_GONE}/b.txt" "after gone sample b\n")
run_cli("0" register "${GONE}" --json)
string(JSON gone_uuid GET "${last_output}" archive_uuid)
string(JSON gone_label GET "${last_output}" label)
string(SUBSTRING "${gone_uuid}" 0 8 gone_prefix)
file(REMOVE "${WORK}/repo/manifests/${gone_uuid}/r0001.vam")
run_cli("2" archive list)
run_cli("11" register "${AFTER_GONE}")
run_cli("12" --lang en archive retire "${gone_prefix}")
set(retire_asked "${last_output}")
set(retire_asked_error "${last_errors}")
run_cli("12" archive retire "${gone_prefix}")
set(retire_asked_chinese "${last_output}${last_errors}")
run_cli("12" archive retire "${gone_prefix}" --json)
string(JSON retire_asked_revision GET "${last_output}" retirement revision)
string(JSON retire_asked_kind GET "${last_output}" retirement manifest_problem kind)
run_cli("2" archive show "${gone_prefix}" --json)
string(JSON retire_unchanged GET "${last_output}" status)
run_cli("0" archive retire "${gone_prefix}" --confirm --json)
string(JSON retired_status GET "${last_output}" status)
string(JSON retired_lost GET "${last_output}" manifest_lost)
run_cli("0" --lang en archive list)
set(retired_listed "${last_output}")
run_cli("0" archive list)
set(retired_listed_chinese "${last_output}")
run_cli("0" archive show "${gone_prefix}" --json)
string(JSON shown_status GET "${last_output}" status)
string(JSON shown_lost GET "${last_output}" manifest_lost)
run_cli("0" register "${AFTER_GONE}")
run_cli("12" --lang en register "${GONE}" --lost-manifest "${gone_prefix}" --confirm)
set(retired_refused "${last_errors}")
set(gone_manifest "manifests/${gone_uuid}/r0001.vam")
string(FIND "${retired_listed}" "\n    manifest lost: the manifest of revision 1 (${gone_manifest}) is missing; put it back with `import <folder>/r0001.vam` from a backup of the repository, an export of revision 1, or the media copy in the archive's .veriarchive folder, where registration wrote one\n" lost_line)
string(FIND "${retired_listed_chinese}" "\n    清單檔無法取回：版次 1 的清單檔（${gone_manifest}）已遺失；請以 `import <folder>/r0001.vam` 從儲存庫的備份、版次 1 的匯出檔，或封存 .veriarchive 資料夾中註冊時寫入的媒體副本放回該清單檔\n" lost_line_chinese)
if(NOT retire_asked MATCHES "^Lost manifest of ${gone_label} \\(${gone_uuid}\\), revision 1\n  the manifest of revision 1 \\(manifests/${gone_uuid}/r0001[.]vam\\) is missing\nRetiring the archive declares that manifest lost for good: "
        OR NOT retire_asked MATCHES "retired, it is no longer taken as a possible owner of one, as identity matching cannot use its latest manifest[.]\n$"
        OR NOT retire_asked_error STREQUAL "error: run the command again with --confirm to retire the archive\n"
        OR NOT retire_asked_chinese MATCHES "^無法取回的清單檔：${gone_label}（${gone_uuid}），版次 1\n"
        OR NOT retire_asked_chinese MATCHES "錯誤：若要停用此封存，請加上 --confirm 再執行一次\n$"
        OR NOT retire_asked_revision EQUAL 1 OR NOT retire_asked_kind STREQUAL "missing"
        OR NOT retire_unchanged STREQUAL "REGISTERED"
        OR NOT retired_status STREQUAL "RETIRED" OR NOT retired_lost
        OR NOT retired_listed MATCHES "\n${gone_label}  ${gone_prefix}  r1  RETIRED\n"
        OR lost_line EQUAL -1 OR lost_line_chinese EQUAL -1
        OR NOT shown_status STREQUAL "RETIRED" OR NOT shown_lost
        OR NOT retired_refused STREQUAL "error: a retired archive takes no new revisions\n")
    message(FATAL_ERROR "retiring an archive whose manifest is lost: asked r${retire_asked_revision} "
        "(${retire_asked_kind}), status ${retire_unchanged}; retired ${retired_status} (lost ${retired_lost}), "
        "shown ${shown_status} (lost ${shown_lost}), line ${lost_line} / ${lost_line_chinese}"
        "\n${retire_asked}\n${retire_asked_error}\n${retire_asked_chinese}\n${retired_listed}\n"
        "${retired_listed_chinese}\n${retired_refused}")
endif()

# Decision 0028, point 9: the lost manifest of a retired archive behind an
# index that seems to be all that is wrong with it, in a repository of its
# own (sample data). A sample folder is registered, and a file under the name
# of its manifest of revision 2 reads as that manifest by its first line,
# while its body does not match its header (sample damage, as a file that
# only bears a manifest's name). `repo rebuild-index` cannot read it, and
# sets the archive at revision 2, of which the index has no record (exit 2).
# Line 1 alone says only that the index has no record of it, so registering
# another sample folder stops (exit 11), naming the rebuild. Retired with
# --confirm, as the whole manifest cannot be read, the archive stops nothing:
# the other folder registers, another archive relocates, and `archive list`
# exits 0, with the line "manifest lost:" and what the whole manifest says.
# Where the sqlite3 command is installed, the same for a recorded manifest
# whose body changed and whose copy of the header in the index differs
# (sample damage): line 1 alone says only that the index differs from it.
file(WRITE "${WORK}/config4.json" "{\"language\": \"zh-TW\"}\n")
set(GLOBAL --config "${WORK}/config4.json")
run_cli("0" --repository "${WORK}/repo4" repo init)
set(BEHIND "${WORK}/behind-index-sample")
set(BEHIND_OTHER "${WORK}/behind-index-other-sample")
set(BEHIND_MOVE "${WORK}/behind-index-move-sample")
set(BEHIND_MOVED "${WORK}/behind-index-moved-sample")
file(WRITE "${BEHIND}/a.txt" "behind the index sample a\n")
file(WRITE "${BEHIND_OTHER}/b.txt" "behind the index sample b\n")
file(WRITE "${BEHIND_MOVE}/c.txt" "behind the index sample c\n")
file(COPY "${BEHIND_MOVE}/" DESTINATION "${BEHIND_MOVED}")
run_cli("0" register "${BEHIND}" --json)
string(JSON behind_uuid GET "${last_output}" archive_uuid)
string(SUBSTRING "${behind_uuid}" 0 8 behind_prefix)
run_cli("0" register "${BEHIND_MOVE}" --json)
string(JSON behind_moving_uuid GET "${last_output}" archive_uuid)
set(behind_folder "${WORK}/repo4/manifests/${behind_uuid}")
file(READ "${behind_folder}/r0001.vam" behind_bytes)
string(REPLACE "\"revision\":1," "\"revision\":2,\"supersedes\":1," behind_bytes "${behind_bytes}")
string(REPLACE "\"p\":\"a.txt\"" "\"p\":\"q.txt\"" behind_bytes "${behind_bytes}")
# Written byte for byte (file(WRITE) may write other line ends; the sample
# manifests hold no "@").
file(CONFIGURE OUTPUT "${behind_folder}/r0002.vam" CONTENT "${behind_bytes}" @ONLY NEWLINE_STYLE UNIX)
run_cli("2" --lang en repo rebuild-index)
set(behind_rebuilt "${last_output}")
run_cli("11" --lang en register "${BEHIND_OTHER}")
set(behind_stopped "${last_errors}")
run_cli("12" --lang en archive retire "${behind_prefix}")
set(behind_asked "${last_output}")
run_cli("0" archive retire "${behind_prefix}" --confirm --json)
string(JSON behind_retired_lost GET "${last_output}" manifest_lost)
run_cli("0" --lang en archive list)
set(behind_listed "${last_output}")
run_cli("0" register "${BEHIND_OTHER}")
run_cli("0;2" archive relocate "${behind_moving_uuid}" "${BEHIND_MOVED}" --json)
string(JSON behind_relocated GET "${last_output}" revision)
set(behind_manifest "the manifest of revision 2 (manifests/${behind_uuid}/r0002.vam)")
string(FIND "${behind_rebuilt}" "not in the index: " behind_not_indexed)
string(FIND "${behind_stopped}" "error: the repository's index has no record of ${behind_manifest}; rebuild the index with `repo rebuild-index`\n" behind_stop_line)
string(FIND "${behind_asked}" "\n  the repository's index has no record of ${behind_manifest}, which cannot be read: manifest body does not match its header\n" behind_asked_line)
string(FIND "${behind_listed}" "\n    manifest lost: the repository's index has no record of ${behind_manifest}, which cannot be read: manifest body does not match its header; put it back with `import <folder>/r0002.vam` from a backup of the repository, an export of revision 2, or the media copy in the archive's .veriarchive folder, where registration wrote one\n" behind_lost_line)
if(behind_not_indexed EQUAL -1 OR behind_stop_line EQUAL -1 OR behind_asked_line EQUAL -1 OR NOT behind_retired_lost
        OR behind_lost_line EQUAL -1 OR NOT behind_relocated EQUAL 2)
    message(FATAL_ERROR "a retired archive whose lost manifest the index has no record of: rebuilt ${behind_not_indexed}, "
        "stopped ${behind_stop_line}, asked ${behind_asked_line}, retired lost ${behind_retired_lost}, listed "
        "${behind_lost_line}, relocated r${behind_relocated}\n${behind_rebuilt}\n${behind_stopped}\n${behind_asked}\n"
        "${behind_listed}")
endif()
find_program(SQLITE3_PROGRAM sqlite3)
if(SQLITE3_PROGRAM)
    set(DIFFERS "${WORK}/behind-differs-sample")
    set(DIFFERS_OTHER "${WORK}/behind-differs-other-sample")
    file(WRITE "${DIFFERS}/a.txt" "behind the index sample a, the index differs\n")
    file(WRITE "${DIFFERS_OTHER}/b.txt" "behind the index sample b, the index differs\n")
    run_cli("0" register "${DIFFERS}" --json)
    string(JSON differs_uuid GET "${last_output}" archive_uuid)
    string(SUBSTRING "${differs_uuid}" 0 8 differs_prefix)
    set(differs_file "${WORK}/repo4/manifests/${differs_uuid}/r0001.vam")
    file(READ "${differs_file}" differs_bytes)
    string(REPLACE "\"p\":\"a.txt\"" "\"p\":\"q.txt\"" differs_bytes "${differs_bytes}")
    file(CONFIGURE OUTPUT "${differs_file}" CONTENT "${differs_bytes}" @ONLY NEWLINE_STYLE UNIX)
    execute_process(COMMAND "${SQLITE3_PROGRAM}" "${WORK}/repo4/veriarchive.db"
        "UPDATE archive_revisions SET header_json = header_json || ' ' WHERE archive_uuid = '${differs_uuid}'"
        RESULT_VARIABLE differs_damaged)
    run_cli("11" --lang en register "${DIFFERS_OTHER}")
    set(differs_stopped "${last_errors}")
    run_cli("12" --lang en archive retire "${differs_prefix}")
    set(differs_asked "${last_output}")
    run_cli("0" archive retire "${differs_prefix}" --confirm)
    run_cli("0" --lang en archive list)
    set(differs_listed "${last_output}")
    run_cli("0" register "${DIFFERS_OTHER}")
    set(differs_manifest "the manifest of revision 1 (manifests/${differs_uuid}/r0001.vam)")
    string(FIND "${differs_stopped}" "error: the repository's index differs from ${differs_manifest}; rebuild the index with `repo rebuild-index`\n" differs_stop_line)
    string(FIND "${differs_asked}" "\n  ${differs_manifest} has changed: its body does not match its header\n" differs_asked_line)
    string(FIND "${differs_listed}" "\n    manifest lost: ${differs_manifest} has changed: its body does not match its header; put it back with `import <folder>/r0001.vam`" differs_lost_line)
    if(NOT differs_damaged EQUAL 0 OR differs_stop_line EQUAL -1 OR differs_asked_line EQUAL -1
            OR differs_lost_line EQUAL -1)
        message(FATAL_ERROR "a retired archive whose lost manifest the index differs from: damaged ${differs_damaged}, "
            "stopped ${differs_stop_line}, asked ${differs_asked_line}, listed ${differs_lost_line}\n"
            "${differs_stopped}\n${differs_asked}\n${differs_listed}")
    endif()
    message(STATUS "a retired archive whose lost manifest the index differs from: checked with ${SQLITE3_PROGRAM}")
else()
    message(STATUS "a retired archive whose lost manifest the index differs from: skipped, as the sqlite3 command that "
        "changes the index (sample damage) is not installed")
endif()
# A manifest changed only in its body (sample damage) keeps its archive a
# candidate of matching, which reads the first line: retiring the archive
# changes nothing of matching, so `archive retire` retires it at once (exit
# 0), as any archive, saying that it is still recognised when attached. A
# verification of its folder by its root still matches it, and fails on the
# body (exit 3); registering its folder needs a decision (exit 12).
set(BODY "${WORK}/body-only-sample")
file(WRITE "${BODY}/a.txt" "body only sample a\n")
run_cli("0" register "${BODY}" --json)
string(JSON body_uuid GET "${last_output}" archive_uuid)
string(SUBSTRING "${body_uuid}" 0 8 body_prefix)
set(body_file "${WORK}/repo4/manifests/${body_uuid}/r0001.vam")
file(READ "${body_file}" body_bytes)
string(REPLACE "\"p\":\"a.txt\"" "\"p\":\"q.txt\"" body_bytes "${body_bytes}")
file(CONFIGURE OUTPUT "${body_file}" CONTENT "${body_bytes}" @ONLY NEWLINE_STYLE UNIX)
run_cli("0" --lang en archive retire "${body_prefix}")
set(body_retired "${last_output}")
run_cli("3" verify --full "${BODY}" --json)
string(JSON body_verified GET "${last_output}" archive_uuid)
string(JSON body_result GET "${last_output}" result)
run_cli("12" --lang en register "${BODY}")
set(body_registered "${last_errors}")
if(NOT body_retired MATCHES "^Retired [^\n]+ \\(${body_uuid}\\)[.] It is no longer expected to be verified, is still recognised when attached, and keeps its history[.]\n$"
        OR NOT body_verified STREQUAL body_uuid OR NOT body_result STREQUAL "FAILED"
        OR NOT body_registered MATCHES "^error: already registered as archive ")
    message(FATAL_ERROR "retiring an archive whose manifest changed only in its body: verified ${body_verified} "
        "(${body_result})\n${body_retired}\n${body_registered}")
endif()
# A registration over the lost manifest of an archive retired since, as a
# version before decision 0028, point 9 let one begin, can never finish.
# Where the sqlite3 command is installed: a sample folder registered, its
# manifest gone, and a registration over it stopped as its manifest cannot be
# written (a folder stands at its place, sample data); the archive then
# retired as an older version retired it, without looking at its
# registrations (sample damage of its record). `archive show` gives the
# registration the state archive_retired in --json and its line in text, its
# resume is refused (exit 12), and it is abandoned.
if(SQLITE3_PROGRAM)
    set(OLDER "${WORK}/older-registration-sample")
    file(WRITE "${OLDER}/a.txt" "older registration sample a\n")
    run_cli("0" register "${OLDER}" --json)
    string(JSON older_uuid GET "${last_output}" archive_uuid)
    string(SUBSTRING "${older_uuid}" 0 8 older_prefix)
    file(REMOVE "${WORK}/repo4/manifests/${older_uuid}/r0001.vam")
    set(OLDER_BLOCKER "${WORK}/repo4/manifests/${older_uuid}/r0002.vam")
    file(MAKE_DIRECTORY "${OLDER_BLOCKER}")
    run_cli("11" register "${OLDER}" --lost-manifest "${older_prefix}" --confirm)
    file(REMOVE_RECURSE "${OLDER_BLOCKER}")
    execute_process(COMMAND "${SQLITE3_PROGRAM}" "${WORK}/repo4/veriarchive.db"
        "UPDATE archives SET status = 'RETIRED' WHERE archive_uuid = '${older_uuid}'"
        RESULT_VARIABLE older_retired)
    run_cli("0" archive show "${older_prefix}" --json)
    string(JSON older_state GET "${last_output}" unfinished_registration state)
    run_cli("0" --lang en archive show "${older_prefix}")
    set(older_shown "${last_output}")
    run_cli("12" --lang en register "${OLDER}" --resume)
    set(older_resumed "${last_errors}")
    run_cli("0" archive abandon-registration "${older_prefix}" --confirm)
    if(NOT older_retired EQUAL 0 OR NOT older_state STREQUAL "archive_retired"
            OR NOT older_shown MATCHES "\n  Unfinished registration of revision 2 from [^\n]*; archive is retired; abandon it\n"
            OR NOT older_resumed MATCHES "is retired and takes no new revisions: abandon its unfinished registration")
        message(FATAL_ERROR "an older version's registration over the lost manifest of a retired archive: retired "
            "${older_retired}, state ${older_state}\n${older_shown}\n${older_resumed}")
    endif()
endif()
set(GLOBAL --config "${WORK}/config.json")

# Decision 0028, point 8 (operator, 2026-10-17): the same sample folder
# registered on purpose as two archives (--force-new-archive), A and its twin
# B. With A's manifest gone, the registration over it takes the folder, B's
# root: not confirmed, it names B as the archive whose root it is
# (`same_root`, and its line in either language), with the match: MATCH where
# the volume shows a device serial, MATCH_WEAK where it shows none (in
# containers). Confirmed, it registers A's revision 2 from the folder, after
# which a verification of the folder by its root matches both archives (exit
# 12).
set(TWIN "${WORK}/twin-sample")
file(WRITE "${TWIN}/a.txt" "twin sample a\n")
run_cli("0" register "${TWIN}" --json)
string(JSON twin_a GET "${last_output}" archive_uuid)
run_cli("0" register "${TWIN}" --force-new-archive --json)
string(JSON twin_b GET "${last_output}" archive_uuid)
string(JSON twin_b_label GET "${last_output}" label)
file(REMOVE "${WORK}/repo/manifests/${twin_a}/r0001.vam")
run_cli("12" register "${TWIN}" --lost-manifest "${twin_a}" --json)
string(JSON twin_same LENGTH "${last_output}" registration same_root)
string(JSON twin_clones LENGTH "${last_output}" registration possible_clones)
set(twin_named "")
set(twin_same_match "")
if(twin_same EQUAL 1)
    string(JSON twin_named GET "${last_output}" registration same_root 0 archive_uuid)
    string(JSON twin_same_match GET "${last_output}" registration same_root 0 match)
endif()
run_cli("12" --lang en register "${TWIN}" --lost-manifest "${twin_a}")
set(twin_asked "${last_output}")
run_cli("12" register "${TWIN}" --lost-manifest "${twin_a}")
set(twin_asked_chinese "${last_output}")
if(twin_same_match STREQUAL "MATCH")
    string(FIND "${twin_asked}" "\n  the same root as archive ${twin_b_label} (${twin_b}): this root is that archive's, on the volume and the device that its latest manifest records; confirm only where this very folder was registered on purpose as both that archive and this one (--force-new-archive)\n" twin_line)
    string(FIND "${twin_asked_chinese}" "\n  與封存 ${twin_b_label}（${twin_b}）的根目錄相同：此根目錄是該封存的根目錄，位於它最新的清單檔所記錄的磁碟區與裝置上；請只在這個資料夾當初刻意同時註冊為該封存與此封存（--force-new-archive）時才確認\n" twin_line_chinese)
else()
    string(FIND "${twin_asked}" "\n  the same root as archive ${twin_b_label} (${twin_b}) on a volume that this one matches only as MATCH_WEAK: this volume may be a sector-by-sector clone of that archive's, or that archive's own; confirm only where this very folder was registered on purpose as both that archive and this one (--force-new-archive), or where this volume is not that archive's\n" twin_line)
    string(FIND "${twin_asked_chinese}" "\n  與封存 ${twin_b_label}（${twin_b}）的根目錄相同，而此磁碟區與該封存的磁碟區比對只得到「弱相符」：此磁碟區可能是該封存的磁碟區逐磁區複製出來的複本，也可能就是該封存自己的磁碟區；請只在這個資料夾當初刻意同時註冊為該封存與此封存（--force-new-archive），或此磁碟區不是該封存的磁碟區時才確認\n" twin_line_chinese)
endif()
run_cli("0" register "${TWIN}" --lost-manifest "${twin_a}" --confirm --json)
string(JSON twin_revision GET "${last_output}" revision)
string(JSON twin_registered GET "${last_output}" archive_uuid)
run_cli("12" verify --full "${TWIN}" --json)
string(JSON twin_match GET "${last_output}" error)
if(NOT twin_same EQUAL 1 OR NOT twin_clones EQUAL 0 OR NOT twin_named STREQUAL twin_b
        OR NOT twin_same_match MATCHES "^(MATCH|MATCH_WEAK)$" OR twin_line EQUAL -1 OR twin_line_chinese EQUAL -1
        OR NOT twin_revision EQUAL 2 OR NOT twin_registered STREQUAL twin_a
        OR NOT twin_match MATCHES "^AMBIGUOUS: the media matches several archives")
    message(FATAL_ERROR "a registration over the lost manifest of a twin: same root ${twin_same} (${twin_same_match}), "
        "clones ${twin_clones}, named ${twin_named} (B ${twin_b}); registered ${twin_registered} r${twin_revision}; by "
        "root ${twin_match}\n${twin_asked}\n${twin_asked_chinese}")
endif()

# The same on a volume that matches the twins only weakly, in a repository of
# its own (sample data): the same sample folder registered as A and its twin
# B, whose manifest is then written as on a volume that shows no device serial
# (its first line without the serial and the WWN of its devices, sample data)
# and indexed again by `repo rebuild-index`. With A's manifest gone, the
# registration over it takes the folder, naming B as the archive whose root it
# is on a volume that this one matches only as MATCH_WEAK, in --json and in
# its line in either language; confirmed, it registers A's revision 2.
file(WRITE "${WORK}/config5.json" "{\"language\": \"zh-TW\"}\n")
set(GLOBAL --config "${WORK}/config5.json")
run_cli("0" --repository "${WORK}/repo5" repo init)
set(WEAK_TWIN "${WORK}/weak-twin-sample")
file(WRITE "${WEAK_TWIN}/a.txt" "weak twin sample a\n")
run_cli("0" register "${WEAK_TWIN}" --json)
string(JSON weak_a GET "${last_output}" archive_uuid)
run_cli("0" register "${WEAK_TWIN}" --force-new-archive --json)
string(JSON weak_b GET "${last_output}" archive_uuid)
string(JSON weak_b_label GET "${last_output}" label)
set(weak_b_file "${WORK}/repo5/manifests/${weak_b}/r0001.vam")
file(READ "${weak_b_file}" weak_b_bytes)
string(FIND "${weak_b_bytes}" "\n" weak_line_end)
string(SUBSTRING "${weak_b_bytes}" 0 ${weak_line_end} weak_b_line)
string(SUBSTRING "${weak_b_bytes}" ${weak_line_end} -1 weak_b_body)
string(REGEX REPLACE "\"serial\":\"[^\"]*\"" "\"serial\":\"\"" weak_b_line "${weak_b_line}")
string(REGEX REPLACE "\"wwn\":\"[^\"]*\"" "\"wwn\":\"\"" weak_b_line "${weak_b_line}")
file(CONFIGURE OUTPUT "${weak_b_file}" CONTENT "${weak_b_line}${weak_b_body}" @ONLY NEWLINE_STYLE UNIX)
file(REMOVE "${WORK}/repo5/manifests/${weak_a}/r0001.vam")
run_cli("2" repo rebuild-index)
run_cli("12" register "${WEAK_TWIN}" --lost-manifest "${weak_a}" --json)
string(JSON weak_same LENGTH "${last_output}" registration same_root)
string(JSON weak_clones LENGTH "${last_output}" registration possible_clones)
set(weak_named "")
set(weak_match "")
if(weak_same EQUAL 1)
    string(JSON weak_named GET "${last_output}" registration same_root 0 archive_uuid)
    string(JSON weak_match GET "${last_output}" registration same_root 0 match)
endif()
run_cli("12" --lang en register "${WEAK_TWIN}" --lost-manifest "${weak_a}")
set(weak_asked "${last_output}")
run_cli("12" register "${WEAK_TWIN}" --lost-manifest "${weak_a}")
set(weak_asked_chinese "${last_output}")
string(FIND "${weak_asked}" "\n  the same root as archive ${weak_b_label} (${weak_b}) on a volume that this one matches only as MATCH_WEAK: this volume may be a sector-by-sector clone of that archive's, or that archive's own; confirm only where this very folder was registered on purpose as both that archive and this one (--force-new-archive), or where this volume is not that archive's\n" weak_line)
string(FIND "${weak_asked_chinese}" "\n  與封存 ${weak_b_label}（${weak_b}）的根目錄相同，而此磁碟區與該封存的磁碟區比對只得到「弱相符」：此磁碟區可能是該封存的磁碟區逐磁區複製出來的複本，也可能就是該封存自己的磁碟區；請只在這個資料夾當初刻意同時註冊為該封存與此封存（--force-new-archive），或此磁碟區不是該封存的磁碟區時才確認\n" weak_line_chinese)
run_cli("0" register "${WEAK_TWIN}" --lost-manifest "${weak_a}" --confirm --json)
string(JSON weak_revision GET "${last_output}" revision)
string(JSON weak_registered GET "${last_output}" archive_uuid)
if(NOT weak_same EQUAL 1 OR NOT weak_clones EQUAL 0 OR NOT weak_named STREQUAL weak_b
        OR NOT weak_match STREQUAL "MATCH_WEAK" OR weak_line EQUAL -1 OR weak_line_chinese EQUAL -1
        OR NOT weak_revision EQUAL 2 OR NOT weak_registered STREQUAL weak_a)
    message(FATAL_ERROR "a registration over the lost manifest of a twin on a volume matched weakly: same root "
        "${weak_same} (${weak_match}), clones ${weak_clones}, named ${weak_named} (B ${weak_b}); registered "
        "${weak_registered} r${weak_revision}\n${weak_asked}\n${weak_asked_chinese}")
endif()
set(GLOBAL --config "${WORK}/config.json")

# Decision 0022, point 7 (operator, 2026-10-20): `register <root> --resume` of
# a registration that an older version left, which may still run it, says
# what happens where that version still runs it and stops (exit 12), changing
# nothing, until it is run again with --confirm. Where the sqlite3 command is
# installed, in a repository of its own (sample data): three sample folders,
# each registration stopped as its manifest cannot be written (a file stands
# where the manifests folder belongs, sample data), the plan of one rewritten
# as a version before decision 0022 wrote it, without "updated", its stop
# mark and its process (sample damage). Another, this version's, resumes at
# once, without the question, as scripts resume it, and so does the third,
# whose plan names its process but lost "updated" and its stop mark (sample
# damage), as a version since decision 0024 left one where every write of
# its plan after the first failed; the older version's is asked about in
# either language and in --json, its plan unchanged, and then resumed with
# --confirm. --confirm with neither --resume nor --lost-manifest is a usage
# error (exit 10).
if(SQLITE3_PROGRAM)
    file(WRITE "${WORK}/config6.json" "{\"language\": \"zh-TW\"}\n")
    set(GLOBAL --config "${WORK}/config6.json")
    run_cli("0" --repository "${WORK}/repo6" repo init)
    set(OLDER_RESUME "${WORK}/older-resume-sample")
    set(CURRENT_RESUME "${WORK}/current-resume-sample")
    set(NAMED_RESUME "${WORK}/named-resume-sample")
    file(WRITE "${OLDER_RESUME}/a.txt" "older resume sample a\n")
    file(WRITE "${OLDER_RESUME}/b.txt" "older resume sample b\n")
    file(WRITE "${CURRENT_RESUME}/a.txt" "current resume sample a\n")
    file(WRITE "${NAMED_RESUME}/a.txt" "named resume sample a\n")
    file(RENAME "${WORK}/repo6/manifests" "${WORK}/repo6/manifests.away")
    file(WRITE "${WORK}/repo6/manifests" "not a folder (sample data)\n")
    run_cli("11" register "${OLDER_RESUME}")
    run_cli("0" archive list --json)
    string(JSON older_resume_uuid GET "${last_output}" archives 0 archive_uuid)
    string(JSON older_resume_label GET "${last_output}" archives 0 label)
    run_cli("11" register "${CURRENT_RESUME}")
    run_cli("11" register "${NAMED_RESUME}")
    file(REMOVE "${WORK}/repo6/manifests")
    file(RENAME "${WORK}/repo6/manifests.away" "${WORK}/repo6/manifests")
    set(older_plan_key "registration:${older_resume_uuid}")
    execute_process(COMMAND "${SQLITE3_PROGRAM}" "${WORK}/repo6/veriarchive.db"
        "SELECT value FROM settings WHERE key = '${older_plan_key}'"
        OUTPUT_VARIABLE older_plan OUTPUT_STRIP_TRAILING_WHITESPACE)
    foreach(member IN ITEMS updated stopped process)
        string(JSON older_plan REMOVE "${older_plan}" ${member})
    endforeach()
    string(REPLACE "'" "''" older_plan_sql "${older_plan}")
    execute_process(COMMAND "${SQLITE3_PROGRAM}" "${WORK}/repo6/veriarchive.db"
        "UPDATE settings SET value = '${older_plan_sql}' WHERE key = '${older_plan_key}'"
        RESULT_VARIABLE older_resume_damaged)
    execute_process(COMMAND "${SQLITE3_PROGRAM}" "${WORK}/repo6/veriarchive.db"
        "SELECT value FROM settings WHERE key = '${older_plan_key}'"
        OUTPUT_VARIABLE older_plan_written OUTPUT_STRIP_TRAILING_WHITESPACE)
    execute_process(COMMAND "${SQLITE3_PROGRAM}" "${WORK}/repo6/veriarchive.db"
        "SELECT key FROM settings WHERE key LIKE 'registration:%' AND value LIKE '%named-resume-sample%'"
        OUTPUT_VARIABLE named_plan_key OUTPUT_STRIP_TRAILING_WHITESPACE)
    execute_process(COMMAND "${SQLITE3_PROGRAM}" "${WORK}/repo6/veriarchive.db"
        "SELECT value FROM settings WHERE key = '${named_plan_key}'"
        OUTPUT_VARIABLE named_plan OUTPUT_STRIP_TRAILING_WHITESPACE)
    foreach(member IN ITEMS updated stopped)
        string(JSON named_plan REMOVE "${named_plan}" ${member})
    endforeach()
    string(REPLACE "'" "''" named_plan_sql "${named_plan}")
    execute_process(COMMAND "${SQLITE3_PROGRAM}" "${WORK}/repo6/veriarchive.db"
        "UPDATE settings SET value = '${named_plan_sql}' WHERE key = '${named_plan_key}'"
        RESULT_VARIABLE named_resume_damaged)
    execute_process(COMMAND "${SQLITE3_PROGRAM}" "${WORK}/repo6/veriarchive.db"
        "SELECT value FROM settings WHERE key = '${named_plan_key}'"
        OUTPUT_VARIABLE named_plan_written OUTPUT_STRIP_TRAILING_WHITESPACE)
    # This version's plan: resumed at once, as before.
    run_cli("0" --lang en register "${CURRENT_RESUME}" --resume --json)
    string(JSON current_resume_revision GET "${last_output}" revision)
    set(current_resume_errors "${last_errors}")
    # This version's plan that names its process but never says when it
    # worked: resumed at once as well.
    run_cli("0" --lang en register "${NAMED_RESUME}" --resume --json)
    string(JSON named_resume_revision GET "${last_output}" revision)
    set(named_resume_errors "${last_errors}")
    # The older version's: asked about, in English, Chinese and --json.
    run_cli("12" --lang en register "${OLDER_RESUME}" --resume)
    set(older_asked "${last_output}")
    set(older_asked_errors "${last_errors}")
    run_cli("12" register "${OLDER_RESUME}" --resume)
    set(older_asked_chinese "${last_output}")
    set(older_asked_chinese_errors "${last_errors}")
    run_cli("12" register "${OLDER_RESUME}" --resume --json)
    string(JSON older_json_error GET "${last_output}" error)
    string(JSON older_json_uuid GET "${last_output}" registration archive_uuid)
    string(JSON older_json_label GET "${last_output}" registration label)
    string(JSON older_json_revision GET "${last_output}" registration revision)
    string(JSON older_json_root GET "${last_output}" registration root)
    execute_process(COMMAND "${SQLITE3_PROGRAM}" "${WORK}/repo6/veriarchive.db"
        "SELECT value FROM settings WHERE key = '${older_plan_key}'"
        OUTPUT_VARIABLE older_plan_asked OUTPUT_STRIP_TRAILING_WHITESPACE)
    # Confirmed, it is resumed and recorded.
    run_cli("0" register "${OLDER_RESUME}" --resume --confirm --json)
    string(JSON older_resumed_uuid GET "${last_output}" archive_uuid)
    string(JSON older_resumed_revision GET "${last_output}" revision)
    run_cli("10" --lang en register "${CURRENT_RESUME}" --confirm)
    set(confirm_alone "${last_errors}")
    string(FIND "${older_asked}" "Unfinished registration of ${older_resume_label} (${older_resume_uuid}), revision 1\n  root: " older_header)
    string(FIND "${older_asked}" "older-resume-sample\n  started: " older_root_line)
    string(FIND "${older_asked}" "\nAn older version of VeriArchive left this registration unfinished. That version does not record when it last worked, so VeriArchive cannot tell whether it has stopped or is still reading a large file or scanning the folder or volume. If it is still running, it will later record its own manifest over the one that this resume records: from then on every verification of the archive fails, and other registrations and relocations stop until the archive gets a new revision. Make sure that the older version's window or command has been closed before you resume.\n" older_warning)
    string(FIND "${older_asked_chinese}" "${older_resume_label}（${older_resume_uuid}）未完成的註冊，版次 1\n  根目錄：" older_header_chinese)
    string(FIND "${older_asked_chinese}" "\n舊版 VeriArchive 留下了這個未完成的註冊。舊版不會記錄它最後一次運作的時間，所以程式無法判斷它已經停止，還是仍在讀取一個大檔案、或掃描資料夾或磁碟區。如果它仍在執行，之後會用自己的清單檔蓋掉這次繼續所記錄的清單檔：從此這個封存的每次驗證都會失敗，其他的註冊與搬移也都會停下來，直到這個封存有了新的版次為止。繼續之前，請先確認舊版的視窗或命令都已關閉。\n" older_warning_chinese)
    if(NOT older_resume_damaged EQUAL 0 OR older_plan_written MATCHES "\"(updated|stopped|process)\""
            OR NOT current_resume_revision EQUAL 1 OR current_resume_errors MATCHES "--confirm|older version"
            OR NOT named_resume_damaged EQUAL 0 OR named_plan_written MATCHES "\"(updated|stopped)\""
            OR NOT named_plan_written MATCHES "\"process\"" OR NOT named_resume_revision EQUAL 1
            OR named_resume_errors MATCHES "--confirm|older version"
            OR older_header EQUAL -1 OR older_root_line EQUAL -1 OR older_warning EQUAL -1
            OR NOT older_asked_errors STREQUAL "error: run the command again with --confirm to resume it\n"
            OR older_header_chinese EQUAL -1 OR older_warning_chinese EQUAL -1
            OR NOT older_asked_chinese_errors STREQUAL "錯誤：若要繼續，請加上 --confirm 再執行一次\n"
            OR NOT older_json_error STREQUAL "run the command again with --confirm to resume it"
            OR NOT older_json_uuid STREQUAL older_resume_uuid OR NOT older_json_label STREQUAL older_resume_label
            OR NOT older_json_revision EQUAL 1 OR NOT older_json_root MATCHES "older-resume-sample$"
            OR NOT older_plan_asked STREQUAL older_plan_written
            OR NOT older_resumed_uuid STREQUAL older_resume_uuid OR NOT older_resumed_revision EQUAL 1
            OR NOT confirm_alone STREQUAL "error: --confirm is used only with --lost-manifest or --resume\n")
        message(FATAL_ERROR "a resume of an older version's registration: damaged ${older_resume_damaged}, current "
            "r${current_resume_revision}, named r${named_resume_revision} (damaged ${named_resume_damaged}: "
            "${named_plan_written}; ${named_resume_errors}), header ${older_header}/${older_header_chinese}, root "
            "${older_root_line}, "
            "warning ${older_warning}/${older_warning_chinese}; json ${older_json_error} ${older_json_uuid} "
            "${older_json_label} r${older_json_revision} ${older_json_root}; resumed ${older_resumed_uuid} "
            "r${older_resumed_revision}\n${older_plan_written}\n${older_plan_asked}\n${current_resume_errors}\n"
            "${older_asked}\n${older_asked_errors}\n${older_asked_chinese}\n${older_asked_chinese_errors}\n"
            "${confirm_alone}")
    endif()
    message(STATUS "a resume of an older version's registration: checked with ${SQLITE3_PROGRAM}")
else()
    message(STATUS "a resume of an older version's registration: skipped, as the sqlite3 command that rewrites "
        "the plan (sample damage) is not installed")
endif()
set(GLOBAL --config "${WORK}/config.json")
# Decision 0032: the first open of a repository of an older schema version
# keeps a copy of its database beside it, then upgrades it, and the CLI says
# once, on stderr, also with --json, where the copy is. A repository of
# sample data is lowered to version 1, as V1 left its databases, and later to
# version 2, with the sqlite3 program (SQLITE3), where there is one.
if(SQLITE3)
    set(OLDER "${WORK}/older-repository")
    function(lower_schema version)
        execute_process(
            COMMAND "${SQLITE3}" "${OLDER}/veriarchive.db"
                "DROP TABLE IF EXISTS verify_progress_chunks; UPDATE schema_version SET version = ${version};"
            RESULT_VARIABLE code
            ERROR_VARIABLE errors)
        if(NOT code EQUAL 0)
            message(FATAL_ERROR "sqlite3 could not lower the sample repository to version ${version}:\n${errors}")
        endif()
    endfunction()
    run_cli("0" --repository "${OLDER}" repo init)
    lower_schema(1)
    # The copy is named for the local date, the clock's here, which near
    # midnight is not the UTC date: the one before the open or after it.
    string(TIMESTAMP before_open "%Y-%m-%d")
    run_cli("0" --repository "${OLDER}" --lang en repo info)
    string(TIMESTAMP after_open "%Y-%m-%d")
    set(upgraded "note: the repository database was upgraded from schema version 1 to 3, which earlier versions of VeriArchive cannot open; a copy of it from before the upgrade is kept at ")
    string(FIND "${last_errors}" "${upgraded}" said)
    if(said EQUAL -1 OR NOT last_errors MATCHES "veriarchive-schema-1-before-upgrade-(${before_open}|${after_open})\\.db\n")
        message(FATAL_ERROR "the first open of a repository of schema version 1:\n${last_errors}")
    endif()
    file(GLOB older_copies "${OLDER}/veriarchive-schema-1-before-upgrade-*.db")
    list(LENGTH older_copies older_copy_count)
    execute_process(COMMAND "${SQLITE3}" "${older_copies}" "SELECT version FROM schema_version;"
        OUTPUT_VARIABLE copy_version OUTPUT_STRIP_TRAILING_WHITESPACE)
    if(NOT older_copy_count EQUAL 1 OR NOT copy_version STREQUAL "1")
        message(FATAL_ERROR "the copy before the upgrade: ${older_copies} at version ${copy_version}")
    endif()
    # Said once: the next open has nothing to upgrade.
    run_cli("0" --repository "${OLDER}" --lang en repo info)
    if(last_errors MATCHES "note:")
        message(FATAL_ERROR "an upgraded repository opened again:\n${last_errors}")
    endif()
    # In Chinese, beside --json output, which stays one document.
    lower_schema(2)
    string(TIMESTAMP before_open "%Y-%m-%d")
    run_cli("0" --repository "${OLDER}" archive list --json)
    string(TIMESTAMP after_open "%Y-%m-%d")
    string(JSON listed_archives LENGTH "${last_output}" archives)
    string(FIND "${last_errors}" "附註：儲存庫資料庫已從結構描述版本 2 升級到 3，較早版本的 VeriArchive 無法再開啟它；升級前的資料庫副本保存在 " said)
    if(said EQUAL -1 OR NOT last_errors MATCHES "veriarchive-schema-2-before-upgrade-(${before_open}|${after_open})\\.db\n"
            OR NOT listed_archives EQUAL 0)
        message(FATAL_ERROR "the first open of a repository of schema version 2, with --json:\n${last_output}\n${last_errors}")
    endif()
    # A version 1 database without one of its tables (a table that V1 created
    # dropped from a sample repository) is damaged: the open names the rebuild,
    # upgrades nothing and keeps no copy (decisions 0032 and 0036), also the
    # next time.
    set(INCOMPLETE "${WORK}/older-incomplete")
    run_cli("0" --repository "${INCOMPLETE}" repo init)
    execute_process(
        COMMAND "${SQLITE3}" "${INCOMPLETE}/veriarchive.db"
            "DROP TABLE verify_progress_chunks; DROP TABLE trusted_keys; UPDATE schema_version SET version = 1;"
        RESULT_VARIABLE code
        ERROR_VARIABLE errors)
    if(NOT code EQUAL 0)
        message(FATAL_ERROR "sqlite3 could not make the sample repository incomplete:\n${errors}")
    endif()
    foreach(attempt first next)
        run_cli("11" --repository "${INCOMPLETE}" --lang en repo info)
        file(GLOB incomplete_copies "${INCOMPLETE}/veriarchive-schema-*")
        execute_process(COMMAND "${SQLITE3}" "${INCOMPLETE}/veriarchive.db" "SELECT version FROM schema_version;"
            OUTPUT_VARIABLE incomplete_version OUTPUT_STRIP_TRAILING_WHITESPACE)
        if(NOT last_errors MATCHES "error: the repository's database [^\n]*veriarchive\\.db is damaged; make it again from the manifests with `repo rebuild-index`\n"
                OR last_errors MATCHES "note:" OR incomplete_copies OR NOT incomplete_version STREQUAL "1")
            message(FATAL_ERROR "the ${attempt} open of a version 1 database without one of its tables "
                "(schema version ${incomplete_version}, copies: ${incomplete_copies}):\n${last_errors}")
        endif()
    endforeach()
    # Said also when the open fails after it upgraded the database, before the
    # error: on Windows, where a read-only file cannot be replaced, when the
    # config file cannot be written as the open records the uuid of the
    # repository it pins (it records another repository's: sample data).
    if(CMAKE_HOST_WIN32)
        set(LOCKED "${WORK}/older-locked")
        set(LOCKED_CONFIG "${WORK}/locked-config.json")
        file(WRITE "${LOCKED_CONFIG}" "{}\n")
        set(GLOBAL --config "${LOCKED_CONFIG}")
        run_cli("0" --repository "${LOCKED}" repo init)
        execute_process(
            COMMAND "${SQLITE3}" "${LOCKED}/veriarchive.db"
                "DROP TABLE verify_progress_chunks; UPDATE schema_version SET version = 1;"
            RESULT_VARIABLE code
            ERROR_VARIABLE errors)
        if(NOT code EQUAL 0)
            message(FATAL_ERROR "sqlite3 could not lower the sample repository to version 1:\n${errors}")
        endif()
        file(WRITE "${LOCKED_CONFIG}"
            "{\"repository\": \"${LOCKED}\", \"last_repository_uuid\": \"c0ffee00-0000-4000-8000-000000000099\"}\n")
        file(CHMOD "${LOCKED_CONFIG}" PERMISSIONS OWNER_READ GROUP_READ WORLD_READ)
        string(TIMESTAMP before_open "%Y-%m-%d")
        execute_process(
            COMMAND "${CLI}" ${GLOBAL} --lang en repo info
            ENCODING UTF-8
            RESULT_VARIABLE code
            OUTPUT_VARIABLE output
            ERROR_VARIABLE errors)
        string(TIMESTAMP after_open "%Y-%m-%d")
        file(CHMOD "${LOCKED_CONFIG}" PERMISSIONS OWNER_READ OWNER_WRITE GROUP_READ WORLD_READ)
        set(GLOBAL --config "${WORK}/config.json")
        if(NOT code EQUAL 11 OR NOT errors MATCHES "note: the repository database was upgraded from schema version 1 to 3, which earlier versions of VeriArchive cannot open; a copy of it from before the upgrade is kept at [^\n]*veriarchive-schema-1-before-upgrade-(${before_open}|${after_open})\\.db\nerror: cannot record repository_uuid in config: ")
            message(FATAL_ERROR "an open that could not record the repository's uuid after the upgrade: exit ${code}\n${errors}")
        endif()
    endif()
else()
    message(STATUS "no sqlite3 program: the upgrade of an older repository is left to the unit tests")
endif()

# R-66: a fixture tree registered on this OS gives the golden manifest.
include("${CMAKE_CURRENT_LIST_DIR}/golden_tree.cmake")
# Decision 0010 (Windows): a file that could not be read at registration and
# can be read now is Unbaselined, and the report of a run whose content
# otherwise matched says that a new revision takes a baseline for it, naming
# the root, in either language (sample data). Linux CI runs as root, which
# reads any file, so this part runs on Windows.
if(CMAKE_HOST_WIN32)
    set(UNBASELINED_ROOT "${WORK}/unbaselined-SAMPLE")
    file(WRITE "${UNBASELINED_ROOT}/SAMPLE-locked.txt" "locked at registration\n")
    file(WRITE "${UNBASELINED_ROOT}/SAMPLE-open.txt" "open\n")
    file(TO_NATIVE_PATH "${UNBASELINED_ROOT}/SAMPLE-locked.txt" locked_file)
    # The registration runs without run_cli, whose failure would end the test
    # while the file is still denied, which the next run could not remove.
    execute_process(COMMAND icacls "${locked_file}" /deny "*S-1-1-0:(R)" RESULT_VARIABLE denied OUTPUT_QUIET ERROR_QUIET)
    execute_process(COMMAND "${CLI}" ${GLOBAL} register "${UNBASELINED_ROOT}" ENCODING UTF-8
        RESULT_VARIABLE registered OUTPUT_VARIABLE registered_output ERROR_VARIABLE registered_errors)
    execute_process(COMMAND icacls "${locked_file}" /remove:d "*S-1-1-0" RESULT_VARIABLE allowed OUTPUT_QUIET ERROR_QUIET)
    if(NOT denied EQUAL 0 OR NOT allowed EQUAL 0 OR NOT registered EQUAL 2)
        message(FATAL_ERROR "a registration with a file that cannot be read: deny ${denied}, allow ${allowed}, "
            "exit ${registered}\n${registered_output}\n${registered_errors}")
    endif()
    run_cli("2" --lang en verify --full "${UNBASELINED_ROOT}")
    set(unbaselined_english "${last_output}")
    run_cli("2" verify --full "${UNBASELINED_ROOT}")
    if(NOT unbaselined_english MATCHES "\n  Unbaselined  SAMPLE-locked\\.txt  "
            OR NOT unbaselined_english MATCHES "\n  The Unbaselined entries could not be read when the archive was registered, so they have no baseline yet\\. To take one for them, register a new revision of the archive: veriarchive-cli register (\"[^\"\n]*unbaselined-SAMPLE\"|[^ \"\n]*unbaselined-SAMPLE) --new-revision\n"
            OR NOT last_output MATCHES "\n  這些「無基準」項目在註冊此封存時無法讀取，所以還沒有基準。若要為它們建立基準，請註冊此封存的新版次：veriarchive-cli register (\"[^\"\n]*unbaselined-SAMPLE\"|[^ \"\n]*unbaselined-SAMPLE) --new-revision\n")
        message(FATAL_ERROR "the advice for Unbaselined entries:\n${unbaselined_english}\n${last_output}")
    endif()
endif()

# R-111: --io hdd|ssd chooses how the media is read, and where it is not
# given, the config's io.rotational_override does by device serial; --io
# takes hdd or ssd only, and an override other than "hdd" or "ssd" makes the
# config invalid (exit 11). Sample data, sample serials.
set(IO_ROOT "${WORK}/io-SAMPLE")
file(WRITE "${IO_ROOT}/SAMPLE-1.txt" "one\n")
file(WRITE "${IO_ROOT}/SAMPLE-2.txt" "two\n")
run_cli("0" register "${IO_ROOT}" --io ssd)
run_cli("0;2" verify --full "${IO_ROOT}" --io hdd)
run_cli("10" --lang en verify --full "${IO_ROOT}" --io fast)
set(io_refused "${last_errors}")
file(WRITE "${WORK}/config-io.json"
    "{\"language\": \"en\", \"io\": {\"rotational_override\": {\"SAMPLE-SERIAL-1\": \"ssd\", \"SAMPLE-SERIAL-2\": \"hdd\"}}}\n")
set(GLOBAL --config "${WORK}/config-io.json" --repository "${WORK}/repo")
run_cli("0;2" verify --full "${IO_ROOT}")
set(io_verified "${last_output}")
file(WRITE "${WORK}/config-io-bad.json" "{\"io\": {\"rotational_override\": {\"SAMPLE-SERIAL-1\": \"fast\"}}}\n")
set(GLOBAL --config "${WORK}/config-io-bad.json" --repository "${WORK}/repo")
run_cli("11" --lang en repo info)
set(io_bad_config "${last_errors}")
set(GLOBAL --config "${WORK}/config.json")
if(NOT io_refused STREQUAL "error: --io takes hdd or ssd\n" OR NOT io_verified MATCHES "^FULL VERIFY PASSED"
        OR NOT io_bad_config MATCHES "io\\.rotational_override must give each device serial \"hdd\" or \"ssd\"")
    message(FATAL_ERROR "--io and io.rotational_override:\n${io_refused}\n${io_verified}\n${io_bad_config}")
endif()

# Decision 0036: `repo rebuild-index` makes a database that is missing, empty
# or not a database again, in a repository of its own (sample data). Every
# command, `repo init` too, names the rebuild for it (exit 11), in either
# language. The rebuild puts what is there aside, makes a new database from
# the manifests and keeps the repository's uuid, saying plainly that the
# verification history is lost (exit 2), in either language and in --json:
# the archive is listed again with its label, never verified, and verifies,
# its history starting anew.
file(WRITE "${WORK}/config7.json" "{\"language\": \"zh-TW\"}\n")
set(GLOBAL --config "${WORK}/config7.json")
run_cli("0" --repository "${WORK}/repo7" repo init)
set(LOSS "${WORK}/database-loss-sample")
file(WRITE "${LOSS}/a.txt" "database loss sample a\n")
run_cli("0" register "${LOSS}" --label ARCH-0042 --json)
string(JSON loss_uuid GET "${last_output}" archive_uuid)
run_cli("0;2" verify --full "${LOSS}")
run_cli("0" repo info --json)
string(JSON loss_repository GET "${last_output}" repository_uuid)
set(LOSS_DB "${WORK}/repo7/veriarchive.db")
cmake_path(NATIVE_PATH LOSS_DB NORMALIZE loss_db_shown)
file(REMOVE "${LOSS_DB}")
run_cli("11" --lang en archive list)
string(FIND "${last_errors}" "error: the repository's database ${loss_db_shown} is missing; make it again from the manifests with `repo rebuild-index`\n" loss_listed)
run_cli("11" repo init)
string(FIND "${last_errors}" "錯誤：儲存庫的資料庫 ${loss_db_shown} 已遺失；請以 `repo rebuild-index` 從清單檔重新建立\n" loss_init)
run_cli("2" --lang en repo rebuild-index)
set(loss_rebuilt "${last_output}")
# The files SQLite may have kept beside the database (its -wal and -shm,
# after a connection that only read) are put aside and named.
string(REGEX MATCH "The repository's database was missing; (the files left beside it were put aside as [^\n]* and )?a new one was made[.]\nRebuilt the index from manifests: " loss_said "${loss_rebuilt}")
string(FIND "${loss_rebuilt}" "\nThe verification history was kept only in the repository's database, so the manifests cannot give it back: every archive is listed as never verified until it is verified again" loss_history)
run_cli("0" archive list --json)
string(JSON loss_label GET "${last_output}" archives 0 label)
string(JSON loss_listed_uuid GET "${last_output}" archives 0 archive_uuid)
string(JSON loss_never TYPE "${last_output}" archives 0 last_full_verify)
run_cli("0" repo info --json)
string(JSON loss_repository_after GET "${last_output}" repository_uuid)
run_cli("0;2" verify --full "${LOSS}")
run_cli("0" history "${loss_uuid}" --json)
string(JSON loss_runs LENGTH "${last_output}" runs)
if(loss_listed EQUAL -1 OR loss_init EQUAL -1 OR loss_said STREQUAL "" OR loss_history EQUAL -1
        OR NOT loss_label STREQUAL "ARCH-0042" OR NOT loss_listed_uuid STREQUAL loss_uuid
        OR NOT loss_never STREQUAL "NULL" OR NOT loss_repository_after STREQUAL loss_repository OR NOT loss_runs EQUAL 1)
    message(FATAL_ERROR "repo rebuild-index of a missing database: listed ${loss_listed}, init ${loss_init}, said "
        "${loss_said}, history ${loss_history}; ${loss_label} ${loss_listed_uuid} (${loss_never}), repository "
        "${loss_repository_after}, runs ${loss_runs}\n${loss_rebuilt}")
endif()
# Not a database (sample bytes), then empty (0 bytes): each put aside.
string(REPEAT "not a database, sample data. " 160 not_a_database)
file(WRITE "${LOSS_DB}" "${not_a_database}")
run_cli("11" --lang en archive list)
string(FIND "${last_errors}" "error: the repository's database ${loss_db_shown} is not a database; make it again" garbage_listed)
run_cli("2" repo rebuild-index --json)
string(JSON garbage_state GET "${last_output}" replaced_database state)
string(JSON garbage_aside GET "${last_output}" replaced_database moved_to)
file(TO_CMAKE_PATH "${garbage_aside}" garbage_aside)
file(READ "${garbage_aside}" garbage_kept)
get_filename_component(garbage_name "${garbage_aside}" NAME)
file(WRITE "${LOSS_DB}" "")
run_cli("2" repo rebuild-index)
string(FIND "${last_output}" "儲存庫的資料庫是空的；已將它改名為 " empty_said)
string(FIND "${last_output}" "驗證歷程只存在儲存庫的資料庫中，清單檔無法還原它" empty_history)
run_cli("0" repo rebuild-index --json)
string(JSON usable_replaced TYPE "${last_output}" replaced_database)
run_cli("0;2" verify --full "${loss_uuid}")
if(garbage_listed EQUAL -1 OR NOT garbage_state STREQUAL "not_a_database" OR NOT garbage_kept STREQUAL not_a_database
        OR NOT garbage_name MATCHES "^veriarchive[.]db[.]damaged-20[0-9][0-9]-[0-9][0-9]-[0-9][0-9]T[0-9][0-9]-[0-9][0-9]-[0-9][0-9]Z(-[0-9]+)?$"
        OR empty_said EQUAL -1 OR empty_history EQUAL -1 OR NOT usable_replaced STREQUAL "NULL")
    message(FATAL_ERROR "repo rebuild-index of a database that is not one: listed ${garbage_listed}, ${garbage_state} "
        "put aside as ${garbage_name}; empty: said ${empty_said}, history ${empty_history}; usable: ${usable_replaced}")
endif()
# A rebuild that fails after it made the database again (manifests/ moved
# away, sample damage) says which database it put aside, that the history is
# lost and that the rebuild must run to its end (exit 11), in --json too;
# every command then names the rebuild that did not finish (exit 11), never
# an empty repository. The next rebuild names the same database put aside and
# the history again (exit 2), and the archive is listed.
file(WRITE "${LOSS_DB}" "${not_a_database}")
file(RENAME "${WORK}/repo7/manifests" "${WORK}/repo7/manifests-away-SAMPLE")
run_cli("11" --lang en repo rebuild-index)
set(stopped_output "${last_output}")
set(stopped_errors "${last_errors}")
string(REGEX MATCH "put aside as ([^\n]*) and a new one was made[.]" stopped_line "${stopped_output}")
set(stopped_aside "${CMAKE_MATCH_1}")
string(FIND "${stopped_output}" "The repository's database was not a database; it was put aside as " stopped_said)
string(FIND "${stopped_output}" "\nThe verification history was kept only in the repository's database" stopped_history)
string(FIND "${stopped_errors}" "error: the rebuild of the index did not fill the new database, and no command can use the repository until `repo rebuild-index` runs to its end: " stopped_error)
run_cli("11" repo rebuild-index --json)
string(JSON stopped_json_aside GET "${last_output}" replaced_database moved_to)
string(JSON stopped_json_code GET "${last_output}" exit_code)
run_cli("11" --lang en archive list)
string(FIND "${last_errors}" "error: the repository's database ${loss_db_shown} was made again, but its rebuild from the manifests did not finish; finish it with `repo rebuild-index`\n" unfilled_listed)
run_cli("11" archive list)
string(FIND "${last_errors}" "錯誤：儲存庫的資料庫 ${loss_db_shown} 已重新建立，但從清單檔重建索引的工作沒有完成；請以 `repo rebuild-index` 完成它\n" unfilled_chinese)
file(RENAME "${WORK}/repo7/manifests-away-SAMPLE" "${WORK}/repo7/manifests")
run_cli("2" --lang en repo rebuild-index)
string(FIND "${last_output}" "The repository's database was not a database; it was put aside as ${stopped_aside} and a new one was made.\nRebuilt the index from manifests: 1 archive" finished_said)
string(FIND "${last_output}" "\nThe verification history was kept only in the repository's database" finished_history)
run_cli("0" archive list --json)
string(JSON finished_label GET "${last_output}" archives 0 label)
if(stopped_said EQUAL -1 OR stopped_history EQUAL -1 OR stopped_error EQUAL -1 OR stopped_aside STREQUAL ""
        OR NOT stopped_json_aside STREQUAL stopped_aside OR NOT stopped_json_code EQUAL 11
        OR unfilled_listed EQUAL -1 OR unfilled_chinese EQUAL -1 OR finished_said EQUAL -1 OR finished_history EQUAL -1
        OR NOT finished_label STREQUAL "ARCH-0042")
    message(FATAL_ERROR "a rebuild that fails after it made the database again: said ${stopped_said}, history "
        "${stopped_history}, error ${stopped_error}, aside '${stopped_aside}' (--json '${stopped_json_aside}', "
        "${stopped_json_code}); listed ${unfilled_listed} ${unfilled_chinese}; finished ${finished_said} "
        "${finished_history} ${finished_label}\n${stopped_output}\n${stopped_errors}")
endif()
# Missing, with a file SQLite would keep beside it (sample bytes): the
# rebuild names the file it put aside, in text and in --json (set_aside),
# where no database put aside would name it.
file(REMOVE "${LOSS_DB}")
file(WRITE "${LOSS_DB}-wal" "sample journal, not SQLite's\n")
run_cli("2" --lang en repo rebuild-index)
string(REGEX MATCH "The repository's database was missing; the files left beside it were put aside as ([^\n]*) and a new one was made[.]\nRebuilt the index from manifests: " wal_said "${last_output}")
string(REGEX MATCH "[^ ,]*veriarchive[.]db[.]damaged-[^ ,]*-wal" wal_named "${CMAKE_MATCH_1}")
file(TO_CMAKE_PATH "${wal_named}" wal_named_file)
if(EXISTS "${wal_named_file}")
    file(READ "${wal_named_file}" wal_kept)
endif()
file(REMOVE "${LOSS_DB}")
file(WRITE "${LOSS_DB}-wal" "sample journal, not SQLite's\n")
run_cli("2" repo rebuild-index --json)
string(JSON wal_json_state GET "${last_output}" replaced_database state)
string(JSON wal_json_moved TYPE "${last_output}" replaced_database moved_to)
string(JSON wal_json_files GET "${last_output}" replaced_database set_aside)
if(wal_said STREQUAL "" OR NOT wal_kept STREQUAL "sample journal, not SQLite's\n"
        OR NOT wal_json_state STREQUAL "missing" OR NOT wal_json_moved STREQUAL "NULL"
        OR NOT wal_json_files MATCHES "veriarchive[.]db[.]damaged-[^\"]*-wal\"")
    message(FATAL_ERROR "a missing database with a file beside it: named '${wal_named}' (${wal_kept}); --json "
        "${wal_json_state} ${wal_json_moved} ${wal_json_files}\n${last_output}")
endif()
# An archive whose volume is attached but whose folder is gone (moved away,
# sample) is not found, naming its folder (exit 10), in either language.
set(GONE_FOLDER "${WORK}/folder-gone-sample")
file(WRITE "${GONE_FOLDER}/a.txt" "folder gone sample\n")
run_cli("0" register "${GONE_FOLDER}" --json)
string(JSON gone_folder_uuid GET "${last_output}" archive_uuid)
file(RENAME "${GONE_FOLDER}" "${GONE_FOLDER}-moved")
cmake_path(NATIVE_PATH GONE_FOLDER NORMALIZE gone_folder_shown)
run_cli("10" --lang en verify --full "${gone_folder_uuid}")
set(gone_english "${last_errors}")
run_cli("10" verify --full "${gone_folder_uuid}")
set(gone_chinese "${last_errors}")
file(RENAME "${GONE_FOLDER}-moved" "${GONE_FOLDER}")
run_cli("0;2" verify --full "${gone_folder_uuid}")
string(FIND "${gone_english}" "error: the folder ${gone_folder_shown} of archive ${gone_folder_uuid} was not found: its volume is attached, but the folder is not there\n" gone_said)
string(FIND "${gone_chinese}" "錯誤：找不到封存 ${gone_folder_uuid} 的資料夾 ${gone_folder_shown}：它所在的磁碟區已連接，但資料夾不在那裡\n" gone_translated)
if(gone_said EQUAL -1 OR gone_translated EQUAL -1)
    message(FATAL_ERROR "verify of an archive whose folder is gone:\n${gone_english}\n${gone_chinese}")
endif()
# An empty folder that is no registered archive, as a mount point on Linux
# whose device is not attached (sample), asks to check that the device is
# attached (exit 10), in either language, never offering `register`.
set(EMPTY_MOUNT "${WORK}/mount-point-sample")
file(MAKE_DIRECTORY "${EMPTY_MOUNT}")
cmake_path(NATIVE_PATH EMPTY_MOUNT NORMALIZE empty_mount_shown)
run_cli("10" --lang en verify --full "${EMPTY_MOUNT}")
string(FIND "${last_errors}" "error: the folder ${empty_mount_shown} is empty, and no archive is registered there; if a volume is mounted at this folder, check that the device that holds it is attached\n" empty_said)
run_cli("10" verify --full "${EMPTY_MOUNT}")
string(FIND "${last_errors}" "錯誤：資料夾 ${empty_mount_shown} 是空的，也沒有任何封存註冊在那裡；如果這個資料夾是某個磁碟區掛載的位置，請確認存放該磁碟區的裝置已連接\n" empty_translated)
if(empty_said EQUAL -1 OR empty_translated EQUAL -1)
    message(FATAL_ERROR "verify of an empty folder that is no archive: ${empty_said} ${empty_translated}\n${last_errors}")
endif()
set(GLOBAL --config "${WORK}/config.json")

message(STATUS "veriarchive-cli end-to-end test passed")
