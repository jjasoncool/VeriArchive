// The body of the notification that an operation of the window ended
// (decision 0043): the first two lines of its report that hold text. The
// reports below are sample data, worded as the views word theirs.
#include "test_support.h"

#include "gui/ended_notification.h"

void ended_notification_tests()
{
    using va::gui::ended_notification_body;

    // A verification's result, then what it is of, past the blank line between them.
    CHECK(ended_notification_body("FULL VERIFY PASSED\n\nARCH-SAMPLE (c0ffee00-0000-4000-8000-000000000001) "
                                  "revision 1\nRoot: D:\\va-sample\nIdentity: MATCH\n") ==
          "FULL VERIFY PASSED\nARCH-SAMPLE (c0ffee00-0000-4000-8000-000000000001) revision 1");
    CHECK(ended_notification_body("完整驗證 通過\n\nARCH-SAMPLE（c0ffee00-0000-4000-8000-000000000001）版次 1\n根目錄：D:\\va-sample\n") ==
          "完整驗證 通過\nARCH-SAMPLE（c0ffee00-0000-4000-8000-000000000001）版次 1");
    // A registration's lines follow one another.
    CHECK(ended_notification_body("Registered ARCH-SAMPLE (c0ffee00-0000-4000-8000-000000000001) revision 1\n\n"
                                  "3 files, 0 empty folders, 0 links, 1.0 KiB hashed\nManifest: r0001.vam\n") ==
          "Registered ARCH-SAMPLE (c0ffee00-0000-4000-8000-000000000001) revision 1\n"
          "3 files, 0 empty folders, 0 links, 1.0 KiB hashed");
    // A refusal is one line.
    CHECK(ended_notification_body("Not verified: the folder D:\\va-sample is not there\n") ==
          "Not verified: the folder D:\\va-sample is not there");
    // Lines of spaces and tabs hold no text.
    CHECK(ended_notification_body("\n  \nARCHIVES IDENTICAL\n \t\nEvery entry of A is in B.\nA (reference): x\n") ==
          "ARCHIVES IDENTICAL\nEvery entry of A is in B.");
    CHECK(ended_notification_body("").empty());
}
