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

void ObjectWriter::key(std::string_view name)
{
    if (text_.size() > 1) {
        text_ += ',';
    }
    text_ += quote(name);
    text_ += ':';
}

ObjectWriter& ObjectWriter::add_string(std::string_view name, std::string_view value)
{
    key(name);
    text_ += quote(value);
    return *this;
}

ObjectWriter& ObjectWriter::add_integer(std::string_view name, std::int64_t value)
{
    key(name);
    text_ += integer(value);
    return *this;
}

ObjectWriter& ObjectWriter::add_unsigned(std::string_view name, std::uint64_t value)
{
    key(name);
    text_ += std::to_string(value);
    return *this;
}

ObjectWriter& ObjectWriter::add_bool(std::string_view name, bool value)
{
    key(name);
    text_ += value ? "true" : "false";
    return *this;
}

ObjectWriter& ObjectWriter::add_strings(std::string_view name, const std::vector<std::string>& values)
{
    key(name);
    text_ += '[';
    for (std::size_t index = 0; index < values.size(); ++index) {
        if (index > 0) {
            text_ += ',';
        }
        text_ += quote(values[index]);
    }
    text_ += ']';
    return *this;
}

ObjectWriter& ObjectWriter::add_raw(std::string_view name, std::string_view json)
{
    key(name);
    text_ += json;
    return *this;
}

} // namespace va::canonical_json
