#include "test_support.h"
#include "common/archive_path.h"
#include "common/hex.h"
#include "common/utf8.h"

namespace {

void plain_names_round_trip()
{
    const auto native = std::filesystem::path("Projects") /
                        std::filesystem::path(u8"中文") / "a.txt";
    const auto archive = va::archive_path_from_native(native);

    CHECK(archive.has_value());
    if (!archive) {
        return;
    }
    CHECK(archive->p == "Projects/中文/a.txt");
    CHECK(archive->enc.empty() && archive->raw.empty());

    const auto round_trip = va::archive_path_to_native(*archive);
    CHECK(round_trip.has_value());
    if (round_trip) {
        CHECK(*round_trip == native);
    }

    const auto escaping_path = std::filesystem::path("..") / "escape";
    CHECK(!va::archive_path_from_native(escaping_path).has_value());
}

// R-40: '\' is a separator in Windows names but an ordinary character on Linux.
void separators_follow_platform_rules()
{
    const auto windows = va::archive_path_from_utf16(u"dir\\file.txt");
    CHECK(windows && windows->p == "dir/file.txt");

    const auto linux_name = va::archive_path_from_bytes("dir\\file.txt");
    CHECK(linux_name && linux_name->p == "dir\\file.txt" && linux_name->enc.empty());

    CHECK(!va::archive_path_from_utf16(u"C:relative").has_value());
    CHECK(!va::archive_path_from_bytes("/absolute").has_value());
}

// R-41: names that are not valid Unicode keep their exact native form in `raw`.
void unpaired_surrogate_is_recorded_as_utf16le()
{
    const std::u16string name = u"dir/bad\xD800name";
    const auto archive = va::archive_path_from_utf16(name);
    CHECK(archive.has_value());
    if (!archive) {
        return;
    }
    CHECK(archive->enc == "utf16le");
    CHECK(archive->p == "dir/bad\xEF\xBF\xBDname");

    std::string little_endian;
    for (const char16_t unit : name) {
        little_endian += static_cast<char>(unit & 0xFF);
        little_endian += static_cast<char>(unit >> 8);
    }
    CHECK(archive->raw == va::to_hex(little_endian));
}

void big5_bytes_are_recorded_as_bytes()
{
    const std::string name = "dir/\xB0\xAA.txt"; // Big5 for a CJK character
    const auto archive = va::archive_path_from_bytes(name);
    CHECK(archive.has_value());
    if (!archive) {
        return;
    }
    CHECK(archive->enc == "bytes");
    CHECK(archive->raw == va::to_hex(name));
    CHECK(va::is_valid_utf8(archive->p));
}

void raw_names_round_trip_on_this_platform()
{
    using native_char = std::filesystem::path::value_type;
    std::filesystem::path native;
    std::string foreign_encoding;
    if constexpr (sizeof(native_char) == sizeof(char16_t)) {
        native = std::filesystem::path(std::wstring{L'd', L'/', L'x', native_char(0xDC00)});
        foreign_encoding = "bytes";
    } else {
        native = std::filesystem::path(std::string("d/x\xFF"));
        foreign_encoding = "utf16le";
    }

    const auto archive = va::archive_path_from_native(native);
    CHECK(archive && !archive->enc.empty());
    if (!archive) {
        return;
    }
    const auto round_trip = va::archive_path_to_native(*archive);
    CHECK(round_trip && round_trip->native() == native.lexically_normal().native());

    auto foreign = *archive;
    foreign.enc = foreign_encoding;
    const auto unrepresentable = va::archive_path_to_native(foreign);
    CHECK(!unrepresentable && unrepresentable.error().kind == va::IoErrorKind::NotSupported);
}

void utf8_helpers()
{
    CHECK(va::is_valid_utf8("plain ascii"));
    CHECK(va::is_valid_utf8("中文 😀"));
    CHECK(!va::is_valid_utf8("\xC0\xAF"));        // overlong '/'
    CHECK(!va::is_valid_utf8("\xED\xA0\x80"));    // encoded surrogate
    CHECK(va::utf8_lossy("a\xFF" "b") == "a\xEF\xBF\xBD" "b");
    CHECK(va::utf8_to_utf16("😀") == u"😀");
    CHECK(va::to_hex("\x01\xAB") == "01ab");
    const auto bytes = va::from_hex("01AB");
    CHECK(bytes && bytes->size() == 2 && std::to_integer<int>((*bytes)[1]) == 0xAB);
    CHECK(!va::from_hex("abc"));
    CHECK(!va::from_hex("zz"));
}

} // namespace

void archive_path_tests()
{
    plain_names_round_trip();
    separators_follow_platform_rules();
    unpaired_surrogate_is_recorded_as_utf16le();
    big5_bytes_are_recorded_as_bytes();
    raw_names_round_trip_on_this_platform();
    utf8_helpers();
}
