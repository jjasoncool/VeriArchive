// Window layout: a sidebar to switch between the five views, and the session
// (repository) shared by all of them.
#include "gui/gui_app.h"

namespace va::gui {

void open_repository_session(GuiState& state, bool initialise)
{
    auto session = app::open_session({initialise, std::nullopt, std::nullopt});
    if (session) {
        state.session = std::move(*session);
        state.session_error.clear();
    } else {
        state.session.reset();
        state.session_error = session.error();
    }
    refresh_settings(state);
    refresh_archive_list(state);
    refresh_archive_choices(state);
}

namespace {

// Creates the window with the sidebar and the five views, in state.text's language.
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
    gtk_stack_add_titled(GTK_STACK(stack), build_history_view(state), "history",
        text("Verification History").c_str());
    gtk_stack_add_titled(GTK_STACK(stack), build_settings_view(state), "settings",
        text("Settings").c_str());

    // Switching back to the list shows the latest results.
    g_signal_connect(stack, "notify::visible-child",
        G_CALLBACK(+[](GObject* source, GParamSpec*, gpointer data) {
            const char* name = gtk_stack_get_visible_child_name(GTK_STACK(source));
            if (name && std::string(name) == "archives") {
                refresh_archive_list(*static_cast<GuiState*>(data));
            }
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

} // namespace

void activate(GuiState& state, bool smoke_test)
{
    build_window(state);
    open_repository_session(state, false);
    if (!state.session) {
        gtk_stack_set_visible_child_name(GTK_STACK(state.stack), "settings");
    }

    if (smoke_test) {
        g_application_quit(G_APPLICATION(state.application));
        return;
    }
    gtk_window_present(state.window);
}

void use_gtk_language(app::Language language)
{
    // GTK translates its own texts with gettext, which takes LANGUAGE ahead of
    // the system language. The Windows package ships GTK's zh_TW catalogs.
    g_setenv("LANGUAGE", language == app::Language::TraditionalChinese ? "zh_TW" : "en", TRUE);
}

void change_language(GuiState& state, app::Language language)
{
    if (language == state.text.language()) {
        return;
    }
    state.settings_view.notice.clear();
    if (auto saved = app::save_language(language, std::nullopt); !saved) {
        state.settings_view.notice = saved.error();
    }
    state.text = app::Text(language);
    use_gtk_language(language);

    // The new window is built before the old one closes, so the application
    // always has a window and keeps running.
    GtkWindow* old_window = state.window;
    build_window(state);
    refresh_settings(state);
    refresh_archive_list(state);
    refresh_archive_choices(state);
    gtk_stack_set_visible_child_name(GTK_STACK(state.stack), "settings");
    gtk_window_present(state.window);
    gtk_window_destroy(old_window);
}

} // namespace va::gui
