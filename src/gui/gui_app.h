#pragma once
// The GUI's state and its six views: the five of HANDOFF §B17 (Archive List,
// Register Archive, Verify Archive, Verification History, Settings) and
// Compare Archives (decision 0020).
#include "app/unfinished_operations.h"
#include "app/session.h"
#include "gui/gui_support.h"

#include <chrono>
#include <filesystem>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace va::gui {

// Unfinished registrations and verifications, wherever they run (the command
// line, this window), read from the repository every few seconds while the
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
    GtkWidget* stack = nullptr;            // the six views
    app::Text text;                        // interface language (decision 0017)
    HostInfo host;
    std::optional<app::Session> session;   // empty until a repository is open
    Report session_error;                  // why no repository is open
    Job job;                               // the one long operation running, if any
    OperationsPoll operations;             // every unfinished operation, for the Archive List
    int exit_status = 0;                   // 1 when an unexpected error cut the smoke test, the start or the screenshots short
    // Set from VERIARCHIVE_GUI_SCREENSHOTS: take the documentation screenshots
    // into this folder, then quit (screenshots.cpp).
    std::optional<std::filesystem::path> screenshot_folder;

    // The widgets below belong to the current window; a language change
    // replaces them. The Reports are what a view shows, kept so the new window
    // can word them in the new language.
    struct {
        GtkWidget* grid = nullptr;
        GtkWidget* status = nullptr;
        GtkWidget* operations = nullptr;       // "Unfinished operations", hidden when there are none
        GtkWidget* operations_note = nullptr;  // what the section shows, or why it cannot
        GtkWidget* operations_grid = nullptr;
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
        GtkWidget* progress = nullptr;
        GtkWidget* report = nullptr;
        Report shown_report;
    } register_view;

    struct {
        GtkWidget* folder = nullptr;
        GtkWidget* archive = nullptr;      // drop-down: "the folder above" or a registered archive
        GtkWidget* strict = nullptr;
        GtkWidget* start = nullptr;
        GtkWidget* resume = nullptr;
        GtkWidget* cancel = nullptr;
        GtkWidget* progress = nullptr;
        GtkWidget* report = nullptr;
        Report shown_report;
        std::vector<Uuid> archive_uuids;   // index i+1 of the drop-down
        std::string incomplete_run;        // last INCOMPLETE run, for Resume
        std::optional<std::filesystem::path> incomplete_root;
        std::optional<Uuid> incomplete_archive;
    } verify_view;

    struct {
        GtkWidget* reference = nullptr;    // drop-down: archive A, the reference
        GtkWidget* copy = nullptr;         // drop-down: archive B, the copy to check
        GtkWidget* start = nullptr;
        GtkWidget* cancel = nullptr;
        GtkWidget* progress = nullptr;
        GtkWidget* report = nullptr;
        Report shown_report;
        std::vector<Uuid> archive_uuids;   // index i of both drop-downs
    } compare_view;

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

// Reload data shown by the views (after the repository changes).
// refresh_archive_list also shows the unfinished operations of the latest
// read again; the reads themselves run while the Archive List is shown.
void refresh_archive_list(GuiState& state);
void refresh_archive_choices(GuiState& state);
void refresh_settings(GuiState& state);

// Shows the recorded runs of the archive chosen in Verification History.
void show_runs(GuiState& state);

// The Settings language drop-down. show_language_in_use selects the language
// in use. lock_language makes the drop-down unavailable while an operation
// runs, because a language change rebuilds the window the operation reports into.
void show_language_in_use(GuiState& state);
void lock_language(GuiState& state, bool locked);

// Opens the repository (or initialises it on first run) and refreshes every view.
void open_repository_session(GuiState& state, bool initialise);

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
