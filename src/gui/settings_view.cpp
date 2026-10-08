// Settings (§B17): the active RepositoryRoot and how it was selected, its uuid,
// the config file, trusted keys (V1.1), policy and the backup reminder (R-86).
#include "gui/gui_app.h"

#include "common/utf8.h"

namespace va::gui {

void refresh_settings(GuiState& state)
{
    auto& view = state.settings_view;
    if (!view.info) {
        return;
    }
    std::string text;
    if (!state.session) {
        text = "No repository is open.\n\n" + state.session_error +
               "\n\nOn first use, initialise a repository: it is created next to the program "
               "(or in your home folder when that is not writable) and remembered in the config file.";
        gtk_widget_set_visible(view.initialise, TRUE);
    } else {
        const auto& context = state.session->context;
        const auto& policy = context.config.policy;
        text = "RepositoryRoot: " + path_to_utf8(context.repo.root) +
               "\nSelected by: " + context.repo_selected_by +
               "\nrepository_uuid: " + context.repository_uuid +
               "\nConfig file: " + path_to_utf8(context.config_file) +
               "\n\nOverdue after: FULL verify " + std::to_string(policy.full_verify_max_age_days) +
               " days, any verify " + std::to_string(policy.any_verify_max_age_days) + " days" +
               "\nStrict metadata: " + (policy.strict_metadata ? "on" : "off") +
               "\n\nTrusted signing keys: signed manifests arrive in V1.1." +
               "\n\nBack up RepositoryRoot. Losing it loses all verification history; manifests "
               "can be recovered only from media copies, if they were written.";
        for (const auto& warning : state.session->warnings) {
            text += "\n\nWarning: " + warning;
        }
        gtk_widget_set_visible(view.initialise, FALSE);
    }
    gtk_label_set_text(GTK_LABEL(view.info), text.c_str());
}

GtkWidget* build_settings_view(GuiState& state)
{
    auto& view = state.settings_view;
    GtkWidget* box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    set_margins(box, 16);
    gtk_box_append(GTK_BOX(box), make_heading("Settings"));
    view.info = make_label("", true);
    gtk_box_append(GTK_BOX(box), view.info);

    view.initialise = gtk_button_new_with_label("Initialise repository");
    gtk_widget_set_halign(view.initialise, GTK_ALIGN_START);
    g_signal_connect(view.initialise, "clicked",
        G_CALLBACK(+[](GtkButton*, gpointer data) {
            open_repository_session(*static_cast<GuiState*>(data), true);
        }),
        &state);
    gtk_box_append(GTK_BOX(box), view.initialise);
    return box;
}

} // namespace va::gui
