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
    hash_tests();
    fs_access_tests();
    return test_failures == 0 ? 0 : 1;
}