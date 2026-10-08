#include "common/hex.h"

namespace va {
namespace {

constexpr char digits[] = "0123456789abcdef";

int digit_value(char digit)
{
    if (digit >= '0' && digit <= '9') {
        return digit - '0';
    }
    if (digit >= 'a' && digit <= 'f') {
        return digit - 'a' + 10;
    }
    if (digit >= 'A' && digit <= 'F') {
        return digit - 'A' + 10;
    }
    return -1;
}

} // namespace

std::string to_hex(std::span<const std::byte> bytes)
{
    std::string text;
    text.reserve(bytes.size() * 2);
    for (const auto byte : bytes) {
        const auto value = std::to_integer<unsigned>(byte);
        text += digits[value >> 4];
        text += digits[value & 0x0F];
    }
    return text;
}

std::string to_hex(std::string_view bytes)
{
    return to_hex(std::as_bytes(std::span(bytes.data(), bytes.size())));
}

std::optional<std::vector<std::byte>> from_hex(std::string_view text)
{
    if (text.size() % 2 != 0) {
        return std::nullopt;
    }
    std::vector<std::byte> bytes;
    bytes.reserve(text.size() / 2);
    for (std::size_t index = 0; index < text.size(); index += 2) {
        const int high = digit_value(text[index]);
        const int low = digit_value(text[index + 1]);
        if (high < 0 || low < 0) {
            return std::nullopt;
        }
        bytes.push_back(static_cast<std::byte>((high << 4) | low));
    }
    return bytes;
}

} // namespace va
