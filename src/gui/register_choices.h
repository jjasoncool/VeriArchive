#pragma once
// What Register Archive says and offers (decision 0046), a dialog of three
// pages: the attached volumes to choose from, beside a folder chosen or typed
// (Choose the source); the options that apply to the folder or volume chosen
// (Options); what the registration will do (Confirm). Which options apply is
// judged as the registration judges the root (register_engine's
// check_existing_archives): the archives whose roots lie on its volume, told
// by identity matching (where_attached), those of which it is an earlier
// carrier (EarlierCarrier), and the unfinished registration of that very
// root. The media identity is the one the views show (displayed_identity):
// the sample one in the screenshot mode, whose list of volumes is itself
// sample data (sample_volumes). Also the report of the registration's job.
//
// Plain C++ without GTK, so the unit tests can check it.
#include "app/text.h"
#include "core/engine/register_engine.h"
#include "core/identity/matcher.h"
#include "gui/archive_rows.h"
#include "gui/device_reads.h"
#include "gui/displayed_identity.h"
#include "gui/history_rows.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace va::gui {

// An attached volume as the first page lists it: its identity, and the space
// in use on it where it could be read.
struct AttachedVolume {
    VolumeInfo volume;
    std::optional<std::uint64_t> used_bytes;
};

// An unfinished registration (ArchiveStore::pending_registrations): its
// archive's label and the root its plan names (UTF-8).
struct PendingRoot {
    Uuid archive_uuid;
    std::string label;
    std::string root;
    bool operator==(const PendingRoot&) const = default;
};

// A row of the attached volumes. A volume that holds the root of an archive,
// or of an unfinished registration, in a folder cannot be registered as a
// whole: its root would overlap theirs (decision 0001); a folder in it can,
// unless one of them is the volume's root itself. A volume whose only roots
// are its own root can be chosen: Options then offers what applies there (a
// new revision, another archive, the resume of the registration).
struct VolumeRow {
    std::size_t volume = 0;     // its index in the volumes given
    std::string root;           // what choosing it registers: its mount point or GUID path (UTF-8)
    std::string title;          // its volume label, or that it has none
    std::string where;          // its mount point or GUID path
    std::string details;        // its devices' model and bus, its capacity, the space in use, read-only
    std::string note;           // the roots it holds, and why it is not chosen as a whole; empty without
    bool selectable = true;     // the whole volume can be chosen
    // "Choose a folder in E:\...", where a folder in it can be chosen, or, for
    // a volume known by its GUID path alone, which no button holds whole,
    // "Choose a folder in it..."; empty otherwise. `folder_tooltip` names the
    // volume in full.
    std::string folder_button;
    std::string folder_tooltip;
    // A relocation's new root (VolumeUse::Relocation): where, in a folder of
    // this volume, the filesystem of the archive relocated is on another
    // device than recorded (MIGRATED: its device in another enclosure, or a
    // copy made sector by sector), which `own_button` ("Use as the new root",
    // `own_tooltip` naming the root) makes the new root; empty otherwise.
    std::string own_root;
    std::string own_button;
    std::string own_tooltip;
};

// The volumes' rows, those with a drive letter or folder path first, by it,
// then those known by their GUID path alone (decision 0029, point 3). The
// archives on each are told by where_attached; those of the unfinished
// registrations by their roots' text (path_within). `names`: the archives'
// labels (an archive not among them is named by the start of its uuid).
// `shown`: Recorded for the screenshot mode's sample list, which is sample
// data itself. `use`: what the volume is chosen for, which the note of a
// volume not chosen whole names: Register Archive's source, or the new root
// of a relocation (decision 0046) of the archive `relocated`. A relocation
// never takes a root that overlaps another archive's, nor an unfinished
// registration's (decision 0001): a volume is chosen whole there only where
// nothing else lies on it, the archive's own filesystem at the volume's root
// aside; a volume whose root is another's offers no folder either. The
// archive's own roots (its filesystem on another device than recorded, or its
// carrier as recorded) never stand in the way: a root of it in a folder,
// MIGRATED, is offered as the new root (own_root).
enum class VolumeUse { Registration, Relocation };
std::vector<VolumeRow> volume_rows(const app::Text& text, const std::vector<AttachedVolume>& volumes,
    std::span<const ArchiveCandidate> candidates, const std::vector<PendingRoot>& pending,
    const std::vector<ArchiveName>& names, IdentityShown shown, VolumeUse use = VolumeUse::Registration,
    const Uuid& relocated = {});

// The folder or volume chosen, as the read of it found it: the root as
// app::typed_root made it, its volume (IMediaEnumerator::volume_for_path),
// its root subpath on that volume and the space in use on the volume.
struct ChosenSource {
    std::filesystem::path root;
    VolumeInfo volume;
    std::string root_subpath;
    std::optional<std::uint64_t> used_bytes;
};

// An archive of which the source's volume is an earlier carrier (decision
// 0004), as matching found it (match_recorded, which reads the manifests of
// its earlier revisions): the carrier of one of its earlier revisions, at the
// archive's root subpath, while its latest revision is on another volume.
// The register engine refuses such a root unless it is registered apart from
// that archive (check_existing_archives: --force-new-archive), and refuses a
// root that overlaps it.
struct EarlierCarrier {
    Uuid archive_uuid;
    std::string root_subpath;
};

// Which options apply to the source chosen, as a registration of it would
// judge them.
struct SourceOptions {
    // Archives at this very root on this volume (MATCH, MATCH_WEAK): one of
    // them takes a new revision (R-97), or the root is registered as another
    // archive (--force-new-archive).
    std::vector<Uuid> registered_here;
    bool new_revision = false;  // offered: one archive is here, and it is not retired
    bool retired = false;       // the one archive here is retired (decision 0003)
    // Archives at this very root whose filesystem this volume is, on another
    // device (MIGRATED: a sector-by-sector clone, or a new enclosure): the
    // root is registered only as another archive.
    std::vector<Uuid> clones_here;
    // Archives at this very root of which this volume is an earlier carrier
    // (PREVIOUS_CARRIER, decision 0004), such as the disk left behind by a
    // relocation: the root is registered only as another archive.
    std::vector<Uuid> earlier_carrier_of;
    // An unfinished registration of this very root (decision 0025): the root
    // takes no other registration until it is resumed or abandoned.
    std::optional<PendingRoot> unfinished;
    // Why no registration of the root can start: it contains, or lies in,
    // the root of an archive on this volume (decision 0001).
    std::optional<std::string> refusal;
    // The label that a new archive takes when none is typed
    // (ArchiveStore::peek_next_label), where it could be read.
    std::optional<std::string> next_label;
};

// `earlier`: the archives that matching found this volume an earlier
// carrier of (EarlierCarrier), among the candidates whose roots overlap the
// source's.
SourceOptions source_options(const app::Text& text, const ChosenSource& source,
    std::span<const ArchiveCandidate> candidates, const std::vector<PendingRoot>& pending,
    const std::vector<ArchiveName>& names, const std::vector<EarlierCarrier>& earlier = {});

// What the operator chose on the Options page.
struct OptionsChosen {
    std::string label;                // as typed; empty: the repository's next label
    bool new_revision = false;
    bool another_archive = false;     // --force-new-archive
    bool media_copy = false;          // R-89, off by default
    std::optional<Uuid> copy_of;      // decision 0005
    // Decision 0047: read the device's own serial behind its enclosure as
    // the registration starts (device_serial_offer), with administrator
    // permission; off by default, the operator chooses.
    bool record_device_serial = false;
};

// Register Archive's "Record the device's own serial" (decision 0047): for a
// source whose one device is behind an enclosure that hides the device's own
// serial (offers_device_read: this computer can read it), and for a
// registration that is no resume, which goes on with the read its plan keeps.
// `kept`: a read of that device kept from before (Devices, or the enclosure
// question of a FULL verify), still of that device (kept_read_of): recorded
// with the registration with no second question of Windows, in place of the
// switch. Never in the screenshot mode (`shown` Sample), whose sources are
// this computer's folders shown as the sample identity.
struct DeviceSerialOffer {
    PhysicalDeviceInfo device;
    std::optional<DeviceIdentityRead> kept;
};
std::optional<DeviceSerialOffer> device_serial_offer(const ChosenSource& source, const SourceOptions& options,
    IdentityShown shown, bool reader, const KeptDeviceReads& reads);

// The lines under the switch: what recording the serial does, what Windows
// asks (app::device_identity_reader_note), and when.
std::string device_serial_note(const app::Text& text);

// The Confirm page's "Device's own serial", where it is offered: the serial
// kept, read; that it is read with administrator permission as the
// registration starts, and recorded; or that it is not recorded (the switch
// off, or a read that gave nothing: `unread`).
std::optional<Field> device_serial_field(
    const app::Text& text, const std::optional<DeviceSerialOffer>& offer, const OptionsChosen& chosen, bool unread);

// The line that the report of a registration given a read adds: the serial
// recorded, the archive's confirmed device from now on.
std::string recorded_device_serial_line(const app::Text& text, const DeviceIdentityRead& read);

// The rows of the Options page that apply, the lines under them, and why
// Next is not offered yet. Where the root has an unfinished registration, it
// is resumed (`resume`, a row without a switch: nothing else can start
// there), and only the media copy is chosen: the plan fixed the rest. A new
// revision keeps the archive's label and the copy it records.
struct OptionsShown {
    bool label = false;
    std::string label_note;           // what an empty label stands for
    bool new_revision = false;
    std::string new_revision_note;
    bool another_archive = false;
    std::string another_archive_note;
    bool resume = false;
    std::string resume_note;
    bool copy_of = false;
    std::optional<std::string> blocked;
};

OptionsShown options_shown(const app::Text& text, const SourceOptions& options, const OptionsChosen& chosen,
    const std::vector<ArchiveName>& names);

// The archives that "Copy of which archive" lists after "Not a copy": those
// at the root, whose filesystem it is, or of which it is an earlier carrier,
// first, then the others in the order given. And the one it starts with: the archive whose filesystem the
// root is on another device, a clone of which this is a copy; none otherwise.
std::vector<ArchiveName> copy_of_choices(const std::vector<ArchiveName>& names, const SourceOptions& options);
std::optional<Uuid> default_copy_of(const SourceOptions& options);

// The registration that Start registration runs, as the command line's
// `register <root>` with the flags the options stand for: --resume (and the
// media copy alone), --new-revision, --force-new-archive, --label,
// --copy-of, --write-media-copy; the readers as the config's override by
// serial says (R-111: the window has no --io). A new revision where another
// archive's filesystem, or an earlier carrier of one, is at the root too is
// `--new-revision --force-new-archive`: the engine refuses it otherwise.
RegisterRequest registration_request(const ChosenSource& source, const SourceOptions& options,
    const OptionsChosen& chosen, const std::map<std::string, bool>& rotational_override);

// What the Confirm page says the registration will do: the source, its
// volume, the label (the repository's next one, named where it was read,
// when none is typed), what it reads, what it registers, the media copy, and
// which archive it is a copy of.
std::vector<Field> registration_summary(const app::Text& text, const ChosenSource& source,
    const SourceOptions& options, const OptionsChosen& chosen, const std::vector<ArchiveName>& names,
    IdentityShown shown);

// The line under the source above the Options page: its volume's label (or
// that it has none) and its devices' model, as the views show them.
std::string source_volume_words(const app::Text& text, const ChosenSource& source, IdentityShown shown);

// Create a new revision... of an archive's details opens Register Archive for
// `archive`: the root it reads is where the attached volumes hold the archive
// now (where_attached: MATCH or MATCH_WEAK, on one volume), as drive letters
// follow the order in which devices are attached; otherwise `recorded`, the
// root its latest revision recorded, which Next then reads and finds the
// archive is not at (new_revision_refusal).
std::string new_revision_source(const std::vector<AttachedVolume>& volumes,
    std::span<const ArchiveCandidate> candidates, const Uuid& archive, const std::string& recorded);

// Why the source read does not take the new revision of `archive` that
// Create a new revision... asked for: the archive is not registered at it
// (another archive may be, at that drive letter now). Nothing where it is:
// Options then turns Create a new revision on where it applies, or says why
// it does not (retired, several archives at the root).
std::optional<std::string> new_revision_refusal(const app::Text& text, const SourceOptions& options,
    const Uuid& archive, const std::vector<ArchiveName>& names, const std::string& root);

// The report of a registration, which its job's details show: the result
// line, the device alerts below it (decision 0044), the counts, the
// manifest, the devices' health, the entries unreadable at registration, the
// mount points skipped, the warnings, a lost revision left out of the index
// (decision 0028, point 8), and the media copy or the reminder to back up
// the repository.
std::string registration_report(const app::Text& text, const RegisterOutcome& outcome);

// The toast once a registration ended: "Registered as ARCH-0004", "Registered
// revision 2 of ARCH-0001"; or, where it registered nothing, "Not registered:
// <root>", whose reason its report gives.
std::string registered_toast(const app::Text& text, const std::string& label, std::uint32_t revision);
std::string not_registered_toast(const app::Text& text, const std::string& root);

} // namespace va::gui
