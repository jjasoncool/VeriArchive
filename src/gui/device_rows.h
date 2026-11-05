#pragma once
// What Devices and Archives say of the devices and volumes attached now
// (decision 0046), from the latest read of them (devices_poll.cpp): each
// device with the health it reports of itself (decision 0044) and its serial
// as it reports it, the volumes on it, and on each volume the archives whose
// roots lie there, told by identity matching alone (where_attached); in
// Archives, where each archive is attached now, or that it is not. No
// platform tells of a device attached or removed, so all of it is as the
// latest read found it, and the operations still decide for themselves: a
// verification looks for its archive again, and refuses as it refuses.
//
// Plain C++ without GTK, so the unit tests can check it.
#include "app/attached_devices.h"
#include "app/text.h"
#include "core/engine/archive_status.h"
#include "core/engine/attached.h"
#include "gui/archive_rows.h"
#include "gui/device_reads.h"
#include "gui/displayed_identity.h"
#include "gui/history_rows.h"
#include "gui/register_choices.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace va::gui {

// What one read of the attached devices found.
struct AttachedRead {
    // The volumes attached now, with the space in use on each where it could
    // be read, and the devices behind them, each with the health it reported,
    // asked once (app::attached_devices): what Devices lists. In the
    // screenshot mode the fixed sample list (sample_volumes), never this
    // computer's devices, and after it each volume of the sample archives'
    // folders that holds them, as the sample identity alone (sample_held).
    std::vector<AttachedVolume> volumes;
    std::vector<app::AttachedDevice> devices;
    // The roots of archives on each of `volumes`, in their order, MIGRATED
    // ones included (where_attached).
    std::vector<std::vector<AttachedRoot>> on_volumes;
    // Where each archive whose latest manifest can be used is attached now,
    // and the volumes that their places index (AttachedRoot::volume). In
    // normal use those of `volumes`; in the screenshot mode the volumes of the
    // sample archives' folders, the only ones of this computer then read, of
    // which the views show nothing but the archives' roots, which are the
    // sample archives' folders, and the sample identity.
    std::vector<ArchiveAttachment> archives;
    std::vector<VolumeInfo> archive_volumes;
    // A repository is open, whose archives were matched.
    bool repository = false;
    // Why its archives could not be matched: its records could not be read
    // (an engine sentence).
    std::optional<std::string> unread;
    bool sample = false;  // the screenshot mode's sample list
};

// Puts one read together: where_attached of `candidates` (the archives whose
// latest manifest can be used, load_candidate_pool) on the volumes of
// `attached`, whose space in use is `used` (in their order). In the screenshot
// mode (`sample`), `attached` is the sample list, and where the archives are
// is told on `own`, the volumes of the sample archives' folders, instead; each
// of those that holds an archive is listed after the sample list as the
// sample identity alone (displayed_identity: SAMPLE-VOLUME on SAMPLE-DEVICE,
// serial SAMPLE0001, 1.0 TiB), with a sample health (healthy) in place of its
// device's, which is not asked, and no space in use, which is not read: so
// Devices shows the sample archives on a volume, as Archives does, and
// nothing of the volume but its mount point, which their roots hold anyway.
AttachedRead attached_read(app::AttachedDevices attached, const std::vector<std::optional<std::uint64_t>>& used,
    std::span<const ArchiveCandidate> candidates, bool repository, std::optional<std::string> unread,
    bool sample = false, std::vector<VolumeInfo> own = {});

// Where an archive is attached now, as Archives says it.
enum class Attached {
    Here,       // MATCH or MATCH_WEAK on one volume: at its root there
    Nowhere,    // on no attached volume
    Migrated,   // its filesystem is on another device than recorded, on no volume as it is recorded
    Ambiguous,  // more than one volume looks like it (a device and a sector-by-sector clone of it)
};

// What Archives shows of where an archive is attached, from the latest read:
// its row's first line (`line`): its root, or that it is not attached, that
// its device differs from the one recorded, that more than one volume looks
// like it; the notice of its details, where it is not attached as recorded
// (with, for MIGRATED, the root the relocation dialog its notice opens
// starts with);
// the tooltip of FULL verify, which stays offered (the read may be old, and
// the verification looks for the archive itself); and, in the group Device of
// its details, where it is attached. Nothing where no read is at hand (`read`
// null) or the read did not match the archive (its latest manifest cannot be
// used, or it was registered since). A retired archive not attached gets no
// notice nor tooltip: it is not expected to be verified.
struct AttachedLine {
    Attached state = Attached::Here;
    std::string words;
    std::string tooltip;  // the roots in full, one on each line
    bool operator==(const AttachedLine&) const = default;
};
struct ArchiveAttached {
    std::optional<AttachedLine> line;
    std::optional<std::string> notice;
    std::optional<std::string> verify_tooltip;
    std::vector<Field> fields;
    // MIGRATED at one root, of an archive that is not retired: the root that
    // Relocate to this carrier... beside the notice opens the relocation
    // dialog with (decision 0046).
    std::optional<std::string> relocate_root;
    // MIGRATED behind another enclosure (enclosure_case, decision 0047): the
    // notice says that FULL verify asks first whether it is the same device,
    // and Confirm and FULL verify... beside it does so; no relocation is
    // offered beside it (the menu of the details still offers one). The
    // enclosure's serial as recorded, which its field "Enclosure serial"
    // shows beside the one seen now.
    bool confirm_first = false;
    std::string enclosure_recorded;
    bool operator==(const ArchiveAttached&) const = default;
};

ArchiveAttached archive_attached(
    const app::Text& text, const AttachedRead* read, const ArchiveStatus& status, IdentityShown shown);

// The group Device of an archive's details: its carrier as recorded
// (`recorded`, device_fields), then where it is attached now (`attached`'s
// fields). Behind another enclosure, the serial recorded is the enclosure's,
// which "Enclosure serial" shows beside the one seen now, so the recorded
// "Serial" (or "WWN") of that value is left out.
std::vector<Field> details_device_fields(
    const app::Text& text, std::vector<Field> recorded, const ArchiveAttached& attached);

// The archive's attachment in `read`, or none where the read did not match it.
const ArchiveAttachment* attachment_of(const AttachedRead& read, const Uuid& archive);

// The archive's device seen behind another enclosure (decision 0047): its
// filesystem found at one root, on another device than recorded (MIGRATED),
// where that is a changed enclosure (enclosure_changed: both behind a USB
// bridge, or the one seen now behind an enclosure that hides its identity),
// of an archive that is not retired (a retired archive takes no new
// revisions). Its FULL verify by its uuid goes on only with a carrier
// confirmation, which the window asks for first (enclosure_flow.h). `where`:
// the volume's mount point, as the question names it; `now`'s one device is
// the one whose own serial a read reads. enclosure_case_of judges an
// attachment found on `volumes` (where_attached); enclosure_case the latest
// read, and never in the screenshot mode (`shown` Sample), whose archives'
// carriers are this computer's own volumes, whose serials no picture shows.
struct EnclosureCase {
    std::string where;
    std::filesystem::path root;
    VolumeInfo recorded;
    VolumeInfo now;
};
std::optional<EnclosureCase> enclosure_case_of(
    const ArchiveAttachment& attachment, std::span<const VolumeInfo> volumes, const VolumeInfo& recorded, bool retired);
std::optional<EnclosureCase> enclosure_case(const AttachedRead* read, const ArchiveStatus& status, IdentityShown shown);
// The archives among `statuses` for which enclosure_case holds on `read`, as
// Devices names them under their volume (device_cards).
std::vector<Uuid> enclosure_cases(
    const AttachedRead& read, const std::vector<ArchiveStatus>& statuses, IdentityShown shown);

// A device's health as a pill (decision 0044): "Healthy" (健康, Success),
// "Warning (predicting failure)" (警告（預測即將故障）, Warning), "Unhealthy
// (...)" (Error), "Cannot be read" (無法讀取, Neutral) where it reports
// nothing or was not asked; a condition that alerts makes it a Warning at
// least. Capitalised as the other pills are.
Pill health_pill(const app::Text& text, const std::optional<DeviceHealth>& health);

// An archive on a volume of Devices: its label and root, which lead to it in
// Archives; for one whose filesystem is on this volume but on another device
// than recorded (MIGRATED), its title says so.
struct ArchiveOnVolume {
    Uuid archive_uuid;
    std::string title;   // its label; or "The filesystem belongs to ARCH-0002, but the device differs ..."
    std::string root;
    bool migrated = false;
    bool operator==(const ArchiveOnVolume&) const = default;
};

// A volume of a device in Devices: its label, where it is mounted (or its
// GUID path), its capacity and the space in use, read-only; the archives on
// it; and, for a mounted volume that holds no archive's root while a
// repository is open whose archives were matched, the root that Register this
// volume opens Register Archive with, or, while a registration works on the
// volume or a folder in it, that root (`registering`) in its place.
struct VolumeLine {
    std::size_t volume = 0;  // its index in AttachedRead::volumes
    std::string title;
    std::string subtitle;
    std::vector<ArchiveOnVolume> archives;
    std::string register_root;  // empty: not offered
    std::string registering;    // "Being registered: <root>"; empty: none is
    bool operator==(const VolumeLine&) const = default;
};

// The device's own serial on a device whose enclosure hides it (decision
// 0047): a read of it kept (KeptDeviceRead), "SERIAL (read)" with whose
// confirmed device it is ("The device of ARCH-0002 (confirmed)"), or that it
// is no archive's; otherwise that the enclosure does not pass it on, with Read
// the device's own serial... and the note of what Windows asks where this
// computer can read it (offers_device_read), or that it is being read.
struct OwnSerialLine {
    std::string device_ref;
    std::string value;
    bool offered = false;
    bool reading = false;
    std::string note;
    bool operator==(const OwnSerialLine&) const = default;
};

// A device in Devices: its model, its bus (bus_words) and capacity, its
// health, its serial as it reports it (or its WWN; "Enclosure serial" where
// its enclosure hides the device's own, decision 0047, then the device's own
// serial in `own`), and its volumes; volumes whose device cannot be told come
// last, in a card of their own without a health. `bus` as the device reports
// it ("usb", ...), for its icon.
struct DeviceCard {
    std::string title;
    std::string subtitle;
    std::string bus;
    std::optional<Pill> health;
    std::vector<Field> fields;
    std::optional<OwnSerialLine> own;
    std::vector<VolumeLine> volumes;
    bool operator==(const DeviceCard&) const = default;
};

// The cards of the devices in `read`: by where their volumes are mounted
// (drive letters and folders first, GUID paths last), as Register Archive
// lists the volumes. `names`: the archives' labels as Archives names them (an
// archive not among them is named by the start of its uuid). `shown`: the
// identity the views show; the screenshot mode's sample list is shown as it
// is, being sample data itself. `registering`: the roots that registrations
// are working on (this window's jobs, and unfinished registrations that work
// as the latest read of them found), each named on its volume in place of
// Register this volume. `reads`: the reads of the devices' own serials kept
// (decision 0047); `reader`: this computer has a device identity reader;
// `reading`: the device whose own serial is being read, if one is;
// `enclosure_changed`: the archives found behind a changed enclosure
// (enclosure_cases), whose MIGRATED root says that the enclosure differs, as
// Archives says it, where any other says that the device differs.
std::vector<DeviceCard> device_cards(const app::Text& text, const AttachedRead& read,
    const std::vector<ArchiveName>& names, IdentityShown shown,
    const std::vector<std::filesystem::path>& registering = {}, const KeptDeviceReads& reads = {},
    bool reader = false, const std::string& reading = {}, const std::vector<Uuid>& enclosure_changed = {});

// The head of Devices: "3 devices attached", or that none is.
std::string devices_status(const app::Text& text, const AttachedRead& read);

// While a read is in flight, for `reading_for` so far: that the attached
// devices are being read; after devices_slow_after, that a device may not be
// answering (a dying disk can hold the system's answer up for long).
inline constexpr std::chrono::seconds devices_slow_after{15};
std::string reading_words(const app::Text& text, std::chrono::steady_clock::duration reading_for);

// How often the attached devices are read while Archives or Devices is shown.
inline constexpr std::chrono::seconds devices_read_interval{30};
// Decision 0047: the reads of the devices' own serials kept are dropped where
// a read of the attached devices began more than this later than the reads'
// spacing (read_due) lets it begin after the one before was shown: the
// devices were not watched meanwhile (the computer slept, say), and another
// device may have been put into an enclosure unseen.
inline constexpr std::chrono::seconds devices_watch_pause{10};

// Whether a timer that ticks `interval` apart reads again: every tick, unless
// the last read took long (`took`, begun at `started`); then once ten times
// as long has passed since it began, so that reading takes at most a tenth of
// the time. The unfinished operations are read so too.
bool read_due(std::chrono::steady_clock::duration interval, std::chrono::steady_clock::duration took,
    std::chrono::steady_clock::time_point started, std::chrono::steady_clock::time_point now);

} // namespace va::gui
