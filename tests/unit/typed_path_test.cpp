#include "test_support.h"

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

} // namespace

void typed_path_tests()
{
    typed_roots_name_their_folders();
    relative_paths_follow_the_working_directory();
}
