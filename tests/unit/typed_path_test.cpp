#include "test_support.h"

#include "app/text.h"
#include "app/typed_path.h"
#include "common/utf8.h"

#include <filesystem>
#include <string>

namespace {

// The root that `text` names, made as the command line and the GUI make it.
std::string root(const char* text)
{
    return va::path_to_utf8(va::app::typed_root(text));
}

// A root typed with '/' or '\', with "." and "..", or with separators at its
// end is the folder it names, while a root keeps its own separator. A
// backslash separates only on Windows (R-40), where a network path and a
// "\\?\" path stay on their own server or volume.
void typed_roots_name_their_folders()
{
    CHECK(va::app::typed_root("").empty());
    if (std::filesystem::path::preferred_separator == '\\') {
        CHECK(root("C:/Photos/2026/") == R"(C:\Photos\2026)");
        CHECK(root(R"(C:\Photos\.\2026\..\2025\)") == R"(C:\Photos\2025)");
        CHECK(root("C:/Photos/./a/../b//") == R"(C:\Photos\b)");
        CHECK(root(R"(C:\Photos\..)") == R"(C:\)");
        CHECK(root("C:/") == R"(C:\)");
        CHECK(root(R"(C:\)") == R"(C:\)");
        CHECK(root(R"(\\server\share\dir\)") == R"(\\server\share\dir)");
        CHECK(root("//server/share/dir/") == "//server/share/dir");
        CHECK(root(R"(\\?\C:\Photos\)") == R"(\\?\C:\Photos)");
    } else {
        CHECK(root("/vol/photos/./2026/../2025/") == "/vol/photos/2025");
        CHECK(root("/vol/photos//") == "/vol/photos");
        CHECK(root("/vol/..") == "/");
        CHECK(root("/") == "/");
        CHECK(root(R"(/vol/name\)") == R"(/vol/name\)");
    }
}

// A relative path is taken relative to the working directory, as shells take
// it: it names the same root as the full path of that folder.
void relative_paths_follow_the_working_directory()
{
    const auto here = std::filesystem::current_path();
    CHECK(va::app::typed_root("photos/./a/../b/") ==
          va::app::typed_root(va::path_to_utf8(here / "photos" / "b")));
    CHECK(va::app::typed_path("sums.txt") == (here / "sums.txt").lexically_normal());
    CHECK(va::app::typed_path("out/../sums.txt") == (here / "sums.txt").lexically_normal());
    CHECK(va::app::typed_path("").empty());
}

// Decision 0029: a root on a network share, and on Windows one written in the
// \\?\ or \\.\ form, is refused before anything of it is read, in words of
// both languages; the refusal names the prefix as it was typed. A folder on a
// local volume is never refused, nor is a name that looks like those forms on
// Linux, where a backslash is an ordinary character (R-40). The share is a
// sample name, refused from its text alone: nothing contacts it.
void roots_on_shares_are_refused()
{
    const va::app::Text chinese(va::app::Language::TraditionalChinese);
    const auto local = std::filesystem::temp_directory_path();
    CHECK(!va::app::root_refusal(va::app::typed_root(va::path_to_utf8(local))));
    if (std::filesystem::path::preferred_separator != '\\') {
        CHECK(!va::app::root_refusal(va::app::typed_root(R"(\\?\K:\SAMPLE)")));
        CHECK(!va::app::root_refusal(va::app::typed_root(R"(\\sample-server\sample-share)")));
        return;
    }
    const auto share = va::app::root_refusal(va::app::typed_root(R"(\\sample-server\sample-share\photos)"));
    CHECK(share && *share ==
                       "the archive root is on a network share; VeriArchive registers and verifies folders on "
                       "volumes attached to this computer");
    CHECK(share && chinese(*share) ==
                       "封存根目錄位於網路共用資料夾上；VeriArchive 只註冊和驗證這台電腦所連接的磁碟區上的資料夾");
    CHECK(va::app::root_refusal(va::app::typed_root("//sample-server/sample-share/photos/")) == share);

    // The temporary folder in the \\?\ and \\.\ forms: on a local volume, so
    // refused for its form only.
    const auto prefixed = va::app::root_refusal(va::app::typed_root(R"(\\?\)" + va::path_to_utf8(local)));
    CHECK(prefixed && *prefixed == R"(write the archive root without \\?\ in front (for example D:\Photos))");
    CHECK(prefixed && chinese(*prefixed) == R"(封存根目錄前面請不要加上 \\?\（例如寫成 D:\Photos）)");
    const auto dotted = va::app::root_refusal(va::app::typed_root(R"(\\.\)" + va::path_to_utf8(local)));
    CHECK(dotted && *dotted == R"(write the archive root without \\.\ in front (for example D:\Photos))");
}

} // namespace

void typed_path_tests()
{
    typed_roots_name_their_folders();
    relative_paths_follow_the_working_directory();
    roots_on_shares_are_refused();
}
