#include "common/uuid.h"

#include <array>
#include <format>
#include <random>

namespace va {

std::string generate_uuid()
{
    std::array<unsigned char, 16> bytes{};
    std::random_device random;
    for (auto& byte : bytes) {
        byte = static_cast<unsigned char>(random());
    }

    bytes[6] = (bytes[6] & 0x0f) | 0x40; // version 4
    bytes[8] = (bytes[8] & 0x3f) | 0x80; // RFC 4122 variant

    return std::format(
        "{:02x}{:02x}{:02x}{:02x}-{:02x}{:02x}-{:02x}{:02x}-"
        "{:02x}{:02x}-{:02x}{:02x}{:02x}{:02x}{:02x}{:02x}",
        bytes[0], bytes[1], bytes[2], bytes[3], bytes[4], bytes[5], bytes[6], bytes[7],
        bytes[8], bytes[9], bytes[10], bytes[11], bytes[12], bytes[13], bytes[14], bytes[15]);
}

bool is_canonical_uuid(std::string_view text)
{
    if (text.size() != 36) {
        return false;
    }
    for (std::size_t index = 0; index < text.size(); ++index) {
        const char character = text[index];
        if (index == 8 || index == 13 || index == 18 || index == 23) {
            if (character != '-') {
                return false;
            }
        } else if (!((character >= '0' && character <= '9') ||
                       (character >= 'a' && character <= 'f'))) {
            return false;
        }
    }
    return true;
}

} // namespace va
