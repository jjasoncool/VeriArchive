// Window layout: a sidebar to switch between the six views, and the session
// (repository) shared by all of them.
#include "gui/gui_app.h"

#include <exception>
#include <string_view>

// Last: on Windows libintl.h redefines printf and its relatives as macros,
// which should not reach the other headers.
#include <libintl.h>

namespace va::gui {
namespace {

// Why no repository is open, followed by what holds all the same: the
// warnings, as running elevated may be why (R-12, decision 0037), and what
// opening it did, said once: the upgrade of its database, and where the copy
// from before it is (decision 0032). Worded in the language shown.
Report session_failure(app::SessionFailure failure)
{
    return [failure = std::move(failure)](const app::Text& text) {
        auto shown = text(failure.message);
        for (const auto& warning : failure.warnings) {
            shown += "\n\n" + text.format("Warning: {}", text(warning));
        }
        for (const auto& note : failure.notes) {
            shown += "\n\n" + text.format("Note: {}", text(note));
        }
        return shown;
    };
}

} // namespace

void open_repository_session(GuiState& state, bool initialise)
{
    // An exception from the engines (a std::filesystem error, say) is one more
    // reason the repository could not be opened, shown in Settings.
    try {
        auto session = app::open_session({initialise, std::nullopt, std::nullopt, {}, false, std::nullopt});
        if (session) {
            state.session = std::move(*session);
            state.session_error = nullptr;
        } else {
            // The warnings too, which hold where no repository could be
            // opened: running elevated may be why (R-12, decision 0037).
            state.session.reset();
            state.session_error = session_failure(std::move(session.error()));
        }
    } catch (const std::exception& error) {
        log_failure(std::string("exception while opening the repository: ") + error.what());
        state.session.reset();
        state.session_error = unexpected_error(error.what());
    }
    refresh_settings(state);
    refresh_archive_list(state);
    refresh_archive_choices(state);
}

namespace {

// Creates the window with the sidebar and the six views, in state.text's language.
void build_window(GuiState& state)
{
    const auto& text = state.text;
    GtkWidget* window = gtk_application_window_new(state.application);
    state.window = GTK_WINDOW(window);
    gtk_window_set_title(state.window, "VeriArchive");
    gtk_window_set_default_size(state.window, 1100, 720);

    GtkWidget* stack = gtk_stack_new();
    state.stack = stack;
    gtk_widget_set_hexpand(stack, TRUE);
    gtk_widget_set_vexpand(stack, TRUE);
    gtk_stack_add_titled(GTK_STACK(stack), build_archive_list_view(state), "archives",
        text("Archive List").c_str());
    gtk_stack_add_titled(GTK_STACK(stack), build_register_view(state), "register",
        text("Register Archive").c_str());
    gtk_stack_add_titled(GTK_STACK(stack), build_verify_view(state), "verify",
        text("Verify Archive").c_str());
    gtk_stack_add_titled(GTK_STACK(stack), build_compare_view(state), "compare",
        text("Compare Archives").c_str());
    gtk_stack_add_titled(GTK_STACK(stack), build_history_view(state), "history",
        text("Verification History").c_str());
    gtk_stack_add_titled(GTK_STACK(stack), build_settings_view(state), "settings",
        text("Settings").c_str());

    // Switching back to the list shows the latest results.
    g_signal_connect(stack, "notify::visible-child",
        G_CALLBACK(+[](GObject* source, GParamSpec*, gpointer data) {
            auto& state = *static_cast<GuiState*>(data);
            guarded(
                [&] {
                    const char* name = gtk_stack_get_visible_child_name(GTK_STACK(source));
                    if (name && std::string_view(name) == "archives") {
                        refresh_archive_list(state);
                    }
                },
                [&](const Report& report) {
                    gtk_label_set_text(GTK_LABEL(state.archive_list.status), report(state.text).c_str());
                });
        }),
        &state);

    GtkWidget* sidebar = gtk_stack_sidebar_new();
    gtk_stack_sidebar_set_stack(GTK_STACK_SIDEBAR(sidebar), GTK_STACK(stack));
    GtkWidget* layout = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_box_append(GTK_BOX(layout), sidebar);
    gtk_box_append(GTK_BOX(layout), gtk_separator_new(GTK_ORIENTATION_VERTICAL));
    gtk_box_append(GTK_BOX(layout), stack);
    gtk_window_set_child(state.window, layout);
}

// What the user typed and chose in the window, carried over when a language
// change replaces it. (What the views show is kept in GuiState as Reports.)
// The window's position cannot be kept: GTK 4 has no way to set it.
struct Kept {
    int width = 0;
    int height = 0;
    bool maximized = false;
    std::string register_folder;
    std::string register_label;
    bool new_revision = false;
    bool force_new_archive = false;
    bool media_copy = false;
    bool resume = false;
    double register_fraction = 0.0;
    bool register_finished = false;
    std::string verify_folder;
    bool strict = false;
    bool add_chunk_hashes = false;
    std::optional<Uuid> verify_archive;  // empty: "The folder or volume above"
    double verify_fraction = 0.0;
    bool verify_finished = false;
    std::optional<Uuid> compare_reference;
    std::optional<Uuid> compare_copy;
    double compare_fraction = 0.0;
    bool compare_finished = false;
    std::optional<Uuid> history_archive;
    bool runs_shown = false;
};

std::string entry_text(GtkWidget* entry)
{
    return gtk_editable_get_text(GTK_EDITABLE(entry));
}

bool checked(GtkWidget* check_button)
{
    return gtk_check_button_get_active(GTK_CHECK_BUTTON(check_button)) != FALSE;
}

// A finished operation leaves "Done" on its progress bar; before the first
// one the bar has no text.
bool finished(GtkWidget* progress_bar)
{
    const char* text = gtk_progress_bar_get_text(GTK_PROGRESS_BAR(progress_bar));
    return text && *text;
}

Kept keep(const GuiState& state)
{
    Kept kept;
    // GTK keeps the default size up to date as the user resizes, and it is the
    // size a maximized window returns to.
    gtk_window_get_default_size(state.window, &kept.width, &kept.height);
    kept.maximized = gtk_window_is_maximized(state.window) != FALSE;

    const auto& registering = state.register_view;
    kept.register_folder = entry_text(registering.folder);
    kept.register_label = entry_text(registering.label);
    kept.new_revision = checked(registering.new_revision);
    kept.force_new_archive = checked(registering.force_new_archive);
    kept.media_copy = checked(registering.media_copy);
    kept.resume = checked(registering.resume);
    kept.register_fraction = gtk_progress_bar_get_fraction(GTK_PROGRESS_BAR(registering.progress));
    kept.register_finished = finished(registering.progress);

    const auto& verifying = state.verify_view;
    kept.verify_folder = entry_text(verifying.folder);
    kept.strict = checked(verifying.strict);
    kept.add_chunk_hashes = checked(verifying.add_chunk_hashes);
    kept.verify_archive = chosen_archive(verifying.archive, verifying.archive_uuids, 1);
    kept.verify_fraction = gtk_progress_bar_get_fraction(GTK_PROGRESS_BAR(verifying.progress));
    kept.verify_finished = finished(verifying.progress);

    const auto& comparing = state.compare_view;
    kept.compare_reference = chosen_archive(comparing.reference, comparing.archive_uuids, 0);
    kept.compare_copy = chosen_archive(comparing.copy, comparing.archive_uuids, 0);
    kept.compare_fraction = gtk_progress_bar_get_fraction(GTK_PROGRESS_BAR(comparing.progress));
    kept.compare_finished = finished(comparing.progress);

    const auto& history = state.history_view;
    kept.history_archive = chosen_archive(history.archive, history.archive_uuids, 0);
    kept.runs_shown = gtk_widget_get_first_child(history.grid) != nullptr;
    return kept;
}

// Puts what `keep` read into the new window, after the views were refreshed.
void restore(GuiState& state, const Kept& kept)
{
    const auto& text = state.text;
    if (kept.width > 0 && kept.height > 0) {
        gtk_window_set_default_size(state.window, kept.width, kept.height);
    }
    if (kept.maximized) {
        gtk_window_maximize(state.window);
    }

    auto& registering = state.register_view;
    gtk_editable_set_text(GTK_EDITABLE(registering.folder), kept.register_folder.c_str());
    gtk_editable_set_text(GTK_EDITABLE(registering.label), kept.register_label.c_str());
    gtk_check_button_set_active(GTK_CHECK_BUTTON(registering.new_revision), kept.new_revision);
    gtk_check_button_set_active(GTK_CHECK_BUTTON(registering.force_new_archive), kept.force_new_archive);
    gtk_check_button_set_active(GTK_CHECK_BUTTON(registering.media_copy), kept.media_copy);
    gtk_check_button_set_active(GTK_CHECK_BUTTON(registering.resume), kept.resume);
    gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(registering.progress), kept.register_fraction);
    if (kept.register_finished) {
        gtk_progress_bar_set_text(GTK_PROGRESS_BAR(registering.progress), text("Done").c_str());
    }
    if (registering.shown_report) {
        set_text(registering.report, registering.shown_report(text));
    }

    auto& verifying = state.verify_view;
    gtk_editable_set_text(GTK_EDITABLE(verifying.folder), kept.verify_folder.c_str());
    gtk_check_button_set_active(GTK_CHECK_BUTTON(verifying.strict), kept.strict);
    choose_archive(verifying.archive, verifying.archive_uuids, 1, kept.verify_archive);
    // Choosing the archive offered the checkbox again, where it may be chosen.
    if (gtk_widget_get_sensitive(verifying.add_chunk_hashes)) {
        gtk_check_button_set_active(GTK_CHECK_BUTTON(verifying.add_chunk_hashes), kept.add_chunk_hashes);
    }
    gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(verifying.progress), kept.verify_fraction);
    if (kept.verify_finished) {
        gtk_progress_bar_set_text(GTK_PROGRESS_BAR(verifying.progress), text("Done").c_str());
    }
    if (verifying.shown_report) {
        set_text(verifying.report, verifying.shown_report(text));
    }

    auto& comparing = state.compare_view;
    choose_archive(comparing.reference, comparing.archive_uuids, 0, kept.compare_reference);
    choose_archive(comparing.copy, comparing.archive_uuids, 0, kept.compare_copy);
    gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(comparing.progress), kept.compare_fraction);
    if (kept.compare_finished) {
        gtk_progress_bar_set_text(GTK_PROGRESS_BAR(comparing.progress), text("Done").c_str());
    }
    if (comparing.shown_report) {
        set_text(comparing.report, comparing.shown_report(text));
    }

    auto& history = state.history_view;
    choose_archive(history.archive, history.archive_uuids, 0, kept.history_archive);
    if (history.chain_result) {
        gtk_label_set_text(GTK_LABEL(history.chain), history.chain_result(text).c_str());
    }
    if (kept.runs_shown) {
        show_runs(state);
    }
}

} // namespace

void activate(GuiState& state, bool smoke_test)
{
    build_window(state);
    open_repository_session(state, false);
    // Settings says why no repository is open, or, once, that its database
    // was upgraded and where the copy from before the upgrade is (decision
    // 0032).
    if (!state.session || !state.session->notes.empty()) {
        gtk_stack_set_visible_child_name(GTK_STACK(state.stack), "settings");
    }

    if (smoke_test) {
        // Like the Settings drop-down, rebuild once this handler has returned:
        // in the other language and back, then quit. Nothing is shown and the
        // config file is not written.
        g_idle_add(
            +[](gpointer data) -> gboolean {
                auto& state = *static_cast<GuiState*>(data);
                guarded(
                    [&] {
                        if (!state.window) {
                            return;  // shutting down
                        }
                        const auto start = state.text.language();
                        const auto other = start == app::Language::English ? app::Language::TraditionalChinese
                                                                           : app::Language::English;
                        rebuild_window(state, other, false);
                        rebuild_window(state, start, false);
                    },
                    [&](const Report& report) {
                        g_printerr("%s\n", report(state.text).c_str());
                        state.exit_status = 1;
                    });
                g_application_quit(G_APPLICATION(state.application));
                return G_SOURCE_REMOVE;
            },
            &state);
        return;
    }
    gtk_window_present(state.window);
    if (state.screenshot_folder) {
        start_screenshot_tour(state);
    }
}

void use_gtk_language(app::Language language)
{
    // GTK translates its own texts with gettext, which takes LANGUAGE ahead of
    // the system language. The Windows package ships GTK's zh_TW catalogs.
    // (Changing the environment is unsafe while another thread reads it; a
    // language change is refused while an operation runs.)
    g_setenv("LANGUAGE", language == app::Language::TraditionalChinese ? "zh_TW" : "en", TRUE);
    // gettext keeps the translations it has already found and does not notice
    // a new LANGUAGE; setting the text domain again makes it look them up
    // afresh. Without this, GTK's own menus stay Chinese after a switch to English.
    textdomain(textdomain(nullptr));
}

void change_language(GuiState& state, app::Language language)
{
    // No window: the application is shutting down (see main).
    if (!state.window || language == state.text.language()) {
        return;
    }
    if (state.job.running()) {
        // An operation started after the choice was made (the drop-down is
        // locked while one runs). It reports into this window, so the window
        // stays and the drop-down shows the language in use again.
        show_language_in_use(state);
        return;
    }
    state.settings_view.notice = nullptr;
    if (auto saved = app::save_language(language, std::nullopt); !saved) {
        state.settings_view.notice = fixed_message(saved.error());
    }
    rebuild_window(state, language, true);
}

void rebuild_window(GuiState& state, app::Language language, bool show)
{
    // Read the old window before build_window points `state` at the new widgets.
    const Kept kept = keep(state);
    state.text = app::Text(language);
    use_gtk_language(language);

    // The new window is built before the old one closes, so the application
    // always has a window and keeps running.
    GtkWindow* old_window = state.window;
    build_window(state);
    const auto replace_old_window = [&] {
        gtk_stack_set_visible_child_name(GTK_STACK(state.stack), "settings");
        if (show) {
            gtk_window_present(state.window);
        }
        gtk_window_destroy(old_window);
    };
    try {
        refresh_settings(state);
        refresh_archive_list(state);
        refresh_archive_choices(state);
        restore(state, kept);
    } catch (...) {
        // `state` already points at the new window, so it replaces the old
        // one even when filling it failed; the caller reports the error.
        replace_old_window();
        throw;
    }
    replace_old_window();
}

} // namespace va::gui
