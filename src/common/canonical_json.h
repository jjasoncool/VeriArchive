#pragma once
// Canonical JSON writer (HANDOFF R-64): keys in the order they are added, no
// whitespace, integers in plain decimal, only '"', '\' and U+0000-U+001F
// escaped. The same data therefore always produces the same bytes, which is
// what manifest hashing and signing need. Parsing may use nlohmann/json.
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace va::canonical_json {

std::string quote(std::string_view value);
std::string integer(std::int64_t value);

// Builds one JSON object. Optional fields are simply not added (R-64: absent
// optionals are omitted, never null).
class ObjectWriter {
public:
    ObjectWriter& add_string(std::string_view key, std::string_view value);
    ObjectWriter& add_integer(std::string_view key, std::int64_t value);
    ObjectWriter& add_unsigned(std::string_view key, std::uint64_t value);
    ObjectWriter& add_bool(std::string_view key, bool value);
    ObjectWriter& add_strings(std::string_view key, const std::vector<std::string>& values);
    // `json` must already be canonical JSON (a nested object or array).
    ObjectWriter& add_raw(std::string_view key, std::string_view json);

    std::string str() const { return text_ + '}'; }

private:
    void key(std::string_view name);
    std::string text_ = "{";
};

} // namespace va::canonical_json
