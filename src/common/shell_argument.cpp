#include "common/shell_argument.h"

#include <algorithm>
#include <filesystem>

namespace va {
namespace {

// A character that no shell reads as anything but itself in an argument.
bool plain(unsigned char character)
{
    return character >= 0x80 || (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') ||
           (character >= '0' && character <= '9') || character == '\\' || character == '/' || character == ':' ||
           character == '.' || character == '_' || character == '-';
}

} // namespace

std::string shell_argument(std::string_view text)
{
    if (!text.empty() && std::all_of(text.begin(), text.end(), [](char c) {
            return plain(static_cast<unsigned char>(c));
        })) {
        return std::string(text);
    }
    if constexpr (std::filesystem::path::preferred_separator == '\\') {
        // A Windows name holds no '"'.
        const auto last = text.find_last_not_of('\\');
        const auto ending = last == std::string_view::npos ? text.size() : text.size() - last - 1;
        std::string quoted = "\"";
        quoted += text;
        quoted.append(ending, '\\');
        quoted += '"';
        return quoted;
    } else {
        std::string quoted = "'";
        for (const char character : text) {
            if (character == '\'') {
                quoted += "'\\''";
            } else {
                quoted += character;
            }
        }
        quoted += '\'';
        return quoted;
    }
}

} // namespace va
