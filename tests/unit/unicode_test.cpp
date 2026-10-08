#include "test_support.h"

#include "common/unicode.h"

void unicode_tests()
{
    const std::string composed = "caf\xC3\xA9";     // "café" with U+00E9
    const std::string decomposed = "cafe\xCC\x81";  // "cafe" + U+0301 combining acute
    CHECK(composed != decomposed);
    CHECK(va::unicode_nfc(decomposed) == composed);
    CHECK(va::unicode_nfc(composed) == composed);

    CHECK(va::unicode_fold("Backups") == va::unicode_fold("backups"));
    CHECK(va::unicode_fold("\xC3\x89T\xC3\x89") == va::unicode_fold("\xC3\xA9t\xC3\xA9")); // ÉTÉ / été
    CHECK(va::unicode_fold("CAFE\xCC\x81") == va::unicode_fold(composed));
    CHECK(va::unicode_fold("中文") == "中文");
    CHECK(va::unicode_fold("a") != va::unicode_fold("b"));
    CHECK(!va::unicode_nfc("bad\xFF").empty());
}
