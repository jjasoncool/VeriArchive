#include "test_support.h"
#include "app/text.h"

#include <array>
#include <format>
#include <set>
#include <string>

namespace {

using va::app::Language;
using va::app::Match;
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

// True when every brace in `text` belongs to a placeholder: "{}", or with
// `positional` also "{0}", "{1}", ... Format specs ("{:s}") and escaped braces
// are left out on purpose: engine sentences are filled by the simple matcher in
// text.cpp, which knows only these forms, and a spec such as "{:s}" works with
// the test's string values but fails with the numbers the code passes.
bool plain_placeholders(std::string_view text, bool positional)
{
    for (std::size_t index = 0; index < text.size(); ++index) {
        if (text[index] == '}') {
            return false; // a '}' outside a placeholder
        }
        if (text[index] != '{') {
            continue;
        }
        auto close = index + 1;
        while (positional && close < text.size() && text[close] >= '0' && text[close] <= '9') {
            ++close;
        }
        if (close >= text.size() || text[close] != '}') {
            return false;
        }
        index = close;
    }
    return true;
}

// Every entry is usable: the English and the translation are valid templates
// that take the same values, each value exactly once, written only as "{}"
// (and "{0}", "{1}", ... in the translation); no English text appears twice; a
// template matched against sentences has enough literal text that it cannot
// match everything; the match mode fits the template.
void catalog_tests()
{
    std::set<std::string_view> seen;
    for (const auto& entry : va::app::traditional_chinese_catalog()) {
        const bool unique = seen.insert(entry.english).second;
        CHECK(unique);
        CHECK(!entry.english.empty() && !entry.translation.empty());

        const bool plain = plain_placeholders(entry.english, false) &&
                           plain_placeholders(entry.translation, true);
        CHECK(plain);
        if (!plain) {
            std::cerr << "  placeholder other than {} or {N}: " << entry.english << '\n';
        }

        // A Cause entry wraps a message: "context: {}". A FormatOnly entry must be
        // a template; a message without values is never matched against sentences.
        bool match_fits = true;
        if (entry.match == Match::Cause) {
            match_fits = entry.english.ends_with(": {}");
        } else if (entry.match == Match::FormatOnly) {
            match_fits = entry.english.contains("{}");
        }
        CHECK(match_fits);
        if (!match_fits) {
            std::cerr << "  match mode does not fit: " << entry.english << '\n';
        }

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

        if (values > 0 && entry.match != Match::FormatOnly) {
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
    // The system value may be a priority list (Linux LANGUAGE): the first
    // recognised entry wins, and none means English.
    const auto from_system = [](std::string_view system_code) {
        return resolve_language(std::nullopt, std::nullopt, system_code);
    };
    CHECK(from_system("fr:zh_TW:en") == Language::TraditionalChinese);
    CHECK(from_system("fr:en:zh_TW") == Language::English);
    CHECK(from_system("fr::zh_TW") == Language::TraditionalChinese);
    CHECK(from_system("fr") == Language::English);
    CHECK(from_system("") == Language::English);
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
    // A Match::Cause template ("error: {}") translates its last value, a message
    // of its own; format carries its values over unchanged.
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

    // A template that starts with literal text wins over a longer one that
    // starts with a value, so the context is translated and the platform error
    // inside it too.
    CHECK(chinese("cannot scan the root: FindFirstFileExW failed (Win32 error 5)") ==
          "無法掃描根目錄：FindFirstFileExW 失敗（Win32 錯誤 5）");
    CHECK(chinese("RepositoryError: CreateFileW failed (Win32 error 2)") ==
          "儲存庫錯誤：CreateFileW 失敗（Win32 錯誤 2）");
    CHECK(chinese("scan: FindFirstFileExW failed (Win32 error 5)") ==
          "掃描：FindFirstFileExW 失敗（Win32 錯誤 5）");
    CHECK(chinese("scan: ReadFile failed (Win32 error 23)") ==
          "掃描：ReadFile 失敗（Win32 錯誤 23）");
    CHECK(chinese("manifest: open failed: No such file or directory") ==
          "清單檔：open 失敗：No such file or directory");
    CHECK(chinese("scan: opendir failed: Permission denied") ==
          "掃描：opendir 失敗：Permission denied");
    CHECK(chinese("open failed: Permission denied") == "open 失敗：Permission denied");
    // Engine warnings whose cause is an engine message or code.
    CHECK(chinese("manifest: the manifest differs from the repository record") ==
          "清單檔：清單檔與儲存庫的紀錄不符");
    CHECK(chinese("identity: volume-label-differs") == "媒體身分：磁碟區標籤不同");
    CHECK(chinese("12 entries carry portability warnings") == "12 個項目帶有可攜性警告");

    // Only Match::Cause templates translate their last value: a path the user
    // typed is shown as typed, even when it looks like a message.
    CHECK(chinese("not a directory: media") == "不是資料夾：media");
    CHECK(chinese("not a directory: New") == "不是資料夾：New");
    CHECK(chinese("not a directory: D:/Old archives") == "不是資料夾：D:/Old archives");
    CHECK(chinese("not a directory: Photos (2019) revision 2") ==
          "不是資料夾：Photos (2019) revision 2");
    CHECK(chinese("error: not a directory: D:/Old archive") == "錯誤：不是資料夾：D:/Old archive");
    // Inside a cause, a template that starts with a value matches only when it
    // is a platform error ("{} failed ..."); others match any text that ends
    // the same way.
    CHECK(chinese("error: 12 entries carry portability warnings") ==
          "錯誤：12 entries carry portability warnings");

    // FormatOnly templates are used through format and never match sentences.
    CHECK(chinese.format("{} archives", 3) == "3 個封存");
    CHECK(chinese("3 archives") == "3 archives");
    CHECK(chinese("cannot open the archive") == "cannot open the archive");

    // Asides take the language's brackets.
    CHECK(english.parenthesized("size differs") == "(size differs)");
    CHECK(chinese.parenthesized(chinese("size differs")) == "（大小不同）");
}

// A translation that does not fit the values falls back to the English
// template instead of throwing std::format_error.
void format_fallback_tests()
{
    const std::size_t five = 5;
    const auto archives = [&](std::string_view translated) {
        return va::app::format_translation(translated, "{} archive", std::make_format_args(five));
    };
    CHECK(archives("{} 個封存") == "5 個封存");
    CHECK(archives("{:s} 個封存") == "5 archive");    // "{:s}" fits a string, not a number
    CHECK(archives("{} {} 個封存") == "5 archive");   // a value the English does not have
    CHECK(archives("{ 個封存") == "5 archive");       // not a valid template
}

} // namespace

void text_tests()
{
    catalog_tests();
    language_tests();
    translation_tests();
    format_fallback_tests();
}
