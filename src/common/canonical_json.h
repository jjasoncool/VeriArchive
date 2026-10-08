#pragma once
#include <cstdint>
#include <string>
#include <string_view>

namespace va::canonical_json {

std::string quote(std::string_view value);
std::string integer(std::int64_t value);

} // namespace va::canonical_json