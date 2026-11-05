#pragma once
// The verification report of one recorded run (decision 0048): one
// self-contained HTML file that says what the run's result page says, in the
// same words, from the same records (result_page, run_result.h), with the
// archive's runs and the state of the history chain when the file was made,
// and this run's hash in that chain, so that a reader can match it against
// the repository later. The window's Save report... and
// `veriarchive-cli report` make the same file.
//
// The file holds no script and loads nothing (it opens offline and can be
// mailed): UTF-8, its language set, inline CSS for the screen in a light and
// a dark style and for clean A4 printing, every value escaped, monospace for
// paths, uuids, hashes and serials, a result said in words beside its colour,
// the read speed chart as inline SVG. The same records and the same time of
// making give the same bytes. Only a run that reached a result has one: an
// INCOMPLETE run makes no PASS/FAIL claim (§A6). The repository stays the
// record: the file is not signed (tamper evidence is not part of this
// version).
//
// Making one reads a run's records once and writes one file, which lists
// every item of the run: it takes no progress sink or cancel token (rule 7),
// as the retirement preview takes none (decision 0048, point 5).
//
// Plain C++ without GTK, so the unit tests can check it.
#include "app/archive_roots.h"
#include "app/run_result.h"
#include "app/text.h"
#include "common/error.h"
#include "common/types.h"
#include "core/repository/archive_store.h"

#include <expected>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace va::app {

// When, on which computer and by which version of the program a report was
// made: its footer.
struct ReportMade {
    Timestamp at;           // UTC (common/time.h), shown in local time with its offset
    std::string computer;   // the computer's name, as the views show it
    std::string version;    // the program's version
};

// What a report of a run says: the run's records (RecordedRun, its archive
// named as the views name it), what a check of the history chain found as
// the report was made (as `repo verify-history` and History check it, or why
// it could not be checked), in `runs` the first run of a broken chain where
// the chain is broken (else nothing), by which the report names it, with its
// archive as `labels` names it (display_label), and when and where the
// report was made.
struct ReportFacts {
    RecordedRun recorded;
    std::expected<HistoryCheck, IoError> chain = HistoryCheck{};
    std::vector<VerifyRunRecord> runs;
    std::map<Uuid, std::string> labels;
    ReportMade made;
};

// Reads from `store` what a report of the run `run_id` says, but when and
// where it is made: the run's records (read_recorded_run), its archive and
// every other named as the views name them (display_label), the chain checked
// now (ArchiveStore::check_history with `repository_uuid`; a check that cannot
// be made is what the report says of the chain), and the first run of a
// broken chain, where it is broken. The volume label of the archive's current carrier is left to the
// caller, who reads that carrier's manifest where it reads one. Nothing where
// no run is recorded under `run_id`.
std::expected<std::optional<ReportFacts>, IoError> read_report_facts(
    ArchiveStore& store, const std::string& repository_uuid, const std::string& run_id);

// The verification report of the run of `facts`, in the language of `text`,
// its devices, volume and computers shown as `shown` says (the run's result
// page says them so too); refused, with no_report_of_incomplete (run_result.h),
// for an INCOMPLETE run.
std::expected<std::string, std::string> run_report_html(
    const Text& text, const ReportFacts& facts, const ShownIdentity& shown = {});

// The name proposed for the report of `recorded`'s run, in the language of
// `text`: its archive's label, "verification report", the local date the run
// ended on and its result, "ARCH-0001 verification report 2026-11-04
// PASSED.html" (ARCH-0001 驗證報告 2026-11-04 通過.html), with every
// character that a file name cannot hold on Windows or Linux (\ / : * ? " <
// > | and control characters) as '_', and no space or dot before ".html".
std::string report_file_name(const Text& text, const RecordedRun& recorded);

// Why a report is not written into `folder`: it lies in, or is, the root of
// an attached archive (`roots`, the window's latest read of the attached
// volumes), as their paths are written (path_within), where the file would be
// an entry that the next FULL verification of that archive reports as added,
// failing it. Nothing where it does not.
std::optional<std::string> report_refusal(
    const Text& text, const std::filesystem::path& folder, const std::vector<AttachedArchiveRoot>& roots);

// The same words for `holding`, a root that holds `folder` as
// app::roots_holding tells it, which may be as the file system names the two
// (through a symbolic link, a junction or a drive letter made with subst)
// where their paths as written do not show it.
std::string report_refusal_in_root(
    const Text& text, const std::filesystem::path& folder, const AttachedArchiveRoot& holding);

// `words` as HTML text or an attribute's value: & < > " ' as references, an
// invalid UTF-8 sequence and a control character (but tab and line feed) as
// U+FFFD, which a page cannot show otherwise.
std::string html_escaped(std::string_view words);

} // namespace va::app
