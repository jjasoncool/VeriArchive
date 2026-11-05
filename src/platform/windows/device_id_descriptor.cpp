#include "platform/windows/device_id_descriptor.h"

#include <algorithm>
#include <cstdint>
#include <cstring>

namespace va::platform::windows {
namespace {

// STORAGE_DEVICE_ID_DESCRIPTOR: Version at 0, Size at 4, NumberOfIdentifiers
// at 8, the first identifier at 12. STORAGE_IDENTIFIER: CodeSet at 0, Type at
// 4, IdentifierSize at 8, NextOffset at 10, Association at 12, its bytes at 16.
constexpr std::size_t count_offset = 8;
constexpr std::size_t identifiers_offset = 12;
constexpr std::size_t type_offset = 4;
constexpr std::size_t size_offset = 8;
constexpr std::size_t next_offset = 10;
constexpr std::size_t association_offset = 12;
constexpr std::size_t bytes_offset = 16;

// STORAGE_IDENTIFIER_TYPE and STORAGE_ASSOCIATION_TYPE.
constexpr std::uint32_t type_eui64 = 2;
constexpr std::uint32_t type_naa = 3;
constexpr std::uint32_t association_device = 0;

template <typename Value>
std::optional<Value> field(std::span<const std::byte> answer, std::size_t offset)
{
    if (offset > answer.size() || answer.size() - offset < sizeof(Value)) {
        return std::nullopt;
    }
    Value value{};
    std::memcpy(&value, answer.data() + offset, sizeof(Value));
    return value;
}

bool worldwide(std::uint32_t type, std::span<const std::byte> bytes)
{
    if (bytes.empty()) {
        return false;
    }
    if (type == type_naa) {
        // The NAA is the high nibble of the first byte; the rest identifies.
        const auto naa = std::to_integer<unsigned>(bytes.front()) >> 4;
        if (bytes.size() < 8 || (naa != 2 && naa != 5 && naa != 6)) {
            return false;
        }
        return (std::to_integer<unsigned>(bytes.front()) & 0x0f) != 0 ||
               std::ranges::any_of(bytes.subspan(1), [](std::byte byte) { return byte != std::byte{0}; });
    }
    if (type == type_eui64) {
        return (bytes.size() == 8 || bytes.size() == 12 || bytes.size() == 16) &&
               std::ranges::any_of(bytes, [](std::byte byte) { return byte != std::byte{0}; });
    }
    return false;
}

} // namespace

bool names_device_worldwide(std::span<const std::byte> answer)
{
    const auto count = field<std::uint32_t>(answer, count_offset);
    if (!count) {
        return false;
    }
    std::size_t offset = identifiers_offset;
    for (std::uint32_t index = 0; index < *count; ++index) {
        const auto type = field<std::uint32_t>(answer, offset + type_offset);
        const auto size = field<std::uint16_t>(answer, offset + size_offset);
        const auto next = field<std::uint16_t>(answer, offset + next_offset);
        const auto association = field<std::uint32_t>(answer, offset + association_offset);
        if (!type || !size || !next || !association || answer.size() - offset - bytes_offset < *size) {
            return false;
        }
        if (*association == association_device &&
            worldwide(*type, answer.subspan(offset + bytes_offset, *size))) {
            return true;
        }
        if (*next == 0) {
            return false;
        }
        offset += *next;
    }
    return false;
}

bool enclosure_hides_identity(
    std::string_view bus, bool removable_media, std::optional<std::span<const std::byte>> answer)
{
    return bus == "usb" && !removable_media && !(answer && names_device_worldwide(*answer));
}

} // namespace va::platform::windows
