#include "app/plain_words.h"

#include <array>
#include <optional>

namespace va::app {
namespace {

// The run-level failure classes (HANDOFF §A6).
constexpr std::array<std::string_view, 8> failure_classes{"MediaIdentityMismatch", "MediaIdentityAmbiguous",
    "ManifestSignatureFailure", "ManifestHashMismatch", "ManifestRevisionRollback", "ChangeTrackerContinuityLost",
    "FilesystemAccessError", "RepositoryError"};

// An entry's type as a manifest writes it ('f', 'd', 'l', 's').
std::optional<std::string_view> type_words(char type)
{
    switch (type) {
    case 'f':
        return "file";
    case 'd':
        return "folder";
    case 'l':
        return "link";
    case 's':
        return "stream";
    default:
        return std::nullopt;
    }
}

// What differs of an entry's metadata ("mtime", "mode", "attrs").
std::optional<std::string_view> metadata_words(std::string_view part)
{
    if (part == "mtime") {
        return "modification time";
    }
    if (part == "mode") {
        return "permissions";
    }
    if (part == "attrs") {
        return "attributes";
    }
    return std::nullopt;
}

} // namespace

std::string item_detail_words(std::string_view detail)
{
    // A change of type: "d -> f".
    if (detail.size() == 6 && detail.substr(1, 4) == " -> ") {
        const auto from = type_words(detail[0]);
        const auto to = type_words(detail[5]);
        if (from && to) {
            return std::string(*from) + " -> " + std::string(*to);
        }
    }
    // The metadata that differs: "mtime", "mtime,mode", ...
    std::string words;
    bool metadata = !detail.empty();
    for (std::size_t start = 0; metadata && start <= detail.size();) {
        auto end = detail.find(',', start);
        if (end == std::string_view::npos) {
            end = detail.size();
        }
        const auto part = metadata_words(detail.substr(start, end - start));
        metadata = part.has_value();
        if (metadata) {
            words += (words.empty() ? "" : ", ") + std::string(*part);
        }
        start = end + 1;
    }
    if (metadata) {
        return words;
    }
    if (detail == "link-target-translated") {
        return "link target converted by this system";
    }
    if (detail == "known-since-registration") {
        return "unreadable since registration";
    }
    if (detail == "name cannot exist on windows") {
        return "name cannot exist on Windows";
    }
    if (detail == "name cannot exist on linux") {
        return "name cannot exist on Linux";
    }
    return std::string(detail);
}

std::string failure_class_words(std::string_view failure_class)
{
    if (failure_class == "MediaIdentityMismatch") {
        return "the media is not the one registered";
    }
    if (failure_class == "MediaIdentityAmbiguous") {
        return "the media matches more than one archive";
    }
    if (failure_class == "ManifestSignatureFailure") {
        return "the manifest's signature does not verify";
    }
    if (failure_class == "ManifestHashMismatch") {
        return "the manifest is not the one recorded";
    }
    if (failure_class == "ManifestRevisionRollback") {
        return "a revision older than one already known";
    }
    if (failure_class == "ChangeTrackerContinuityLost") {
        return "the record of changes on the volume was interrupted";
    }
    if (failure_class == "FilesystemAccessError") {
        return "files could not be read";
    }
    if (failure_class == "RepositoryError") {
        return "the repository could not be read or written";
    }
    return std::string(failure_class);
}

std::string_view without_failure_class(std::string_view message)
{
    for (const auto failure_class : failure_classes) {
        if (message.size() > failure_class.size() + 2 && message.starts_with(failure_class) &&
            message.substr(failure_class.size(), 2) == ": ") {
            return message.substr(failure_class.size() + 2);
        }
    }
    return message;
}

std::string failure_classes_words(const Text& text, std::string_view classes)
{
    std::string result;
    for (std::size_t start = 0; !classes.empty() && start <= classes.size();) {
        auto end = classes.find(',', start);
        if (end == std::string_view::npos) {
            end = classes.size();
        }
        result += (result.empty() ? "" : "; ") + text(failure_class_words(classes.substr(start, end - start)));
        start = end + 1;
    }
    return result;
}

std::string count_of(const Text& text, std::uint64_t count, Counted counted)
{
    // Two calls each: the compiler checks each literal template against its value.
    const bool one = count == 1;
    switch (counted) {
    case Counted::File:
        return one ? text.format("{} file", count) : text.format("{} files", count);
    case Counted::EmptyDirectory:
        return one ? text.format("{} empty directory", count) : text.format("{} empty directories", count);
    case Counted::Link:
        return one ? text.format("{} link", count) : text.format("{} links", count);
    case Counted::Byte:
        return one ? text.format("{} byte", count) : text.format("{} bytes", count);
    case Counted::Entry:
        return one ? text.format("{} entry", count) : text.format("{} entries", count);
    case Counted::Archive:
        return one ? text.format("{} archive", count) : text.format("{} archives", count);
    case Counted::Revision:
        return one ? text.format("{} revision", count) : text.format("{} revisions", count);
    }
    return std::to_string(count);
}

} // namespace va::app
