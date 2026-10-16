#include "test_support.h"

int main()
{
    archive_path_tests();
    canonical_json_tests();
    repository_tests();
    config_tests();
    file_writer_tests();
    platform_tests();
    unicode_tests();
    identity_tests();
    displayed_identity_tests();
    hash_tests();
    fs_access_tests();
    scanner_tests();
    manifest_tests();
    hash_pipeline_tests();
    archive_store_tests();
    register_engine_tests();
    verify_engine_tests();
    archive_status_tests();
    sha256sums_tests();
    rebuild_index_tests();
    transfer_tests();
    relocate_tests();
    text_tests();
    local_time_tests();
    compare_archives_tests();
    vpd_tests();
    progress_estimate_tests();
    unfinished_operations_tests();
    abandon_registration_tests();
    return test_failures == 0 ? 0 : 1;
}