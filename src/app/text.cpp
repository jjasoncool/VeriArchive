#include "app/text.h"

#include <algorithm>
#include <optional>
#include <span>
#include <unordered_map>
#include <vector>

namespace va::app {

namespace {

std::string lowercase(std::string_view text)
{
    std::string result(text);
    for (auto& character : result) {
        if (character >= 'A' && character <= 'Z') {
            character = static_cast<char>(character - 'A' + 'a');
        }
    }
    return result;
}

// A catalog prepared for lookups: exact messages in a hash map, and templates
// (messages with "{}") split into their literal parts. Templates that start
// with literal text are tried before templates that start with a value, so
// "cannot scan the root: {}" wraps "opendir failed: Permission denied"
// instead of "{} failed: {}" taking "cannot scan the root: opendir" as its
// first value. Within each group the longest template goes first, so the most
// specific one wins.
struct PreparedCatalog {
    struct Pattern {
        std::vector<std::string> literals;  // the text around the "{}": one more than the values
        std::string translation;
        std::size_t length;
        std::optional<std::size_t> cause;  // Match::Cause: the value that is itself a message
        bool may_match_a_cause;            // may match inside a cause (see Text::translate)
    };
    std::unordered_map<std::string, std::string> exact;
    std::vector<Pattern> patterns;
};

// True when the template starts with literal text rather than with a value.
bool anchored(const PreparedCatalog::Pattern& pattern)
{
    return !pattern.literals.front().empty();
}

PreparedCatalog prepare(std::span<const CatalogEntry> entries)
{
    PreparedCatalog catalog;
    for (const auto& entry : entries) {
        // Every entry is found by exact lookup; Text::format needs nothing more.
        catalog.exact.emplace(std::string(entry.english), std::string(entry.translation));
        const bool is_template = entry.english.find("{}") != std::string_view::npos;
        if (!is_template || entry.match == Match::FormatOnly) {
            continue;
        }
        PreparedCatalog::Pattern pattern{{}, std::string(entry.translation), entry.english.size(), std::nullopt,
            false};
        std::size_t start = 0;
        for (auto found = entry.english.find("{}"); found != std::string_view::npos;
             found = entry.english.find("{}", start)) {
            pattern.literals.emplace_back(entry.english.substr(start, found - start));
            start = found + 2;
        }
        pattern.literals.emplace_back(entry.english.substr(start));
        // The cause is the value after ": " ("context: {}"), the last such
        // value: usually the last of all, but a hint may follow it ("context:
        // {}; continue it with ..."). Each literal but the last comes before
        // the value of the same index.
        if (entry.match == Match::Cause) {
            for (std::size_t index = 0; index + 1 < pattern.literals.size(); ++index) {
                if (pattern.literals[index].ends_with(": ")) {
                    pattern.cause = index;
                }
            }
        }
        // Linux's platform errors start with the name of the call that failed
        // ("open failed: Permission denied"), Windows' with what went wrong
        // ("access denied (CreateFileW, Windows error 5)"). They are the usual
        // cause, so they may match one although Linux's start with a value.
        pattern.may_match_a_cause = anchored(pattern) || entry.english.starts_with("{} failed");
        catalog.patterns.push_back(std::move(pattern));
    }
    // A stable sort keeps catalog order between templates that compare equal.
    std::stable_sort(catalog.patterns.begin(), catalog.patterns.end(),
        [](const auto& left, const auto& right) {
            if (anchored(left) != anchored(right)) {
                return anchored(left);
            }
            return left.length > right.length;
        });
    return catalog;
}

// The values of `sentence` when it has the shape of a template, else nothing:
// it starts with the first literal and ends with the last, and the literals in
// between follow in order. Each value is as short as possible (the first
// occurrence of the next literal); the last value runs up to the final literal.
// Linear in the sentence length, so long paths in messages are cheap.
std::optional<std::vector<std::string>> match_template(
    const std::vector<std::string>& literals, std::string_view sentence)
{
    const auto& first = literals.front();
    const auto& last = literals.back();
    if (sentence.size() < first.size() + last.size() || !sentence.starts_with(first) ||
        !sentence.ends_with(last)) {
        return std::nullopt;
    }
    const auto body = sentence.substr(0, sentence.size() - last.size());
    std::vector<std::string> values;
    std::size_t position = first.size();
    for (std::size_t index = 1; index + 1 < literals.size(); ++index) {
        const auto found = body.find(literals[index], position);
        if (found == std::string_view::npos) {
            return std::nullopt;
        }
        values.emplace_back(body.substr(position, found - position));
        position = found + literals[index].size();
    }
    values.emplace_back(body.substr(position));
    return values;
}

const PreparedCatalog& catalog_for(Language language)
{
    static const PreparedCatalog chinese = prepare(traditional_chinese_catalog());
    static const PreparedCatalog empty;
    return language == Language::TraditionalChinese ? chinese : empty;
}

// Puts the captured values into the translated template: "{}" takes the next
// value, "{0}", "{1}", ... a value by position (when Chinese word order differs).
std::string fill(std::string_view translated, const std::vector<std::string>& values)
{
    std::string result;
    std::size_t next = 0;
    std::size_t index = 0;
    while (index < translated.size()) {
        const auto close = translated.find('}', index);
        if (translated[index] == '{' && close != std::string_view::npos) {
            const auto inside = translated.substr(index + 1, close - index - 1);
            std::size_t group = 0;
            bool numeric = !inside.empty();
            for (const char character : inside) {
                numeric = numeric && character >= '0' && character <= '9';
                group = group * 10 + static_cast<std::size_t>(character - '0');
            }
            if (inside.empty() || numeric) {
                if (inside.empty()) {
                    group = next++;
                }
                result += group < values.size() ? values[group] : std::string{};
                index = close + 1;
                continue;
            }
        }
        result += translated[index++];
    }
    return result;
}

} // namespace

std::optional<Language> parse_language(std::string_view code)
{
    const auto lower = lowercase(code);
    if (lower.starts_with("zh")) {
        return Language::TraditionalChinese; // the only Chinese catalog
    }
    if (lower.starts_with("en") || lower == "c" || lower == "posix") {
        return Language::English;
    }
    return std::nullopt;
}

std::string_view language_code(Language language)
{
    return language == Language::TraditionalChinese ? "zh-TW" : "en";
}

std::string_view language_name(Language language)
{
    return language == Language::TraditionalChinese ? "繁體中文" : "English";
}

Language resolve_language(std::optional<std::string> explicit_code,
    std::optional<std::string> configured_code, std::string_view system_code)
{
    for (const auto& code : {explicit_code, configured_code}) {
        if (code && !code->empty()) {
            if (const auto language = parse_language(*code)) {
                return *language;
            }
        }
    }
    // The system value may be a ':'-separated priority list (Linux LANGUAGE,
    // "fr:zh_TW:en"): the first entry with a catalog wins. None means English,
    // like gettext's implicit "C" at the end of the list.
    std::string_view list = system_code;
    while (!list.empty()) {
        const auto colon = list.find(':');
        if (const auto language = parse_language(list.substr(0, colon))) {
            return *language;
        }
        if (colon == std::string_view::npos) {
            break;
        }
        list.remove_prefix(colon + 1);
    }
    return Language::English;
}

std::string format_translation(std::string_view translated, std::string_view english_template,
    std::format_args values)
{
    try {
        return std::vformat(translated, values);
    } catch (const std::format_error&) {
        // The English template was checked at compile time, so this cannot throw.
        return std::vformat(english_template, values);
    }
}

std::string Text::operator()(std::string_view english) const
{
    return translate(english, 0);
}

// Engine errors wrap their cause: "cannot scan the root: access denied
// (FindFirstFileExW, Windows error 5)". A template marked Match::Cause
// translates its cause (the value after ": ", usually the last) as a message
// of its own; all other values (paths, labels, uuids, numbers) are carried
// over unchanged. Inside a cause
// (depth > 0) only exact messages, templates that start with literal text, and
// platform errors may match: any other template that starts with a value
// matches every text with the same ending, which is too loose for a piece of
// another message.
std::string Text::translate(std::string_view english, int depth) const
{
    const auto& catalog = catalog_for(language_);
    if (catalog.exact.empty()) {
        return std::string(english);
    }
    if (const auto found = catalog.exact.find(std::string(english)); found != catalog.exact.end()) {
        return found->second;
    }
    for (const auto& pattern : catalog.patterns) {
        if (depth > 0 && !pattern.may_match_a_cause) {
            continue;
        }
        auto values = match_template(pattern.literals, english);
        if (!values) {
            continue;
        }
        if (pattern.cause && depth < 4) {
            auto& cause = (*values)[*pattern.cause];
            cause = translate(cause, depth + 1);
        }
        return fill(pattern.translation, *values);
    }
    return std::string(english);
}

std::string Text::translate_template(std::string_view english_template) const
{
    const auto& catalog = catalog_for(language_);
    const auto found = catalog.exact.find(std::string(english_template));
    return found == catalog.exact.end() ? std::string(english_template) : found->second;
}

std::string Text::parenthesized(std::string_view inner) const
{
    // Chinese text uses full-width brackets, as the catalog does throughout.
    if (language_ == Language::TraditionalChinese) {
        return "（" + std::string(inner) + "）";
    }
    return "(" + std::string(inner) + ")";
}

} // namespace va::app
