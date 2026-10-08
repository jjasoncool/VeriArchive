#include "core/engine/item_rules.h"

#include "common/utf8.h"

#include <cstdlib>
#include <string>

namespace va {
namespace {

bool reserved_device_name(std::string_view component)
{
    // A reserved name stays reserved with any extension: "NUL.txt" is NUL.
    const auto stem = component.substr(0, component.find('.'));
    std::string upper;
    for (const char character : stem) {
        upper += (character >= 'a' && character <= 'z') ? static_cast<char>(character - 32) : character;
    }
    while (!upper.empty() && upper.back() == ' ') {
        upper.pop_back();
    }
    if (upper == "CON" || upper == "PRN" || upper == "AUX" || upper == "NUL") {
        return true;
    }
    return upper.size() == 4 && (upper.starts_with("COM") || upper.starts_with("LPT")) &&
           upper[3] >= '1' && upper[3] <= '9';
}

bool component_representable(std::string_view component)
{
    if (component.empty() || component.back() == ' ' || component.back() == '.') {
        return false;
    }
    for (const unsigned char character : component) {
        if (character < 0x20 || std::string_view("<>:\"|?*\\").find(static_cast<char>(character)) !=
                                    std::string_view::npos) {
            return false;
        }
    }
    return !reserved_device_name(component) && utf8_to_utf16(component).size() <= 255;
}

} // namespace

bool mtime_matches(std::string_view filesystem_type, std::int64_t recorded_ns, std::int64_t current_ns)
{
    const auto difference = std::llabs(recorded_ns - current_ns);
    if (filesystem_type == "vfat") {
        return true;
    }
    if (filesystem_type == "exfat") {
        constexpr std::int64_t ten_ms = 10'000'000;
        constexpr std::int64_t quarter_hour = 15LL * 60 * 1'000'000'000;
        if (difference <= ten_ms) {
            return true;
        }
        const auto remainder = difference % quarter_hour;
        return difference <= 14 * 4 * quarter_hour &&
               (remainder <= ten_ms || quarter_hour - remainder <= ten_ms);
    }
    return difference <= 1'000'000'000;
}

bool representable_on_windows(std::string_view archive_path)
{
    std::size_t start = 0;
    while (start <= archive_path.size()) {
        auto end = archive_path.find('/', start);
        if (end == std::string_view::npos) {
            end = archive_path.size();
        }
        if (!component_representable(archive_path.substr(start, end - start))) {
            return false;
        }
        start = end + 1;
    }
    return true;
}

} // namespace va
