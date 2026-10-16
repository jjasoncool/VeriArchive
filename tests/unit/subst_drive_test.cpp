#include "test_support.h"

#include "platform/windows/subst_drive.h"

#include <map>
#include <optional>
#include <string>
#include <vector>

namespace {

using va::platform::windows::resolve_subst;
using va::platform::windows::subst_target;

// The answers of QueryDosDeviceW on a sample computer (sample data): C: and
// D: are volumes, Z: is a network drive, the others were made with subst.
const std::map<std::wstring, std::wstring> sample_devices{
    {L"C:", LR"(\Device\HarddiskVolume3)"},
    {L"D:", LR"(\Device\HarddiskVolume5)"},
    {L"Z:", LR"(\Device\LanmanRedirector\;Z:000000000001a2b3\server\share)"},
    {L"S:", LR"(\??\D:\Data)"},              // subst S: D:\Data
    {L"T:", LR"(\??\D:\Data\)"},             // subst keeps a trailing separator it is given
    {L"R:", LR"(\??\D:\)"},                  // a whole volume
    {L"Q:", LR"(\??\D:)"},
    {L"N:", LR"(\??\S:\Nested)"},            // a letter for a folder reached through S:
    {L"U:", LR"(\??\UNC\server\share\dir)"}, // a folder on a network share
    {L"L:", LR"(\??\M:\a)"},                 // two letters for each other: a loop
    {L"M:", LR"(\??\L:\b)"},
};

// Resolves `path` against the sample computer; `queried` collects the drives asked about.
std::wstring resolve(std::wstring_view path, std::vector<std::wstring>* queried = nullptr)
{
    return resolve_subst(path, [queried](std::wstring_view drive) -> std::optional<std::wstring> {
        if (queried) {
            queried->emplace_back(drive);
        }
        const auto found = sample_devices.find(std::wstring(drive));
        return found == sample_devices.end() ? std::nullopt : std::optional(found->second);
    });
}

void targets_are_read_from_dos_devices()
{
    CHECK(subst_target(LR"(\??\D:\Data)") == std::wstring(LR"(D:\Data)"));
    CHECK(subst_target(LR"(\??\D:\)") == std::wstring(LR"(D:\)"));
    CHECK(subst_target(LR"(\??\D:)") == std::wstring(L"D:"));
    CHECK(subst_target(LR"(\??\d:\data\中文)") == std::wstring(LR"(d:\data\中文)"));
    CHECK(subst_target(LR"(\??\UNC\server\share\dir)") == std::wstring(LR"(\\server\share\dir)"));
    CHECK(subst_target(LR"(\??\unc\server\share)") == std::wstring(LR"(\\server\share)"));

    // Volumes, network drives and anything else are not folders made with subst.
    CHECK(!subst_target(LR"(\Device\HarddiskVolume3)"));
    CHECK(!subst_target(LR"(\Device\LanmanRedirector\;Z:000000000001a2b3\server\share)"));
    CHECK(!subst_target(LR"(\??\Volume{0b1c2d3e-0000-4000-8000-000000000001}\)"));
    CHECK(!subst_target(LR"(\??\UNC\)"));
    CHECK(!subst_target(LR"(\??\D:Data)"));
    CHECK(!subst_target(L""));
}

void letters_made_with_subst_are_resolved()
{
    CHECK(resolve(LR"(S:\Photos\2026)") == LR"(D:\Data\Photos\2026)");
    CHECK(resolve(LR"(S:\)") == LR"(D:\Data)");
    CHECK(resolve(LR"(S:\\Photos)") == LR"(D:\Data\Photos)");
    // Forward slashes after the letter are kept; Windows reads both.
    CHECK(resolve(L"S:/Photos/2026") == L"D:\\Data\\Photos/2026");
    // The letter is asked for in capitals, whatever case the path has.
    std::vector<std::wstring> queried;
    CHECK(resolve(LR"(s:\photos)", &queried) == LR"(D:\Data\photos)");
    CHECK(!queried.empty() && queried.front() == L"S:");

    // A trailing separator in the target is not doubled, nor kept on its own.
    CHECK(resolve(LR"(T:\Photos)") == LR"(D:\Data\Photos)");
    CHECK(resolve(LR"(T:\)") == LR"(D:\Data)");
    // A letter for a whole volume gives that volume's root.
    CHECK(resolve(LR"(R:\)") == LR"(D:\)");
    CHECK(resolve(LR"(R:\Photos)") == LR"(D:\Photos)");
    CHECK(resolve(LR"(Q:\)") == LR"(D:\)");
    CHECK(resolve(LR"(Q:\Photos)") == LR"(D:\Photos)");
    // One letter through another, down to the volume's own letter.
    queried.clear();
    CHECK(resolve(LR"(N:\x)", &queried) == LR"(D:\Data\Nested\x)");
    CHECK((queried == std::vector<std::wstring>{L"N:", L"S:", L"D:"}));
    // A folder on a network share.
    CHECK(resolve(LR"(U:\x)") == LR"(\\server\share\dir\x)");
}

void other_paths_are_unchanged()
{
    for (const std::wstring_view path : {
             LR"(D:\Data\Photos)", // a volume's own letter
             LR"(Z:\x)",           // a network drive
             LR"(K:\x)",           // a letter that is not defined
             LR"(L:\x)",           // letters that point at each other
             L"S:",                // the current folder on S:, not its root
             L"S:Photos",
             LR"(\\server\share\x)",
             LR"(\\?\S:\Photos)",
             LR"(relative\path)",
             L"",
         }) {
        CHECK(resolve(path) == path);
    }
}

} // namespace

void subst_drive_tests()
{
    targets_are_read_from_dos_devices();
    letters_made_with_subst_are_resolved();
    other_paths_are_unchanged();
}
