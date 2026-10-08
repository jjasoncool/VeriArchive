#pragma once
// The GUI's state and the five views of HANDOFF §B17: Archive List, Register
// Archive, Verify Archive, Verification History and Settings.
#include "app/session.h"
#include "gui/gui_support.h"

#include <optional>
#include <string>

namespace va::gui {

struct GuiState {
    GtkApplication* application = nullptr;
    GtkWindow* window = nullptr;
    GtkWidget* stack = nullptr;            // the five views
    app::Text text;                        // interface language (decision 0017)
    HostInfo host;
    std::optional<app::Session> session;   // empty until a repository is open
    std::string session_error;
    Job job;                               // the one long operation running, if any

    struct {
        GtkWidget* grid = nullptr;
        GtkWidget* status = nullptr;
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
        std::vector<Uuid> archive_uuids;   // index i+1 of the drop-down
        std::string incomplete_run;        // last INCOMPLETE run, for Resume
        std::optional<std::filesystem::path> incomplete_root;
        std::optional<Uuid> incomplete_archive;
    } verify_view;

    struct {
        GtkWidget* archive = nullptr;
        GtkWidget* grid = nullptr;
        GtkWidget* chain = nullptr;
        std::vector<Uuid> archive_uuids;
    } history_view;

    struct {
        GtkWidget* info = nullptr;
        GtkWidget* initialise = nullptr;
        GtkWidget* language = nullptr;     // drop-down: English, 繁體中文
        std::string notice;                // e.g. the language could not be saved
    } settings_view;
};

GtkWidget* build_archive_list_view(GuiState& state);
GtkWidget* build_register_view(GuiState& state);
GtkWidget* build_verify_view(GuiState& state);
GtkWidget* build_history_view(GuiState& state);
GtkWidget* build_settings_view(GuiState& state);

// Reload data shown by the views (after the repository changes).
void refresh_archive_list(GuiState& state);
void refresh_archive_choices(GuiState& state);
void refresh_settings(GuiState& state);

// Opens the repository (or initialises it on first run) and refreshes every view.
void open_repository_session(GuiState& state, bool initialise);

// Builds the window; `smoke_test` builds everything without showing it, then quits.
void activate(GuiState& state, bool smoke_test);

// Makes GTK's own texts (dialog buttons, ...) follow the interface language;
// call before GTK starts.
void use_gtk_language(app::Language language);

// Switches the interface language: remembers it in the config file and
// rebuilds the window in the new language, showing Settings.
void change_language(GuiState& state, app::Language language);

} // namespace va::gui
