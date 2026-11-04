// What the window says around its pages (decision 0046): the repository's
// folder under the sidebar's title, and how an alert lays out a message. The
// paths and messages below are sample data (K:/SAMPLE, ARCH-SAMPLE, no real
// repository or archive), worded as the window words its own.
#include "test_support.h"

#include "common/utf8.h"
#include "gui/window_words.h"

#include <filesystem>
#include <string>

void window_words_tests()
{
    using va::path_from_utf8;
    using va::gui::alert_words;
    using va::gui::repository_folder;
    using std::filesystem::path;

    // The repository's last folder, also when the root ends with a separator.
    CHECK(repository_folder(path("K:/SAMPLE/repo")) == "repo");
    CHECK(repository_folder(path("K:/SAMPLE/repo/")) == "repo");
    CHECK(repository_folder(path_from_utf8("K:/SAMPLE/儲存庫")) == "儲存庫");
    // The root of a volume has no folder name.
    CHECK(repository_folder(path("/")).empty());
    CHECK(repository_folder(path()).empty());
#ifdef _WIN32
    CHECK(repository_folder(path("K:/")).empty());
    CHECK(repository_folder(path("K:\\SAMPLE\\repo\\")) == "repo");
#endif

    // A one-line message is a heading alone.
    {
        const auto words = alert_words("Another step of the window is running; wait for it to end.");
        CHECK(words.heading == "Another step of the window is running; wait for it to end.");
        CHECK(words.body.empty());
    }
    // A short first line heads the lines after it; the blank lines between
    // them and at the end are left out, but not those within the rest.
    {
        const auto words = alert_words("A\n\nB\n");
        CHECK(words.heading == "A");
        CHECK(words.body == "B");
    }
    {
        const auto words = alert_words("Not abandoned: ARCH-SAMPLE\n\n\nFirst line.\n\nSecond line.\n \n");
        CHECK(words.heading == "Not abandoned: ARCH-SAMPLE");
        CHECK(words.body == "First line.\n\nSecond line.");
    }
    // Nothing: no heading, nothing under it.
    {
        const auto words = alert_words("");
        CHECK(words.heading.empty() && words.body.empty());
        const auto blank = alert_words("\n \n");
        CHECK(blank.heading.empty() && blank.body.empty());
    }
    // A long first line is no heading: the whole message goes under none.
    {
        const std::string refusal = "A job of this window is working on this operation. Stop it in Jobs first, "
                                    "or wait for it to end.";
        const auto words = alert_words(refusal);
        CHECK(words.heading.empty());
        CHECK(words.body == refusal);
        const auto two = alert_words(refusal + "\nARCH-SAMPLE\n");
        CHECK(two.heading.empty() && two.body == refusal + "\nARCH-SAMPLE");
    }
    // Columns, not bytes: a Chinese character takes two. 30 of them make a
    // heading of 60 columns, 31 none.
    {
        std::string thirty;
        for (int index = 0; index < 30; ++index) {
            thirty += "封";
        }
        CHECK(alert_words(thirty).heading == thirty);
        const auto longer = alert_words(thirty + "存\n說明");
        CHECK(longer.heading.empty() && longer.body == thirty + "存\n說明");
        CHECK(alert_words(std::string(60, 'a')).heading == std::string(60, 'a'));
        CHECK(alert_words(std::string(61, 'a')).heading.empty());
    }
}
