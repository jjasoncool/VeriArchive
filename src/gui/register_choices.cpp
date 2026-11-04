#include "gui/register_choices.h"

#include "app/device_alerts.h"
#include "app/plain_words.h"
#include "common/archive_path.h"
#include "common/unicode.h"
#include "common/utf8.h"
#include "core/engine/attached.h"
#include "core/engine/candidates.h"
#include "gui/report.h"

#include <algorithm>
#include <regex>

namespace va::gui {
namespace {

// The archive as the views name it: its label, or the start of its uuid.
std::string label_of(const std::vector<ArchiveName>& names, const Uuid& archive)
{
    const auto found = std::ranges::find(names, archive, &ArchiveName::archive_uuid);
    return found != names.end() && !found->label.empty() ? found->label : archive.substr(0, 8);
}

// The archives' labels in a list: "ARCH-0001, ARCH-0002" (ARCH-0001、ARCH-0002).
std::string listed(const app::Text& text, const std::vector<std::string>& items)
{
    std::string list;
    for (const auto& item : items) {
        list = list.empty() ? item : text.format("{}, {}", list, item);
    }
    return list;
}

std::string labels_of(const app::Text& text, const std::vector<ArchiveName>& names, const std::vector<Uuid>& archives)
{
    std::vector<std::string> labels;
    for (const auto& archive : archives) {
        labels.push_back(label_of(names, archive));
    }
    return listed(text, labels);
}

std::string joined(const app::Text& text, const std::vector<std::string>& parts)
{
    std::string line;
    for (const auto& part : parts) {
        if (!part.empty()) {
            line = line.empty() ? part : text.format("{} · {}", line, part);
        }
    }
    return line;
}

// A folder as a registration finds its plan (register_engine's find_pending):
// normal, without the separators it ends with, folded as roots are compared.
std::string folder_key(const std::filesystem::path& path)
{
    return unicode_fold(path_to_utf8(without_trailing_separators(path.lexically_normal())));
}

// Known by its GUID path alone (decision 0029, point 3): "\\?\Volume{...}\".
bool guid_path(const std::filesystem::path& mount_point)
{
    return path_to_utf8(mount_point).starts_with("\\\\?\\");
}

std::string volume_title(const app::Text& text, const VolumeInfo& volume)
{
    return volume.volume_label.empty() ? text("Volume without a label") : volume.volume_label;
}

// The models of the volume's devices, joined: most volumes have one.
std::string models_of(const app::Text& text, const VolumeInfo& volume)
{
    std::vector<std::string> models;
    for (const auto& device : volume.devices) {
        if (!device.model.empty() && std::ranges::find(models, device.model) == models.end()) {
            models.push_back(device.model);
        }
    }
    return listed(text, models);
}

std::string bus_of(const VolumeInfo& volume)
{
    return volume.devices.empty() ? std::string{} : bus_words(volume.devices.front().bus);
}

// The root of an archive at `root_subpath` on the source's volume, as the
// rows name roots (where_attached makes them alike).
std::string root_on(const VolumeInfo& volume, const std::string& root_subpath)
{
    if (root_subpath.empty()) {
        return path_to_utf8(without_trailing_separators(volume.mount_point));
    }
    const auto relative = archive_path_to_native(ArchivePath{root_subpath, "", ""});
    return relative ? path_to_utf8(without_trailing_separators(volume.mount_point / *relative)) : root_subpath;
}

bool among(const std::vector<Uuid>& archives, const Uuid& archive)
{
    return std::ranges::find(archives, archive) != archives.end();
}

const std::regex label_form("ARCH-[0-9]{4,}");

} // namespace

std::vector<VolumeRow> volume_rows(const app::Text& text, const std::vector<AttachedVolume>& volumes,
    std::span<const ArchiveCandidate> candidates, const std::vector<PendingRoot>& pending,
    const std::vector<ArchiveName>& names, IdentityShown shown)
{
    std::vector<VolumeInfo> identities;
    for (const auto& attached : volumes) {
        identities.push_back(attached.volume);
    }
    const auto attachments = where_attached(identities, candidates);
    std::vector<VolumeRow> rows;
    for (std::size_t index = 0; index < volumes.size(); ++index) {
        const auto& attached = volumes[index];
        const auto& mount_point = attached.volume.mount_point;
        if (mount_point.empty()) {
            continue;  // nothing to register on it
        }
        const auto volume = displayed_identity(attached.volume, shown);
        VolumeRow row;
        row.volume = index;
        row.root = path_to_utf8(mount_point);
        row.title = volume_title(text, volume);
        row.where = row.root;
        std::vector<std::string> details{models_of(text, volume), bus_of(volume)};
        if (volume.capacity_bytes > 0) {
            details.push_back(format_bytes(volume.capacity_bytes));
        }
        if (attached.used_bytes) {
            details.push_back(text.format("{} used", format_bytes(*attached.used_bytes)));
        }
        if (volume.read_only) {
            details.push_back(text("read-only"));
        }
        row.details = joined(text, details);

        // What lies on it: the archives' roots, those of a filesystem seen on
        // another device too, and the roots of unfinished registrations.
        std::vector<std::string> held;
        bool whole = false;      // one of them is the volume's root
        bool in_folder = false;  // one of them is a folder in it
        for (const auto& root : attachments.volumes[index]) {
            const auto label = label_of(names, root.archive_uuid);
            const auto where = path_to_utf8(root.root);
            held.push_back(root.match == MatchResult::Migrated
                               ? text.format("the filesystem of {}, behind another enclosure or on another device "
                                             "than recorded ({})",
                                     label, where)
                               : text.format("{} ({})", label, where));
            (root.root_subpath.empty() ? whole : in_folder) = true;
        }
        for (const auto& registration : pending) {
            const auto root = path_from_utf8(registration.root);
            if (!path_within(mount_point, root)) {
                continue;
            }
            held.push_back(text.format("an unfinished registration of {} ({})", registration.label, registration.root));
            (folder_key(root) == folder_key(mount_point) ? whole : in_folder) = true;
        }
        if (!held.empty()) {
            const auto holds = text.format("Holds {}", listed(text, held));
            // Its own root alone: chosen, Options offers what applies there.
            row.selectable = whole && !in_folder;
            if (whole) {
                row.note = holds;
            } else {
                row.note = text.format("{}; the whole volume cannot be registered: choose a folder in it that does not "
                                       "overlap them",
                    holds);
                row.folder_tooltip = text.format("Choose a folder in {}...", row.where);
                // A GUID path is too long for a button.
                row.folder_button =
                    guid_path(mount_point) ? text("Choose a folder in it...") : row.folder_tooltip;
            }
        }
        rows.push_back(std::move(row));
    }
    std::ranges::stable_sort(rows, [&](const VolumeRow& left, const VolumeRow& right) {
        const auto& a = volumes[left.volume].volume.mount_point;
        const auto& b = volumes[right.volume].volume.mount_point;
        if (guid_path(a) != guid_path(b)) {
            return !guid_path(a);
        }
        return unicode_fold(left.root) < unicode_fold(right.root);
    });
    return rows;
}

SourceOptions source_options(const app::Text& text, const ChosenSource& source,
    std::span<const ArchiveCandidate> candidates, const std::vector<PendingRoot>& pending,
    const std::vector<ArchiveName>& names, const std::vector<EarlierCarrier>& earlier)
{
    SourceOptions options;
    const auto wanted = folder_key(source.root);
    for (const auto& registration : pending) {
        if (folder_key(path_from_utf8(registration.root)) == wanted) {
            options.unfinished = registration;
            break;
        }
    }
    const auto overlapping = [&](const Uuid& archive, const std::string& root_subpath) {
        return text.format("This folder or volume overlaps the root of {}: archive roots on one volume may not contain "
                           "each other. Choose a folder that does not overlap it.",
            text.format("{} ({})", label_of(names, archive), root_on(source.volume, root_subpath)));
    };
    const auto found = where_attached(std::span(&source.volume, 1), candidates);
    const auto here = unicode_fold(source.root_subpath);
    bool retired_here = false;
    for (const auto& root : found.volumes.front()) {
        if (unicode_fold(root.root_subpath) == here) {
            if (root.match == MatchResult::Migrated) {
                options.clones_here.push_back(root.archive_uuid);
            } else {
                options.registered_here.push_back(root.archive_uuid);
                retired_here = retired_here || std::ranges::find(root.details, "retired") != root.details.end();
            }
        } else if (!options.refusal && roots_overlap(root.root_subpath, source.root_subpath)) {
            options.refusal = overlapping(root.archive_uuid, root.root_subpath);
        }
    }
    // As check_existing_archives refuses an earlier carrier of an archive
    // (decision 0004): at its root, unless registered apart from it; a root
    // that overlaps it, always.
    for (const auto& carrier : earlier) {
        if (unicode_fold(carrier.root_subpath) == here) {
            if (!among(options.registered_here, carrier.archive_uuid) && !among(options.clones_here, carrier.archive_uuid) &&
                !among(options.earlier_carrier_of, carrier.archive_uuid)) {
                options.earlier_carrier_of.push_back(carrier.archive_uuid);
            }
        } else if (!options.refusal && roots_overlap(carrier.root_subpath, source.root_subpath)) {
            options.refusal = overlapping(carrier.archive_uuid, carrier.root_subpath);
        }
    }
    // Several archives at one root (AMBIGUOUS) take no new revision, nor
    // does a retired one (decision 0003).
    options.retired = options.registered_here.size() == 1 && retired_here;
    options.new_revision = options.registered_here.size() == 1 && !retired_here;
    return options;
}

OptionsShown options_shown(const app::Text& text, const SourceOptions& options, const OptionsChosen& chosen,
    const std::vector<ArchiveName>& names)
{
    OptionsShown shown;
    shown.label_note = options.next_label
                           ? text.format("Leave it empty to take {}, the repository's next label.", *options.next_label)
                           : text("Leave it empty to take the repository's next label (ARCH-NNNN).");
    if (options.unfinished) {
        // Resumed: nothing else can start at this root until then.
        shown.resume = true;
        shown.resume_note = text.format("An interrupted registration of {} exists at this folder or volume: it "
                                        "continues without reading again the files it hashed.",
            options.unfinished->label);
        return shown;
    }
    const bool new_revision = options.new_revision && chosen.new_revision;
    if (!options.registered_here.empty()) {
        const auto labels = labels_of(text, names, options.registered_here);
        shown.new_revision = options.new_revision;
        shown.new_revision_note = text.format("Reads all of {} again and makes its current content the new baseline. "
                                              "Use it after you changed the content on purpose.",
            labels);
        shown.another_archive = true;
        shown.another_archive_note = text.format("This folder or volume is registered as {}. Another archive gets "
                                                 "revisions and a history of its own.",
            labels);
        if (!new_revision && !chosen.another_archive) {
            // Why no new revision is offered, where none is.
            if (options.new_revision) {
                shown.blocked = text("Choose whether to create a new revision of the archive registered here or to "
                                     "register another archive.");
            } else if (options.registered_here.size() > 1) {
                shown.blocked = text.format("Several archives are registered here ({}), so none takes a new revision: "
                                            "register another archive, or choose another folder or volume.",
                    labels);
            } else if (options.retired) {
                shown.blocked = text.format("{} is retired and takes no new revision: register another archive, or "
                                            "choose another folder or volume.",
                    labels);
            } else {
                shown.blocked = text("This folder or volume is registered already: choose to register another "
                                     "archive, or choose another folder or volume.");
            }
        }
    } else if (!options.clones_here.empty()) {
        shown.another_archive = true;
        shown.another_archive_note = text.format("The filesystem of this folder or volume is that of {}, on another "
                                                 "device than the one recorded. Register it as another archive only "
                                                 "if it is another device, such as a sector-by-sector clone.",
            labels_of(text, names, options.clones_here));
        if (!chosen.another_archive) {
            shown.blocked = text("The filesystem of this folder or volume is a registered archive's, but the enclosure "
                                 "or device differs from the one recorded: choose to register another archive, or "
                                 "choose another folder or volume.");
        }
    } else if (!options.earlier_carrier_of.empty()) {
        const auto labels = labels_of(text, names, options.earlier_carrier_of);
        shown.another_archive = true;
        shown.another_archive_note = text.format("This folder or volume is an earlier carrier of {}: an earlier "
                                                 "revision of it was registered here, and its latest revision is on "
                                                 "another volume. Register it as another archive only if it is to be "
                                                 "kept and verified apart from {}.",
            labels, labels);
        if (!chosen.another_archive) {
            shown.blocked = text("This folder or volume is an earlier carrier of a registered archive: choose to "
                                 "register another archive, or choose another folder or volume.");
        }
    }
    // A new revision keeps the archive's label and the copy it records.
    shown.label = !new_revision;
    shown.copy_of = !new_revision;
    if (shown.label && !shown.blocked && !chosen.label.empty() && !std::regex_match(chosen.label, label_form)) {
        shown.blocked = text("A label has the form ARCH-NNNN, such as ARCH-0012.");
    }
    return shown;
}

std::vector<ArchiveName> copy_of_choices(const std::vector<ArchiveName>& names, const SourceOptions& options)
{
    std::vector<ArchiveName> first;
    std::vector<ArchiveName> rest;
    for (const auto& name : names) {
        const bool near = among(options.registered_here, name.archive_uuid) ||
                          among(options.clones_here, name.archive_uuid) ||
                          among(options.earlier_carrier_of, name.archive_uuid);
        (near ? first : rest).push_back(name);
    }
    first.insert(first.end(), rest.begin(), rest.end());
    return first;
}

std::optional<Uuid> default_copy_of(const SourceOptions& options)
{
    return options.clones_here.empty() ? std::nullopt : std::optional(options.clones_here.front());
}

RegisterRequest registration_request(const ChosenSource& source, const SourceOptions& options,
    const OptionsChosen& chosen, const std::map<std::string, bool>& rotational_override)
{
    RegisterRequest request;
    request.root = source.root;
    request.write_media_copy = chosen.media_copy;
    request.rotational_override = rotational_override;
    // As `register <root> --resume`: the plan fixed the rest.
    if (options.unfinished) {
        request.resume = true;
        return request;
    }
    request.new_revision = options.new_revision && chosen.new_revision;
    // As `register <root> --new-revision --force-new-archive`: the filesystem
    // of another archive at this root on another device (MIGRATED), or an
    // earlier carrier of one, refuses a new revision unless the root is
    // registered apart from that archive.
    const bool others_here = !options.clones_here.empty() || !options.earlier_carrier_of.empty();
    request.force_new_archive = request.new_revision
                                    ? others_here
                                    : chosen.another_archive && (!options.registered_here.empty() || others_here);
    if (!request.new_revision) {
        if (!chosen.label.empty()) {
            request.label = chosen.label;
        }
        request.copy_of = chosen.copy_of;
    }
    return request;
}

std::vector<Field> registration_summary(const app::Text& text, const ChosenSource& source,
    const SourceOptions& options, const OptionsChosen& chosen, const std::vector<ArchiveName>& names,
    IdentityShown shown)
{
    const auto request = registration_request(source, options, chosen, {});
    std::vector<Field> fields;
    fields.push_back({text("Source"), path_to_utf8(source.root)});
    fields.push_back({text("Volume"), source_volume_words(text, source, shown)});

    std::string label;
    std::string registers;
    if (request.resume) {
        label = text.format("{}, as the interrupted registration planned it", options.unfinished->label);
        registers = text.format("the rest of the interrupted registration of {}", options.unfinished->label);
    } else if (request.new_revision) {
        const auto archive = label_of(names, options.registered_here.front());
        label = text.format("{}, the archive's own", archive);
        registers = text.format("a new revision of {}", archive);
    } else {
        label = request.label        ? *request.label
                : options.next_label ? text.format("{}, the repository's next label", *options.next_label)
                                     : text("the next label of the repository");
        auto besides = options.registered_here;
        besides.insert(besides.end(), options.clones_here.begin(), options.clones_here.end());
        besides.insert(besides.end(), options.earlier_carrier_of.begin(), options.earlier_carrier_of.end());
        registers = request.force_new_archive
                        ? text.format("another archive, besides {}", labels_of(text, names, besides))
                        : text("a new archive");
    }
    fields.push_back({text("Label"), label});
    if (request.resume) {
        fields.push_back({text("To read"), text("the files that the interrupted registration has not hashed yet; "
                                                "those it hashed are not read again")});
    } else if (source.root_subpath.empty()) {
        fields.push_back({text("To read"),
            source.used_bytes ? text.format("every file of the volume, about {} in use; their number is known after "
                                            "the scan",
                                    format_bytes(*source.used_bytes))
                              : text("every file of the volume; their number is known after the scan")});
    } else {
        fields.push_back({text("To read"), text("every file in this folder; their number and size are known after the "
                                                "scan")});
    }
    fields.push_back({text("Registration"), registers});
    fields.push_back({text("Media copy"), request.write_media_copy
                                              ? text("written to the .veriarchive folder in the root")
                                              : text("not written: nothing is written to the archive")});
    if (!request.resume && !request.new_revision) {
        fields.push_back(
            {text("Copy of"), request.copy_of ? label_of(names, *request.copy_of) : text("not a copy")});
    }
    return fields;
}

std::string source_volume_words(const app::Text& text, const ChosenSource& source, IdentityShown shown)
{
    const auto volume = displayed_identity(source.volume, shown);
    return joined(text, {volume_title(text, volume), models_of(text, volume)});
}

std::string new_revision_source(const std::vector<AttachedVolume>& volumes,
    std::span<const ArchiveCandidate> candidates, const Uuid& archive, const std::string& recorded)
{
    std::vector<VolumeInfo> identities;
    for (const auto& attached : volumes) {
        identities.push_back(attached.volume);
    }
    for (const auto& attachment : where_attached(identities, candidates).archives) {
        if (attachment.archive_uuid == archive &&
            (attachment.result == MatchResult::Match || attachment.result == MatchResult::MatchWeak)) {
            return path_to_utf8(attachment.places.front().root);
        }
    }
    return recorded;
}

std::optional<std::string> new_revision_refusal(const app::Text& text, const SourceOptions& options,
    const Uuid& archive, const std::vector<ArchiveName>& names, const std::string& root)
{
    if (among(options.registered_here, archive)) {
        return std::nullopt;
    }
    return text.format("{} is not at {} now: its volume may be attached at another drive letter or mount point, or "
                       "not attached. Choose the folder or volume where it is now.",
        label_of(names, archive), root);
}

std::string registration_report(const app::Text& text, const RegisterOutcome& outcome)
{
    std::string body =
        text.format("Registered {} ({}) revision {}", outcome.label, outcome.archive_uuid, outcome.revision) +
        "\n\n";
    // Decision 0044: a device alert comes first, below the result line.
    for (const auto& line : app::health_alert_lines(text, outcome.devices)) {
        body += line + "\n";
    }
    body += text.format("{}, {}, {}, {} hashed", app::count_of(text, outcome.counts.files, app::Counted::File),
                app::count_of(text, outcome.counts.dirs, app::Counted::EmptyDirectory),
                app::count_of(text, outcome.counts.links, app::Counted::Link), format_bytes(outcome.counts.bytes)) +
            "\n" + text.format("Manifest: {}", path_to_utf8(outcome.manifest)) + "\n";
    for (const auto& device : outcome.devices) {
        body += text.format("Device health: {}", app::device_health_words(text, device)) + "\n";
    }
    if (!outcome.unreadable.empty()) {
        body += "\n" + text("Unreadable at registration (recorded without hashes, never reported as verified):") +
                "\n";
        for (const auto& entry : outcome.unreadable) {
            body += "  " + text(entry.unreadable) + "  " + entry.path.p + "\n";
        }
    }
    for (const auto& mount_point : outcome.skipped_mount_points) {
        body += text.format("Skipped mount point: {}", mount_point.p) + "\n";
    }
    for (const auto& warning : outcome.warnings) {
        body += text.format("Warning: {}", text(warning)) + "\n";
    }
    // A registration over a lost manifest, resumed here (decision 0028, point 8).
    if (outcome.lost_revision && outcome.lost_revision_left_out) {
        body += text.format("Revision {}, whose manifest was lost, was left out of the index; its verifications "
                            "stay in the history.",
                    *outcome.lost_revision) +
                "\n";
    }
    body += outcome.media_copy.empty()
                ? "\n" + text("The baseline exists only in the repository: back up RepositoryRoot.") + "\n"
                : "\n" + text.format("Media copy: {}", path_to_utf8(outcome.media_copy)) + "\n";
    return body;
}

std::string registered_toast(const app::Text& text, const std::string& label, std::uint32_t revision)
{
    return revision <= 1 ? text.format("Registered as {}", label)
                         : text.format("Registered revision {} of {}", revision, label);
}

std::string not_registered_toast(const app::Text& text, const std::string& root)
{
    return text.format("Not registered: {}", root);
}

} // namespace va::gui
