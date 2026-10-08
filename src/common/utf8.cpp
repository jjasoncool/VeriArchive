#include "common/utf8.h"

#include <optional>

namespace va {
namespace {

constexpr char32_t replacement_character = 0xFFFD;

void append_code_point(std::string& output, char32_t code_point)
{
    if (code_point < 0x80) {
        output += static_cast<char>(code_point);
    } else if (code_point < 0x800) {
        output += static_cast<char>(0xC0 | (code_point >> 6));
        output += static_cast<char>(0x80 | (code_point & 0x3F));
    } else if (code_point < 0x10000) {
        output += static_cast<char>(0xE0 | (code_point >> 12));
        output += static_cast<char>(0x80 | ((code_point >> 6) & 0x3F));
        output += static_cast<char>(0x80 | (code_point & 0x3F));
    } else {
        output += static_cast<char>(0xF0 | (code_point >> 18));
        output += static_cast<char>(0x80 | ((code_point >> 12) & 0x3F));
        output += static_cast<char>(0x80 | ((code_point >> 6) & 0x3F));
        output += static_cast<char>(0x80 | (code_point & 0x3F));
    }
}

// Decodes one UTF-8 sequence starting at bytes[index]. Returns the code point and
// advances index, or returns nullopt (advancing past one byte) when the sequence
// is invalid: truncated, overlong, a surrogate, or above U+10FFFF.
std::optional<char32_t> decode_utf8(std::string_view bytes, std::size_t& index)
{
    const auto lead = static_cast<unsigned char>(bytes[index]);
    if (lead < 0x80) {
        ++index;
        return lead;
    }

    std::size_t length = 0;
    char32_t code_point = 0;
    char32_t minimum = 0;
    if ((lead & 0xE0) == 0xC0) {
        length = 2;
        code_point = lead & 0x1F;
        minimum = 0x80;
    } else if ((lead & 0xF0) == 0xE0) {
        length = 3;
        code_point = lead & 0x0F;
        minimum = 0x800;
    } else if ((lead & 0xF8) == 0xF0) {
        length = 4;
        code_point = lead & 0x07;
        minimum = 0x10000;
    } else {
        ++index;
        return std::nullopt;
    }

    if (index + length > bytes.size()) {
        ++index;
        return std::nullopt;
    }
    for (std::size_t offset = 1; offset < length; ++offset) {
        const auto continuation = static_cast<unsigned char>(bytes[index + offset]);
        if ((continuation & 0xC0) != 0x80) {
            ++index;
            return std::nullopt;
        }
        code_point = (code_point << 6) | (continuation & 0x3F);
    }
    if (code_point < minimum || code_point > 0x10FFFF ||
        (code_point >= 0xD800 && code_point <= 0xDFFF)) {
        ++index;
        return std::nullopt;
    }
    index += length;
    return code_point;
}

} // namespace

bool is_valid_utf8(std::string_view bytes)
{
    std::size_t index = 0;
    while (index < bytes.size()) {
        if (!decode_utf8(bytes, index)) {
            return false;
        }
    }
    return true;
}

std::string utf8_lossy(std::string_view bytes)
{
    std::string output;
    output.reserve(bytes.size());
    std::size_t index = 0;
    while (index < bytes.size()) {
        const auto code_point = decode_utf8(bytes, index);
        append_code_point(output, code_point.value_or(replacement_character));
    }
    return output;
}

std::string utf16_to_utf8(std::u16string_view units, bool* lossless)
{
    if (lossless) {
        *lossless = true;
    }
    std::string output;
    output.reserve(units.size());
    for (std::size_t index = 0; index < units.size(); ++index) {
        const char32_t unit = units[index];
        if (unit >= 0xD800 && unit <= 0xDBFF && index + 1 < units.size() &&
            units[index + 1] >= 0xDC00 && units[index + 1] <= 0xDFFF) {
            const char32_t low = units[++index];
            append_code_point(output, 0x10000 + ((unit - 0xD800) << 10) + (low - 0xDC00));
        } else if (unit >= 0xD800 && unit <= 0xDFFF) {
            append_code_point(output, replacement_character);
            if (lossless) {
                *lossless = false;
            }
        } else {
            append_code_point(output, unit);
        }
    }
    return output;
}

std::u16string utf8_to_utf16(std::string_view utf8)
{
    std::u16string output;
    output.reserve(utf8.size());
    std::size_t index = 0;
    while (index < utf8.size()) {
        const char32_t code_point = decode_utf8(utf8, index).value_or(replacement_character);
        if (code_point >= 0x10000) {
            output += static_cast<char16_t>(0xD800 + ((code_point - 0x10000) >> 10));
            output += static_cast<char16_t>(0xDC00 + ((code_point - 0x10000) & 0x3FF));
        } else {
            output += static_cast<char16_t>(code_point);
        }
    }
    return output;
}

// The two overloads below are chosen by the type of path::native(): std::wstring
// (UTF-16) on Windows, std::string (bytes) on Linux. Only one is ever called on a
// given platform, but both compile everywhere, so no #ifdef is needed.
namespace {

[[maybe_unused]] std::string native_to_utf8(const std::wstring& native)
{
    return utf16_to_utf8(std::u16string(native.begin(), native.end()));
}

[[maybe_unused]] std::string native_to_utf8(const std::string& native)
{
    return utf8_lossy(native);
}

std::filesystem::path::string_type utf8_to_native(std::string_view utf8)
{
    using native_char = std::filesystem::path::value_type;
    if constexpr (sizeof(native_char) == sizeof(char16_t)) {
        const auto units = utf8_to_utf16(utf8);
        return {units.begin(), units.end()};
    } else {
        return std::filesystem::path::string_type(utf8.begin(), utf8.end());
    }
}

} // namespace

std::string path_to_utf8(const std::filesystem::path& path)
{
    return native_to_utf8(path.native());
}

std::filesystem::path path_from_utf8(std::string_view value)
{
    return std::filesystem::path(utf8_to_native(value));
}

} // namespace va
