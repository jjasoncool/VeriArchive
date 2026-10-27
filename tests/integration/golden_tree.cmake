# R-66 (decisions 0008, 0015): the same fixture tree registered on Windows
# and on Linux gives the same manifest, byte for byte, once a helper blanks
# what differs by operating system and by registration. Each OS registers
# the tree and compares the blanked manifest with one checked-in golden,
# tests/golden/golden_tree_r0001.vam, so every OS gives the same bytes.
#
# Included by cli_e2e.cmake: uses its run_cli, GLOBAL and WORK. Sample data.

# The fixture tree. Its files get one fixed modification time through a tar
# round trip: CMake has no command that sets a file's time, but extracting an
# archive made with --mtime does. The archive holds ASCII names only, so no
# locale converts them; the other names are given by renaming after the
# extraction, which keeps each file's time. Links are left out: a relative
# symbolic link needs elevation or developer mode on Windows, and absolute
# ones are not part of a golden tree (decision 0008).
set(golden_staging "${WORK}/golden-tree-staging")
set(golden_tree "${WORK}/golden-tree")
file(REMOVE_RECURSE "${golden_staging}" "${golden_tree}")
file(WRITE "${golden_staging}/readme-SAMPLE.txt" "VeriArchive golden tree of R-66 (sample data).\n")
file(WRITE "${golden_staging}/empty-SAMPLE.bin" "")
file(WRITE "${golden_staging}/cjk/data.txt" "中文內容（範例）\n")
file(WRITE "${golden_staging}/emoji/nested/deep-SAMPLE.bin" "0123456789")
file(MAKE_DIRECTORY "${golden_staging}/empty-folder-SAMPLE")
# The default exclusions match at the root only (R-54): these two folders
# are left out, the one below the root is kept.
file(WRITE "${golden_staging}/$RECYCLE.BIN/excluded-SAMPLE.txt" "left out at the root\n")
file(WRITE "${golden_staging}/.Trash-1000/excluded-SAMPLE.txt" "left out at the root\n")
file(WRITE "${golden_staging}/nested/$RECYCLE.BIN/kept-SAMPLE.txt" "kept below the root\n")
execute_process(COMMAND "${CMAKE_COMMAND}" -E tar cf "${WORK}/golden-tree.tar" --format=pax
        "--mtime=2026-01-02 03:04:05 UTC" .
    WORKING_DIRECTORY "${golden_staging}" RESULT_VARIABLE packed)
file(MAKE_DIRECTORY "${golden_tree}")
execute_process(COMMAND "${CMAKE_COMMAND}" -E tar xf "${WORK}/golden-tree.tar"
    WORKING_DIRECTORY "${golden_tree}" RESULT_VARIABLE unpacked)
if(NOT packed EQUAL 0 OR NOT unpacked EQUAL 0)
    message(FATAL_ERROR "cannot make the golden tree (tar exit ${packed}, ${unpacked})")
endif()
file(RENAME "${golden_tree}/cjk/data.txt" "${golden_tree}/cjk/資料-範例.txt")
file(RENAME "${golden_tree}/cjk" "${golden_tree}/中文-範例")
file(RENAME "${golden_tree}/emoji" "${golden_tree}/emoji 😀 SAMPLE")

# Registered with a fixed label, into a repository and a config file of its
# own, so that the test's other repositories and config are left as they are.
set(golden_saved_global ${GLOBAL})
set(GLOBAL --config "${WORK}/golden-tree-config.json" --repository "${WORK}/repo-golden")
file(WRITE "${WORK}/golden-tree-config.json" "{}\n")
run_cli("0" repo init)
run_cli("0" register "${golden_tree}" --label ARCH-0066 --json)
string(JSON golden_uuid GET "${last_output}" archive_uuid)
set(GLOBAL ${golden_saved_global})
file(READ "${WORK}/repo-golden/manifests/${golden_uuid}/r0001.vam" golden_manifest)

# The helper blanks what R-66 lets differ: in the header archive_uuid (each
# registration draws its own), created, created_by, identity and root_hint,
# and the body's blake3 and length, which cover mode and attrs; in every
# entry mode and attrs (decision 0015). The entry lines themselves are
# compared, so the body is still compared byte for byte.
string(FIND "${golden_manifest}" "\n" header_end)
string(SUBSTRING "${golden_manifest}" 0 ${header_end} golden_header)
math(EXPR body_start "${header_end} + 1")
string(SUBSTRING "${golden_manifest}" ${body_start} -1 golden_body)
string(REGEX REPLACE "\"archive_uuid\":\"[^\"]*\"" "\"archive_uuid\":\"\"" golden_header "${golden_header}")
string(REGEX REPLACE "\"created\":\"[^\"]*\"" "\"created\":\"\"" golden_header "${golden_header}")
string(REGEX REPLACE "\"created_by\":{[^}]*}" "\"created_by\":{}" golden_header "${golden_header}")
string(REGEX REPLACE "\"identity\":{.*},\"root_hint\":" "\"identity\":{},\"root_hint\":" golden_header
    "${golden_header}")
string(REGEX REPLACE "\"root_hint\":\"([^\"\\\\]|\\\\.)*\"" "\"root_hint\":\"\"" golden_header "${golden_header}")
string(REGEX REPLACE "\"body\":{\"blake3\":\"[0-9a-f]*\",\"length\":[0-9]*," "\"body\":{\"blake3\":\"\",\"length\":0,"
    golden_header "${golden_header}")
string(REGEX REPLACE ",\"(mode|attrs)\":[0-9]+" "" golden_body "${golden_body}")
set(golden_blanked "${golden_header}\n${golden_body}")
# Compared as bytes: on Windows file(READ) without HEX turns CR LF into LF,
# and file(WRITE) writes LF as CR LF, so the dump of a differing manifest is
# written with file(CONFIGURE), which keeps LF (no "@" is in it).
string(HEX "${golden_blanked}" golden_blanked_hex)
set(golden_expected_hex "")
if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/../golden/golden_tree_r0001.vam")
    file(READ "${CMAKE_CURRENT_LIST_DIR}/../golden/golden_tree_r0001.vam" golden_expected_hex HEX)
endif()
if(NOT golden_blanked_hex STREQUAL golden_expected_hex)
    file(CONFIGURE OUTPUT "${WORK}/golden_tree_r0001.actual.vam" CONTENT "${golden_blanked}" @ONLY
        NEWLINE_STYLE UNIX)
    message(FATAL_ERROR "the golden tree's manifest, blanked, differs from tests/golden/golden_tree_r0001.vam "
        "(R-66); it is in ${WORK}/golden_tree_r0001.actual.vam")
endif()
message(STATUS "R-66: the golden tree's manifest is the golden one")
