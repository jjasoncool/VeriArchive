#include "test_support.h"

#include "app/text.h"
#include "app/typed_path.h"
#include "common/utf8.h"

#include <chrono>
#include <filesystem>
#include <fstream>
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
        // A volume's GUID path is the root folder of its volume, as "C:\" is
        // (decision 0029, point 3): its separator stays; a folder below it
        // drops its own. (Sample GUIDs.)
        CHECK(root(R"(\\?\Volume{c0ffee00-0000-4000-8000-000000000001}\)") ==
              R"(\\?\Volume{c0ffee00-0000-4000-8000-000000000001}\)");
        CHECK(root(R"(\\?\volume{c0ffee00-0000-4000-8000-000000000001}\\)") ==
              R"(\\?\volume{c0ffee00-0000-4000-8000-000000000001}\)");
        CHECK(root(R"(\\?\Volume{c0ffee00-0000-4000-8000-000000000001}\Photos\)") ==
              R"(\\?\Volume{c0ffee00-0000-4000-8000-000000000001}\Photos)");
        // Written with "." or "..", or two separators in a row, after its
        // GUID, it names the folder they resolve to, as a path through a
        // drive letter does, and its prefix is written with '\': Windows
        // reads such a path as given, so the root read is then the root
        // identified. The volume's root folder stays its lowest folder.
        CHECK(root(R"(\\?\Volume{c0ffee00-0000-4000-8000-000000000001}\SAMPLE\..\Photos\)") ==
              R"(\\?\Volume{c0ffee00-0000-4000-8000-000000000001}\Photos)");
        CHECK(root(R"(\\?\Volume{c0ffee00-0000-4000-8000-000000000001}\Photos\.)") ==
              R"(\\?\Volume{c0ffee00-0000-4000-8000-000000000001}\Photos)");
        CHECK(root(R"(\\?\Volume{c0ffee00-0000-4000-8000-000000000001}\\Photos\\2026)") ==
              R"(\\?\Volume{c0ffee00-0000-4000-8000-000000000001}\Photos\2026)");
        CHECK(root(R"(\\?\Volume{c0ffee00-0000-4000-8000-000000000001}\Photos\..\..)") ==
              R"(\\?\Volume{c0ffee00-0000-4000-8000-000000000001}\)");
        CHECK(root("//?/Volume{c0ffee00-0000-4000-8000-000000000001}/Photos/") ==
              R"(\\?\Volume{c0ffee00-0000-4000-8000-000000000001}\Photos)");
        // Any other path in the \\?\ or \\.\ form stays as typed, and is
        // refused naming its prefix as typed.
        CHECK(root(R"(\\?\C:\Photos\..\Music)") == R"(\\?\C:\Photos\..\Music)");
        CHECK(root("//?/C:/Photos") == "//?/C:/Photos");
        CHECK(root(R"(\\.\Volume{c0ffee00-0000-4000-8000-000000000001}\Photos\..)") ==
              R"(\\.\Volume{c0ffee00-0000-4000-8000-000000000001}\Photos\..)");
    } else {
        CHECK(root("/vol/photos/./2026/../2025/") == "/vol/photos/2025");
        CHECK(root("/vol/photos//") == "/vol/photos");
        CHECK(root("/vol/..") == "/");
        CHECK(root("/") == "/");
        CHECK(root(R"(/vol/name\)") == R"(/vol/name\)");
        // A backslash is no separator (R-40): such a name is one name.
        CHECK(root(R"(/vol/\\?\Volume{c0ffee00-0000-4000-8000-000000000001}\a\..\b)") ==
              R"(/vol/\\?\Volume{c0ffee00-0000-4000-8000-000000000001}\a\..\b)");
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
        CHECK(!va::app::root_refusal(va::app::typed_root(R"(\\.\Volume{c0ffee00-0000-4000-8000-000000000001}\)")));
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

    // Decision 0029, point 3: a volume's GUID path in the \\?\ form is taken,
    // as is a folder below it (sample GUIDs, judged from the text alone). One
    // that names a volume's GUID otherwise, in the \\.\ form or with its GUID
    // cut short, says how to write it, in either language.
    CHECK(!va::app::root_refusal(va::app::typed_root(R"(\\?\Volume{c0ffee00-0000-4000-8000-000000000001}\)")));
    CHECK(!va::app::root_refusal(va::app::typed_root(R"(\\?\Volume{c0ffee00-0000-4000-8000-000000000001}\SAMPLE)")));
    const auto guid_words = R"(write a volume's GUID path exactly as mountvol lists it, with \\?\ in front)";
    for (const auto* written : {R"(\\.\Volume{c0ffee00-0000-4000-8000-000000000001}\SAMPLE)",
             R"(\\?\VOLUME{c0ffee00-0000-4000-8000-00000000001}\SAMPLE)"}) {
        const auto guid = va::app::root_refusal(va::app::typed_root(written));
        CHECK(guid && *guid == guid_words);
        CHECK(guid && chinese(*guid) == R"(磁碟區的 GUID 路徑請完全照 mountvol 列出的樣子寫，前面加上 \\?\)");
    }
}

// The root of `register`, `archive relocate` and `verify --full`, and the
// root that Resume in the Archive List takes from a registration's plan
// (decision 0022, point 7), is taken only when it is a folder and not
// refused: one that is missing says that the folder or volume was not found
// (decision 0029, point 7), one that is no folder says so, each naming the
// root as it was typed, in either language; a refused one says why it is
// refused. (Sample names, in a folder of their own in the temporary folder.)
void roots_that_are_no_folder_are_not_taken()
{
    const va::app::Text chinese(va::app::Language::TraditionalChinese);
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto folder = std::filesystem::temp_directory_path() / ("va-SAMPLE-root-" + std::to_string(stamp));
    std::filesystem::create_directories(folder);
    std::ofstream(folder / "SAMPLE.txt") << "sample data\n";
    CHECK(!va::app::root_not_taken(va::app::typed_root(va::path_to_utf8(folder)), va::path_to_utf8(folder)));
    const auto missing = va::app::root_not_taken(folder / "SAMPLE-missing", "SAMPLE-missing/");
    CHECK(missing && *missing == "the folder or volume SAMPLE-missing/ was not found");
    CHECK(missing && chinese(*missing) == "找不到資料夾或磁碟區 SAMPLE-missing/");
    CHECK(va::app::root_absent(folder / "SAMPLE-missing", "SAMPLE-missing/") == missing);
    CHECK(va::app::root_absent(folder / "SAMPLE-missing" / "below", "x") ==
          "the folder or volume x was not found");
    CHECK(!va::app::root_absent(folder, "folder") && !va::app::root_absent(folder / "SAMPLE.txt", "file"));
    const auto file = va::app::root_not_taken(folder / "SAMPLE.txt", "SAMPLE.txt");
    CHECK(file && *file == "not a directory: SAMPLE.txt");
    CHECK(chinese("the folder or volume K:\\SAMPLE was not found: no volume is attached as K:; check that the device "
                  "that holds it is attached") ==
          "找不到資料夾或磁碟區 K:\\SAMPLE：目前沒有磁碟區連接為 K:；請確認存放它的裝置已連接");
    if (std::filesystem::path::preferred_separator == '\\') {
        const auto share = va::app::root_not_taken(
            va::app::typed_root(R"(\\sample-server\sample-share)"), R"(\\sample-server\sample-share)");
        CHECK(share && *share == va::app::root_refusal(va::app::typed_root(R"(\\sample-server\sample-share)")));
    }
    std::error_code error;
    std::filesystem::remove_all(folder, error);
}

} // namespace

void typed_path_tests()
{
    typed_roots_name_their_folders();
    relative_paths_follow_the_working_directory();
    roots_on_shares_are_refused();
    roots_that_are_no_folder_are_not_taken();
}
