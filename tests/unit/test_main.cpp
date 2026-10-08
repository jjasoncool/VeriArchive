#include "test_support.h"

int main()
{
    archive_path_tests();
    canonical_json_tests();
    config_tests();
    return test_failures == 0 ? 0 : 1;
}
