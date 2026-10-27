// Settings (§B17): this program's version, the active RepositoryRoot and how it
// was selected, its uuid, the config file, trusted keys (V1.1), policy and the
// backup reminder (R-86).
#include "gui/gui_app.h"

#include "common/utf8.h"

#include <iterator>
#include <memory>
#include <string>
#include <vector>

namespace va::gui {
namespace {

// The language drop-down's items, in order; each is shown by its own name.
constexpr app::Language languages[] = {app::Language::English, app::Language::TraditionalChinese};

guint index_of(app::Language language)
{
    for (guint index = 0; index < std::size(languages); ++index) {
        if (languages[index] == language) {
            return index;
        }
    }
    return 0;
}

// Shows an unexpected error of a Settings handler below the settings.
void show_notice(GuiState& state, Report report)
{
    state.settings_view.notice = std::move(report);
    refresh_settings(state);
}

} // namespace

void refresh_settings(GuiState& state)
{
    auto& view = state.settings_view;
    const auto& text = state.text;
    if (!view.info) {
        return;
    }
    // This program's version, as `veriarchive-cli --version` and `repo info`
    // show it (decision 0037), whether or not a repository is open.
    std::string info = text.format("Program version: {}", state.host.app_version) + "\n\n";
    if (!state.session) {
        info += text("No repository is open.") + "\n\n" + (state.session_error ? state.session_error(text) : "") +
               "\n\n" +
               text("On first use, initialise a repository: it is created next to the program "
                    "(or in your home folder when that is not writable) and remembered in the config file.");
        gtk_widget_set_visible(view.initialise, TRUE);
    } else {
        const auto& context = state.session->context;
        const auto& policy = context.config.policy;
        // A day in the singular, the days in the plural; as the config file
        // gives them, also where it gives no positive number.
        const auto days = [&](int count) {
            return count == 1 ? text.format("{} day", count) : text.format("{} days", count);
        };
        info += text.format("RepositoryRoot: {}", path_to_utf8(context.repo.root)) + "\n" +
               text.format("Selected by: {}", text(context.repo_selected_by)) + "\n" +
               text.format("repository_uuid: {}", context.repository_uuid) + "\n" +
               text.format("Config file: {}", path_to_utf8(context.config_file)) + "\n\n" +
               text.format("Overdue after: FULL verify {}, any verify {}", days(policy.full_verify_max_age_days),
                   days(policy.any_verify_max_age_days)) +
               "\n" +
               text(policy.strict_metadata ? "Strict metadata: on" : "Strict metadata: off") + "\n\n" +
               text("Trusted signing keys: signed manifests are not part of this version.") + "\n\n" +
               text("Back up RepositoryRoot. Losing it loses all verification history; manifests "
                    "can be recovered only from media copies, if they were written.");
        for (const auto& warning : state.session->warnings) {
            info += "\n\n" + text.format("Warning: {}", text(warning));
        }
        // Where the copy of the database from before its upgrade is (decision
        // 0032), for as long as the window that upgraded it runs.
        for (const auto& note : state.session->notes) {
            info += "\n\n" + text.format("Note: {}", text(note));
        }
        gtk_widget_set_visible(view.initialise, FALSE);
    }
    if (view.notice) {
        info += "\n\n" + view.notice(text);
    }
    gtk_label_set_text(GTK_LABEL(view.info), info.c_str());
}

void show_language_in_use(GuiState& state)
{
    gtk_drop_down_set_selected(GTK_DROP_DOWN(state.settings_view.language), index_of(state.text.language()));
}

void lock_language(GuiState& state, bool locked)
{
    GtkWidget* language = state.settings_view.language;
    gtk_widget_set_sensitive(language, !locked);
    // GTK 4 shows a tooltip on an unavailable widget too, so it can say why.
    gtk_widget_set_tooltip_text(language,
        locked ? state.text("Finish or cancel the running operation before changing the language.").c_str()
               : nullptr);
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
    gtk_box_append(GTK_BOX(language_row), make_row_label("Language / 語言:"));
    std::vector<std::string> names;
    for (const auto language : languages) {
        names.emplace_back(app::language_name(language));
    }
    // GTK wants a list of C strings ending with nullptr; it copies them.
    std::vector<const char*> name_pointers;
    for (const auto& name : names) {
        name_pointers.push_back(name.c_str());
    }
    name_pointers.push_back(nullptr);
    view.language = gtk_drop_down_new_from_strings(name_pointers.data());
    show_language_in_use(state);
    g_signal_connect(view.language, "notify::selected",
        G_CALLBACK(+[](GObject* source, GParamSpec*, gpointer data) {
            auto& state = *static_cast<GuiState*>(data);
            guarded(
                [&] {
                    const auto selected = gtk_drop_down_get_selected(GTK_DROP_DOWN(source));
                    if (selected >= std::size(languages)) {  // GTK_INVALID_LIST_POSITION: nothing chosen
                        return;
                    }
                    const auto language = languages[selected];
                    if (language == state.text.language()) {
                        return;
                    }
                    if (state.job.running()) {
                        // lock_language keeps this from happening; should it
                        // anyway, the running operation keeps its window.
                        show_language_in_use(state);
                        return;
                    }
                    // Rebuild after this signal returns: the drop-down belongs
                    // to the window being replaced.
                    struct Change {
                        GuiState* state;
                        app::Language language;
                    };
                    g_idle_add(
                        +[](gpointer data) -> gboolean {
                            std::unique_ptr<Change> change(static_cast<Change*>(data));
                            auto& state = *change->state;
                            guarded([&] { change_language(state, change->language); },
                                [&](const Report& report) { show_notice(state, report); });
                            return G_SOURCE_REMOVE;
                        },
                        new Change{&state, language});
                },
                [&](const Report& report) { show_notice(state, report); });
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
            // Once this signal has returned: a repository found at last may
            // need its database upgraded, and the notice of that open takes
            // the place of the views, this button among them.
            g_idle_add(
                +[](gpointer data) -> gboolean {
                    auto& state = *static_cast<GuiState*>(data);
                    guarded(
                        [&] {
                            if (!state.window) {
                                return;  // shutting down
                            }
                            if (!open_with_upgrade_notice(state, true)) {
                                open_repository_session(state, true);
                            }
                        },
                        [&](const Report& report) { show_notice(state, report); });
                    return G_SOURCE_REMOVE;
                },
                data);
        }),
        &state);
    gtk_box_append(GTK_BOX(box), view.initialise);
    return box;
}

} // namespace va::gui
