#pragma once
// nlohmann::json exceptions start their text with a tag meant for
// programmers: what() is
//   "[json.exception.type_error.302] type must be string, but is number".
// Messages for people use only the sentence after the tag. The tag means
// nothing to a reader, and it would stop the interface language catalog
// (decision 0017) from recognising the sentence.
#include <exception>
#include <string>
#include <string_view>

namespace va {

// error.what() without a leading "[json.exception.<kind>.<id>] " tag. Text of
// any other exception is returned unchanged.
inline std::string json_error_text(const std::exception& error)
{
    std::string_view text = error.what();
    constexpr std::string_view tag = "[json.exception.";
    if (text.starts_with(tag)) {
        if (const auto end = text.find("] "); end != std::string_view::npos) {
            text.remove_prefix(end + 2);
        }
    }
    return std::string(text);
}

} // namespace va
