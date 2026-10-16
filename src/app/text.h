#pragma once
// User-interface text in English and Traditional Chinese (decision 0017).
//
// English is the source language: every message is written in English in the
// code. When another language is selected, Text looks the message up in that
// language's catalog. Messages that carry values are written as templates with
// "{}" placeholders, e.g. "Registered {} revision {}", in both languages.
//
// Messages that come from the engines (core) are plain English sentences with
// values already inserted, e.g. "cannot scan the root: access denied". Text
// still translates them: a catalog template "cannot scan the root: {}" matches
// the sentence and the value is carried over into the translation. Values are
// data (paths, labels, uuids, numbers) and are never translated. The one
// exception is a template marked Match::Cause: a wrapper "context: {}" whose
// value after ": " is itself an engine message (the cause), which is
// translated too. The cause is usually the last value; a hint may follow it
// ("context: {}; continue it with ...").
//
// A translation may refer to values by position ("{1}...{0}") when Chinese
// word order differs from English.
//
// Machine-readable output (--json, exit codes) is never translated, so scripts
// keep working in every language; status codes such as PASSED or Modified are
// translated only where people read them.
#include <format>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace va::app {

enum class Language { English, TraditionalChinese };

// Accepts "en", "en-US", "zh-TW", "zh_TW.UTF-8", "zh-Hant", "zh", ...
std::optional<Language> parse_language(std::string_view code);
std::string_view language_code(Language language);  // "en" or "zh-TW"
std::string_view language_name(Language language);  // "English" or "繁體中文"

// Resolution order: explicit choice (--lang, VERIARCHIVE_LANG), the config
// file's "language", the operating system's interface language, English.
// The system value may be a ':'-separated priority list, as in Linux's
// LANGUAGE ("fr:zh_TW:en"): its first recognised entry wins.
Language resolve_language(std::optional<std::string> explicit_code,
    std::optional<std::string> configured_code, std::string_view system_code);

// Fills `translated` with the values. When the translation does not fit the
// values (std::format_error), the English template is used instead: the
// compiler has already checked it, so a bad catalog entry shows English text
// rather than ending the program. Used by Text::format.
std::string format_translation(std::string_view translated, std::string_view english_template,
    std::format_args values);

class Text {
public:
    explicit Text(Language language = Language::English) : language_(language) {}

    Language language() const { return language_; }

    // Translates a fixed message or an engine sentence (see above); text without
    // a translation is returned unchanged.
    std::string operator()(std::string_view english) const;

    // Translates a "{}" template, then fills in the values:
    //   text.format("Registered {} revision {}", label, revision)
    // The template must be a string literal: like std::format, the compiler
    // checks that it fits the values. To choose between two messages, call
    // format twice instead of passing `condition ? "a {}" : "b {}"`.
    template <typename... Args>
    std::string format(std::format_string<const Args&...> english_template,
        const Args&... values) const
    {
        const auto english = english_template.get();
        return format_translation(translate_template(english), english,
            std::make_format_args(values...));
    }

    // An aside in the language's brackets: "(x)" in English, "（x）" in Chinese.
    std::string parenthesized(std::string_view inner) const;

private:
    std::string translate(std::string_view english, int depth) const;
    std::string translate_template(std::string_view english_template) const;
    Language language_;
};

// How a catalog template takes part in translating engine sentences.
enum class Match {
    Values,      // the values (paths, labels, uuids, numbers) are carried over unchanged
    Cause,       // a wrapper "context: {}": the value after ": " is an engine message, translated too
    FormatOnly,  // used only through Text::format, never matched against sentences
};

// One English -> Chinese pair of a catalog.
struct CatalogEntry {
    std::string_view english;
    std::string_view translation;
    Match match = Match::Values;
};

// The Traditional Chinese catalog (text_zh_tw.cpp).
std::span<const CatalogEntry> traditional_chinese_catalog();

} // namespace va::app
