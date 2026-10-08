#include "common/canonical_json.h"

#include <cstdio>

namespace va::canonical_json {

std::string quote(std::string_view value)
{
    std::string result{"\""};
    char escaped_control[7];

    for (const unsigned char character : value) {
        if (character == '"' || character == '\\') {
            result += '\\';
            result += static_cast<char>(character);
        } else if (character < 0x20) {
            std::snprintf(
                escaped_control,
                sizeof(escaped_control),
                "\\u%04x",
                character);
            result += escaped_control;
        } else {
            result += static_cast<char>(character);
        }
    }

    result += '"';
    return result;
}

std::string integer(std::int64_t value)
{
    return std::to_string(value);
}

} // namespace va::canonical_json