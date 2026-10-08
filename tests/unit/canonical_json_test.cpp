#include "test_support.h"
#include "common/canonical_json.h"

void canonical_json_tests()
{
    CHECK(
        va::canonical_json::quote("a\n\t\"\\") ==
        "\"a\\u000a\\u0009\\\"\\\\\"");
    CHECK(va::canonical_json::quote("中文") == "\"中文\"");
    CHECK(va::canonical_json::integer(-42) == "-42");

    // Keys stay in insertion order (not alphabetical), no whitespace (R-64).
    va::canonical_json::ObjectWriter object;
    object.add_string("t", "f")
        .add_unsigned("size", 18446744073709551615ull)
        .add_integer("mtime_ns", -1)
        .add_bool("signed", false)
        .add_strings("warn", {"case-collision"})
        .add_raw("nested", "{\"a\":1}");
    CHECK(object.str() ==
          "{\"t\":\"f\",\"size\":18446744073709551615,\"mtime_ns\":-1,\"signed\":false,"
          "\"warn\":[\"case-collision\"],\"nested\":{\"a\":1}}");
    CHECK(va::canonical_json::ObjectWriter{}.str() == "{}");
}
