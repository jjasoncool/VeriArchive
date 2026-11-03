#pragma once
// The GUI's state and its seven views: the five of HANDOFF §B17 (Archive List,
// Register Archive, Verify Archive, Verification History, Settings), Compare
// Archives (decision 0020) and Jobs (decision 0045).
#include "app/adding_chunk_hashes.h"
#include "app/refusal_in_window.h"
#include "app/unfinished_operations.h"
#include "app/session.h"
#include "gui/displayed_identity.h"
#include "gui/gui_support.h"
#include "gui/jobs.h"

#include <chrono>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace va::gui {

// Unfinished registrations, verifications and relocations, wherever they run
// (the command line, this window), read from the repository every few seconds while the
// Archive List is shown (archive_list_view.cpp). Each read runs on a thread of
// its own, so a slow one never holds up the window. Kept across windows, so a
// language change keeps the speed and time left.
struct OperationsPoll {
    std::thread thread;                         // the read in flight, or the last one
    bool busy = false;                          // a read is in flight (main thread only)
    std::chrono::steady_clock::time_point started{};  // when the last read began
    std::chrono::steady_clock::duration took{};       // how long the last read took
    guint timer = 0;                            // the GLib timeout; 0 while the list is hidden
    std::chrono::steady_clock::time_point ticked{};   // its last tick, or its start: a tick far later
                                                      // than this one tells that the window was held up
    GtkWidget* served = nullptr;                // the Archive List the timer refreshes
    std::filesystem::path repository;           // the RepositoryRoot the watches belong to
    std::string archives_shown;                 // the mark of the archives the table shows (empty: none),
                                                // to tell when a read finds them changed
    std::vector<app::OperationWatch> watches;   // the operations of the latest read
    Report error;                               // why the latest read failed

    OperationsPoll() = default;
    OperationsPoll(const OperationsPoll&) = delete;
    OperationsPoll& operator=(const OperationsPoll&) = delete;
    // The program waits for a read still in flight before it ends.
    ~OperationsPoll()
    {
        if (thread.joinable()) {
            thread.join();
        }
    }
};

struct GuiState {
    GtkApplication* application = nullptr;
    GtkWindow* window = nullptr;
    GtkWidget* stack = nullptr;            // the seven views
    app::Text text;                        // interface language (decision 0017)
    HostInfo host;
    std::optional<app::Session> session;   // empty until a repository is open
    Report session_error;                  // why no repository is open
    // The registrations, verifications and comparisons started in the
    // window, each a job of its own (decision 0045).
    Jobs jobs;
    // A step that runs besides the jobs, one at a time: the upgrade of the
    // repository's database as it opens (decision 0032), and abandoning an
    // unfinished operation (decisions 0025, 0026, 0033).
    Job job;
    OperationsPoll operations;             // every unfinished operation, for the Archive List
    int exit_status = 0;                   // 1 when an unexpected error cut the smoke test, the start or the screenshots short
    // Set from VERIARCHIVE_GUI_SCREENSHOTS: take the documentation screenshots
    // into this folder, then quit (screenshots.cpp).
    std::optional<std::filesystem::path> screenshot_folder;
    // The media identity the views show, through displayed_identity: Sample
    // while the screenshot mode runs, as its pictures are published;
    // otherwise what was recorded.
    IdentityShown identity_shown = IdentityShown::Recorded;

    // The widgets below belong to the current window; a language change
    // replaces them. The Reports are what a view shows, kept so the new window
    // can word them in the new language.
    struct {
        GtkWidget* grid = nullptr;
        GtkWidget* status = nullptr;
        GtkWidget* operations = nullptr;       // "Unfinished operations", hidden when there are none
        GtkWidget* operations_note = nullptr;  // what the section shows, or why it cannot
        GtkWidget* operations_grid = nullptr;
        GtkWidget* abandoning = nullptr;       // progress while a registration (decision 0025) or a
                                               // relocation (decision 0026) is abandoned
    } archive_list;

    struct {
        GtkWidget* folder = nullptr;
        GtkWidget* label = nullptr;
        GtkWidget* new_revision = nullptr;
        GtkWidget* force_new_archive = nullptr;
        GtkWidget* media_copy = nullptr;
        GtkWidget* resume = nullptr;
        GtkWidget* start = nullptr;
        GtkWidget* cancel = nullptr;
        GtkWidget* alert = nullptr;        // a device alert, above the progress (decision 0044)
        GtkWidget* progress = nullptr;
        GtkWidget* report = nullptr;
        Report shown_report;
        std::uint64_t job = 0;             // the job the view shows: the last it started (0: none)
    } register_view;

    struct {
        GtkWidget* folder = nullptr;
        GtkWidget* archive = nullptr;      // drop-down: "the folder or volume above" or a registered archive
        GtkWidget* strict = nullptr;
        GtkWidget* add_chunk_hashes = nullptr;  // decision 0033: offered for an archive without chunk hashes
        GtkWidget* start = nullptr;
        GtkWidget* resume = nullptr;
        GtkWidget* cancel = nullptr;
        GtkWidget* alert = nullptr;        // a device alert, above the progress (decision 0044)
        GtkWidget* progress = nullptr;
        GtkWidget* report = nullptr;
        Report shown_report;
        std::uint64_t job = 0;             // the job the view shows: the last it started (0: none)
        std::vector<Uuid> archive_uuids;   // index i+1 of the drop-down
        std::string incomplete_run;        // last INCOMPLETE run, for Resume
        std::optional<std::filesystem::path> incomplete_root;
        std::optional<Uuid> incomplete_archive;
        // The stopped verification that adds chunk hashes of the archive
        // chosen in the drop-down (decision 0033), for Resume as
        // app::run_to_resume says; read again whenever the choice changes and
        // after each run.
        std::optional<app::RunToResume> chosen_adding;
    } verify_view;

    struct {
        GtkWidget* reference = nullptr;    // drop-down: archive A, the reference
        GtkWidget* copy = nullptr;         // drop-down: archive B, the copy to check
        GtkWidget* start = nullptr;
        GtkWidget* cancel = nullptr;
        GtkWidget* progress = nullptr;
        GtkWidget* report = nullptr;
        Report shown_report;
        std::uint64_t job = 0;             // the job the view shows: the last it started (0: none)
        std::vector<Uuid> archive_uuids;   // index i of both drop-downs
    } compare_view;

    // The Jobs page (decision 0045): a row for each job, and the window of
    // details of each job whose Details was opened.
    struct JobRow {
        GtkWidget* title = nullptr;
        GtkWidget* state = nullptr;
        GtkWidget* bar = nullptr;
        GtkWidget* alert = nullptr;
        GtkWidget* stop = nullptr;
    };
    struct JobDetails {
        GtkWidget* window = nullptr;
        GtkWidget* state = nullptr;
        GtkWidget* bar = nullptr;
        GtkWidget* alert = nullptr;
        GtkWidget* report = nullptr;
        GtkWidget* stop = nullptr;
    };
    struct {
        GtkWidget* list = nullptr;
        GtkWidget* empty = nullptr;        // "No jobs yet."
        GtkWidget* clear = nullptr;        // "Clear ended jobs"
        std::map<std::uint64_t, JobRow> rows;
        std::map<std::uint64_t, JobDetails> details;
    } jobs_view;

    struct {
        GtkWidget* archive = nullptr;
        GtkWidget* show = nullptr;         // "Show runs"
        GtkWidget* check = nullptr;        // "Check history chain"
        GtkWidget* grid = nullptr;
        GtkWidget* chain = nullptr;
        Report chain_result;               // shown in `chain`
        std::vector<Uuid> archive_uuids;
    } history_view;

    struct {
        GtkWidget* info = nullptr;
        GtkWidget* initialise = nullptr;
        GtkWidget* language = nullptr;     // drop-down: English, 繁體中文
        Report notice;                     // e.g. the language could not be saved
    } settings_view;
};

GtkWidget* build_archive_list_view(GuiState& state);
GtkWidget* build_register_view(GuiState& state);
GtkWidget* build_verify_view(GuiState& state);
GtkWidget* build_compare_view(GuiState& state);
GtkWidget* build_history_view(GuiState& state);
GtkWidget* build_settings_view(GuiState& state);
GtkWidget* build_jobs_view(GuiState& state);

// What the window does once a job changed (Jobs::on_change; 0: jobs were
// added or removed): the Jobs page, the job's window of details, and the
// view that started it show it as it is now, and the language drop-down is
// locked while a job waits or works.
void job_changed(GuiState& state, std::uint64_t job);

// Closes every window of a job's details (before a language change replaces
// the main window).
void close_job_details(GuiState& state);

// The devices that a job of `root`, or of the archive `archive_uuid`, reads
// (device_keys, recorded_device_keys), as the device queue compares them;
// asked on the job's thread. None where they cannot be told: the job then
// waits for nothing, and its operation says what is wrong.
std::vector<std::string> devices_of_root(const std::filesystem::path& root);
std::vector<std::string> devices_of_archive(const RepositoryLayout& layout, const Uuid& archive_uuid);

// Whether a job of the window waits or works on the operation with `root`
// or `archive` (either may be empty), which a step that abandons or resumes
// it must not take over (decision 0045); if so, says so in a message.
bool refused_while_a_job_reads(GuiState& state, const std::string& root, const Uuid& archive);

// Reload data shown by the views (after the repository changes).
// refresh_archive_list also shows the unfinished operations of the latest
// read again; the reads themselves run while the Archive List is shown.
void refresh_archive_list(GuiState& state);
void refresh_archive_choices(GuiState& state);
void refresh_settings(GuiState& state);

// Shows the recorded runs of the archive chosen in Verification History.
void show_runs(GuiState& state);

// Resume on a stopped verification or registration of the Archive List
// (decision 0022, point 7): continues it exactly as its command-line resume
// does, with the request that app::verification_resume and
// app::registration_resume make, as they make it: no check box of the views
// takes part. It runs as a job (decision 0045), shown in the view where an
// operation of its kind started in the window shows its progress and report
// (Verify Archive, Register Archive), which it brings into view. The caller
// has made sure that a repository is open and that no job of the window
// reads the operation (refused_while_a_job_reads).
// `confirmed`: the operator chose Resume in the question that a resume of a
// registration an older version left asks first (ask_before_resuming), as
// `register <root> --resume --confirm`; a resume of such a registration that
// is not confirmed asks it in Register Archive.
void resume_verification(GuiState& state, const app::UnfinishedOperation& verification);
void resume_registration(GuiState& state, const app::UnfinishedOperation& registration, bool confirmed = false);

// Asks, with Cancel as the default, before a registration that an older
// version of VeriArchive left is resumed, as that version may still run it
// (decision 0022, point 7; app::older_version_resume_warning): Resume in the
// Archive List on such a registration asks it, and so does a resume of one
// in Register Archive, which its own resume box or a stopped row's Resume
// starts, when the resume says it waits for the operator's word
// (RegisterFailure::older_version). `resume` runs only once the operator
// chose Resume; Cancel changes nothing.
void ask_before_resuming(GuiState& state, const std::string& label, const std::string& root,
    std::function<void(GuiState&)> resume);

// The Settings language drop-down. show_language_in_use selects the language
// in use. lock_language makes the drop-down unavailable while a job waits or
// works, or a step of the window runs, and available again once none does,
// because a language change rebuilds the window they report into.
void show_language_in_use(GuiState& state);
void lock_language(GuiState& state);

// Opens the repository (or initialises it on first run) and refreshes every view.
void open_repository_session(GuiState& state, bool initialise);

// Makes the open of open_repository_session(state, initialise) with the
// window shown, when that open will upgrade the repository's database
// (decision 0032): it keeps a copy of the database first, which takes a while
// for a large repository, and an open while another process upgrades it waits
// for that process. The window shows a notice in place of its views, with a
// progress bar that moves, while the open runs on the job's thread; once it is
// done, the window shows its views, Settings first, which says where the copy
// is, or, in place of the notice, why no repository is open. Returns false,
// having done nothing, when that open will not upgrade the database, while a
// job runs, and in the screenshot mode, whose sample repository is new.
bool open_with_upgrade_notice(GuiState& state, bool initialise);

// Builds the window and shows it. With `smoke_test` nothing is shown: the
// window is built, rebuilt in the other language and back (as a language
// change does), and the program quits. With state.screenshot_folder set, the
// screenshot tour runs once the window is shown.
void activate(GuiState& state, bool smoke_test);

// The documentation screenshots (screenshots.cpp): walks through the views,
// saves each one as a PNG in state.screenshot_folder and quits; exit status 1
// and a message on stderr when something fails.
void start_screenshot_tour(GuiState& state);

// Makes GTK's own texts (dialog buttons, ...) follow the interface language.
void use_gtk_language(app::Language language);

// Switches the interface language: remembers it in the config file, then
// rebuilds the window in the new language and shows it, on Settings.
void change_language(GuiState& state, app::Language language);

// Replaces the window with one in `language`, keeping what the user typed,
// chose and was shown; nothing is saved. `show` presents the new window (the
// smoke test rebuilds without showing anything).
void rebuild_window(GuiState& state, app::Language language, bool show);

} // namespace va::gui
