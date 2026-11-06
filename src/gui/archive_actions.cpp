#include "gui/archive_actions.h"

#include "app/plain_words.h"
#include "app/retirement_report.h"
#include "common/utf8.h"
#include "core/engine/attached.h"
#include "core/engine/candidates.h"
#include "gui/report.h"

#include <algorithm>

namespace va::gui {
namespace {

// The archive as the views name it: its label, or the start of its uuid.
std::string label_of(const std::vector<ArchiveName>& names, const Uuid& archive)
{
    const auto found = std::ranges::find(names, archive, &ArchiveName::archive_uuid);
    return found != names.end() && !found->label.empty() ? found->label : archive.substr(0, 8);
}

} // namespace

std::optional<ListedRelocation> listed_relocation(const app::Text& text,
    const std::vector<app::OperationWatch>& watches, const Uuid& archive, app::OperationWatch::Clock::time_point now,
    std::chrono::sys_seconds wall_now)
{
    for (const auto& watch : watches) {
        const auto& operation = watch.operation();
        if (operation.kind == app::OperationKind::Relocation && operation.archive_uuid == archive) {
            // As its row under Unfinished operations offers Resume.
            const auto row = app::describe_operation(text, watch, now, wall_now);
            return ListedRelocation{operation, row.offers_resume, row.state};
        }
    }
    return std::nullopt;
}

Choice relocation_choice(
    const app::Text& text, const ArchiveStatus& status, const std::optional<ListedRelocation>& unfinished)
{
    if (status.archive.status == "RETIRED") {
        return {false, text("A retired archive takes no new revisions.")};
    }
    if (!status.header) {
        return {false, text("The manifest of its latest revision cannot be used, and a relocation compares the new "
                            "carrier with it.")};
    }
    // One relocation of an archive at a time (decision 0026).
    if (unfinished && unfinished->resumable) {
        return {true, text.format("Resumes the stopped relocation to {}, also at another root where the new carrier "
                                  "is attached at another drive letter or mount point now.",
                          unfinished->operation.root)};
    }
    if (unfinished) {
        return {false, text.format("An unfinished relocation of this archive to {} is listed under Unfinished "
                                   "operations in Jobs ({}).",
                           unfinished->operation.root, unfinished->state)};
    }
    return {true, text("After you copied the archive to another device, or put its device in another enclosure: "
                       "reads the new carrier in full and records it as the archive's next revision.")};
}

Choice export_choice(const app::Text& text, const ArchiveStatus& status)
{
    if (!status.header) {
        return {false, text("The manifest of its latest revision cannot be used: there is nothing to export.")};
    }
    return {true, text.format("Writes the manifest of revision {} and identity.json into a folder named for the "
                              "archive inside the folder you choose; `import` on the command line takes it into this "
                              "repository or another.",
                      status.header->revision)};
}

Choice retire_choice(const app::Text& text, const ArchiveStatus& status)
{
    if (status.archive.status == "RETIRED") {
        return {false, text("This archive is retired already.")};
    }
    return {true, text("It is no longer expected to be verified and keeps its history. A retired archive cannot be "
                       "made active again.")};
}

std::string relocation_intro(const app::Text& text)
{
    return text("VeriArchive copies nothing: first copy the archive's files to the new carrier with your own tools, "
                "then choose here the folder or volume that holds them.");
}

std::string relocation_reading(const app::Text& text, std::uint32_t revision)
{
    return text.format("The relocation reads the new carrier in full and accepts it only when every entry matches "
                       "revision {}; it then records revision {} with the new carrier, and the archive keeps its UUID "
                       "and its history.",
        revision, revision + 1);
}

std::string relocation_resuming(const app::Text& text, const std::string& root)
{
    return text.format("The relocation of this archive to {} stopped. Resume relocation continues it without reading "
                       "again what matched; where the new carrier is attached at another drive letter or mount point "
                       "now, give its root below.",
        root);
}

std::optional<std::string> relocation_root_refusal(const app::Text& text, const VolumeInfo& volume,
    const std::string& root_subpath, std::span<const ArchiveCandidate> candidates, const Uuid& archive,
    const std::vector<ArchiveName>& names)
{
    // As the relocation judges it: another archive whose identity matches
    // this volume, or whose filesystem is on it (MIGRATED), at a root that
    // contains the new root or lies in it.
    const auto found = where_attached(std::span(&volume, 1), candidates);
    for (const auto& root : found.volumes.front()) {
        if (root.archive_uuid != archive && roots_overlap(root.root_subpath, root_subpath)) {
            return text.format("This folder or volume overlaps the root of {}: archive roots on one volume may not "
                               "contain each other. Choose a folder that does not overlap it.",
                text.format("{} ({})", label_of(names, root.archive_uuid), path_to_utf8(root.root)));
        }
    }
    return std::nullopt;
}

std::vector<Field> relocation_summary(
    const app::Text& text, const ArchiveStatus& status, const std::string& root, IdentityShown shown)
{
    std::vector<Field> fields;
    auto archive = display_label(status);
    if (status.header) {
        if (const auto volume = displayed_identity(status.header->identity, shown).volume_label; !volume.empty()) {
            archive = text.format("{} · {}", archive, volume);
        }
    }
    fields.push_back({text("Archive"), archive});
    const auto recorded = status.header ? status.header->root_hint : std::string{};
    fields.push_back({text("Recorded root"), recorded.empty() ? text("not known") : recorded, !recorded.empty()});
    fields.push_back({text("New root"), root.empty() ? text("not chosen yet") : root, !root.empty()});
    if (status.header) {
        const auto& counts = status.header->counts;
        // The files and their size as the details' Content shows them.
        fields.push_back({text("Reads"),
            text.format("{} ({}), each in full", app::count_of(text, counts.files, app::Counted::File),
                format_bytes(counts.bytes))});
        fields.push_back({text("Records"), text.format("revision {} with the new carrier, once every entry matches",
                                               status.header->revision + 1)});
    }
    return fields;
}

std::string relocation_title(const app::Text& text, const std::string& label, const std::string& root, bool resume)
{
    return resume ? text.format("Resume the relocation of {} to {}", label, root)
                  : text.format("Relocate {} to {}", label, root);
}

std::string relocated_toast(const app::Text& text, const std::string& label, std::uint32_t revision)
{
    return text.format("{}: relocated as revision {}", label, revision);
}

std::string not_relocated_toast(const app::Text& text, const std::string& label, bool stopped)
{
    return stopped ? text.format("Relocation of {}: stopped", label) : text.format("Not relocated: {}", label);
}

std::string export_folder_name(const ArchiveStatus& status)
{
    const auto short_uuid = status.archive.archive_uuid.substr(0, 8);
    std::string name = status.archive.label.empty() ? short_uuid : status.archive.label + " " + short_uuid;
    for (auto& character : name) {
        const auto byte = static_cast<unsigned char>(character);
        // The characters Windows refuses in a name (Linux only '/'), and
        // control characters; bytes of UTF-8 sequences stay.
        if (byte < 0x20 || character == '<' || character == '>' || character == ':' || character == '"' ||
            character == '/' || character == '\\' || character == '|' || character == '?' || character == '*') {
            character = '_';
        }
    }
    // Windows drops dots and spaces at the end of a name.
    while (!name.empty() && (name.back() == '.' || name.back() == ' ')) {
        name.pop_back();
    }
    return name.empty() ? short_uuid : name;
}

std::optional<std::string> export_refusal(
    const app::Text& text, const std::filesystem::path& folder, const std::vector<AttachedArchiveRoot>& roots)
{
    for (const auto& attached : roots) {
        if (!attached.root.empty() && path_within(attached.root, folder)) {
            return export_refusal_in_root(text, folder, attached);
        }
    }
    return std::nullopt;
}

std::string export_refusal_in_root(
    const app::Text& text, const std::filesystem::path& folder, const AttachedArchiveRoot& holding)
{
    return text.format("{} lies in the root of {} ({}): an export there would add files that its next FULL "
                       "verification reports as added. Choose a folder outside every archive.",
        path_to_utf8(folder), holding.label, path_to_utf8(holding.root));
}

std::string export_title(const app::Text& text, const std::string& label)
{
    return text.format("Export the manifest of {}", label);
}

std::string export_report(
    const app::Text& text, const std::string& label, std::uint32_t revision, const ExportResult& written)
{
    return text.format("Exported the manifest of revision {} of {}", revision, label) + "\n\n" +
           text.format("Manifest: {}", path_to_utf8(written.manifest)) + "\n" +
           text.format("Identity file: {}", path_to_utf8(written.identity)) + "\n\n" +
           text("This repository or another takes it in with `import <file.vam>` on the command line, also to put "
                "back a lost manifest; it is checked there against the revisions that repository holds.") +
           "\n";
}

std::string exported_toast(const app::Text& text, const std::string& label)
{
    return text.format("Exported the manifest of {}", label);
}

std::string not_exported_toast(const app::Text& text, const std::string& label, bool stopped)
{
    return stopped ? text.format("Export of {}: stopped", label) : text.format("Not exported: {}", label);
}

RetireQuestion retire_question(const app::Text& text, const std::string& label, const Retirement& retirement)
{
    RetireQuestion question;
    question.heading = text.format("Retire {}?", label);
    question.retire = text("Retire");
    const auto what = text("A retired archive is no longer expected to be verified and is never overdue. It keeps its "
                           "verification history and is still recognised when attached, but it takes no new "
                           "revisions. A retired archive cannot be made active again.");
    if (!retirement.lost_manifest) {
        question.body = what;
        return question;
    }
    // Decision 0028, point 9: what retiring an archive whose latest manifest
    // is lost declares, as `archive retire` says it before --confirm, its
    // lines as one paragraph (without the command line's indent); that the
    // commands it names are the command line's; and that there is no way
    // back. Not `what`: such an archive is no longer recognised when attached
    // (GLOSSARY, Retired archive).
    auto explanation = app::retirement_explanation(text, retirement);
    for (auto at = explanation.find("\n  "); at != std::string::npos; at = explanation.find("\n  ", at + 1)) {
        explanation.erase(at + 1, 2);
    }
    while (!explanation.empty() && explanation.back() == '\n') {
        explanation.pop_back();
    }
    question.body = explanation + "\n\n" +
                    text("`import` and `register --lost-manifest` are commands of the command line.") + "\n\n" +
                    text("A retired archive cannot be made active again.");
    question.confirm = true;
    question.confirmation = text("I confirm that this manifest is lost for good and the device is gone");
    return question;
}

std::string retired_toast(const app::Text& text, const std::string& label)
{
    return text.format("Retired {}", label);
}

} // namespace va::gui
