#include "test_support.h"
#include "app/text.h"

#include <array>
#include <format>
#include <set>
#include <string>

namespace {

using va::app::Language;
using va::app::Text;

// Formats `pattern` with the values "<v0>" ... "<v7>"; empty when the pattern is
// not a valid format string.
std::string format_with_markers(std::string_view pattern)
{
    std::array<std::string, 8> values;
    for (std::size_t index = 0; index < values.size(); ++index) {
        values[index] = "<v" + std::to_string(index) + ">";
    }
    try {
        return std::vformat(pattern, std::make_format_args(values[0], values[1], values[2], values[3],
            values[4], values[5], values[6], values[7]));
    } catch (const std::format_error&) {
        return {};
    }
}

std::size_t occurrences(std::string_view text, std::string_view part)
{
    std::size_t count = 0;
    for (auto found = text.find(part); found != std::string_view::npos; found = text.find(part, found + 1)) {
        ++count;
    }
    return count;
}

// Every entry is usable: the English and the translation are valid templates
// that take the same values, each value exactly once; no English text appears
// twice; a template has enough literal text that it cannot match everything.
void catalog_tests()
{
    std::set<std::string_view> seen;
    for (const auto& entry : va::app::traditional_chinese_catalog()) {
        const bool unique = seen.insert(entry.english).second;
        CHECK(unique);
        CHECK(!entry.english.empty() && !entry.translation.empty());

        const auto values = occurrences(entry.english, "{}");
        const auto english = format_with_markers(entry.english);
        const auto translation = format_with_markers(entry.translation);
        const bool english_valid = !english.empty();
        const bool translation_valid = !translation.empty();
        CHECK(english_valid);
        CHECK(translation_valid);
        if (!english_valid || !translation_valid) {
            std::cerr << "  entry: " << entry.english << '\n';
            continue;
        }
        for (std::size_t index = 0; index <= values && index < 8; ++index) {
            const auto marker = "<v" + std::to_string(index) + ">";
            const auto expected = index < values ? 1u : 0u;
            const bool same_values = occurrences(english, marker) == expected &&
                                     occurrences(translation, marker) == expected;
            CHECK(same_values);
            if (!same_values) {
                std::cerr << "  entry: " << entry.english << '\n';
            }
        }

        if (values > 0) {
            std::size_t letters = 0;
            for (const char character : entry.english) {
                letters += (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z');
            }
            const bool specific = letters >= 3;
            CHECK(specific);
            if (!specific) {
                std::cerr << "  template too general: " << entry.english << '\n';
            }
        }
    }
}

void language_tests()
{
    using va::app::parse_language;
    CHECK(parse_language("zh-TW") == Language::TraditionalChinese);
    CHECK(parse_language("zh_TW.UTF-8") == Language::TraditionalChinese);
    CHECK(parse_language("zh-Hant") == Language::TraditionalChinese);
    CHECK(parse_language("ZH") == Language::TraditionalChinese);
    CHECK(parse_language("en-US") == Language::English);
    CHECK(parse_language("C") == Language::English);
    CHECK(parse_language("POSIX") == Language::English);
    CHECK(!parse_language("fr"));
    CHECK(!parse_language(""));

    using va::app::resolve_language;
    CHECK(resolve_language("en", "zh-TW", "zh-TW") == Language::English);
    CHECK(resolve_language("fr", "zh-TW", "en-US") == Language::TraditionalChinese);
    CHECK(resolve_language(std::nullopt, "", "zh-TW") == Language::TraditionalChinese);
    CHECK(resolve_language(std::nullopt, std::nullopt, "fr-FR") == Language::English);
    CHECK(va::app::language_code(Language::TraditionalChinese) == "zh-TW");
}

void translation_tests()
{
    const Text english;
    CHECK(english("PASSED") == "PASSED");
    CHECK(english("cannot read: anything") == "cannot read: anything");
    CHECK(english.format("hashing: {}/{} files, {} MiB", 1, 2, 3) == "hashing: 1/2 files, 3 MiB");

    const Text chinese(Language::TraditionalChinese);
    CHECK(chinese("PASSED") == "通過");
    CHECK(chinese("Unreadable(media)") == "無法讀取（媒體錯誤）");
    CHECK(chinese("Unreadable(something new)") == "無法讀取（something new）");
    CHECK(chinese("a sentence without a translation") == "a sentence without a translation");
    // The cause after a final ": " is a message of its own and is translated too.
    CHECK(chinese("error: PASSED") == "錯誤：通過");
    CHECK(chinese.format("error: {}", "PASSED") == "錯誤：PASSED");
    // Positional values follow the Chinese word order.
    CHECK(chinese("coverage: 7 of 13 registered bytes read and matched") ==
          "涵蓋範圍：已註冊的 13 位元組中，有 7 位元組已讀取且相符");
    CHECK(chinese.format("coverage: {} of {} registered bytes read and matched", 7, 13) ==
          "涵蓋範圍：已註冊的 13 位元組中，有 7 位元組已讀取且相符");
    // Very long values (Windows long paths) are matched in linear time.
    const std::string long_path(100000, 'x');
    CHECK(chinese("error: " + long_path) == "錯誤：" + long_path);
}

} // namespace

void text_tests()
{
    catalog_tests();
    language_tests();
    translation_tests();
}
