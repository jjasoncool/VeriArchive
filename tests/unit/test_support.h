#pragma once
#include <chrono>
#include <iostream> // IWYU pragma: keep (std::cerr in CHECK, expanded in every test file)

inline int test_failures = 0;

namespace test {

// The local date on which the tests open repositories: it names the copy of a
// database kept before its upgrade (decision 0032).
inline constexpr std::chrono::year_month_day today{
    std::chrono::year{2026}, std::chrono::October, std::chrono::day{17}};

} // namespace test

#define CHECK(expression)                                                                  \
    do {                                                                                   \
        if (!(expression)) {                                                               \
            std::cerr << __FILE__ << ':' << __LINE__                                       \
                      << ": CHECK failed: " #expression "\n";                            \
            ++test_failures;                                                               \
        }                                                                                  \
    } while (false)

void archive_path_tests();
void canonical_json_tests();
void repository_tests();
void config_tests();
void file_writer_tests();
void platform_tests();
void platform_error_tests();
void unicode_tests();
void identity_tests();
void displayed_identity_tests();
void hash_tests();
void merkle_tests();
void fs_access_tests();
void scanner_tests();
void manifest_tests();
void hash_pipeline_tests();
void archive_store_tests();
void register_engine_tests();
void verify_engine_tests();
void archive_status_tests();
void sha256sums_tests();
void rebuild_index_tests();
void transfer_tests();
void relocate_tests();
void text_tests();
void local_time_tests();
void session_tests();
void compare_archives_tests();
void vpd_tests();
void subst_drive_tests();
void progress_estimate_tests();
void unfinished_operations_tests();
void abandon_registration_tests();
void retire_archive_tests();
void add_chunk_hashes_tests();
void typed_path_tests();
void interrupt_tests();
void keep_awake_tests();
void plain_words_tests();
