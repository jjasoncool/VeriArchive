#include "gui/device_rows.h"

#include "app/device_alerts.h"
#include "common/unicode.h"
#include "common/utf8.h"
#include "core/engine/candidates.h"
#include "core/engine/device_alerts.h"
#include "gui/report.h"

#include <algorithm>
#include <utility>

namespace va::gui {
namespace {

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

// Items in a list: "a, b" (a、b).
std::string listed(const app::Text& text, const std::vector<std::string>& items)
{
    std::string list;
    for (const auto& item : items) {
        list = list.empty() ? item : text.format("{}, {}", list, item);
    }
    return list;
}

std::string label_of(const std::vector<ArchiveName>& names, const Uuid& archive)
{
    const auto found = std::ranges::find(names, archive, &ArchiveName::archive_uuid);
    return found != names.end() && !found->label.empty() ? found->label : archive.substr(0, 8);
}

// Known by its GUID path alone (decision 0029, point 3): "\\?\Volume{...}\".
bool guid_path(const std::filesystem::path& mount_point)
{
    return path_to_utf8(mount_point).starts_with("\\\\?\\");
}

// Drive letters and folders first, by them, then GUID paths, as Register
// Archive lists the volumes; a volume without a mount point last.
bool listed_before(const std::filesystem::path& left, const std::filesystem::path& right)
{
    if (left.empty() != right.empty()) {
        return !left.empty();
    }
    if (guid_path(left) != guid_path(right)) {
        return !guid_path(left);
    }
    return unicode_fold(path_to_utf8(left)) < unicode_fold(path_to_utf8(right));
}

std::string volume_title(const app::Text& text, const VolumeInfo& volume)
{
    return volume.volume_label.empty() ? text("Volume without a label") : volume.volume_label;
}

// The roots of an archive's places, one on each line.
std::string roots_of(const ArchiveAttachment& attachment)
{
    std::string roots;
    for (const auto& place : attachment.places) {
        roots += (roots.empty() ? "" : "\n") + path_to_utf8(place.root);
    }
    return roots;
}

std::string roots_listed(const app::Text& text, const ArchiveAttachment& attachment)
{
    std::vector<std::string> roots;
    for (const auto& place : attachment.places) {
        roots.push_back(path_to_utf8(place.root));
    }
    return listed(text, roots);
}

// A recorded device as the notice of an archive not attached names it: its
// model with its serial, or its WWN where it has no serial.
std::string recorded_device_words(const app::Text& text, const PhysicalDeviceInfo& device)
{
    const auto model = device.model.empty() ? text("not known") : device.model;
    if (!device.serial.empty()) {
        return text.format("{}, serial {}", model, device.serial);
    }
    if (!device.wwn.empty()) {
        return text.format("{}, WWN {}", model, device.wwn);
    }
    return model;
}

std::string not_attached_notice(const app::Text& text, const ArchiveStatus& status, IdentityShown shown)
{
    if (!status.header) {
        return text("The volume of this archive is not attached now. Once it is attached, the archive can be "
                    "verified.");
    }
    // As the archive's current revision recorded its carrier, or the sample
    // identity in the screenshot mode.
    const auto identity = displayed_identity(status.header->identity, shown);
    if (!identity.devices.empty()) {
        std::vector<std::string> devices;
        for (const auto& device : identity.devices) {
            devices.push_back(recorded_device_words(text, device));
        }
        return text.format("The device of this archive ({}) is not attached now. Once it is attached, the archive "
                           "can be verified.",
            listed(text, devices));
    }
    if (!identity.volume_label.empty()) {
        return text.format("The volume of this archive ({}) is not attached now. Once it is attached, the archive "
                           "can be verified.",
            identity.volume_label);
    }
    return text("The volume of this archive is not attached now. Once it is attached, the archive can be verified.");
}

// MIGRATED: what it means, then, on a line of its own, how the archive is
// moved to the new carrier in the window (decision 0046): Relocate to this
// carrier... beside the notice, which opens the relocation dialog with the
// root found there, or, where it is found at more than one root, Relocate to
// a new carrier... in the menu of the details. A retired archive takes no new
// revisions (decision 0003): its window offers neither, which it says.
std::string migrated_notice(const app::Text& text, const ArchiveAttachment& attachment, bool retired)
{
    const auto meaning = text.format("The filesystem of this archive is attached at {}, but on another device than "
                                     "the one recorded (MIGRATED): the archive's device in another enclosure, or a "
                                     "sector-by-sector clone of it. FULL verify does not read it there; a clone is "
                                     "registered as a separate archive in Register Archive.",
        roots_listed(text, attachment));
    if (retired) {
        return meaning + "\n" + text("This archive is retired and takes no new revisions, so it is not relocated.");
    }
    const auto pointer = attachment.places.size() == 1
                             ? text("If it is this archive's device in another enclosure, or you moved the archive to "
                                    "that device on purpose, relocate the archive to it with Relocate to this "
                                    "carrier..., which reads it in full first.")
                             : text("If one of them is this archive's device in another enclosure, or you moved the "
                                    "archive to that device on purpose, relocate the archive to it with Relocate to a "
                                    "new carrier... in the menu of these details, which reads it in full first.");
    return meaning + "\n" + pointer;
}

// "Healthy", as the other pills begin: the first letter of English words
// made a capital (Chinese has none).
std::string capitalised(std::string words)
{
    if (!words.empty() && words.front() >= 'a' && words.front() <= 'z') {
        words.front() = static_cast<char>(words.front() - 'a' + 'A');
    }
    return words;
}

} // namespace

AttachedRead attached_read(app::AttachedDevices attached, const std::vector<std::optional<std::uint64_t>>& used,
    std::span<const ArchiveCandidate> candidates, bool repository, std::optional<std::string> unread, bool sample,
    std::vector<VolumeInfo> own)
{
    AttachedRead read;
    read.repository = repository;
    read.unread = std::move(unread);
    read.sample = sample;
    for (std::size_t index = 0; index < attached.volumes.size(); ++index) {
        read.volumes.push_back({std::move(attached.volumes[index]), index < used.size() ? used[index] : std::nullopt});
    }
    read.devices = std::move(attached.devices);
    std::vector<VolumeInfo> identities;
    for (const auto& volume : read.volumes) {
        identities.push_back(volume.volume);
    }
    auto found = where_attached(identities, candidates);
    read.on_volumes = std::move(found.volumes);
    if (!sample) {
        read.archives = std::move(found.archives);
        read.archive_volumes = std::move(identities);
        return read;
    }
    auto mine = where_attached(own, candidates);
    read.archives = std::move(mine.archives);
    for (std::size_t index = 0; index < own.size(); ++index) {
        if (mine.volumes[index].empty()) {
            continue;
        }
        // Listed as the sample identity alone, the sample list being shown
        // as it is: SAMPLE DATA but for its mount point. Its device is not
        // asked for its health, nor is the space in use on it read.
        auto shown = displayed_identity(own[index], IdentityShown::Sample);
        for (auto& device : shown.devices) {
            device.health = DeviceHealth{HealthStatus::Healthy, {}};
        }
        const auto at = read.volumes.size();
        for (const auto& device : shown.devices) {
            read.devices.push_back({device, {at}});
        }
        auto roots = std::move(mine.volumes[index]);
        for (auto& root : roots) {
            root.volume = at;
        }
        read.volumes.push_back({std::move(shown), std::nullopt});
        read.on_volumes.push_back(std::move(roots));
    }
    read.archive_volumes = std::move(own);
    return read;
}

const ArchiveAttachment* attachment_of(const AttachedRead& read, const Uuid& archive)
{
    const auto found = std::ranges::find(read.archives, archive, &ArchiveAttachment::archive_uuid);
    return found != read.archives.end() ? &*found : nullptr;
}

ArchiveAttached archive_attached(
    const app::Text& text, const AttachedRead* read, const ArchiveStatus& status, IdentityShown shown)
{
    ArchiveAttached attached;
    const auto* attachment = read ? attachment_of(*read, status.archive.archive_uuid) : nullptr;
    if (!attachment) {
        return attached;
    }
    switch (attachment->result) {
    case MatchResult::Match:
    case MatchResult::MatchWeak: {
        const auto& place = attachment->places.front();
        const auto root = path_to_utf8(place.root);
        attached.line = AttachedLine{Attached::Here, root, root};
        // The volume as the views show it: the sample identity in the
        // screenshot mode.
        if (place.volume < read->archive_volumes.size()) {
            attached.fields.push_back({text("Attached volume"),
                volume_title(text, displayed_identity(read->archive_volumes[place.volume], shown))});
        }
        attached.fields.push_back({text("Attached at"), root, true});
        break;
    }
    case MatchResult::Migrated:
        attached.line =
            AttachedLine{Attached::Migrated, text("The device differs from the one recorded"), roots_of(*attachment)};
        attached.notice = migrated_notice(text, *attachment, status.archive.status == "RETIRED");
        // Relocate to this carrier... beside the notice, where one root holds
        // the filesystem; never for a retired archive, which takes no new
        // revisions.
        if (attachment->places.size() == 1 && status.archive.status != "RETIRED") {
            attached.relocate_root = path_to_utf8(attachment->places.front().root);
        }
        attached.verify_tooltip = text("The filesystem of this archive is on another device than the one recorded "
                                       "(MIGRATED): FULL verify does not read it there.");
        attached.fields.push_back({text("Attached at"),
            text.format("not as recorded: the filesystem is on another device ({})", roots_listed(text, *attachment))});
        break;
    case MatchResult::Ambiguous:
        attached.line =
            AttachedLine{Attached::Ambiguous, text("Several volumes look like this archive"), roots_of(*attachment)};
        attached.notice = text.format("More than one attached volume looks like this archive ({}), as a device and a "
                                      "sector-by-sector clone of it would: a verification by its UUID cannot tell "
                                      "which one to read. Choose \"Verify a folder or volume...\" in the menu beside "
                                      "\"FULL verify\", then choose the root of the one to verify, one of those above.",
            roots_listed(text, *attachment));
        attached.verify_tooltip = text("More than one attached volume looks like this archive: choose the one to "
                                       "verify with \"Verify a folder or volume...\" in the menu.");
        attached.fields.push_back({text("Attached at"), roots_of(*attachment), true});
        break;
    default:
        attached.line = AttachedLine{Attached::Nowhere, text("Not attached"), {}};
        attached.fields.push_back({text("Attached at"), text("not attached")});
        // A retired archive is not expected to be verified (its banner says
        // so): nothing asks for its device.
        if (status.archive.status == "RETIRED") {
            break;
        }
        attached.notice = not_attached_notice(text, status, shown);
        attached.verify_tooltip = text("This archive was not attached when the attached volumes were read last. FULL "
                                       "verify looks for it again.");
        break;
    }
    return attached;
}

Pill health_pill(const app::Text& text, const std::optional<DeviceHealth>& health)
{
    const auto told = health.value_or(DeviceHealth{});
    Pill pill{capitalised(app::health_words(text, told)), Tone::Neutral};
    switch (told.status) {
    case HealthStatus::Healthy:
        pill.tone = Tone::Success;
        break;
    case HealthStatus::Warning:
        pill.tone = Tone::Warning;
        break;
    case HealthStatus::Unhealthy:
        pill.tone = Tone::Error;
        break;
    case HealthStatus::Unknown:
        break;
    }
    if (health_alerts(told) && pill.tone != Tone::Error) {
        pill.tone = Tone::Warning;
    }
    return pill;
}

std::vector<DeviceCard> device_cards(const app::Text& text, const AttachedRead& read,
    const std::vector<ArchiveName>& names, IdentityShown shown, const std::vector<std::filesystem::path>& registering)
{
    // The sample list is sample data itself, shown as it is.
    const auto identity = read.sample ? IdentityShown::Recorded : shown;
    const auto volume_line = [&](std::size_t index) {
        const auto& attached = read.volumes[index];
        const auto volume = displayed_identity(attached.volume, identity);
        VolumeLine line;
        line.volume = index;
        line.title = volume_title(text, volume);
        std::vector<std::string> parts{
            attached.volume.mount_point.empty() ? text("no mount point") : path_to_utf8(attached.volume.mount_point)};
        if (volume.capacity_bytes > 0) {
            parts.push_back(format_bytes(volume.capacity_bytes));
        }
        if (attached.used_bytes) {
            parts.push_back(text.format("{} used", format_bytes(*attached.used_bytes)));
        }
        if (volume.read_only) {
            parts.push_back(text("read-only"));
        }
        line.subtitle = joined(text, parts);
        const auto& roots = index < read.on_volumes.size() ? read.on_volumes[index] : std::vector<AttachedRoot>{};
        for (const auto& root : roots) {
            const auto label = label_of(names, root.archive_uuid);
            const bool migrated = root.match == MatchResult::Migrated;
            line.archives.push_back({root.archive_uuid,
                migrated ? text.format("The filesystem belongs to {}, but the device differs from the one recorded "
                                       "(MIGRATED)",
                               label)
                         : label,
                path_to_utf8(root.root), migrated});
        }
        // Registered as a whole only where nothing lies on it that matching
        // can tell, as Register Archive judges it again; while a registration
        // works on it or on a folder in it, that is said instead.
        if (read.repository && !read.unread && roots.empty() && !attached.volume.mount_point.empty()) {
            const auto working = std::ranges::find_if(registering, [&](const std::filesystem::path& root) {
                return path_within(attached.volume.mount_point, root);
            });
            if (working != registering.end()) {
                line.registering = text.format("Being registered: {}", path_to_utf8(*working));
            } else {
                line.register_root = path_to_utf8(attached.volume.mount_point);
            }
        }
        return line;
    };
    const auto by_mount_point = [&](std::vector<std::size_t> volumes) {
        std::ranges::stable_sort(volumes, [&](std::size_t left, std::size_t right) {
            return listed_before(read.volumes[left].volume.mount_point, read.volumes[right].volume.mount_point);
        });
        return volumes;
    };

    std::vector<std::pair<std::filesystem::path, DeviceCard>> cards;
    for (const auto& attached : read.devices) {
        const auto device = identity == IdentityShown::Sample
                                ? displayed_devices({attached.device}, identity).front()
                                : attached.device;
        DeviceCard card;
        card.title = device.model.empty() ? text("Device") : device.model;
        card.bus = device.bus;
        std::vector<std::string> parts{bus_words(device.bus)};
        if (device.capacity_bytes > 0) {
            parts.push_back(format_bytes(device.capacity_bytes));
        }
        card.subtitle = joined(text, parts);
        card.health = health_pill(text, device.health);
        // As the device reports it (decision 0044 asks nothing more of it).
        card.fields.push_back({text("Serial"), device.serial.empty() ? text("not reported") : device.serial});
        if (!device.wwn.empty()) {
            card.fields.push_back({text("WWN"), device.wwn});
        }
        const auto volumes = by_mount_point(attached.volumes);
        for (const auto index : volumes) {
            card.volumes.push_back(volume_line(index));
        }
        cards.emplace_back(volumes.empty() ? std::filesystem::path{} : read.volumes[volumes.front()].volume.mount_point,
            std::move(card));
    }
    std::ranges::stable_sort(cards, [](const auto& left, const auto& right) {
        return listed_before(left.first, right.first);
    });
    std::vector<DeviceCard> shown_cards;
    for (auto& card : cards) {
        shown_cards.push_back(std::move(card.second));
    }
    // A volume whose devices the system does not name.
    std::vector<std::size_t> unowned;
    for (std::size_t index = 0; index < read.volumes.size(); ++index) {
        if (read.volumes[index].volume.devices.empty()) {
            unowned.push_back(index);
        }
    }
    if (!unowned.empty()) {
        DeviceCard card;
        card.title = text("Volumes whose device is not known");
        for (const auto index : by_mount_point(unowned)) {
            card.volumes.push_back(volume_line(index));
        }
        shown_cards.push_back(std::move(card));
    }
    return shown_cards;
}

std::string devices_status(const app::Text& text, const AttachedRead& read)
{
    if (!read.devices.empty()) {
        return read.devices.size() == 1 ? text("1 device attached")
                                        : text.format("{} devices attached", read.devices.size());
    }
    // Volumes whose devices the system does not name, or nothing.
    if (read.volumes.empty()) {
        return text("No device is attached.");
    }
    return read.volumes.size() == 1 ? text("1 volume attached")
                                    : text.format("{} volumes attached", read.volumes.size());
}

std::string reading_words(const app::Text& text, std::chrono::steady_clock::duration reading_for)
{
    return reading_for >= devices_slow_after ? text("Still reading: a device may not be responding")
                                             : text("Reading the attached devices...");
}

bool read_due(std::chrono::steady_clock::duration interval, std::chrono::steady_clock::duration took,
    std::chrono::steady_clock::time_point started, std::chrono::steady_clock::time_point now)
{
    const auto spacing = 10 * took;
    return spacing <= interval || now - started >= spacing;
}

} // namespace va::gui
