#pragma once
// The GUI's state, its window (decision 0046: a sidebar of pages beside the
// page shown, the header's jobs button) and its pages: the five views of
// HANDOFF §B17 (Archive List, Register Archive, Verify Archive, Verification
// History, Settings), Compare Archives (decision 0020) and Jobs (decision
// 0045).
#include "app/adding_chunk_hashes.h"
#include "app/refusal_in_window.h"
#include "app/unfinished_operations.h"
#include "app/session.h"
#include "gui/displayed_identity.h"
#include "gui/gui_support.h"
#include "gui/job_words.h"
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
    GtkWindow* window = nullptr;           // an AdwApplicationWindow
    // The pages, each a child named as the sidebar names it (archives, jobs,
    // history, settings, register, verify, compare): the one shown is the
    // window's content, whichever way it was chosen.
    GtkWidget* stack = nullptr;
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

    // The window around the pages (decision 0046).
    struct {
        GtkWidget* split = nullptr;        // AdwNavigationSplitView: the sidebar beside the page shown
        GtkWidget* sidebar = nullptr;      // GtkListBox, a row for each page but Register Archive
        GtkWidget* title = nullptr;        // AdwWindowTitle above the sidebar: VeriArchive, the repository's folder
        GtkWidget* content = nullptr;      // AdwNavigationPage of the page shown, titled as it
        GtkWidget* toasts = nullptr;       // AdwToastOverlay over the whole window
        GtkWidget* jobs_badge = nullptr;   // the Jobs row's count of jobs that run or wait
        bool asking_to_close = false;      // the question before the window closes is shown
    } shell;

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

    // The Jobs page and the header's jobs button (decisions 0045, 0046): a
    // row for each job in the group it is in, the button's popover with the
    // jobs that run and wait and the last ended ones, and the dialog of
    // details of each job whose Details was opened.
    struct JobRow {
        GtkWidget* title = nullptr;
        GtkWidget* meta = nullptr;         // job_meta: how far it is, whom it waits for, its result
        GtkWidget* tone = nullptr;         // once it ended: the icon of its result's tone, before `meta`
        GtkWidget* bar = nullptr;          // while it runs
        GtkWidget* alert = nullptr;
        GtkWidget* stop = nullptr;         // until it ended
        GtkWidget* details = nullptr;
        JobGroup group = JobGroup::Waiting;  // the group the row is listed in
    };
    struct JobList {
        GtkWidget* box = nullptr;          // its groups, or that there are no jobs
        std::map<std::uint64_t, JobRow> rows;
    };
    struct JobDetails {
        GtkWidget* dialog = nullptr;       // AdwDialog
        GtkWidget* state = nullptr;
        GtkWidget* bar = nullptr;
        GtkWidget* alert = nullptr;
        GtkWidget* report = nullptr;
        GtkWidget* stop = nullptr;
    };
    struct {
        JobList page;                      // the Jobs page
        GtkWidget* clear = nullptr;        // "Clear ended jobs"
        GtkWidget* button = nullptr;       // the header's jobs button (GtkMenuButton)
        GtkWidget* ring = nullptr;         // its ring: how far the jobs that run are (running_fraction)
        GtkWidget* count = nullptr;        // its count of jobs that run or wait, hidden at 0
        GtkWidget* word = nullptr;         // its word "Jobs", hidden in a narrow window
        GtkWidget* popover = nullptr;
        GtkWidget* popover_box = nullptr;  // what the popover holds, narrower in a narrow window
        JobList popover_list;              // the popover's jobs
        std::map<std::uint64_t, JobDetails> details;
    } jobs_page;

    struct {
        GtkWidget* archive = nullptr;
        GtkWidget* show = nullptr;         // "Show runs"
        GtkWidget* check = nullptr;        // "Check history chain"
        GtkWidget* grid = nullptr;
        GtkWidget* chain = nullptr;
        Report chain_result;               // shown in `chain`
        std::vector<Uuid> archive_uuids;
    } history_view;

    // Settings, an AdwPreferencesPage (decision 0046): its groups, and the
    // rows that refresh_settings put into them.
    struct {
        GtkWidget* repository = nullptr;   // RepositoryRoot and how it was chosen, or why none is open
        GtkWidget* language = nullptr;     // AdwComboRow: English, 繁體中文
        GtkWidget* policy = nullptr;       // overdue limits, strict metadata, trusted keys
        GtkWidget* about = nullptr;        // the program's version, what it detects, the notice
        std::vector<std::pair<GtkWidget*, GtkWidget*>> rows;  // (group, row) that refresh_settings added
        Report notice;                     // e.g. the language could not be saved
    } settings_page;
};

GtkWidget* build_archive_list_view(GuiState& state);
GtkWidget* build_register_view(GuiState& state);
GtkWidget* build_verify_view(GuiState& state);
GtkWidget* build_compare_view(GuiState& state);
GtkWidget* build_history_view(GuiState& state);
GtkWidget* build_settings_page(GuiState& state);
GtkWidget* build_jobs_page(GuiState& state);

// The icon of the Jobs page on its sidebar row, beside its name, as the
// approved design of decision 0046 draws it. The header's jobs button has a
// ring of its own instead, beside its word, so that it never looks like the
// header's Reload.
inline constexpr const char* jobs_icon = "view-refresh-symbolic";

// The width of what the jobs popover holds, and its width in a narrow window
// (whose least width is 360), where it would reach past the window.
inline constexpr int jobs_popover_width = 420;
inline constexpr int jobs_popover_narrow_width = 300;

// The header's jobs button (decision 0046): a ring of how far the jobs that
// run are, the count of the jobs that run or wait and the word Jobs, and its
// popover, which lists them and the last 3 ended and leads to the Jobs page.
GtkWidget* build_jobs_button(GuiState& state);

// Creates the window, in state.text's language (window.cpp): the sidebar of
// pages beside the page shown, the header's jobs button, and the question
// before the window closes while jobs run. It shows Archives.
void build_window(GuiState& state);

// Shows the page named `page` (a child of state.stack); the sidebar selects
// its row, the header names it, and a narrow window shows it in place of the
// sidebar.
void show_page(GuiState& state, const char* page);

// The window's own part of what a repository changes: the repository's
// folder under the sidebar's title.
void refresh_shell(GuiState& state);

// A short confirmation at the bottom of the window (AdwToast), with a button
// that runs `action` when there is one; it goes by itself.
struct ToastAction {
    std::string label;
    std::function<void(GuiState&)> run;
};
void show_toast(GuiState& state, const std::string& text, std::optional<ToastAction> action = std::nullopt);

// What the window does once a job changed (Jobs::on_change; 0: jobs were
// added or removed): the Jobs page, the header's jobs button and its
// popover, the job's details, and the view that started it show it as it is
// now, and the language drop-down is locked while a job waits or works.
void job_changed(GuiState& state, std::uint64_t job);

// Shows the details of `job` in a dialog of their own, or nothing for a job
// removed.
void open_job_details(GuiState& state, std::uint64_t job);

// Closes every dialog of a job's details (before a language change replaces
// the main window); returns the jobs whose details were open, so the new
// window can show them again.
std::vector<std::uint64_t> close_job_details(GuiState& state);

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

// The Settings language row. show_language_in_use selects the language in
// use. lock_language makes the row unavailable while a job waits or works, or
// a step of the window runs, saying so under its title, and available again
// once none does, because a language change rebuilds the window they report
// into.
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
// chose and was shown: the page shown, the details of jobs open; nothing is
// saved. `show` presents the new window (the smoke test rebuilds without
// showing anything).
void rebuild_window(GuiState& state, app::Language language, bool show);

} // namespace va::gui
