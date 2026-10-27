#include "common/shell_argument.h"

#include <algorithm>
#include <array>
#include <filesystem>
#include <string_view>

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
        // PowerShell reads `$name` and a backtick within double quotes, and
        // takes a typographic double quote (U+201C, U+201D, U+201E) for one:
        // such a path goes in single quotes, within which it reads nothing
        // but a single quote, also a typographic one (U+2018 to U+201B),
        // which is written twice.
        constexpr std::array<std::string_view, 3> double_quotes{"\u201c", "\u201d", "\u201e"};
        constexpr std::array<std::string_view, 4> single_quotes{"\u2018", "\u2019", "\u201a", "\u201b"};
        const auto holds = [&](std::string_view part) { return text.find(part) != std::string_view::npos; };
        if (holds("$") || holds("`") || std::ranges::any_of(double_quotes, holds)) {
            std::string quoted = "'";
            for (std::size_t index = 0; index < text.size();) {
                const auto rest = text.substr(index);
                const auto quote = std::ranges::find_if(
                    single_quotes, [&](std::string_view mark) { return rest.starts_with(mark); });
                const auto length = rest.starts_with('\'') ? 1 : quote != single_quotes.end() ? quote->size() : 0;
                if (length > 0) {
                    quoted.append(rest.substr(0, length)).append(rest.substr(0, length));
                    index += length;
                } else {
                    quoted += text[index++];
                }
            }
            quoted += '\'';
            return quoted;
        }
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
