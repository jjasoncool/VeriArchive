#pragma once
// The GUI's state, its window (decision 0046: a sidebar of pages beside the
// page shown, the header's jobs button) and its pages: Archives (the list of
// archives beside the details of the one selected, where verifications start,
// and the unfinished operations above them), Register Archive, Verification
// History and Settings of HANDOFF §B17, Compare Archives (decision 0020) and
// Jobs (decision 0045).
#include "app/adding_chunk_hashes.h"
#include "app/refusal_in_window.h"
#include "app/unfinished_operations.h"
#include "app/session.h"
#include "core/engine/archive_status.h"
#include "gui/archive_rows.h"
#include "gui/displayed_identity.h"
#include "gui/gui_support.h"
#include "gui/job_words.h"
#include "gui/jobs.h"

#include <chrono>
#include <expected>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace va::gui {

// Unfinished registrations, verifications and relocations, wherever they run
// (the command line, this window), read from the repository every few seconds
// while Archives is shown (operations_poll.cpp). Each read runs on a thread of
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
    GtkWidget* served = nullptr;                // the Archives page the timer refreshes
    std::filesystem::path repository;           // the RepositoryRoot the watches belong to
    std::string archives_shown;                 // the mark of the archives the list shows (empty: none),
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
    OperationsPoll operations;             // every unfinished operation, for Archives
    int exit_status = 0;                   // 1 when an unexpected error cut the smoke test, the start or the screenshots short
    // Set from VERIARCHIVE_GUI_SCREENSHOTS: take the documentation screenshots
    // into this folder, then quit (screenshots.cpp).
    std::optional<std::filesystem::path> screenshot_folder;
    // The media identity the views show, through displayed_identity: Sample
    // while the screenshot mode runs, as its pictures are published;
    // otherwise what was recorded.
    IdentityShown identity_shown = IdentityShown::Recorded;

    // What Archives shows, kept across windows: the archive selected, whose
    // details show beside the list; the verification that each archive's
    // details show (decision 0045: the last one started there, or by Resume
    // of its stopped verification); and the run that the window interrupted
    // last (R-104), which Resume in its archive's details continues as the
    // window ran it (details_resume).
    std::optional<Uuid> archive_selected;
    std::map<Uuid, std::uint64_t> verification_jobs;
    std::optional<InterruptedRun> interrupted_run;

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
        AdwBreakpoint* narrow = nullptr;   // the window's breakpoint, below which it shows one pane at a time
        AdwToast* toast = nullptr;         // the last toast while it is shown (a weak pointer: nothing once gone)
        bool asking_to_close = false;      // the question before the window closes is shown
    } shell;

    // A job's row, as the Jobs page, the header's popover and an archive's
    // details show it (jobs_page.cpp).
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

    // A row of the unfinished operations in Archives as it is drawn: what
    // makes it up (its operation, its buttons, whether it has a bar and
    // details), and the widgets whose words and bar a later read changes. A
    // read that finds the same rows changes only those, so the keyboard
    // focus on a row's Resume or Abandon... stays where it is.
    struct OperationShown {
        std::string key;
        bool offers_resume = false;
        bool resume_asks_first = false;
        bool abandonable = false;
        bool has_bar = false;
        bool has_details = false;
        GtkWidget* icon = nullptr;
        GtkWidget* title = nullptr;
        GtkWidget* root = nullptr;
        GtkWidget* bar = nullptr;
        GtkWidget* progress = nullptr;
        GtkWidget* details = nullptr;
        GtkWidget* state = nullptr;
    };

    // Archives (decision 0046): the page's head; the unfinished operations
    // (decision 0022) above the archives, hidden when there are none; the
    // list of archives beside the details of the one selected, or, without
    // archives, a page that leads to the first registration. A narrow window
    // shows the list or the details.
    struct {
        GtkWidget* page = nullptr;             // the page: the operations are read while it is shown
        GtkWidget* status = nullptr;           // how many archives, or why none can be listed
        GtkWidget* refresh = nullptr;          // Refresh, which holds the focus while what had it is made anew
        GtkWidget* abandoning = nullptr;       // progress while a step of abandoning a registration
                                               // (decision 0025), a relocation (decision 0026) or a
                                               // verification that adds chunk hashes (decision 0033) runs
        GtkWidget* operations = nullptr;       // "Unfinished operations", hidden when there are none
        GtkWidget* operations_note = nullptr;  // what the group shows, or why it cannot
        GtkWidget* operations_list = nullptr;  // GtkListBox, a row for each operation
        std::vector<OperationShown> operation_rows;  // its rows, in its order
        GtkWidget* views = nullptr;            // GtkStack: "archives" (the split view), or "empty"
        GtkWidget* empty = nullptr;            // AdwStatusPage: no archives yet, or no repository
        GtkWidget* empty_button = nullptr;     // what it leads to: Register Archive, or Settings
        GtkWidget* split = nullptr;            // AdwNavigationSplitView: the list beside the details
        GtkWidget* list = nullptr;             // GtkListBox, a row for each archive
        GtkWidget* detail_page = nullptr;      // AdwNavigationPage of the details, titled by the archive
        GtkWidget* detail = nullptr;           // the details of the archive selected, made again for each
        GtkWidget* back = nullptr;             // back to the list, shown in a narrow window
        GtkWidget* notice = nullptr;           // the stopped verification of the archive shown, from the
                                               // latest read of the unfinished operations
        std::optional<std::string> notice_key; // the operation `notice` shows, once it shows one
        GtkWidget* notice_words = nullptr;     // its words, which a later read of it changes in place
        GtkWidget* verify = nullptr;           // FULL verify (AdwSplitButton)
        GtkWidget* verify_menu = nullptr;      // the popover of the menu of FULL verify
        GtkWidget* adding = nullptr;           // its "FULL verify, and add chunk hashes", where it lists it
        GtkWidget* adding_note = nullptr;      // the line under it: what it does, or why it is not offered
        // Why chunk hashes cannot be added to the archive shown, as
        // why_chunk_hashes_cannot_be_added said it when the details were made.
        std::expected<std::optional<std::string>, IoError> adding_refused;
        JobList card;                          // the archive's job, as a card in its details
        std::vector<ArchiveStatus> statuses;   // the archives the list shows, in its order
        bool filling = false;                  // the list is being filled: a row selected is not the operator's
    } archives_page;

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

GtkWidget* build_archives_page(GuiState& state);
GtkWidget* build_register_view(GuiState& state);
GtkWidget* build_compare_view(GuiState& state);
GtkWidget* build_history_view(GuiState& state);
GtkWidget* build_settings_page(GuiState& state);
GtkWidget* build_jobs_page(GuiState& state);

// The icon of the Jobs page on its sidebar row, beside its name, and of an
// unfinished operation that works. Not the circling arrows of the approved
// design of decision 0046: those are Refresh in the head of Archives, beside
// them on one screen. The header's jobs button has a ring of its own
// instead, beside its word.
inline constexpr const char* jobs_icon = "system-run-symbolic";

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

// The window's narrow layout (window.cpp): sets `property` of `object` to
// `value` while the window is narrower than its breakpoint, as the pages ask
// it while they are built (Archives shows its list or the details of an
// archive, not both). Once build_window has made the breakpoint.
void set_while_narrow(GuiState& state, GtkWidget* object, const char* property, bool value);

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

// Takes the last toast away at once (the screenshot tour, before its next
// picture).
void dismiss_toast(GuiState& state);

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

// A card of `job` in `list` (an archive's details): its row as the Jobs page
// shows it, with its title, how far it is, its device alert, its bar, Stop and
// Details; job_changed keeps it up to date while `list` holds it.
GtkWidget* make_job_card(GuiState& state, GuiState::JobList& list, const JobShown& job);

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
// refresh_archives reads the archives again and shows them, the details of
// the one selected, and the unfinished operations of the latest read; the
// reads of those run while Archives is shown (watch_operations).
void refresh_archives(GuiState& state);
void refresh_archive_choices(GuiState& state);
void refresh_settings(GuiState& state);

// ---- Archives (archives_page.cpp, archive_detail.cpp) -----------------------

// Selects `archive` in the list of Archives and shows its details; in a narrow
// window with `show_details`, in place of the list. Nothing for an archive
// the list does not show.
void select_archive(GuiState& state, const Uuid& archive, bool show_details = false);

// The details of the archive selected (state.archive_selected), made again
// from what the list read; the first archive's where none is selected.
void show_details(GuiState& state);

// The part of the details that the reads of the unfinished operations change
// (details_operations): the notice of a stopped verification of the archive
// shown, which Resume continues, and whether "FULL verify, and add chunk
// hashes" is offered while such a verification is unfinished.
void show_details_operations(GuiState& state);

// Draws the unfinished operations of the latest read above the archives, in
// the interface language; hidden when nothing is unfinished.
void show_operations(GuiState& state);

// Says `report` in the head of Archives, where an unexpected error of its
// handlers shows.
void show_archives_status(GuiState& state, const Report& report);

// Before a part of Archives (`part`) is made anew: where it holds the
// keyboard focus, the focus goes to the page's Refresh, so that it never goes
// with the widgets taken away; returns whether it did, for the part to take
// the focus back once it is made.
bool keep_focus_from(GuiState& state, GtkWidget* part);

// A pill (archive_rows.h) and a status dot, in the colour of their tone
// (tone_class, the window's style).
GtkWidget* make_pill(const Pill& pill);
GtkWidget* make_dot(Tone tone);

// ---- The unfinished operations (operations_poll.cpp) ------------------------

// The watches belong to one repository; another one opened in Settings
// starts afresh.
void use_session_repository(GuiState& state);

// Reads the unfinished operations on a thread, unless a read is in flight;
// the result comes back to the main thread, which shows it. `when_due`:
// called by the timer, which a slow read spaces out.
void start_poll(GuiState& state, bool when_due = false);

// Shows why the operations could not be read, keeping the rows of the last read.
void show_poll_error(GuiState& state, Report report);

// Archives came on screen (`page`): reads now, then every few seconds.
// It left the screen (another page, or its window closed): no more reads.
void watch_operations(GuiState& state, GtkWidget* page);
void stop_watching(GuiState& state, GtkWidget* page);

// ---- Resume and Abandon... of the unfinished operations (unfinished_actions.cpp)

// The operator chose Resume on the stopped verification or registration `key`
// of the latest read (decision 0022, point 7): it continues as its
// command-line resume does. ask_to_resume asks first, for a registration that
// an older version left and that may have stopped
// (OperationRow::resume_asks_first), and resumes only once the operator chose
// Resume. ask_to_abandon reads what abandoning the registration, relocation
// or verification that adds chunk hashes `key` removes, then asks (decisions
// 0025, 0026, 0033). Each says in a message why it cannot: no repository, a
// step of abandoning that runs, an operation no longer listed, or one that a
// job of the window works on.
void resume_from_list(GuiState& state, const std::string& key);
void ask_to_resume(GuiState& state, const std::string& key);
void ask_to_abandon(GuiState& state, const std::string& key);

// Whether the window can resume an operation now, as a job: not without a
// repository, nor while a step of abandoning runs, which may be abandoning
// it; otherwise says why not in a message.
bool can_resume(GuiState& state);

// Says `report` in a message of its own (AdwAlertDialog).
void show_message(GuiState& state, const Report& report);

// Shows the recorded runs of the archive chosen in Verification History.
void show_runs(GuiState& state);

// Resume on a stopped verification or registration of the unfinished
// operations (decision 0022, point 7): continues it exactly as its
// command-line resume does, with the request that app::verification_resume
// and app::registration_resume make, as they make it: no choice of the views
// takes part. It runs as a job (decision 0045), shown where an operation of
// its kind started in the window shows it: a verification in its archive's
// details in Archives, a registration in Register Archive, which it brings
// into view. The caller has made sure that a repository is open and that no
// job of the window reads the operation (refused_while_a_job_reads).
// `confirmed`: the operator chose Resume in the question that a resume of a
// registration an older version left asks first (ask_before_resuming), as
// `register <root> --resume --confirm`; a resume of such a registration that
// is not confirmed asks it in Register Archive.
void resume_verification(GuiState& state, const app::UnfinishedOperation& verification);
void resume_registration(GuiState& state, const app::UnfinishedOperation& registration, bool confirmed = false);

// Asks, with Cancel as the default, before a registration that an older
// version of VeriArchive left is resumed, as that version may still run it
// (decision 0022, point 7; app::older_version_resume_warning): Resume under
// Unfinished operations on such a registration asks it, and so does a resume
// of one in Register Archive, which its own resume box or a stopped row's
// Resume starts, when the resume says it waits for the operator's word
// (RegisterFailure::older_version). `resume` runs only once the operator
// chose Resume; Cancel changes nothing.
void ask_before_resuming(GuiState& state, const std::string& label, const std::string& root,
    std::function<void(GuiState&)> resume);

// ---- Verifications (verification.cpp) ---------------------------------------

// How FULL verify of an archive's details reads it: in full; in full adding
// chunk hashes (decision 0033); in full with strict metadata (R-51).
enum class VerifyChoice { Full, AddingChunkHashes, StrictMetadata };

// FULL verify of `archive`, found by its uuid on the attached volumes, as a
// job titled by its label (decision 0045), shown in its details; strict
// metadata where `choice` asks for it or the policy sets it.
void verify_archive(GuiState& state, const Uuid& archive, VerifyChoice choice);

// "Verify a folder or volume...": asks, in a dialog over the window, for the
// folder or volume to verify, typed or pasted (a root that a refusal named,
// a volume's GUID path) or chosen with Choose... (GtkFileDialog), and
// whether to fail metadata differences; then verifies it by its root as
// verify_root does.
void choose_and_verify_root(GuiState& state, const Uuid& shown_in);

// A FULL verification of the archive that `root` holds, told by the root's
// identity, as a job titled by the root, shown in the details of `shown_in`
// (the archive whose details started it); for an archive that more than one
// attached volume holds, which a verification by uuid cannot tell apart.
// Strict metadata where `strict` asks for it or the policy sets it.
void verify_root(GuiState& state, const Uuid& shown_in, const std::filesystem::path& root, bool strict = false);

// Resume in the details of `archive` on its stopped verification `key` (as
// the latest read lists it), as details_resume makes its request: the run
// that this window interrupted last as the window ran it, where it is that
// one; otherwise as Resume on its row under Unfinished operations.
void resume_in_details(GuiState& state, const Uuid& archive, const std::string& key);

// Whether `archive` (whose recorded root is `root`, which may be empty)
// already has a job of this window that waits or works on it or its root,
// which a second verification would only follow with another whole read of
// its device; if so, says so in a message (archive_has_a_job). Every
// verification that Archives starts asks it first.
bool refused_while_the_archive_has_a_job(GuiState& state, const Uuid& archive, const std::string& root);

// ---- Other pages, preset from an archive's details --------------------------

// Compare Archives with `reference` as A and `copy` as B, where it lists
// them; without `copy`, B is another archive than A.
void compare_with(GuiState& state, const Uuid& reference, const std::optional<Uuid>& copy);

// Register Archive with the folder or volume `root` and "New revision of the
// archive registered at this folder or volume" ticked, not started.
void register_new_revision(GuiState& state, const std::string& root);

// Verification History with the runs of `archive` shown.
void show_history_of(GuiState& state, const Uuid& archive);

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
