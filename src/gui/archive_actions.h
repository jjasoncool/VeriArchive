#pragma once
// What the menu of an archive's details offers besides a comparison and a new
// revision (decision 0046): the relocation of the archive to a new carrier
// (decision 0004), the export of its manifest (R-87) and its retirement
// (decision 0003); which of them apply, and why not; the words of the
// relocation dialog and its summary; the folder that an export writes into;
// the question before a retirement; and the titles, reports and toasts of
// their jobs.
//
// Plain C++ without GTK, so the unit tests can check it.
#include "app/text.h"
#include "app/unfinished_operations.h"
#include "core/engine/archive_status.h"
#include "core/engine/retire_archive.h"
#include "core/engine/transfer.h"
#include "core/identity/matcher.h"
#include "gui/archive_rows.h"
#include "gui/displayed_identity.h"
#include "gui/history_rows.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace va::gui {

// The archive's unfinished relocation, as the latest read of the unfinished
// operations lists it (decision 0026): the operation, whether its row offers
// Resume (describe_operation: it stopped, and can finish), and the state its
// row says. An archive has one at a time.
struct ListedRelocation {
    app::UnfinishedOperation operation;
    bool resumable = false;
    std::string state;
};

std::optional<ListedRelocation> listed_relocation(const app::Text& text,
    const std::vector<app::OperationWatch>& watches, const Uuid& archive, app::OperationWatch::Clock::time_point now,
    std::chrono::sys_seconds wall_now);

// "Relocate to a new carrier...": offered for an archive whose latest
// manifest can be used, which the relocation compares the new carrier with,
// and that is not retired (a retired archive takes no new revisions). Where
// the archive has an unfinished relocation (`unfinished`), no second one
// begins: offered to resume it, where it stopped and can finish, at its root
// or another one where its new carrier is attached at another drive letter or
// mount point now; otherwise not, saying where it is listed.
Choice relocation_choice(const app::Text& text, const ArchiveStatus& status,
    const std::optional<ListedRelocation>& unfinished = std::nullopt);

// "Export manifest...": offered for an archive whose latest manifest can be
// used, the one an export copies.
Choice export_choice(const app::Text& text, const ArchiveStatus& status);

// "Retire archive...": offered for an archive that is not retired.
Choice retire_choice(const app::Text& text, const ArchiveStatus& status);

// ---- Relocation (decision 0004) ---------------------------------------------

// What the relocation dialog says relocation does: above the attached volumes,
// that VeriArchive copies nothing and the operator copies the archive to the
// new carrier first (relocation_intro); above its summary, that it reads the
// new carrier in full, accepts it only when every entry matches `revision`,
// the archive's latest, and then records the next revision with the new
// carrier, the archive keeping its UUID and its history (relocation_reading).
// In the dialog that resumes a stopped relocation, below its intro: where it
// stopped, and that a new root is given where the new carrier is attached at
// another drive letter or mount point now (relocation_resuming).
std::string relocation_intro(const app::Text& text);
std::string relocation_reading(const app::Text& text, std::uint32_t revision);
std::string relocation_resuming(const app::Text& text, const std::string& root);

// Why `root_subpath` on `volume` (the new root's, as volume_for_path and
// root_subpath_of told them) cannot be the new root of `archive`: it overlaps
// the root of another archive on that volume (decision 0001), as the
// relocation itself refuses it, the archives told by identity matching
// (where_attached) from `candidates`, named as `names` name them. Nothing
// where it overlaps none: the archive's own root never counts.
std::optional<std::string> relocation_root_refusal(const app::Text& text, const VolumeInfo& volume,
    const std::string& root_subpath, std::span<const ArchiveCandidate> candidates, const Uuid& archive,
    const std::vector<ArchiveName>& names);

// What the dialog's summary says the relocation of `status` to `root` (as
// typed or chosen; empty: none chosen yet) will do: the archive, the root its
// latest revision recorded, the new root, what it reads, and the revision it
// records, its carrier shown as the views show it (displayed_identity).
std::vector<Field> relocation_summary(
    const app::Text& text, const ArchiveStatus& status, const std::string& root, IdentityShown shown);

// The job's title: "Relocate ARCH-0002 to D:\Copy", or, resumed, "Resume the
// relocation of ARCH-0002 to D:\Copy".
std::string relocation_title(const app::Text& text, const std::string& label, const std::string& root, bool resume);

// The toast once a relocation ended: "ARCH-0002: relocated as revision 3";
// "Relocation of ARCH-0002: stopped" (it can be resumed); "Not relocated:
// ARCH-0002", whose reason its report gives.
std::string relocated_toast(const app::Text& text, const std::string& label, std::uint32_t revision);
std::string not_relocated_toast(const app::Text& text, const std::string& label, bool stopped);

// ---- Export (R-87, R-94) ----------------------------------------------------

// The folder, inside the one the operator chose, that an export of `status`
// writes into, so that the identity.json of a second archive never replaces
// the first's: the archive's label and the start of its UUID ("ARCH-0002
// 97487b4a"), each character that a file name cannot hold on Windows or
// Linux written as "_", without dots or spaces at its end.
std::string export_folder_name(const ArchiveStatus& status);

// An archive root where the attached volumes hold an archive now, as the
// latest read of them found it: the export never writes into one, where its
// files would be entries that the next FULL verification of that archive
// reports as added.
struct AttachedArchiveRoot {
    std::filesystem::path root;
    std::string label;
};

// Why the export does not write into `folder`: it lies in, or is, the root of
// an attached archive (path_within). Nothing where it does not.
std::optional<std::string> export_refusal(
    const app::Text& text, const std::filesystem::path& folder, const std::vector<AttachedArchiveRoot>& roots);

// The job's title, "Export the manifest of ARCH-0002"; its report, the two
// files written ("Manifest: ...", "Identity file: ...") and what a repository
// does with them (this one too, to put back a lost manifest); and the toasts
// once it ended.
std::string export_title(const app::Text& text, const std::string& label);
std::string export_report(
    const app::Text& text, const std::string& label, std::uint32_t revision, const ExportResult& written);
std::string exported_toast(const app::Text& text, const std::string& label);
std::string not_exported_toast(const app::Text& text, const std::string& label, bool stopped);

// ---- Retirement (decision 0003) ---------------------------------------------

// The question before an archive is retired, from what retirement_of read:
// what retiring does (no longer expected to be verified, never overdue, its
// history kept, still recognised when attached, no new revisions, and no way
// back); for a lost manifest, the explanation the command line gives
// (app::retirement_explanation, as one paragraph), that the commands it
// names are the command line's, and that there is no way back, with an
// explicit confirmation, without which Retire is not offered.
struct RetireQuestion {
    std::string heading;
    std::string body;
    bool confirm = false;
    std::string confirmation;  // the words of the confirmation's switch
    std::string retire;        // the destructive answer: Retire
};

RetireQuestion retire_question(const app::Text& text, const std::string& label, const Retirement& retirement);

// The toast once an archive was retired.
std::string retired_toast(const app::Text& text, const std::string& label);

} // namespace va::gui
