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
          "清單檔：open 失敗：找不到檔案或資料夾");
    CHECK(chinese("scan: opendir failed: Permission denied") ==
          "掃描：opendir 失敗：權限不足");
    CHECK(chinese("open failed: Permission denied") == "open 失敗：權限不足");
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

    // Library and system causes (findings 8, 22, 23).
    CHECK(chinese("cannot run SQL: database is locked") == "無法執行 SQL 陳述式：資料庫已被鎖定");
    CHECK(chinese("cannot execute SQL: database is locked") == "無法執行 SQL：資料庫已被鎖定");
    CHECK(chinese("cannot open repository database: unable to open database file") ==
          "無法開啟儲存庫資料庫：無法開啟資料庫檔案");
    CHECK(chinese("cannot record the archive: cannot run SQL: database is locked") ==
          "無法記錄封存：無法執行 SQL 陳述式：資料庫已被鎖定");
    CHECK(chinese("cannot run SQL: UNIQUE constraint failed: t.a, t.b") ==
          "無法執行 SQL 陳述式：違反唯一性限制：t.a, t.b");
    CHECK(chinese("UNIQUE constraint failed: t.a") == "違反唯一性限制：t.a");
    CHECK(chinese("cannot prepare SQL: no such table: archives") ==
          "無法準備 SQL 陳述式：找不到資料表：archives");
    CHECK(chinese("cannot prepare SQL: file is not a database") == "無法準備 SQL 陳述式：檔案不是資料庫");
    CHECK(chinese("pread failed: Input/output error") == "pread 失敗：輸入/輸出錯誤");
    CHECK(chinese("cannot scan the root: open directory failed: Permission denied") ==
          "無法掃描根目錄：open directory 失敗：權限不足");
    CHECK(chinese("cannot identify the volume of the root: canonical failed: No such file or directory") ==
          "無法識別根目錄所在的磁碟區：canonical 失敗：找不到檔案或資料夾");
    CHECK(chinese("scan: readdir failed: Input/output error") == "掃描：readdir 失敗：輸入/輸出錯誤");
    CHECK(chinese("Permission denied") == "權限不足");
    // JSON library sentences inside config and manifest messages.
    CHECK(chinese("Invalid config: key 'key_id' not found") == "設定檔無效：找不到欄位「key_id」");
    CHECK(chinese("Invalid config: type must be string, but is number") ==
          "設定檔無效：類型必須是 string，實際是 number");
    CHECK(chinese("invalid manifest header: not a version 1 .vam manifest") ==
          "清單檔標頭無效：不是格式版本 1 的 .vam 清單檔");
    CHECK(chinese("invalid manifest header: cannot use at() with array") ==
          "清單檔標頭無效：需要 JSON 物件，實際是 array");
    CHECK(chinese("invalid manifest entry: unknown entry type x") == "清單檔項目無效：未知的項目類型 x");
    CHECK(chinese("Invalid config: parse error at line 1, column 1: attempting to parse an empty input; "
                  "check that your input string or stream contains the expected JSON") ==
          "設定檔無效：JSON 語法錯誤（第 1 行，第 1 欄）：內容是空的，沒有 JSON 資料");
    CHECK(chinese("invalid manifest header: parse error at line 1, column 2: syntax error while parsing "
                  "object key - unexpected end of input; expected string literal") ==
          "清單檔標頭無效：JSON 語法錯誤（第 1 行，第 2 欄）：syntax error while parsing "
          "object key - unexpected end of input; expected string literal");
    // Rebuild-index skipped lines.
    const std::string uuid = "0b6c8ad6-2a51-4d8e-9a6a-0f1c2d3e4f50";
    CHECK(chinese("cannot read manifest " + uuid + "/r0001.vam: empty manifest") ==
          "無法讀取清單檔 " + uuid + "/r0001.vam：清單檔是空的");
    CHECK(chinese("cannot read manifest " + uuid + "/r0002.vam: header does not match its file name") ==
          "無法讀取清單檔 " + uuid + "/r0002.vam：標頭與其檔名不符");
    CHECK(chinese("cannot read manifest " + uuid + "/r0003.vam: invalid manifest header: key 'format' not found") ==
          "無法讀取清單檔 " + uuid + "/r0003.vam：清單檔標頭無效：找不到欄位「format」");
    CHECK(chinese("cannot list the manifests of archive " + uuid + ": FindFirstFileExW failed (Win32 error 5)") ==
          "無法列出封存 " + uuid + " 的清單檔：FindFirstFileExW 失敗（Win32 錯誤 5）");
    CHECK(chinese.format("skipped: {}", chinese("cannot read manifest " + uuid + "/r0001.vam: empty manifest")) ==
          "已略過：無法讀取清單檔 " + uuid + "/r0001.vam：清單檔是空的");

    // Comparing archives (CLI `archive compare`, GUI Compare Archives). The
    // result line keeps its mode word in every language, as verification's does.
    const std::string uuid_b = "94bbff52-39fa-458a-8831-9b30fb0cadf1";
    CHECK(chinese.format("COMPARE {}  {} ({}) r{}  vs  {} ({}) r{}", chinese("DIFFERENT"), "ARCH-0001", uuid, 1,
              "ARCH-0002", uuid_b, 2) ==
          "比較結果：不同  ARCH-0001（" + uuid + "）r1  與  ARCH-0002（" + uuid_b + "）r2");
    CHECK(english.format("COMPARE {}  {} ({}) r{}  vs  {} ({}) r{}", english("IDENTICAL"), "ARCH-0001", uuid, 1,
              "ARCH-0002", uuid_b, 1) ==
          "COMPARE IDENTICAL  ARCH-0001 (" + uuid + ") r1  vs  ARCH-0002 (" + uuid_b + ") r1");
    CHECK(chinese.format("ARCHIVES {}", chinese("IDENTICAL")) == "比較結果：相同");
    CHECK(chinese.format("ARCHIVES {}", chinese("IDENTICAL_WITH_WARNINGS")) == "比較結果：相同（有警告）");
    CHECK(chinese.format("same: {}", 4) + "  " + chinese("Missing") + " 1" == "相同：4  遺失 1");
    // Item details: sizes are values; the reason an entry was unreadable is
    // translated, for every reason the scanner records.
    CHECK(chinese("size 7 -> 14") == "大小 7 → 14");
    CHECK(chinese.parenthesized(chinese("size 7 -> 14")) == "（大小 7 → 14）");
    for (const std::string reason : {"permission", "media", "other"}) {
        for (const std::string side : {"A", "B"}) {
            CHECK(chinese("unreadable in " + side + " (" + reason + ")") ==
                  side + " 註冊時無法讀取（" + chinese(reason) + "）");
        }
    }
    CHECK(chinese("inside a directory unreadable in B") == "位於 B 註冊時無法讀取的資料夾內");
    CHECK(chinese("unreadable in both archives") == "兩個封存註冊時都無法讀取");
    // Failures: the CLI puts the failure class first, the GUI shows the message.
    CHECK(chinese("error: ManifestHashMismatch: cannot read the manifest of ARCH-0002 revision 2: "
                  "manifest body does not match its header (ManifestHashMismatch)") ==
          "錯誤：清單檔雜湊不符：無法讀取 ARCH-0002 版次 2 的清單檔：清單檔主體與其標頭不符（清單檔雜湊不符）");
    CHECK(chinese("error: RepositoryError: archive " + uuid + " has no revision yet") ==
          "錯誤：儲存庫錯誤：封存 " + uuid + " 尚無版次");
    CHECK(chinese("error: the comparison was cancelled") == "錯誤：比較已取消");
    CHECK(chinese.format("Not compared: {}",
              chinese("cannot read the manifest of ARCH-0002 revision 2: revision 2 is not recorded")) ==
          "未能比較：無法讀取 ARCH-0002 版次 2 的清單檔：版次 2 沒有紀錄");
    CHECK(english("cannot read the manifest of ARCH-0002 revision 2: revision 2 is not recorded") ==
          "cannot read the manifest of ARCH-0002 revision 2: revision 2 is not recorded");
    // Wording fixes (findings 27, 29, 30, 31, 32).
    CHECK(chinese("warning: identity: retired") == "警告：媒體身分：封存已停用");
    CHECK(chinese("RETIRED") == "已停用");
    CHECK(chinese("l -> f") == "連結 → 檔案");
    CHECK(chinese.format("files: {}  empty directories: {}  links: {}  hashed bytes: {}", 2, 1, 0, 12) ==
          "檔案：2  空資料夾：1  連結：0  已計算雜湊位元組：12");
    CHECK(chinese("warning: the media holds revision 2, newer than the repository's 1: "
                  "the repository may have been rolled back") ==
          "警告：媒體上有版次 2，比儲存庫的版次 1 新：儲存庫可能已被回復到較舊的狀態");
    CHECK(chinese.format("repo_selected_by: {}", chinese("first-run:exe-dir")) ==
          "儲存庫選定方式：第一次使用（程式所在資料夾）");
    CHECK(chinese("error: Trusted keys must be an array") == "錯誤：設定檔中的 trusted_keys 必須是 JSON 陣列");
    CHECK(chinese("Finish or cancel the running operation before changing the language.") ==
          "請先完成或取消正在執行的作業，再切換語言。");
    // English stays byte-identical.
    CHECK(english("cannot run SQL: database is locked") == "cannot run SQL: database is locked");
    CHECK(english("first-run:home") == "first-run:home");

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
