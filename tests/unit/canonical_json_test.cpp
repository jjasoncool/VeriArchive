#include "test_support.h"
#include "common/canonical_json.h"

void canonical_json_tests()
{
    CHECK(
        va::canonical_json::quote("a\n\t\"\\") ==
        "\"a\\u000a\\u0009\\\"\\\\\"");
    CHECK(va::canonical_json::quote("中文") == "\"中文\"");
    CHECK(va::canonical_json::integer(-42) == "-42");
}