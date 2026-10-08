#include "test_support.h"
#include "common/archive_path.h"

void archive_path_tests()
{
    const auto native = std::filesystem::path("Projects") /
                        std::filesystem::path(u8"中文") / "a.txt";
    const auto archive = va::archive_path_from_native(native);

    CHECK(archive.has_value());
    if (!archive) {
        return;
    }
    CHECK(archive->p == "Projects/中文/a.txt");

    const auto round_trip = va::archive_path_to_native(*archive);
    CHECK(round_trip.has_value());
    if (!round_trip) {
        return;
    }
    CHECK(*round_trip == native);

    const auto escaping_path = std::filesystem::path("..") / "escape";
    CHECK(!va::archive_path_from_native(escaping_path).has_value());
}