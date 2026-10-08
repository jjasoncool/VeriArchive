// Settings (§B17): the active RepositoryRoot and how it was selected, its uuid,
// the config file, trusted keys (V1.1), policy and the backup reminder (R-86).
#include "gui/gui_app.h"

#include "common/utf8.h"

#include <memory>

namespace va::gui {

void refresh_settings(GuiState& state)
{
    auto& view = state.settings_view;
    const auto& text = state.text;
    if (!view.info) {
        return;
    }
    std::string info;
    if (!state.session) {
        info = text("No repository is open.") + "\n\n" + text(state.session_error) + "\n\n" +
               text("On first use, initialise a repository: it is created next to the program "
                    "(or in your home folder when that is not writable) and remembered in the config file.");
        gtk_widget_set_visible(view.initialise, TRUE);
    } else {
        const auto& context = state.session->context;
        const auto& policy = context.config.policy;
        info = text.format("RepositoryRoot: {}", path_to_utf8(context.repo.root)) + "\n" +
               text.format("Selected by: {}", text(context.repo_selected_by)) + "\n" +
               text.format("repository_uuid: {}", context.repository_uuid) + "\n" +
               text.format("Config file: {}", path_to_utf8(context.config_file)) + "\n\n" +
               text.format("Overdue after: FULL verify {} days, any verify {} days",
                   policy.full_verify_max_age_days, policy.any_verify_max_age_days) + "\n" +
               text(policy.strict_metadata ? "Strict metadata: on" : "Strict metadata: off") + "\n\n" +
               text("Trusted signing keys: signed manifests arrive in V1.1.") + "\n\n" +
               text("Back up RepositoryRoot. Losing it loses all verification history; manifests "
                    "can be recovered only from media copies, if they were written.");
        for (const auto& warning : state.session->warnings) {
            info += "\n\n" + text.format("Warning: {}", text(warning));
        }
        gtk_widget_set_visible(view.initialise, FALSE);
    }
    if (!view.notice.empty()) {
        info += "\n\n" + text(view.notice);
    }
    gtk_label_set_text(GTK_LABEL(view.info), info.c_str());
}

GtkWidget* build_settings_view(GuiState& state)
{
    auto& view = state.settings_view;
    const auto& text = state.text;
    GtkWidget* box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    set_margins(box, 16);
    gtk_box_append(GTK_BOX(box), make_heading(text("Settings")));

    // Each language is named in itself, so it can be found in any language.
    GtkWidget* language_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_append(GTK_BOX(language_row), make_label("Language / 語言:"));
    const char* languages[] = {"English", "繁體中文", nullptr};
    view.language = gtk_drop_down_new_from_strings(languages);
    gtk_drop_down_set_selected(GTK_DROP_DOWN(view.language),
        state.text.language() == app::Language::TraditionalChinese ? 1 : 0);
    g_signal_connect(view.language, "notify::selected",
        G_CALLBACK(+[](GObject* source, GParamSpec*, gpointer data) {
            auto& state = *static_cast<GuiState*>(data);
            const auto language = gtk_drop_down_get_selected(GTK_DROP_DOWN(source)) == 1
                                      ? app::Language::TraditionalChinese
                                      : app::Language::English;
            if (state.job.running()) {
                // The running operation reports into this window; switch afterwards.
                gtk_drop_down_set_selected(GTK_DROP_DOWN(source),
                    state.text.language() == app::Language::TraditionalChinese ? 1 : 0);
                return;
            }
            // Rebuild after this signal returns: the drop-down belongs to the
            // window being replaced.
            struct Change {
                GuiState* state;
                app::Language language;
            };
            g_idle_add(
                +[](gpointer data) -> gboolean {
                    std::unique_ptr<Change> change(static_cast<Change*>(data));
                    change_language(*change->state, change->language);
                    return G_SOURCE_REMOVE;
                },
                new Change{&state, language});
        }),
        &state);
    gtk_box_append(GTK_BOX(language_row), view.language);
    gtk_box_append(GTK_BOX(box), language_row);

    view.info = make_label("", true);
    gtk_box_append(GTK_BOX(box), view.info);

    view.initialise = gtk_button_new_with_label(text("Initialise repository").c_str());
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
