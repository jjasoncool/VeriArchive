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

void activate(GuiState& state, bool smoke_test)
{
    GtkWidget* window = gtk_application_window_new(state.application);
    state.window = GTK_WINDOW(window);
    gtk_window_set_title(state.window, "VeriArchive");
    gtk_window_set_default_size(state.window, 1100, 720);

    GtkWidget* stack = gtk_stack_new();
    gtk_widget_set_hexpand(stack, TRUE);
    gtk_widget_set_vexpand(stack, TRUE);
    gtk_stack_add_titled(GTK_STACK(stack), build_archive_list_view(state), "archives", "Archive List");
    gtk_stack_add_titled(GTK_STACK(stack), build_register_view(state), "register", "Register Archive");
    gtk_stack_add_titled(GTK_STACK(stack), build_verify_view(state), "verify", "Verify Archive");
    gtk_stack_add_titled(GTK_STACK(stack), build_history_view(state), "history", "Verification History");
    gtk_stack_add_titled(GTK_STACK(stack), build_settings_view(state), "settings", "Settings");

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

    open_repository_session(state, false);
    if (!state.session) {
        gtk_stack_set_visible_child_name(GTK_STACK(stack), "settings");
    }

    if (smoke_test) {
        g_application_quit(G_APPLICATION(state.application));
        return;
    }
    gtk_window_present(state.window);
}

} // namespace va::gui
