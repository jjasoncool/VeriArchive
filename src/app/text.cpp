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
// (messages with "{}") split into their literal parts, longest template first
// so the most specific one wins.
struct PreparedCatalog {
    struct Pattern {
        std::vector<std::string> literals;  // the text around the "{}": one more than the values
        std::string translation;
        std::size_t length;
        bool ends_with_cause;  // "context: {}": the last value is itself a message
    };
    std::unordered_map<std::string, std::string> exact;
    std::vector<Pattern> patterns;
};

PreparedCatalog prepare(std::span<const CatalogEntry> entries)
{
    PreparedCatalog catalog;
    for (const auto& entry : entries) {
        catalog.exact.emplace(std::string(entry.english), std::string(entry.translation));
        if (entry.english.find("{}") == std::string_view::npos) {
            continue;
        }
        PreparedCatalog::Pattern pattern{{}, std::string(entry.translation), entry.english.size(),
            entry.english.ends_with(": {}")};
        std::size_t start = 0;
        for (auto found = entry.english.find("{}"); found != std::string_view::npos;
             found = entry.english.find("{}", start)) {
            pattern.literals.emplace_back(entry.english.substr(start, found - start));
            start = found + 2;
        }
        pattern.literals.emplace_back(entry.english.substr(start));
        catalog.patterns.push_back(std::move(pattern));
    }
    std::stable_sort(catalog.patterns.begin(), catalog.patterns.end(),
        [](const auto& left, const auto& right) { return left.length > right.length; });
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
    return parse_language(system_code).value_or(Language::English);
}

std::string Text::operator()(std::string_view english) const
{
    return translate(english, 0);
}

// Engine errors wrap their cause: "cannot read the manifest: access denied".
// When a template ends in ": {}", the cause is translated as well; other values
// (paths, labels, numbers) are carried over unchanged.
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
        auto values = match_template(pattern.literals, english);
        if (!values) {
            continue;
        }
        if (pattern.ends_with_cause && depth < 4) {
            values->back() = translate(values->back(), depth + 1);
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

} // namespace va::app
