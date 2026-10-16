#include "test_support.h"
#include "app/history.h"
#include "app/plain_words.h"
#include "app/text.h"
#include "app/unfinished_operations.h"
#include "app/verify_report.h"
#include "common/utf8.h"

#include <array>
#include <filesystem>
#include <format>
#include <set>
#include <string>

namespace {

using va::app::Language;
using va::app::Match;
using va::app::Text;

// The most values a catalog entry takes, as format_with_markers fills them.
constexpr std::size_t marker_count = 10;

// Formats `pattern` with the values "<v0>" ... "<v9>"; empty when the pattern is
// not a valid format string.
std::string format_with_markers(std::string_view pattern)
{
    std::array<std::string, marker_count> values;
    for (std::size_t index = 0; index < values.size(); ++index) {
        values[index] = "<v" + std::to_string(index) + ">";
    }
    try {
        return std::vformat(pattern, std::make_format_args(values[0], values[1], values[2], values[3],
            values[4], values[5], values[6], values[7], values[8], values[9]));
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

// True when `text` names a version of the program as the specification does,
// "V" and a digit at the start of a word ("V1", "V1.1"): an interface text that
// promises something for a version stays true in no other (decision 0017).
bool names_a_version(std::string_view text)
{
    const auto letter_or_digit = [](char character) {
        return (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') ||
               (character >= '0' && character <= '9');
    };
    for (std::size_t index = 0; index + 1 < text.size(); ++index) {
        if (text[index] == 'V' && text[index + 1] >= '0' && text[index + 1] <= '9' &&
            (index == 0 || !letter_or_digit(text[index - 1]))) {
            return true;
        }
    }
    return false;
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
// match everything; the match mode fits the template. No entry names a version
// of the program: what a version does not do yet is said of "this version".
void catalog_tests()
{
    std::set<std::string_view> seen;
    for (const auto& entry : va::app::traditional_chinese_catalog()) {
        const bool unique = seen.insert(entry.english).second;
        CHECK(unique);
        CHECK(!entry.english.empty() && !entry.translation.empty());

        const bool versionless = !names_a_version(entry.english) && !names_a_version(entry.translation);
        CHECK(versionless);
        if (!versionless) {
            std::cerr << "  names a version of the program: " << entry.english << '\n';
        }

        const bool plain = plain_placeholders(entry.english, false) &&
                           plain_placeholders(entry.translation, true);
        CHECK(plain);
        if (!plain) {
            std::cerr << "  placeholder other than {} or {N}: " << entry.english << '\n';
        }

        // A Cause entry wraps a message: "context: {}", which a hint may follow.
        // A FormatOnly entry must be a template; a message without values is
        // never matched against sentences.
        bool match_fits = true;
        if (entry.match == Match::Cause) {
            match_fits = entry.english.contains(": {}");
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
        for (std::size_t index = 0; index <= values && index < marker_count; ++index) {
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
    CHECK(english.format("hashing: {}", "1.00 GiB of 2.00 GiB (50%)") == "hashing: 1.00 GiB of 2.00 GiB (50%)");

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
    CHECK(chinese("cannot scan the root: access denied (FindFirstFileExW, Windows error 5)") ==
          "無法掃描根目錄：存取被拒（FindFirstFileExW，Windows 錯誤 5）");
    CHECK(chinese("not found (CreateFileW, Windows error 2)") == "找不到（CreateFileW，Windows 錯誤 2）");
    CHECK(chinese("scan: access denied (FindFirstFileExW, Windows error 5)") ==
          "掃描：存取被拒（FindFirstFileExW，Windows 錯誤 5）");
    CHECK(chinese("scan: the device reported an error (ReadFile, Windows error 23)") ==
          "掃描：裝置回報錯誤（ReadFile，Windows 錯誤 23）");
    CHECK(chinese("manifest: open failed: No such file or directory") ==
          "清單檔：open 失敗：找不到檔案或資料夾");
    CHECK(chinese("scan: opendir failed: Permission denied") ==
          "掃描：opendir 失敗：權限不足");
    CHECK(chinese("open failed: Permission denied") == "open 失敗：權限不足");
    // Engine warnings whose cause is an engine message or code.
    CHECK(chinese("manifest: the manifest differs from the repository record") ==
          "清單檔：清單檔與儲存庫的紀錄不符");
    CHECK(chinese("identity: the volume label differs from the one registered") == "媒體身分：磁碟區標籤與註冊時不同");
    CHECK(chinese("12 entries carry portability warnings") == "12 個項目帶有可攜性警告");
    // §A6, R-42: a verification of a revision whose manifest records names
    // that collide by case or by Unicode normalisation says so.
    CHECK(chinese("some names differ from another name in the same folder only in upper and lower case, or in how "
                  "a letter with an accent or a mark is encoded (Unicode normalisation); a volume or system that "
                  "does not tell such names apart cannot keep them all") ==
          "有些名稱與同一資料夾中的另一個名稱只差在大小寫，或帶重音、標記的字母的編碼方式（Unicode 正規化）；"
          "不區分這些差異的磁碟區或作業系統無法同時保存它們");
    // Decision 0006: the note of a verification read through ntfs-3g.
    CHECK(chinese.format("note: {}", chinese("driver: ntfs-3g")) == "附註：驅動程式：ntfs-3g");
    CHECK(chinese.format("Note: {}", chinese("driver: ntfs-3g")) == "附註：驅動程式：ntfs-3g");
    // Decision 0034: an INCOMPLETE run whose device went away says so.
    CHECK(chinese("the device that holds the archive went away while it was read (unplugged, or no longer "
                  "answering); attach it again, then resume this run") ==
          "存放此封存的裝置在讀取期間離開了（被拔除，或不再回應）；請重新連接它，再繼續這次執行");
    // Decision 0034: a verification, a registration and a relocation whose
    // device stopped answering say so, and what to do.
    CHECK(chinese("the device that holds the archive stopped answering while it was read: 10 entries in a row could "
                  "not be read; unplug it and attach it again, then resume this run; if it stops again at the same "
                  "place, the device may be damaged") ==
          "存放此封存的裝置在讀取期間停止回應：連續 10 個項目無法讀取；請拔除它再重新連接，然後繼續這次執行；"
          "如果它又在同一個地方停止，裝置可能已經損壞");
    CHECK(chinese("the device stopped answering during registration: 10 entries in a row could not be read; unplug "
                  "it and attach it again; continue with `register Z:\\範例 --resume`; if it stops again at the "
                  "same place, the device may be damaged") ==
          "裝置在註冊期間停止回應：連續 10 個項目無法讀取；請拔除它再重新連接，然後以 `register Z:\\範例 --resume` "
          "繼續；如果它又在同一個地方停止，裝置可能已經損壞");
    CHECK(chinese("relocation was interrupted: the device of the new carrier stopped answering (10 entries in a row "
                  "could not be read); unplug it and attach it again, then continue it with `archive relocate "
                  "c0ffee00-0000-4000-8000-000000000034 \"Z:\\範例 SAMPLE\" --resume`; if it stops again at the "
                  "same place, the device may be damaged") ==
          "搬移已中斷：新載體的裝置停止回應（連續 10 個項目無法讀取）；請拔除它再重新連接，然後以 "
          "`archive relocate c0ffee00-0000-4000-8000-000000000034 \"Z:\\範例 SAMPLE\" --resume` 繼續；"
          "如果它又在同一個地方停止，裝置可能已經損壞");

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

    // What a check of the history chain tells (app/history.h), which the
    // command line and the GUI show after a chain that holds. Signed history
    // checkpoints are 歷程簽章, never the 檢查點 of a resume.
    CHECK(chinese(va::app::history_chain_limits) ==
          "檢查歷程鏈可偵測已記錄的執行紀錄被修改或刪除，但偵測不到最新幾筆執行紀錄被刪除，也偵測不到修改後連雜湊鏈都"
          "重新計算過的紀錄；能偵測這些情況的歷程簽章，這個版本尚未提供。");

    // FormatOnly templates are used through format and never match sentences.
    CHECK(chinese.format("{} archives", 3) == "3 個封存");
    CHECK(chinese("3 archives") == "3 archives");
    CHECK(chinese("cannot open the archive") == "cannot open the archive");

    // The progress of hashing (app::describe_progress), whose pieces are
    // FormatOnly: "{} · {}" or "{}/s" must not take over engine sentences.
    CHECK(chinese.format("hashing: {}", chinese.format("{} · {}", chinese.format("{} of {} ({}%)", "2.16 TiB",
              "9.44 TiB", 23), chinese.format("about {}-{} hours left", 8, 9))) ==
          "計算雜湊中：2.16 TiB / 9.44 TiB（23%）・約剩 8～9 小時");
    CHECK(chinese.format("{}/s", "240 MiB") == "每秒 240 MiB");
    CHECK(chinese.format("{} of {} files ({}%)", 12, 345, 3) == "12 / 345 個檔案（3%）");
    CHECK(chinese("estimating time left") == "剩餘時間估算中");
    CHECK(chinese("PASSED · FAILED") == "PASSED · FAILED");
    CHECK(chinese("files/s") == "files/s");
    CHECK(chinese("about 8-9 hours left") == "about 8-9 hours left");

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
    // A repository that cannot be opened is named with its path (decision 0037).
    CHECK(chinese("error: the configured repository K:/VeriArchive-SAMPLE/bin/data is unavailable or is not a VeriArchive "
                  "repository (the config file K:/VeriArchive-SAMPLE/bin/config.json names it)") ==
          "錯誤：設定的儲存庫 K:/VeriArchive-SAMPLE/bin/data 無法使用，或不是 VeriArchive 儲存庫（記錄在設定檔 "
          "K:/VeriArchive-SAMPLE/bin/config.json 中）");
    CHECK(chinese("error: VERIARCHIVE_REPOSITORY names the repository K:/repository-SAMPLE, which is unavailable or "
                  "is not a VeriArchive repository") ==
          "錯誤：VERIARCHIVE_REPOSITORY 指定的儲存庫 K:/repository-SAMPLE 無法使用，或不是 VeriArchive 儲存庫");
    CHECK(chinese("error: the repository K:/repository-SAMPLE is unavailable or is not a VeriArchive repository") ==
          "錯誤：儲存庫 K:/repository-SAMPLE 無法使用，或不是 VeriArchive 儲存庫");
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
    // Chunk lines and the Merkle root (decision 0031).
    CHECK(chinese("invalid manifest header: chunk_size 1000 is not 0 or a multiple of 65536 up to 1073741824") ==
          "清單檔標頭無效：chunk_size 1000 不是 0，也不是 65536 的倍數（上限 1073741824）");
    CHECK(chinese("invalid manifest header: merkle_root is missing or not 64 lowercase hex digits") ==
          "清單檔標頭無效：merkle_root 缺少或不是 64 個小寫十六進位數字");
    CHECK(chinese("chunk 1 of big.bin is missing or does not match the file") == "big.bin 的區塊 1 遺失或與檔案不符");
    CHECK(chinese("chunk 0 of locked.bin belongs to no file") == "locked.bin 的區塊 0 不屬於任何檔案");
    CHECK(chinese("invalid chunk line: key 'idx' not found") == "區塊行無效：找不到欄位「idx」");
    CHECK(chinese("invalid chunk line: b3 is not 64 lowercase hex digits") ==
          "區塊行無效：b3 不是 64 個小寫十六進位數字");
    CHECK(chinese("invalid manifest entry: sha256 is not 64 lowercase hex digits") ==
          "清單檔項目無效：sha256 不是 64 個小寫十六進位數字");
    CHECK(chinese("cannot write the manifest: chunk hashes of big.bin: 2 given, 3 expected") ==
          "無法寫入清單檔：big.bin 的區塊雜湊：提供 2 個，應為 3 個");
    // The index of chunk hashes (decision 0032).
    CHECK(chinese("cannot upgrade the repository database to schema version 2: cannot execute SQL: database is "
                  "locked") == "無法將儲存庫資料庫升級到結構描述版本 2：無法執行 SQL：資料庫已被鎖定");
    CHECK(chinese("chunk hashes of big.bin: 2 given, 3 expected") == "big.bin 的區塊雜湊：提供 2 個，應為 3 個");
    // Each revision's chunks in archive show (decision 0032): the Merkle root is 總碼 there.
    CHECK(chinese.format("chunk size {}, {} chunks, Merkle root {}", "64 MiB", 160, "4f3f") ==
          "區塊大小 64 MiB，160 個區塊，總碼 4f3f");
    CHECK(chinese("chunk size 0: whole-file hashes only") == "區塊大小 0：只有整個檔案的雜湊");
    // A file whose content differs names the chunks that differ (decision
    // 0032): the chunks, their number and the byte are values, the word that
    // joins the last of a list is translated.
    CHECK(chinese("content differs: chunk 1 of 3 differs, from byte 65,536") ==
          "內容不同：共 3 個區塊，其中區塊 1 不同，從位元組 65,536 開始");
    CHECK(chinese("content differs: chunks 3 and 17 of 160 differ, the first from byte 201,326,592") ==
          "內容不同：共 160 個區塊，其中區塊 3 和 17 不同，第一個從位元組 201,326,592 開始");
    CHECK(chinese("content differs: chunks 3, 5 and 17 of 160 differ, the first from byte 201,326,592") ==
          "內容不同：共 160 個區塊，其中區塊 3, 5 和 17 不同，第一個從位元組 201,326,592 開始");
    CHECK(chinese("content differs: chunks 20-21 of 40 differ, the first from byte 1,310,720") ==
          "內容不同：共 40 個區塊，其中區塊 20-21 不同，第一個從位元組 1,310,720 開始");
    CHECK(chinese("content differs: chunks 1-3, 5, 8-9, 12, 14, 16, 18, 20, 22, 24 and 4 more of 40 differ, the first "
                  "from byte 65,536") ==
          "內容不同：共 40 個區塊，其中區塊 1-3, 5, 8-9, 12, 14, 16, 18, 20, 22, 24 及另外 4 個區塊不同，第一個從位元組 "
          "65,536 開始");
    CHECK(english("content differs: chunk 1 of 3 differs, from byte 65,536") ==
          "content differs: chunk 1 of 3 differs, from byte 65,536");
    // A warning of the run for chunk hashes that do not match a file that
    // matches (decision 0032): the file's name is a value, as are the chunks.
    const std::string whole = ", while the whole file matches its hashes; the file is fine, and a new revision fixes them";
    const std::string whole_chinese = "，但整個檔案與其雜湊相符；檔案本身沒有問題，建立新版次即可修正";
    CHECK(chinese("the chunk hashes of big.bin cannot be relied on: chunk 1 of 3 does not match" + whole) ==
          "big.bin 的區塊雜湊不可靠：共 3 個區塊，其中區塊 1 不符" + whole_chinese);
    CHECK(chinese.format("warning: {}",
              chinese("the chunk hashes of 影片/raw of 2026.mov cannot be relied on: chunks 0 and 2 of 3 do not match" +
                      whole)) == "警告：影片/raw of 2026.mov 的區塊雜湊不可靠：共 3 個區塊，其中區塊 0 和 2 不符" +
                                     whole_chinese);
    CHECK(chinese("the chunk hashes of big.bin cannot be relied on: chunks 1-3 of 40 do not match" + whole) ==
          "big.bin 的區塊雜湊不可靠：共 40 個區塊，其中區塊 1-3 不符" + whole_chinese);
    CHECK(chinese("the chunk hashes of big.bin cannot be relied on: chunks 1-3, 5, 8-9, 12, 14, 16, 18, 20, 22, 24 and "
                  "4 more of 40 do not match" + whole) ==
          "big.bin 的區塊雜湊不可靠：共 40 個區塊，其中區塊 1-3, 5, 8-9, 12, 14, 16, 18, 20, 22, 24 及另外 4 個區塊不符" +
              whole_chinese);
    // Adding chunk hashes (decision 0033): the refusal, the revision that
    // records them, why none does, and a run that keeps its saved progress.
    const std::string sample = "c0ffee00-0000-4000-8000-000000000001";
    CHECK(chinese("revision 1 of archive " + sample +
                  " has chunk hashes already; a FULL verify compares them, and there are none to add") ==
          "封存 " + sample + " 的版次 1 已有區塊雜湊；完整驗證會比對它們，沒有可加入的區塊雜湊");
    CHECK(chinese.format("chunk hashes added: revision {} records them (chunk size {}, {} chunks, Merkle root {})", 2,
              "64 MiB", 160, "4f3f") == "已加入區塊雜湊：由版次 2 記錄（區塊大小 64 MiB，160 個區塊，總碼 4f3f）");
    CHECK(chinese.format("chunk hashes not added: {}",
              chinese("the verification did not pass; revision 1 stays without chunk hashes")) ==
          "未加入區塊雜湊：驗證未通過；版次 1 仍不含區塊雜湊");
    CHECK(chinese("not every entry matches revision 1 as recorded; revision 1 stays without chunk hashes (`register "
                  "--new-revision` records the archive as it is now, with chunk hashes)") ==
          "並非每個項目都與版次 1 的紀錄相符；版次 1 仍不含區塊雜湊（`register --new-revision` "
          "會依封存目前的內容記錄新版次，並含區塊雜湊）");
    CHECK(chinese("archive " + sample + " is at revision 2 now, recorded by another operation while this verification "
                                        "ran") == "封存 " + sample + " 現在是版次 2，由其他作業在這次驗證期間記錄");
    CHECK(chinese("verification finished as PASSED but could not be recorded: cannot execute SQL: database is locked; "
                  "resume the run with `verify --full " + sample + " --resume R` to record it") ==
          "驗證結果為通過，但無法記錄：無法執行 SQL：資料庫已被鎖定；請以 `verify --full " + sample +
              " --resume R` 繼續該執行，以記錄該版次");
    CHECK(chinese("verification finished as PASSED but could not be recorded: cannot execute SQL: database is locked") ==
          "驗證結果為通過，但無法記錄：無法執行 SQL：資料庫已被鎖定");
    CHECK(chinese("run R adds no chunk hashes; resume it without --add-chunk-hashes, or start a new verification with "
                  "it") == "執行 R 不會加入區塊雜湊；請不加 --add-chunk-hashes 繼續它，或加上 --add-chunk-hashes 開始新的驗證");
    // Rebuild-index skipped lines.
    const std::string uuid = "0b6c8ad6-2a51-4d8e-9a6a-0f1c2d3e4f50";
    CHECK(chinese("cannot read manifest " + uuid + "/r0001.vam: empty manifest") ==
          "無法讀取清單檔 " + uuid + "/r0001.vam：清單檔是空的");
    CHECK(chinese("cannot read manifest " + uuid + "/r0002.vam: header does not match its file name") ==
          "無法讀取清單檔 " + uuid + "/r0002.vam：標頭與其檔名不符");
    CHECK(chinese("cannot read manifest " + uuid + "/r0003.vam: invalid manifest header: key 'format' not found") ==
          "無法讀取清單檔 " + uuid + "/r0003.vam：清單檔標頭無效：找不到欄位「format」");
    // The entries read again give other bytes than the body validated.
    CHECK(chinese("cannot read manifest " + uuid + "/r0004.vam: the manifest changed while it was read") ==
          "無法讀取清單檔 " + uuid + "/r0004.vam：清單檔在讀取期間被變更");
    CHECK(chinese("cannot list the manifests of archive " + uuid + ": access denied (FindFirstFileExW, Windows error 5)") ==
          "無法列出封存 " + uuid + " 的清單檔：存取被拒（FindFirstFileExW，Windows 錯誤 5）");
    CHECK(chinese.format("skipped: {}", chinese("cannot read manifest " + uuid + "/r0001.vam: empty manifest")) ==
          "已略過：無法讀取清單檔 " + uuid + "/r0001.vam：清單檔是空的");
    CHECK(chinese.format("unfinished registrations whose saved progress was kept: {}", 2) ==
          "保留了已儲存進度的未完成註冊：2 個"); // decision 0026
    CHECK(chinese("cannot read manifest " + uuid + "/r0004.vam: manifest content hashes do not match its "
                  "merkle_root") ==
          "無法讀取清單檔 " + uuid + "/r0004.vam：清單檔的內容雜湊與其 Merkle 根雜湊不符");

    // Which process runs a registration (decision 0024) and abandoning one
    // (decision 0025): the process id, the program and the uuid are values.
    CHECK(chinese("this registration is still running in process 4242 (veriarchive-cli.exe) on this computer; "
                  "resume it once that process has ended") ==
          "此註冊仍在這台電腦的處理程序 4242（veriarchive-cli.exe）中執行；請等該處理程序結束後再繼續");
    CHECK(chinese("error: the registration of archive " + uuid + " is still running in process 77 "
                  "(veriarchive-gui.exe) on this computer; abandon it once that process has ended") ==
          "錯誤：封存 " + uuid + " 的註冊仍在這台電腦的處理程序 77（veriarchive-gui.exe）中執行；請等該處理程序結束後再放棄");
    CHECK(chinese("an unfinished registration of archive " + uuid + " (root \"Z:\\範例\") is creating revision 2; "
                  "nothing was changed (finish it with `register Z:\\範例 --resume` first, or abandon it with "
                  "`archive abandon-registration " + uuid + "`)") ==
          "封存 " + uuid + " 有一個未完成的註冊（根目錄「Z:\\範例」）正在建立版次 2；未做任何變更（請先以 "
          "`register Z:\\範例 --resume` 完成該註冊，或以 `archive abandon-registration " + uuid + "` 放棄它）");
    // A resume names the root, in quotes where a shell would read it otherwise
    // (decision 0022, amended 2026-10-27).
    CHECK(chinese("error: registration was cancelled; continue with `register \"Z:\\範例 SAMPLE\" --resume`") ==
          "錯誤：註冊已取消；請以 `register \"Z:\\範例 SAMPLE\" --resume` 繼續");
    CHECK(chinese("the device disappeared during registration; continue with `register Z:\\範例 --resume`") ==
          "裝置在註冊期間中斷連線；請以 `register Z:\\範例 --resume` 繼續");
    CHECK(chinese("registration was interrupted while scanning; continue with `register '/media/sample/範例 SAMPLE' --resume`") ==
          "註冊在掃描時中斷；請以 `register '/media/sample/範例 SAMPLE' --resume` 繼續");
    CHECK(chinese("error: an interrupted registration of this root exists (archive " + uuid +
                  "); finish it with --resume, or abandon it with `archive abandon-registration " + uuid + "`") ==
          "錯誤：此根目錄有一個中斷的註冊（封存 " + uuid + "）；請以 --resume 完成它，或以 `archive "
          "abandon-registration " + uuid + "` 放棄它");
    CHECK(chinese.format("Reading the registration of {} ...", "ARCH-0007") == "正在讀取 ARCH-0007 的註冊……");
    CHECK(chinese.format("Not abandoned: {}", chinese("archive " + uuid + " has no unfinished registration")) ==
          "未能放棄：封存 " + uuid + " 沒有未完成的註冊");
    CHECK(chinese("cannot abandon the registration: cannot run SQL: database is locked") ==
          "無法放棄註冊：無法執行 SQL 陳述式：資料庫已被鎖定");
    CHECK(chinese("the unfinished registration of archive " + uuid +
                  " was abandoned while this attempt ran; this attempt recorded nothing") ==
          "封存 " + uuid + " 未完成的註冊已在這次執行期間被放棄；這次執行未記錄任何內容");
    CHECK(chinese.format("Process: {} ({})", 4242, "veriarchive-cli.exe") == "處理程序：4242（veriarchive-cli.exe）");
    CHECK(english("archive " + uuid + " has no unfinished registration") ==
          "archive " + uuid + " has no unfinished registration");

    // A relocation resumes, and may be abandoned (decision 0026). Revision
    // numbers keep their place in the Chinese word order.
    CHECK(chinese("error: relocation was interrupted; continue it with `archive relocate " + uuid +
                  " \"Z:\\範例 SAMPLE\" --resume`") ==
          "錯誤：搬移已中斷；請以 `archive relocate " + uuid + " \"Z:\\範例 SAMPLE\" --resume` 繼續");
    CHECK(chinese("an unfinished relocation of archive " + uuid + " (to \"Z:\\範例\") exists; resume it with `archive "
                  "relocate " + uuid + " Z:\\範例 --resume`, or abandon it with `archive abandon-relocation " +
                  uuid + "`") ==
          "封存 " + uuid + " 有一個未完成的搬移（至「Z:\\範例」）；請以 `archive relocate " + uuid +
              " Z:\\範例 --resume` 繼續，或以 `archive abandon-relocation " + uuid + "` 放棄它");
    CHECK(chinese("this relocation is still running in process 4242 (veriarchive-cli.exe) on this computer; "
                  "resume it once that process has ended") ==
          "此搬移仍在這台電腦的處理程序 4242（veriarchive-cli.exe）中執行；請等該處理程序結束後再繼續");
    CHECK(chinese("archive " + uuid + " is at revision 3, not at revision 2 that its unfinished relocation "
                  "verifies; it cannot be resumed (abandon it with `archive abandon-relocation " + uuid + "`)") ==
          "封存 " + uuid + " 目前是版次 3，不是其未完成的搬移所驗證的版次 2；無法繼續（請以 `archive "
          "abandon-relocation " + uuid + "` 放棄它）");
    CHECK(chinese("revision 3 of archive " + uuid + " was recorded by another operation; the unfinished relocation "
                  "can no longer finish, and its saved progress was removed") ==
          "封存 " + uuid + " 的版次 3 已由其他作業記錄；未完成的搬移再也無法完成，其已儲存的進度已刪除");
    CHECK(chinese("revision 3 of archive " + uuid + " was recorded by another operation while this relocation ran; "
                  "nothing was changed") ==
          "封存 " + uuid + " 的版次 3 已在這次搬移期間由其他作業記錄；未做任何變更");
    CHECK(chinese("archive " + uuid + " has no unfinished relocation to resume") ==
          "封存 " + uuid + " 沒有可繼續的未完成搬移");
    CHECK(chinese.format("Not abandoned: {}", chinese("archive " + uuid + " has no unfinished relocation")) ==
          "未能放棄：封存 " + uuid + " 沒有未完成的搬移");
    CHECK(chinese("cannot abandon the relocation: cannot run SQL: database is locked") ==
          "無法放棄搬移：無法執行 SQL 陳述式：資料庫已被鎖定");
    CHECK(chinese("an unfinished registration of archive " + uuid + " (root \"Z:\\範例\") is creating revision 2; "
                  "this relocation recorded nothing (finish that registration with `register Z:\\範例 --resume`, "
                  "then relocate again; or abandon it with `archive abandon-registration " + uuid + "`, then continue "
                  "this relocation with `archive relocate " + uuid + " \"Y:\\新 SAMPLE\" --resume`)") ==
          "封存 " + uuid + " 有一個未完成的註冊（根目錄「Z:\\範例」）正在建立版次 2；這次搬移未記錄任何內容（請以 "
          "`register Z:\\範例 --resume` 完成該註冊後再重新搬移；或以 `archive abandon-registration " + uuid +
              "` 放棄該註冊後，以 `archive relocate " + uuid + " \"Y:\\新 SAMPLE\" --resume` 繼續這次搬移）");
    CHECK(chinese.format("Abandon the unfinished relocation of {}?", "ARCH-0007") == "要放棄 ARCH-0007 未完成的搬移嗎？");
    // Resume on a registration that an older version left and that may have
    // stopped asks first (decision 0022, point 7; operator, 2026-10-20): the
    // question, and what it says (src/app/unfinished_operations.h).
    CHECK(chinese.format("Resume the unfinished registration of {}?", "ARCH-0007") ==
          "要繼續 ARCH-0007 未完成的註冊嗎？");
    CHECK(chinese(va::app::older_version_resume_warning) ==
          "舊版 VeriArchive 留下了這個未完成的註冊。舊版不會記錄它最後一次運作的時間，所以程式無法判斷它已經停止，"
          "還是仍在讀取一個大檔案、或掃描資料夾或磁碟區。如果它仍在執行，之後會用自己的清單檔蓋掉這次繼續所記錄的"
          "清單檔：從此這個封存的每次驗證都會失敗，其他的註冊與搬移也都會停下來，直到這個封存有了新的版次為止。"
          "繼續之前，請先確認舊版的視窗或命令都已關閉。");
    // A relocation whose new carrier differs is never resumed (decision 0026).
    CHECK(chinese("error: the unfinished relocation of archive " + uuid + " (to \"Z:\\範例\") found that the new "
                  "carrier does not hold the recorded content and cannot be resumed; abandon it with `archive "
                  "abandon-relocation " + uuid + "`, then relocate again") ==
          "錯誤：封存 " + uuid + " 未完成的搬移（至「Z:\\範例」）發現新載體上的內容與記錄不符，無法繼續；請以 `archive "
          "abandon-relocation " + uuid + "` 放棄它，再重新搬移");
    CHECK(chinese("new carrier differs; abandon it") == "新載體上的內容不符；請放棄它");
    // `verify --resume` of an interrupted attempt of a relocation (decision 0026).
    const std::string run = "5d0f3c1e-8b7a-4c69-9e21-3a4b5c6d7e8f";
    CHECK(chinese("error: run " + run + " is an interrupted attempt of the unfinished relocation of archive " + uuid +
                  "; continue it with `archive relocate " + uuid + " Y:\\範例 --resume`") ==
          "錯誤：執行 " + run + " 是封存 " + uuid + " 未完成的搬移中斷時留下的紀錄；請以 `archive relocate " + uuid +
              " Y:\\範例 --resume` 繼續");
    CHECK(chinese("error: run " + run + " is an interrupted attempt of a relocation of archive " + uuid +
                  " that has ended; nothing is left to resume") ==
          "錯誤：執行 " + run + " 是封存 " + uuid + " 某次搬移中斷時留下的紀錄，該搬移已結束；沒有可繼續的作業");
    // A run resumed by its archive's uuid, whose archive more than one
    // attached volume holds (decision 0022, point 7): the roots, quoted, are
    // one value; the root the run read, when its record says it, another.
    const std::string clones = "\"K:\\SAMPLE\", \"X:\\SAMPLE\"";
    CHECK(chinese("error: AMBIGUOUS: archive " + uuid + " is on more than one attached volume (cloned media): " +
                  clones + "; run " + run + " read \"X:\\SAMPLE\": resume it at the root on the volume it read with "
                  "`verify --full <root_path> --resume " + run + "`, or detach the other volumes and resume it "
                  "again") ==
          "錯誤：無法確定：封存 " + uuid + " 位於多個已連接的磁碟區上（複製的媒體）：" + clones + "；執行 " + run +
              " 讀取的是「X:\\SAMPLE」：請以 `verify --full <root_path> --resume " + run +
              "` 從它讀取的那個磁碟區上的根目錄繼續，或讓其他磁碟區中斷連線後再繼續一次");
    CHECK(chinese("AMBIGUOUS: archive " + uuid + " is on more than one attached volume (cloned media): " + clones +
                  "; resume run " + run + " at the root on the volume it read with `verify --full <root_path> "
                  "--resume " + run + "`, or detach the other volumes and resume it again") ==
          "無法確定：封存 " + uuid + " 位於多個已連接的磁碟區上（複製的媒體）：" + clones + "；請以 `verify --full "
          "<root_path> --resume " + run + "` 從執行 " + run + " 讀取的那個磁碟區上的根目錄繼續，或讓其他磁碟區中斷連線後再"
          "繼續一次");
    // A verification by the archive's uuid that is not a resume stops the same
    // way, and Verify Archive says what to do in it (decision 0022, point 7).
    CHECK(chinese("error: AMBIGUOUS: archive " + uuid + " is on more than one attached volume (cloned media): " +
                  clones + "; verify it at the root on one of them with `verify --full <root_path>`, or detach the "
                  "other volumes and verify it again") ==
          "錯誤：無法確定：封存 " + uuid + " 位於多個已連接的磁碟區上（複製的媒體）：" + clones +
              "；請以 `verify --full <root_path>` 驗證其中一個磁碟區上的根目錄，或讓其他磁碟區中斷連線後再驗證一次");
    const std::string roots_found = "\n  K:\\SAMPLE\n  X:\\SAMPLE";
    CHECK(chinese.format("Not verified: archive {} is on more than one attached volume, as a device and a "
                         "sector-by-sector clone of it would be:{}\nVeriArchive cannot tell which one to verify. Type "
                         "the folder of the one to verify above, or choose it with \"Choose...\", choose \"The folder "
                         "or volume above\" under \"Verify:\" and press \"Verify (FULL)\"; or detach the other volumes "
                         "and verify the archive again.",
              "ARCH-0002" + chinese.parenthesized(uuid.substr(0, 8)), roots_found) ==
          "未能驗證：封存 ARCH-0002（" + uuid.substr(0, 8) +
              "） 位於多個已連接的磁碟區上，例如一個裝置和它逐磁區複製出來的複本：" + roots_found +
              "\n程式無法判斷要驗證哪一個。請在上方輸入要驗證的那一個的資料夾，或按「選擇…」挑選它，在「驗證：」選擇"
              "「上方的資料夾或磁碟區」，再按「完整驗證」；或讓其他磁碟區中斷連線後，再驗證一次這個封存。");
    // An import that loses its revision, or leaves it to a registration with
    // saved files (decision 0030).
    CHECK(chinese("error: revision 2 of archive " + uuid + " was recorded by another operation while this import ran; "
                  "nothing was imported (import it again to compare it with the recorded manifest)") ==
          "錯誤：封存 " + uuid + " 的版次 2 已在這次匯入期間由其他作業記錄；未匯入任何內容（再匯入一次，即可與已記錄的清單檔比較）");
    CHECK(chinese("an unfinished registration of archive " + uuid + " (root \"Z:\\範例\") is creating revision 2; "
                  "nothing was imported (finish that registration with `register Z:\\範例 --resume`, or abandon it "
                  "with `archive abandon-registration " + uuid + "` and import again)") ==
          "封存 " + uuid + " 有一個未完成的註冊（根目錄「Z:\\範例」）正在建立版次 2；未匯入任何內容（請以 "
          "`register Z:\\範例 --resume` 完成該註冊，或以 `archive abandon-registration " + uuid +
              "` 放棄它後再匯入一次）");
    CHECK(chinese(std::string(va::app::without_failure_class(
              "ManifestHashMismatch: the manifest changed while it was imported; nothing was imported"))) ==
          "清單檔在匯入期間被變更；未匯入任何內容");
    // An import that puts back the manifest recorded, or finds its record
    // changed meanwhile (decision 0028).
    CHECK(chinese.format("Restored the manifest of {} ({}) revision {}: the file is the one the repository recorded.",
              "ARCH-0007", uuid, 2) ==
          "已還原 ARCH-0007（" + uuid + "）版次 2 的清單檔：此檔案就是儲存庫所記錄的那一份。");
    CHECK(chinese("error: the repository's record of revision 2 of archive " + uuid +
                  " changed while this import ran; nothing was imported (import it again)") ==
          "錯誤：儲存庫中封存 " + uuid + " 版次 2 的紀錄已在這次匯入期間變更；未匯入任何內容（請再匯入一次）");
    // A registration or relocation made for an older revision never records
    // (decision 0030); the revisions and the root keep their places.
    CHECK(chinese("error: the unfinished registration of archive " + uuid + " (root \"Z:\\範例\") would create "
                  "revision 2, but the archive now has a later revision (3); the registration can no longer finish: "
                  "abandon it with `archive abandon-registration " + uuid + "`") ==
          "錯誤：封存 " + uuid + " 未完成的註冊（根目錄「Z:\\範例」）原本要建立版次 2，但封存現在已有較新的版次（3）；"
          "這個註冊再也無法完成：請以 `archive abandon-registration " + uuid + "` 放棄它");
    CHECK(chinese("the unfinished relocation of archive " + uuid + " (to \"Y:\\範例\") verifies revision 1, but the "
                  "archive now has a later revision (3); the relocation can no longer finish: abandon it with "
                  "`archive abandon-relocation " + uuid + "`, then relocate again") ==
          "封存 " + uuid + " 未完成的搬移（至「Y:\\範例」）驗證的是版次 1，但封存現在已有較新的版次（3）；"
          "這次搬移再也無法完成：請以 `archive abandon-relocation " + uuid + "` 放棄它，再重新搬移");
    CHECK(chinese("error: run " + run + " is an interrupted attempt of the unfinished relocation of archive " + uuid +
                  ", which verifies revision 1, but the archive now has a later revision (3); the relocation can no "
                  "longer finish: abandon it with `archive abandon-relocation " + uuid + "`, then relocate again") ==
          "錯誤：執行 " + run + " 是封存 " + uuid + " 未完成的搬移中斷時留下的紀錄，該搬移驗證的是版次 1，但封存現在已有"
          "較新的版次（3）；該搬移再也無法完成：請以 `archive abandon-relocation " + uuid + "` 放棄它，再重新搬移");
    // A registration or relocation that another operation overtakes before it
    // begins (decision 0030): the revision and the archive change places.
    CHECK(chinese("error: revision 3 of archive " + uuid + " was recorded by another operation before this "
                  "registration could begin; nothing was changed (run `register --new-revision` again)") ==
          "錯誤：封存 " + uuid + " 的版次 3 已在這次註冊開始前由其他作業記錄；未做任何變更（請再執行一次 "
          "`register --new-revision`）");
    CHECK(chinese("revision 2 of archive " + uuid + " was recorded by another operation before this relocation could "
                  "begin; nothing was changed (run `archive relocate` again)") ==
          "封存 " + uuid + " 的版次 2 已在這次搬移開始前由其他作業記錄；未做任何變更（請再執行一次 `archive relocate`）");
    // A cause that a hint follows is translated too, and the uuid after it is
    // carried over.
    CHECK(chinese.format("RELOCATION REFUSED: {}",
              chinese("the new carrier does not hold the recorded content; nothing was changed, but the "
                      "relocation's saved progress could not be removed: cannot execute SQL: database is locked; "
                      "abandon the relocation with `archive abandon-relocation " + uuid +
                      "` before relocating again")) ==
          "拒絕搬移：新載體上的內容與記錄不符；未做任何變更，但無法刪除搬移已儲存的進度：無法執行 SQL：資料庫已被鎖定；"
          "請先以 `archive abandon-relocation " + uuid + "` 放棄這次搬移，再重新搬移");
    // A failure that stops a relocation says how to go on (decision 0026).
    const auto kept = "; the relocation keeps its saved progress: continue it with `archive relocate " + uuid +
                      " \"Y:\\範例 SAMPLE\" --resume`";
    const auto kept_chinese =
        "；搬移保留其已儲存的進度：請以 `archive relocate " + uuid + " \"Y:\\範例 SAMPLE\" --resume` 繼續";
    CHECK(chinese("error: cannot record the relocation: access denied (MoveFileExW, Windows error 5)" + kept) ==
          "錯誤：無法記錄搬移：存取被拒（MoveFileExW，Windows 錯誤 5）" + kept_chinese);
    CHECK(chinese("cannot record the relocation: cannot execute SQL: database is locked" + kept) ==
          "無法記錄搬移：無法執行 SQL：資料庫已被鎖定" + kept_chinese);
    CHECK(chinese("cannot scan the new root: access denied (FindFirstFileExW, Windows error 5)" + kept) ==
          "無法掃描新根目錄：存取被拒（FindFirstFileExW，Windows 錯誤 5）" + kept_chinese);
    CHECK(chinese("cannot write the manifest: open failed: Permission denied" + kept) ==
          "無法寫入清單檔：open 失敗：權限不足" + kept_chinese);
    CHECK(chinese("cannot write the manifest: open failed: Permission denied") == "無法寫入清單檔：open 失敗：權限不足");
    CHECK(english("cannot record the relocation: access denied (MoveFileExW, Windows error 5)" + kept) ==
          "cannot record the relocation: access denied (MoveFileExW, Windows error 5)" + kept);
    CHECK(english("the new carrier does not hold the recorded content; nothing was changed, but the relocation's "
                  "saved progress could not be removed: access denied (CreateFileW, Windows error 5); abandon the relocation "
                  "with `archive abandon-relocation " + uuid + "` before relocating again") ==
          "the new carrier does not hold the recorded content; nothing was changed, but the relocation's saved "
          "progress could not be removed: access denied (CreateFileW, Windows error 5); abandon the relocation with `archive "
          "abandon-relocation " + uuid + "` before relocating again");

    // A root that matches several archives (§B10): select one by its uuid,
    // and where their volumes are attached together, detach all but the one to
    // verify, as the verification by uuid then finds it on each (decision
    // 0022, point 7).
    CHECK(chinese("error: AMBIGUOUS: the media matches several archives (cloned media); select one with `verify "
                  "--full <archive_uuid>`; where the volumes of those archives are attached together, detach all but "
                  "the volume of the one to verify, then verify it") ==
          "錯誤：無法確定：此媒體符合多個封存（複製的媒體）；請以 `verify --full <archive_uuid>` 選擇其中一個；若這些封存的"
          "磁碟區同時連接著，請讓要驗證的那個封存以外的磁碟區都中斷連線，再驗證它");

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
    // Failures: --json puts the failure class first; people read the message
    // without it, and the class in plain words (decision 0017).
    CHECK(chinese("error: cannot read the manifest of ARCH-0002 revision 2: manifest body does not match its header") ==
          "錯誤：無法讀取 ARCH-0002 版次 2 的清單檔：清單檔主體與其標頭不符");
    CHECK(chinese("error: archive " + uuid + " has no revision yet") == "錯誤：封存 " + uuid + " 尚無版次");
    CHECK(chinese.format("Failure: {}", chinese(va::app::failure_class_words("ManifestHashMismatch"))) ==
          "失敗：清單檔不是記錄中的那一個");
    CHECK(chinese("error: the comparison was cancelled") == "錯誤：比較已取消");
    CHECK(chinese.format("Not compared: {}",
              chinese("cannot read the manifest of ARCH-0002 revision 2: revision 2 is not recorded")) ==
          "未能比較：無法讀取 ARCH-0002 版次 2 的清單檔：版次 2 沒有紀錄");
    CHECK(english("cannot read the manifest of ARCH-0002 revision 2: revision 2 is not recorded") ==
          "cannot read the manifest of ARCH-0002 revision 2: revision 2 is not recorded");
    // A manifest that cannot be used (decision 0028): the manifest by its
    // place in the repository, what is wrong, then how to fix it. The index's
    // copy of a header is never taken for the manifest's (R-14); a manifest
    // missing, unreadable or changed is put back with `import` of its file,
    // and where nothing holds it, the archive's next revision is registered
    // over its lost manifest, or, where the device is gone as well, the
    // archive is retired. The cause is translated, also inside a comparison's
    // failure and a verification's warning.
    const auto manifest = "manifests/" + uuid + "/r0002.vam";
    const auto index_differs = "the repository's index differs from the manifest of revision 2 (" + manifest +
                               "); rebuild the index with `repo rebuild-index`";
    const auto index_differs_chinese =
        "儲存庫的索引與版次 2 的清單檔（" + manifest + "）不符；請以 `repo rebuild-index` 重建索引";
    CHECK(chinese("error: " + index_differs) == "錯誤：" + index_differs_chinese);
    CHECK(chinese("cannot read the manifest of ARCH-0002 revision 2: " + index_differs) ==
          "無法讀取 ARCH-0002 版次 2 的清單檔：" + index_differs_chinese);
    const auto retire = "; if the device that held the archive is gone as well, retire the archive with `archive "
                        "retire " + uuid + " --confirm`";
    const auto retire_chinese = "；若存放此封存的裝置也已不在，請以 `archive retire " + uuid + " --confirm` 停用此封存";
    const auto restore = "; put it back with `import <folder>/r0002.vam` from a backup of the repository, an export "
                         "of revision 2, or the media copy in the archive's .veriarchive folder, where registration "
                         "wrote one; if none of these holds it, register revision 3 from what the archive's root holds "
                         "now with `register <root_path> --lost-manifest " +
                         uuid + "`" + retire;
    const auto restore_chinese = "；請以 `import <folder>/r0002.vam` 從儲存庫的備份、版次 2 的匯出檔，或封存 "
                                 ".veriarchive 資料夾中註冊時寫入的媒體副本放回該清單檔；若這些都沒有，請以 "
                                 "`register <root_path> --lost-manifest " +
                                 uuid + "` 用封存根目錄目前的內容註冊版次 3" + retire_chinese;
    CHECK(chinese("the manifest of revision 2 (" + manifest + ") is missing" + restore) ==
          "版次 2 的清單檔（" + manifest + "）已遺失" + restore_chinese);
    CHECK(chinese("error: the manifest of revision 2 (" + manifest + ") cannot be read: access denied (CreateFileW, "
                  "Windows error 5)" + restore) ==
          "錯誤：無法讀取版次 2 的清單檔（" + manifest + "）：存取被拒（CreateFileW，Windows 錯誤 5）" + restore_chinese);
    CHECK(chinese("manifest: the manifest of revision 2 (" + manifest +
                  ") has changed: its first line differs from what the repository recorded" + restore) ==
          "清單檔：版次 2 的清單檔（" + manifest + "）已被變更：其第一行與儲存庫所記錄的不同" + restore_chinese);
    CHECK(chinese("warning: manifest: the manifest of revision 2 (" + manifest +
                  ") has changed: its body does not match its header" + restore) ==
          "警告：清單檔：版次 2 的清單檔（" + manifest + "）已被變更：其主體與標頭不符" + restore_chinese);
    CHECK(chinese("cannot read the manifest of ARCH-0002 revision 2: the manifest of revision "
                  "2 (" + manifest + ") has changed: it gave other bytes when it was read again" + restore) ==
          "無法讀取 ARCH-0002 版次 2 的清單檔：版次 2 的清單檔（" + manifest +
              "）已被變更：再次讀取時得到不同的內容" + restore_chinese);
    CHECK(chinese("manifest: the manifest of revision 2 (" + manifest +
                  ") has changed: its content hashes do not match its merkle_root" + restore) ==
          "清單檔：版次 2 的清單檔（" + manifest + "）已被變更：其內容雜湊與其 Merkle 根雜湊不符" + restore_chinese);
    // A verification by root stops naming the archive that may be at the root
    // (decision 0028): what is wrong with its manifest, then to verify it by
    // its uuid to record the failure, then to fix the manifest as the plain
    // message says: put it back with `import`, or the last resort, or retire
    // the archive where its device is gone as well.
    const auto may_be = "this root may belong to archive ARCH-0002 (" + uuid + "), whose manifest cannot be used: ";
    const auto may_be_chinese = "此根目錄可能屬於封存 ARCH-0002（" + uuid + "），而它的清單檔無法使用：";
    const auto record_it = "; verify that archive by its uuid (`verify --full " + uuid + "`) to record the failure, then ";
    const auto record_it_chinese = "；請先以 uuid 驗證該封存（`verify --full " + uuid + "`）以記錄這次失敗，再";
    const std::string restore_it = "put its manifest back with `import <folder>/r0002.vam` from a backup of the "
                                   "repository, an export of revision 2, or the media copy in the archive's "
                                   ".veriarchive folder, where registration wrote one; if none of these holds it, "
                                   "register revision 3 from what the archive's root holds now with `register "
                                   "<root_path> --lost-manifest " + uuid + "`" + retire;
    const std::string restore_it_chinese = "以 `import <folder>/r0002.vam` 從儲存庫的備份、版次 2 的匯出檔，或封存 "
                                           ".veriarchive 資料夾中註冊時寫入的媒體副本放回它的清單檔；若這些都沒有，請以 "
                                           "`register <root_path> --lost-manifest " + uuid +
                                           "` 用封存根目錄目前的內容註冊版次 3" + retire_chinese;
    CHECK(chinese("error: " + may_be + "the manifest of revision 2 (" + manifest + ") is missing" + record_it +
                  restore_it) ==
          "錯誤：" + may_be_chinese + "版次 2 的清單檔（" + manifest + "）已遺失" + record_it_chinese + restore_it_chinese);
    CHECK(chinese(may_be + "the manifest of revision 2 (" + manifest + ") cannot be read: access denied (CreateFileW, "
                  "Windows error 5)" + record_it + restore_it) ==
          may_be_chinese + "無法讀取版次 2 的清單檔（" + manifest + "）：存取被拒（CreateFileW，Windows 錯誤 5）" +
              record_it_chinese + restore_it_chinese);
    CHECK(chinese(may_be + "the manifest of revision 2 (" + manifest +
                  ") has changed: its first line differs from what the repository recorded" + record_it + restore_it) ==
          may_be_chinese + "版次 2 的清單檔（" + manifest + "）已被變更：其第一行與儲存庫所記錄的不同" + record_it_chinese +
              restore_it_chinese);
    CHECK(chinese(may_be + "the repository's index differs from the manifest of revision 2 (" + manifest + ")" +
                  record_it + "rebuild the index with `repo rebuild-index`") ==
          may_be_chinese + "儲存庫的索引與版次 2 的清單檔（" + manifest + "）不符" + record_it_chinese +
              "以 `repo rebuild-index` 重建索引");
    // The manifest of the archive's revision of which the index has no
    // record (decision 0028): what the file at its place is, then the fix,
    // the index rebuilt where the file reads as the manifest, otherwise the
    // manifest put back, which records it, or the last resort.
    const auto no_record = "the repository's index has no record of the manifest of revision 2 (" + manifest + ")";
    const auto no_record_chinese = "儲存庫的索引沒有版次 2 的清單檔（" + manifest + "）的紀錄";
    CHECK(chinese(no_record + "; rebuild the index with `repo rebuild-index`") ==
          no_record_chinese + "；請以 `repo rebuild-index` 重建索引");
    CHECK(chinese("problem: " + no_record + ", which is missing" + restore) ==
          "問題：" + no_record_chinese + "，而該清單檔已遺失" + restore_chinese);
    CHECK(chinese("manifest: " + no_record + ", which cannot be read: header does not match its file name" + restore) ==
          "清單檔：" + no_record_chinese + "，而且無法讀取該清單檔：標頭與其檔名不符" + restore_chinese);
    CHECK(chinese(no_record + ", which cannot be read: manifest body does not match its header" +
                  restore) ==
          no_record_chinese + "，而且無法讀取該清單檔：清單檔主體與其標頭不符" + restore_chinese);
    CHECK(chinese("error: " + may_be + no_record + record_it + "rebuild the index with `repo rebuild-index`") ==
          "錯誤：" + may_be_chinese + no_record_chinese + record_it_chinese + "以 `repo rebuild-index` 重建索引");
    // Put back, the manifest is recorded by the import: no rebuild follows.
    CHECK(chinese(may_be + no_record + ", which is missing" + record_it + restore_it) ==
          may_be_chinese + no_record_chinese + "，而該清單檔已遺失" + record_it_chinese + restore_it_chinese);
    CHECK(chinese(may_be + no_record + ", which cannot be read: access denied (CreateFileW, Windows error 5)" + record_it +
                  restore_it) ==
          may_be_chinese + no_record_chinese + "，而且無法讀取該清單檔：存取被拒（CreateFileW，Windows 錯誤 5）" +
              record_it_chinese + restore_it_chinese);
    // Each revision whose manifest `repo rebuild-index` could not read, by
    // what became of its record (decision 0028), the archive's words carried
    // over and what is wrong translated.
    const auto r1 = "manifests/" + uuid + "/r0001.vam";
    CHECK(chinese.format("kept as recorded: {} ({}): {}", "ARCH-0007", uuid,
              chinese("the manifest of revision 2 (" + manifest + ") is missing")) ==
          "保留原紀錄：ARCH-0007（" + uuid + "）：版次 2 的清單檔（" + manifest + "）已遺失");
    CHECK(chinese("kept as recorded: ARCH-0007 (" + uuid + "): the manifest of revision 2 (" + manifest +
                  ") cannot be read: manifest body does not match its header") ==
          "保留原紀錄：ARCH-0007（" + uuid + "）：無法讀取版次 2 的清單檔（" + manifest +
              "）：清單檔主體與其標頭不符");
    CHECK(chinese("left out of the index: ARCH-0007 (" + uuid + "): the manifest of revision 1 (" + r1 +
                  ") is missing") ==
          "已從索引移除：ARCH-0007（" + uuid + "）：版次 1 的清單檔（" + r1 + "）已遺失");
    CHECK(chinese("not in the index: ARCH-0007 (" + uuid + "): the manifest of revision 2 (" + manifest +
                  ") is missing") ==
          "不在索引中：ARCH-0007（" + uuid + "）：版次 2 的清單檔（" + manifest + "）已遺失");
    // The manifest of an earlier revision, put back as the latest's is, with
    // no last resort: the archive has a later revision already; where nothing
    // holds it, `repo rebuild-index` leaves that revision out (decision 0028).
    const auto earlier_put_back = "; put it back with `import <folder>/r0001.vam` from a backup of the repository, an "
                                  "export of revision 1, or the media copy in the archive's .veriarchive folder, where "
                                  "registration wrote one; if none of these holds it, leave revision 1 out of the index "
                                  "with `repo rebuild-index`";
    const auto earlier_put_back_chinese = "；請以 `import <folder>/r0001.vam` 從儲存庫的備份、版次 1 的匯出檔，或封存 "
                                          ".veriarchive 資料夾中註冊時寫入的媒體副本放回該清單檔；若這些都沒有，請以 "
                                          "`repo rebuild-index` 將版次 1 從索引移除";
    CHECK(chinese("problem: the manifest of revision 1 (" + r1 + ") is missing" + earlier_put_back) ==
          "問題：版次 1 的清單檔（" + r1 + "）已遺失" + earlier_put_back_chinese);
    CHECK(chinese("problem: the manifest of revision 1 (" + r1 + ") cannot be read: access denied (CreateFileW, Windows error 5)" +
                  earlier_put_back) ==
          "問題：無法讀取版次 1 的清單檔（" + r1 + "）：存取被拒（CreateFileW，Windows 錯誤 5）" + earlier_put_back_chinese);
    CHECK(chinese("problem: the manifest of revision 1 (" + r1 +
                  ") has changed: its first line differs from what the repository recorded" + earlier_put_back) ==
          "問題：版次 1 的清單檔（" + r1 + "）已被變更：其第一行與儲存庫所記錄的不同" + earlier_put_back_chinese);
    // A registration over a lost manifest (decision 0028, point 8): what it
    // would register before it is confirmed, its refusals, and its outcome.
    CHECK(chinese.format("Lost manifest of {} ({}), revision {}", "ARCH-0002", uuid, 2) ==
          "無法取回的清單檔：ARCH-0002（" + uuid + "），版次 2");
    CHECK(chinese.format("Registering revision {} from this root takes what it holds now as the archive's new "
                         "baseline, without comparing it with revision {}, whose manifest is lost: damage since "
                         "revision {} was registered would not be found. Do this only where no backup of the "
                         "repository, export of revision {} or media copy holds that manifest. The archive keeps its "
                         "verification history and its hash chain, and never goes back to an earlier revision.",
              3, 2, 2, 2) ==
          "從此根目錄註冊版次 3，會把它目前的內容當作封存新的基準，而不與清單檔已無法取回的版次 2 比對：自版次 2 註冊以來"
          "發生的損壞將不會被發現。只有在儲存庫的備份、版次 2 的匯出檔和媒體副本中都沒有該清單檔時，才這麼做。封存會保留"
          "它的驗證歷程及其雜湊鏈，也絕不會回到較早的版次。");
    CHECK(chinese("error: run the command again with --confirm to register revision 3") ==
          "錯誤：若要註冊版次 3，請加上 --confirm 再執行一次");
    CHECK(chinese("error: the manifest of revision 2 of archive " + uuid +
                  " can be used: register its next revision with `register <root_path> --new-revision`") ==
          "錯誤：封存 " + uuid + " 版次 2 的清單檔可以使用：請以 `register <root_path> --new-revision` 註冊它的下一個版次");
    CHECK(chinese("error: the manifest of revision 2 of archive " + uuid + " is not lost: " + index_differs) ==
          "錯誤：封存 " + uuid + " 版次 2 的清單檔並非無法取回：" + index_differs_chinese);
    CHECK(chinese("error: the identity file of this root (.veriarchive/identity.json) names archive " + uuid_b +
                  ", not archive " + uuid) ==
          "錯誤：此根目錄的身分檔案（.veriarchive/identity.json）指向封存 " + uuid_b + "，而不是封存 " + uuid);
    CHECK(chinese("error: the media copy at this root holds the manifest of revision 2 of archive " + uuid +
                  ": put it back with `import K:\\SAMPLE\\.veriarchive\\r0002.vam`") ==
          "錯誤：此根目錄的媒體副本中有封存 " + uuid +
              " 版次 2 的清單檔：請以 `import K:\\SAMPLE\\.veriarchive\\r0002.vam` 放回該清單檔");
    CHECK(chinese("error: the root overlaps the root of archive " + uuid +
                  ", whose latest manifest records this volume's filesystem id and device") ==
          "錯誤：根目錄與封存 " + uuid + " 的根目錄重疊，而該封存最新的清單檔所記錄的正是此磁碟區的檔案系統 UUID 與裝置");
    // An archive whose root the root given overlaps on a volume that may be a
    // clone of this one, named before the operator confirms; the match code
    // translated before it is put in.
    for (const auto& [code, code_chinese] : {std::pair{"MIGRATED", "已遷移"}, std::pair{"MATCH_WEAK", "弱相符"}}) {
        CHECK(chinese.format("possibly a clone: this root overlaps the root of archive {} ({}) on a volume that this "
                             "one matches only as {}: this volume may be a sector-by-sector clone of that archive's, "
                             "or that archive's own; confirm only where this root is not that archive's",
                  "ARCH-0002", uuid_b, chinese(code)) ==
              "可能是複本：此根目錄與封存 ARCH-0002（" + uuid_b + "）的根目錄重疊，而此磁碟區與該封存的磁碟區比對只得到「" +
                  code_chinese + "」：此磁碟區可能是該封存的磁碟區逐磁區複製出來的複本，也可能就是該封存自己的磁碟區；"
                  "請只在此根目錄不是該封存的根目錄時才確認");
    }
    // An archive whose very root the root given is, of which the archive named
    // may be the twin, named before the operator confirms.
    CHECK(chinese.format("the same root as archive {} ({}): this root is that archive's, on the volume and the device "
                         "that its latest manifest records; confirm only where this very folder was registered on "
                         "purpose as both that archive and this one (--force-new-archive)",
              "ARCH-0002", uuid_b) ==
          "與封存 ARCH-0002（" + uuid_b + "）的根目錄相同：此根目錄是該封存的根目錄，位於它最新的清單檔所記錄的磁碟區與"
                                        "裝置上；請只在這個資料夾當初刻意同時註冊為該封存與此封存（--force-new-archive）"
                                        "時才確認");
    // The same on a volume that this one matches only as MIGRATED or
    // MATCH_WEAK, the match code translated before it is put in.
    for (const auto& [code, code_chinese] : {std::pair{"MIGRATED", "已遷移"}, std::pair{"MATCH_WEAK", "弱相符"}}) {
        CHECK(chinese.format("the same root as archive {} ({}) on a volume that this one matches only as {}: this "
                             "volume may be a sector-by-sector clone of that archive's, or that archive's own; confirm "
                             "only where this very folder was registered on purpose as both that archive and this one "
                             "(--force-new-archive), or where this volume is not that archive's",
                  "ARCH-0002", uuid_b, chinese(code)) ==
              "與封存 ARCH-0002（" + uuid_b + "）的根目錄相同，而此磁碟區與該封存的磁碟區比對只得到「" + code_chinese +
                  "」：此磁碟區可能是該封存的磁碟區逐磁區複製出來的複本，也可能就是該封存自己的磁碟區；請只在這個資料夾當初"
                  "刻意同時註冊為該封存與此封存（--force-new-archive），或此磁碟區不是該封存的磁碟區時才確認");
    }
    CHECK(chinese("error: --lost-manifest takes none of --label, --new-revision, --force-new-archive and --resume") ==
          "錯誤：--lost-manifest 不能與 --label、--new-revision、--force-new-archive 或 --resume 一起使用");
    CHECK(chinese("error: --confirm is used only with --lost-manifest or --resume") ==
          "錯誤：--confirm 只能與 --lost-manifest 或 --resume 一起使用");
    // A resume of a registration that an older version left, not confirmed
    // (decision 0022, point 7).
    CHECK(chinese("error: run the command again with --confirm to resume it") ==
          "錯誤：若要繼續，請加上 --confirm 再執行一次");
    CHECK(chinese.format("revision {}, whose manifest was lost, was left out of the index; its verifications stay in "
                         "the history",
              2) == "清單檔已無法取回的版次 2 已從索引移除；它的驗證紀錄仍保留在歷程中");
    CHECK(chinese.format("Revision {}, whose manifest was lost, was left out of the index; its verifications stay in "
                         "the history.",
              2) == "清單檔已無法取回的版次 2 已從索引移除；它的驗證紀錄仍保留在歷程中。");
    // Retiring an archive whose latest manifest is lost (decision 0028, point
    // 9): what it means before it is confirmed, its outcome and its refusals,
    // and the archive listed as retired with its manifest lost, whose message
    // ends with the import, as a retired archive takes no new revision.
    CHECK(chinese.format("Retiring the archive declares that manifest lost for good: do this only where no backup of "
                         "the repository, export of revision {} or media copy holds it, and the device that held the "
                         "archive is gone as well (broken, lost or wiped); otherwise put the manifest back with "
                         "`import`, or register the archive's next revision from its folder or volume with `register "
                         "<root_path> --lost-manifest {}`. The archive keeps its verification history and its hash "
                         "chain, but can no longer be verified or take a new revision. It no longer stops the "
                         "registration of a new folder or volume or a relocation: retired, it is no longer taken as "
                         "a possible owner of one, as identity matching cannot use its latest manifest.",
              2, uuid) ==
          "停用此封存，就是宣告該清單檔再也無法取回：只有在儲存庫的備份、版次 2 的匯出檔和媒體副本中都沒有該清單檔，而且存放"
          "此封存的裝置也已不在（損壞、遺失或已清除）時，才這麼做；否則請以 `import` 放回該清單檔，或以 `register "
          "<root_path> --lost-manifest " + uuid +
              "` 用封存的資料夾或磁碟區目前的內容註冊它的下一個版次。封存會保留它的驗證歷程及其雜湊鏈，但再也無法驗證，"
              "也不再接受新的版次。它不再讓註冊新的資料夾或磁碟區、或搬移停下來：停用之後，因為比對媒體身分時無法使用它最新"
              "的清單檔，它不再被視為可能擁有它們的封存。");
    CHECK(chinese("error: run the command again with --confirm to retire the archive") ==
          "錯誤：若要停用此封存，請加上 --confirm 再執行一次");
    CHECK(chinese.format("Retired {} ({}), whose manifest of revision {} is lost. It keeps its verification history and "
                         "its hash chain; it can no longer be verified or take a new revision, and no longer stops the "
                         "registration of a new folder or volume or a relocation.",
              "ARCH-0002", uuid, 2) ==
          "已停用 ARCH-0002（" + uuid + "），它版次 2 的清單檔已無法取回。此封存會保留它的驗證歷程及其雜湊鏈；它再也無法"
                                        "驗證，也不再接受新的版次，並且不再讓註冊新的資料夾或磁碟區、或搬移停下來。");
    CHECK(chinese("error: archive " + uuid + " now has revision 3, not revision 2; nothing was changed (run the command "
                  "again)") ==
          "錯誤：封存 " + uuid + " 現在的版次是 3，而不是版次 2；未做任何變更（請再執行一次此命令）");
    CHECK(chinese("error: archive " + uuid + " is retired and takes no new revisions: abandon its "
                  "unfinished registration with `archive abandon-registration " + uuid + "`") ==
          "錯誤：封存 " + uuid + " 已停用，不再接受新的版次：請以 `archive abandon-registration " + uuid +
              "` 放棄它未完成的註冊");
    CHECK(chinese("error: a retired archive takes no new revisions") ==
          "錯誤：已停用的封存不再接受新的版次");
    const std::string retired_put_back = "; put it back with `import <folder>/r0002.vam` from a backup of the "
                                         "repository, an export of revision 2, or the media copy in the archive's "
                                         ".veriarchive folder, where registration wrote one";
    const std::string retired_put_back_chinese = "；請以 `import <folder>/r0002.vam` 從儲存庫的備份、版次 2 的匯出檔，"
                                                 "或封存 .veriarchive 資料夾中註冊時寫入的媒體副本放回該清單檔";
    CHECK(chinese("manifest lost: the manifest of revision 2 (" + manifest + ") is missing" + retired_put_back) ==
          "清單檔無法取回：版次 2 的清單檔（" + manifest + "）已遺失" + retired_put_back_chinese);
    CHECK(chinese("manifest lost: the manifest of revision 2 (" + manifest + ") cannot be read: access denied (CreateFileW, "
                  "Windows error 5)" + retired_put_back) ==
          "清單檔無法取回：無法讀取版次 2 的清單檔（" + manifest + "）：存取被拒（CreateFileW，Windows 錯誤 5）" +
              retired_put_back_chinese);
    CHECK(chinese("warning: manifest: the manifest of revision 2 (" + manifest +
                  ") has changed: its first line differs from what the repository recorded" + retired_put_back) ==
          "警告：清單檔：版次 2 的清單檔（" + manifest + "）已被變更：其第一行與儲存庫所記錄的不同" +
              retired_put_back_chinese);
    CHECK(chinese("manifest lost: " + no_record + ", which is missing" + retired_put_back) ==
          "清單檔無法取回：" + no_record_chinese + "，而該清單檔已遺失" + retired_put_back_chinese);
    CHECK(chinese("manifest lost: " + no_record + ", which cannot be read: header does not match its file name" +
                  retired_put_back) ==
          "清單檔無法取回：" + no_record_chinese + "，而且無法讀取該清單檔：標頭與其檔名不符" + retired_put_back_chinese);
    const auto lost = "the manifest of revision 2 (" + manifest + ") is missing" + retired_put_back;
    CHECK(chinese.format("manifest lost: {}", chinese(lost)) ==
          "清單檔無法取回：版次 2 的清單檔（" + manifest + "）已遺失" + retired_put_back_chinese);
    CHECK(chinese.format("Manifest lost: {}", chinese(lost)) ==
          "清單檔無法取回：版次 2 的清單檔（" + manifest + "）已遺失" + retired_put_back_chinese);
    CHECK(english.format("Manifest lost: {}", english(lost)) == "Manifest lost: " + lost);
    // An archive listed with the problem of its manifest (decision 0028): the
    // CLI's line and the GUI's row, the message worded as everywhere else.
    const auto missing = "the manifest of revision 2 (" + manifest + ") is missing" + restore;
    const auto missing_chinese = "版次 2 的清單檔（" + manifest + "）已遺失" + restore_chinese;
    CHECK(chinese.format("problem: {}", chinese(missing)) == "問題：" + missing_chinese);
    CHECK(chinese("problem: " + missing) == "問題：" + missing_chinese);
    CHECK(chinese.format("Problem: {}", chinese(missing)) == "問題：" + missing_chinese);
    CHECK(english.format("Problem: {}", english(missing)) == "Problem: " + missing);
    // A verification that failed on its manifest read no media (decision 0028).
    CHECK(chinese("media: not read, as the manifest of this revision cannot be used") ==
          "媒體：未讀取，因為這個版次的清單檔無法使用");
    CHECK(chinese("Media: not read, as the manifest of this revision cannot be used") ==
          "媒體：未讀取，因為這個版次的清單檔無法使用");
    // Wording fixes (findings 27, 29, 30, 31, 32).
    CHECK(chinese("warning: identity: the archive is retired") == "警告：媒體身分：封存已停用");
    CHECK(chinese("RETIRED") == "已停用");
    CHECK(chinese(va::app::item_detail_words("l -> f")) == "連結 → 檔案");
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
    // Register and Verify name their target a folder or volume: a volume root
    // registers the whole volume.
    CHECK(chinese("Choose the folder or volume to register.") == "請選擇要註冊的資料夾或磁碟區。");
    CHECK(chinese("Register as a separate archive even though this folder or volume is registered") ==
          "即使此資料夾或磁碟區已註冊，仍註冊為另一個封存");
    CHECK(chinese("The folder or volume above") == "上方的資料夾或磁碟區");
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

// Decision 0010: a verification whose content matched and that has
// Unbaselined entries says how to take a baseline for them: a new revision,
// named on the command line with the root the run read, in the window as the
// Register Archive view's box, in either language. Nothing for a run that
// FAILED, whose entries that differ a new revision would take for good ones,
// nor for one without such entries, nor for a retired archive, which takes
// no new revision. (A sample run.)
void unbaselined_advice_tests()
{
    using va::app::ReportedIn;
    va::VerifyOutcome outcome;
    outcome.result = "PASSED_WITH_WARNINGS";
    outcome.root = va::path_from_utf8("/vol/SAMPLE-範例");
    outcome.counts = {{"Unbaselined", 2}, {"Verified", 5}};
    const Text english;
    const Text chinese(Language::TraditionalChinese);
    CHECK(va::app::unbaselined_advice(english, outcome, ReportedIn::CommandLine) ==
          "The Unbaselined entries could not be read when the archive was registered, so they have no baseline yet. "
          "To take one for them, register a new revision of the archive: veriarchive-cli register "
          "/vol/SAMPLE-範例 --new-revision");
    CHECK(va::app::unbaselined_advice(chinese, outcome, ReportedIn::CommandLine) ==
          "這些「無基準」項目在註冊此封存時無法讀取，所以還沒有基準。若要為它們建立基準，請註冊此封存的新版次："
          "veriarchive-cli register /vol/SAMPLE-範例 --new-revision");
    // A root with a space is quoted as every command a message names writes
    // a root (shell_argument): in double quotes on Windows, in single quotes
    // on Linux.
    auto spaced = outcome;
    spaced.root = va::path_from_utf8("/vol/SAMPLE 範例");
    CHECK(va::app::unbaselined_advice(english, spaced, ReportedIn::CommandLine).value_or("").ends_with(
        std::filesystem::path::preferred_separator == '\\'
            ? "veriarchive-cli register \"/vol/SAMPLE 範例\" --new-revision"
            : "veriarchive-cli register '/vol/SAMPLE 範例' --new-revision"));
    CHECK(va::app::unbaselined_advice(english, outcome, ReportedIn::Window) ==
          "The Unbaselined entries could not be read when the archive was registered, so they have no baseline yet. "
          "To take one for them, register a new revision of the archive: in Register Archive, choose its folder or "
          "volume (/vol/SAMPLE-範例), tick \"New revision of the archive registered at this folder or volume\" and "
          "press \"Register\".");
    CHECK(va::app::unbaselined_advice(chinese, outcome, ReportedIn::Window) ==
          "這些「無基準」項目在註冊此封存時無法讀取，所以還沒有基準。若要為它們建立基準，請註冊此封存的新版次：在"
          "「註冊封存」頁選擇它的資料夾或磁碟區（/vol/SAMPLE-範例），勾選「為此資料夾或磁碟區已註冊的封存建立新版次」，"
          "再按「註冊」。");

    auto failed = outcome;
    failed.result = "FAILED";
    failed.counts["Modified"] = 1;
    CHECK(!va::app::unbaselined_advice(english, failed, ReportedIn::CommandLine));
    auto without = outcome;
    without.counts.erase("Unbaselined");
    CHECK(!va::app::unbaselined_advice(english, without, ReportedIn::Window));

    auto retired = outcome;
    retired.identity.details = {"retired"};
    CHECK(!va::app::unbaselined_advice(english, retired, ReportedIn::CommandLine));
    CHECK(!va::app::unbaselined_advice(chinese, retired, ReportedIn::Window));
}

} // namespace

void text_tests()
{
    catalog_tests();
    language_tests();
    translation_tests();
    format_fallback_tests();
    unbaselined_advice_tests();
}
