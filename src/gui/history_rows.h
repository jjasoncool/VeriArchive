#pragma once
// History (decision 0046): the recorded runs of every archive, newest first,
// or of the archive the filter names, each a row that leads to its result
// page; and the check of the hash chain that protects them (R-18, R-90),
// said in the page. Every column of the history of §B17 stays in reach: a
// row gives when the run ended, its result with its mode, what it read and
// matched, how long it took, how fast it read, and its notes (what else the
// run is, its device alerts); its result page gives its revision, its
// failures and its run.
//
// Plain C++ without GTK, so the unit tests can check it.
#include "app/text.h"
#include "common/error.h"
#include "core/engine/archive_status.h"
#include "core/repository/archive_store.h"
#include "gui/archive_rows.h"
#include "gui/displayed_identity.h"

#include <cstddef>
#include <expected>
#include <optional>
#include <string>
#include <vector>

namespace va::gui {

// An archive as History names it: in its rows by its label (display_label),
// in the filter by its label and the volume label of its current carrier.
struct ArchiveName {
    Uuid archive_uuid;
    std::string label;
    std::string volume_label;  // as the views show it (displayed_identity); empty without a manifest to read it
    bool operator==(const ArchiveName&) const = default;
};

std::vector<ArchiveName> archive_names(const std::vector<ArchiveStatus>& statuses, IdentityShown shown);

// An item of the filter: "ARCH-0001 · SAMPLE-VOLUME", or the label alone.
std::string filter_words(const app::Text& text, const ArchiveName& name);

// An archive as a list to choose it from names it (the comparison's A and B,
// Register Archive's "Copy of which archive"): as the filter does, with the
// start of its uuid, "ARCH-0001 · SAMPLE-VOLUME · 81dd9114". Labels are not
// unique (GLOSSARY Label), and a sector-by-sector clone keeps its volume
// label: two archives never read the same there.
std::string choice_words(const app::Text& text, const ArchiveName& name);

// A run in History.
struct HistoryRow {
    std::string run_id;
    Uuid archive_uuid;
    Pill result;               // the result with its mode ("FULL VERIFY PASSED"), in its tone
    std::string title;         // the archive and when the run ended: "ARCH-0001 · 2026-11-04 10:12:33 (UTC+8)"
    // How far an INCOMPLETE run came, where it can be told; what it read and
    // matched; how long it took; how fast it read (run_read_speed).
    std::string meta;
    // What else the run is (app::history_note), Neutral, and its device
    // alerts (app::alert_marks), Error.
    std::vector<Pill> marks;
};

// The runs of `runs` (every archive's, oldest first, as all_verify_runs
// gives them), newest first; only those of `only` where it names an archive.
// An archive that `names` does not hold is named by the start of its uuid.
// Of these, `limit` from the `from`th on (0: the newest), so that a long
// history is worded a page at a time.
std::vector<HistoryRow> history_rows(const app::Text& text, const std::vector<VerifyRunRecord>& runs,
    const std::vector<ArchiveName>& names, const std::optional<Uuid>& only, std::size_t from = 0,
    std::size_t limit = static_cast<std::size_t>(-1));

// How many runs history_rows lists without `from` and `limit`.
std::size_t history_count(const std::vector<VerifyRunRecord>& runs, const std::optional<Uuid>& only);

// What History says when it lists no run: of every archive, that none is
// recorded yet; of one archive, that it was never verified.
std::string no_runs_words(const app::Text& text, const std::optional<Uuid>& only);

// What the check of the chain found, as the page says it: the history of
// every archive intact with the number of runs (Success), which the chain of
// the whole repository holds whatever the filter lists; BROKEN at the first
// run that does not link, named by its id and, where `runs` holds it, by its
// archive (as `names` names it) and when it ended (Error); or why it could
// not be checked (Error); and, under it, what such a check cannot detect
// (app::history_chain_limits), where it was checked.
struct ChainShown {
    Tone tone = Tone::Neutral;
    std::string result;
    std::string limits;
};

ChainShown chain_words(const app::Text& text, const std::expected<HistoryCheck, IoError>& check,
    const std::vector<VerifyRunRecord>& runs, const std::vector<ArchiveName>& names);

// Whether what a check of the chain found still holds for the history as now
// read (`runs`: every run of every archive): one that found it intact checked
// as many runs as the history holds; once another run was recorded, it says
// nothing of that run, and History drops it. One that found it BROKEN, or
// could not check it, holds until it is checked again: a run recorded since
// mends nothing.
bool chain_check_holds(const std::expected<HistoryCheck, IoError>& check, std::size_t runs);

} // namespace va::gui
