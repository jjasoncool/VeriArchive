#include "test_support.h"

int main()
{
    archive_path_tests();
    canonical_json_tests();
    repository_tests();
    config_tests();
    file_writer_tests();
    platform_tests();
    platform_error_tests();
    unicode_tests();
    identity_tests();
    device_health_tests();
    displayed_identity_tests();
    ended_notification_tests();
    hash_tests();
    merkle_tests();
    fs_access_tests();
    scanner_tests();
    manifest_tests();
    hash_pipeline_tests();
    archive_store_tests();
    register_engine_tests();
    verify_engine_tests();
    add_chunk_hashes_tests();
    archive_status_tests();
    sha256sums_tests();
    rebuild_index_tests();
    transfer_tests();
    relocate_tests();
    read_speed_tests();
    text_tests();
    local_time_tests();
    session_tests();
    compare_archives_tests();
    vpd_tests();
    subst_drive_tests();
    progress_estimate_tests();
    unfinished_operations_tests();
    abandon_registration_tests();
    retire_archive_tests();
    typed_path_tests();
    interrupt_tests();
    keep_awake_tests();
    plain_words_tests();
    return test_failures == 0 ? 0 : 1;
}