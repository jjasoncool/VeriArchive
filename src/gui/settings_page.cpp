// Settings (§B17, decision 0046), an AdwPreferencesPage: the repository (its
// RepositoryRoot, how it was selected, its uuid, the config file, where each
// copy of its database kept before an upgrade is, the warnings that hold, such
// as running elevated (R-12), and what opening it did), or why none is open,
// with Initialise repository; the interface language, locked while a job
// waits or works; the verification policy (the overdue limits, strict
// metadata, trusted keys); the backup reminder (R-86); and this program's
// version (decision 0037) with what it detects (R-02).
#include "gui/gui_app.h"

#include "common/utf8.h"

#include <iterator>
#include <memory>
#include <string>
#include <vector>

namespace va::gui {
namespace {

// The language row's items, in order; each is shown by its own name.
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

// A row's title is markup by default (AdwPreferencesRow); these show data,
// such as paths, as they are.
void set_plain_title(GtkWidget* row, const std::string& title)
{
    adw_preferences_row_set_use_markup(ADW_PREFERENCES_ROW(row), FALSE);
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), title.c_str());
}

// A setting: what it is, and under it its value, which can be selected and
// copied.
GtkWidget* value_row(const std::string& title, const std::string& value)
{
    GtkWidget* row = adw_action_row_new();
    set_plain_title(row, title);
    adw_action_row_set_subtitle(ADW_ACTION_ROW(row), value.c_str());
    adw_action_row_set_subtitle_selectable(ADW_ACTION_ROW(row), TRUE);
    gtk_widget_add_css_class(row, "property");
    return row;
}

// A sentence of its own (a warning, a note, where a copy of the database is),
// which can be selected and copied; with an icon, in the colour of `tone`,
// where they are given.
GtkWidget* sentence_row(const std::string& sentence, const char* icon = nullptr, const char* tone = nullptr)
{
    GtkWidget* row = adw_action_row_new();
    set_plain_title(row, sentence);
    adw_preferences_row_set_title_selectable(ADW_PREFERENCES_ROW(row), TRUE);
    if (icon) {
        GtkWidget* image = gtk_image_new_from_icon_name(icon);
        if (tone) {
            gtk_widget_add_css_class(image, tone);
        }
        adw_action_row_add_prefix(ADW_ACTION_ROW(row), image);
    }
    return row;
}

// Adds `widget` to `group` (a row to its list, anything else under it), to be
// taken out again by the next refresh.
void add(GuiState& state, GtkWidget* group, GtkWidget* widget)
{
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(group), widget);
    state.settings_page.rows.emplace_back(group, widget);
}

// A hint under a group's rows.
GtkWidget* hint(const std::string& text)
{
    GtkWidget* label = make_label(text);
    gtk_widget_add_css_class(label, "dim-label");
    gtk_widget_set_margin_top(label, 12);
    return label;
}

// Shows an unexpected error of a Settings handler in About.
void show_notice(GuiState& state, Report report)
{
    state.settings_page.notice = std::move(report);
    refresh_settings(state);
}

// Initialise repository: once this signal has returned, as a repository
// found at last may need its database upgraded, and the notice of that open
// takes the place of the pages, this button among them.
void initialise(GtkButton*, gpointer data)
{
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
}

} // namespace

void refresh_settings(GuiState& state)
{
    auto& page = state.settings_page;
    const auto& text = state.text;
    if (!page.repository) {
        return;
    }
    for (const auto& [group, row] : page.rows) {
        adw_preferences_group_remove(ADW_PREFERENCES_GROUP(group), row);
    }
    page.rows.clear();

    if (!state.session) {
        adw_preferences_group_set_description(
            ADW_PREFERENCES_GROUP(page.repository), text("No repository is open.").c_str());
        // Why, with what holds all the same (running elevated, R-12) and
        // where each copy of the database kept before an upgrade is in the
        // RepositoryRoot the open resolved, read again now (decision 0032):
        // where the repository cannot be opened, a copy is the way back.
        if (state.session_error) {
            add(state, page.repository, sentence_row(state.session_error(text), "dialog-warning-symbolic", "warning"));
        }
        GtkWidget* first_use = adw_action_row_new();
        set_plain_title(first_use,
            text("On first use, initialise a repository: it is created next to the program "
                 "(or in your home folder when that is not writable) and remembered in the config file."));
        GtkWidget* button = gtk_button_new_with_label(text("Initialise repository").c_str());
        gtk_widget_add_css_class(button, "suggested-action");
        gtk_widget_set_valign(button, GTK_ALIGN_CENTER);
        g_signal_connect(button, "clicked", G_CALLBACK(initialise), &state);
        adw_action_row_add_suffix(ADW_ACTION_ROW(first_use), button);
        add(state, page.repository, first_use);
        gtk_widget_set_visible(page.policy, FALSE);
    } else {
        adw_preferences_group_set_description(ADW_PREFERENCES_GROUP(page.repository), nullptr);
        const auto& context = state.session->context;
        add(state, page.repository, value_row(text("RepositoryRoot"), path_to_utf8(context.repo.root)));
        add(state, page.repository, value_row(text("Selected by"), text(context.repo_selected_by)));
        add(state, page.repository, value_row(text("repository_uuid"), context.repository_uuid));
        add(state, page.repository, value_row(text("Config file"), path_to_utf8(context.config_file)));
        // Where each copy of the database kept before an upgrade is, for as
        // long as it is in RepositoryRoot (decision 0032), not only in the
        // window whose open made it: the files themselves say it, read again
        // whenever Settings is shown.
        for (const auto& line : app::copy_before_upgrade_lines(text, context.repo)) {
            add(state, page.repository, sentence_row(line, "document-save-symbolic"));
        }
        for (const auto& warning : state.session->warnings) {
            add(state, page.repository,
                sentence_row(text.format("Warning: {}", text(warning)), "dialog-warning-symbolic", "warning"));
        }
        // What the open of this window's session did, said in this window:
        // the upgrade of the database, and where its copy is (decision 0032).
        for (const auto& note : state.session->notes) {
            add(state, page.repository,
                sentence_row(text.format("Note: {}", text(note)), "dialog-information-symbolic", "accent"));
        }
        add(state, page.repository,
            hint(text("Back up RepositoryRoot. Losing it loses all verification history; "
                      "manifests can be recovered only from media copies, if they were written.")));

        const auto& policy = context.config.policy;
        // A day in the singular, the days in the plural; as the config file
        // gives them, also where it gives no positive number.
        const auto days = [&](int count) {
            return count == 1 ? text.format("{} day", count) : text.format("{} days", count);
        };
        add(state, page.policy,
            value_row(text("Overdue after"), text.format("FULL verify {}, any verify {}",
                                                 days(policy.full_verify_max_age_days),
                                                 days(policy.any_verify_max_age_days))));
        add(state, page.policy, value_row(text("Strict metadata"), text(policy.strict_metadata ? "On" : "Off")));
        add(state, page.policy,
            value_row(text("Trusted signing keys"), text("Signed manifests are not part of this version.")));
        gtk_widget_set_visible(page.policy, TRUE);
    }

    // This program's version, as `veriarchive-cli --version` and `repo info`
    // show it (decision 0037), whether or not a repository is open, with what
    // it detects (R-02).
    GtkWidget* version = value_row(text.format("Program version: {}", state.host.app_version), text(guarantee));
    gtk_widget_remove_css_class(version, "property");
    add(state, page.about, version);
    if (page.notice) {
        add(state, page.about, sentence_row(page.notice(text), "dialog-warning-symbolic", "warning"));
    }
}

void show_language_in_use(GuiState& state)
{
    if (state.settings_page.language) {
        adw_combo_row_set_selected(ADW_COMBO_ROW(state.settings_page.language), index_of(state.text.language()));
    }
}

void lock_language(GuiState& state)
{
    GtkWidget* language = state.settings_page.language;
    if (!language) {
        return;
    }
    const bool locked = state.jobs.active() || state.job.running();
    const auto why = state.text("Wait for the jobs to end, or stop them, before changing the language.");
    gtk_widget_set_sensitive(language, !locked);
    adw_action_row_set_subtitle(ADW_ACTION_ROW(language), locked ? why.c_str() : "");
    // GTK 4 shows a tooltip on an unavailable widget too.
    gtk_widget_set_tooltip_text(language, locked ? why.c_str() : nullptr);
}

GtkWidget* build_settings_page(GuiState& state)
{
    auto& page = state.settings_page;
    const auto& text = state.text;
    page.rows.clear();
    GtkWidget* preferences = adw_preferences_page_new();
    const auto group = [&](const char* title) {
        GtkWidget* group = adw_preferences_group_new();
        adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(group), text(title).c_str());
        adw_preferences_page_add(ADW_PREFERENCES_PAGE(preferences), ADW_PREFERENCES_GROUP(group));
        return group;
    };
    page.repository = group("Repository");
    GtkWidget* interface = group("Interface");
    page.policy = group("Verification policy");
    page.about = group("About");

    // Each language is named in itself, and the row in both, so it can be
    // found in any language.
    page.language = adw_combo_row_new();
    set_plain_title(page.language, "Language / 語言");
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
    GtkStringList* model = gtk_string_list_new(name_pointers.data());
    adw_combo_row_set_model(ADW_COMBO_ROW(page.language), G_LIST_MODEL(model));
    g_object_unref(model);
    show_language_in_use(state);
    g_signal_connect(page.language, "notify::selected",
        G_CALLBACK(+[](GObject* source, GParamSpec*, gpointer data) {
            auto& state = *static_cast<GuiState*>(data);
            guarded(
                [&] {
                    const auto selected = adw_combo_row_get_selected(ADW_COMBO_ROW(source));
                    if (selected >= std::size(languages)) {  // GTK_INVALID_LIST_POSITION: nothing chosen
                        return;
                    }
                    const auto language = languages[selected];
                    if (language == state.text.language()) {
                        return;
                    }
                    if (state.jobs.active() || state.job.running()) {
                        // lock_language keeps this from happening; should it
                        // anyway, the jobs keep their window.
                        show_language_in_use(state);
                        return;
                    }
                    // Rebuild after this signal returns: the row belongs to
                    // the window being replaced.
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
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(interface), page.language);
    lock_language(state);
    return preferences;
}

} // namespace va::gui
