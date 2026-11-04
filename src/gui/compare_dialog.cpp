// The comparison dialog (decisions 0020, 0046): checks that a copy (B) holds
// the same entries as its reference (A), using the manifests of both
// archives' current revisions in the repository. No media is read, so a
// comparison is quick; what it compares is what each registration read.
// Whether the media still hold it is what a FULL verification of each archive
// proves. Opened from an archive's details, its menu and its Copies, with A
// and B chosen; B lists the copies of A first. A and B the same archive is
// refused in the dialog. Compare runs it as a job (decision 0045), whose
// details show the report, and a toast says its result.
#include "gui/gui_app.h"

#include "gui/compare_choices.h"
#include "platform/file_system.h"

#include <algorithm>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace va::gui {
namespace {

// An archive as a job's title names it: as Archives lists it (display_label),
// or by the start of its uuid.
std::string label_of(const GuiState& state, const Uuid& archive_uuid)
{
    for (const auto& status : state.archives_page.statuses) {
        if (status.archive.archive_uuid == archive_uuid) {
            return display_label(status);
        }
    }
    return archive_uuid.substr(0, 8);
}

void start_comparison(GuiState& state, const Uuid& reference, const Uuid& copy)
{
    if (!state.session) {
        show_message(state, fixed_message("Open a repository first (Settings)."));
        return;
    }
    // The worker returns its report unworded; it is worded when shown, in the
    // language in use then. A comparison reads no media: it waits for no
    // device (decision 0045).
    JobSpec job;
    job.title = [a = label_of(state, reference), b = label_of(state, copy)](const app::Text& text) {
        return text.format("Compare {} with {}", a, b);
    };
    job.counting = Counting::Entries;
    // What the toast says once it ended: its result, or that it stopped.
    struct Ended {
        std::string result;
        bool stopped = false;
    };
    auto ended = std::make_shared<Ended>();
    job.work = [&state, a = reference, b = copy, ended](JobContext& context) -> Report {
        // Not compared, until a result says otherwise.
        context.tone = JobTone::Error;
        auto& session = *state.session;
        auto store = ArchiveStore::open(session.context.repo);
        if (!store) {
            return [message = store.error().message](const app::Text& text) {
                return text.format("Cannot open the repository: {}", text(message));
            };
        }
        const auto repository_files = platform::make_file_system_access();
        auto comparison =
            compare_archives(*store, *repository_files, session.context.repo, a, b, context.cancel, context.progress);
        if (!comparison) {
            if (comparison.error().failure_class == "Cancelled") {
                context.tone = JobTone::Stopped;
                ended->stopped = true;
            }
            return [failure = comparison.error()](const app::Text& text) {
                return comparison_failure_report(text, failure);
            };
        }
        context.tone = comparison_tone(comparison->result);
        ended->result = comparison->result;
        return [shown = shown_comparison(std::move(*comparison))](const app::Text& text) {
            return comparison_report(text, shown);
        };
    };
    job.done = [&state, ended](std::uint64_t id, const Report&) {
        guarded(
            [&] {
                const auto* job = state.jobs.find(id);
                const auto title = job && job->title ? job->title(state.text) : std::string{};
                show_toast(state,
                    ended->result.empty() ? not_compared_toast(state.text, title, ended->stopped)
                                          : compared_toast(state.text, title, ended->result),
                    ToastAction{state.text("Details"), [id](GuiState& state) { open_job_details(state, id); }});
            },
            [&](const Report& error) { show_archives_status(state, error); });
    };
    const auto& text = state.text;
    const auto title = job.title(text);
    const auto id = state.jobs.start(text, std::move(job));
    job_changed(state, id);
    show_toast(state, text.format("{}: started", title),
        ToastAction{text("Details"), [id](GuiState& state) { open_job_details(state, id); }}, ToastKind::Started);
}

using Dialog = GuiState::CompareDialog;

// Puts `archives` into the combo row `row`, as Archives names them with the
// start of their uuids (choice_words), and chooses `chosen`, or the first
// where it is not listed.
void fill(GuiState& state, GtkWidget* row, const std::vector<Uuid>& archives, const std::optional<Uuid>& chosen)
{
    const auto names = archive_names(state.archives_page.statuses, state.identity_shown);
    std::vector<std::string> items;
    guint selected = 0;
    for (const auto& archive : archives) {
        const auto name = std::ranges::find(names, archive, &ArchiveName::archive_uuid);
        items.push_back(name != names.end() ? choice_words(state.text, *name) : archive.substr(0, 8));
        if (chosen == archive) {
            selected = static_cast<guint>(items.size() - 1);
        }
    }
    std::vector<const char*> pointers;
    for (const auto& item : items) {
        pointers.push_back(item.c_str());
    }
    pointers.push_back(nullptr);
    GtkStringList* model = gtk_string_list_new(pointers.data());
    adw_combo_row_set_model(ADW_COMBO_ROW(row), G_LIST_MODEL(model));
    g_object_unref(model);
    if (!archives.empty()) {
        adw_combo_row_set_selected(ADW_COMBO_ROW(row), selected);
    }
}

std::optional<Uuid> chosen_in(GtkWidget* row, const std::vector<Uuid>& archives)
{
    const auto selected = adw_combo_row_get_selected(ADW_COMBO_ROW(row));
    return selected != GTK_INVALID_LIST_POSITION && selected < archives.size() ? std::optional(archives[selected])
                                                                                : std::nullopt;
}

// Compare where A and B can be compared, or why not.
void show_refusal(GuiState& state)
{
    auto& dialog = state.compare_dialog;
    const auto refusal = comparison_refusal(
        state.text, chosen_in(dialog.reference, dialog.references), chosen_in(dialog.copy, dialog.copies));
    gtk_label_set_text(GTK_LABEL(dialog.refusal_words), refusal ? refusal->c_str() : "");
    gtk_widget_set_visible(dialog.refusal, refusal.has_value());
    gtk_widget_set_sensitive(dialog.start, !refusal);
}

// B's list for the A chosen: the copies of A first; B as `wanted` where it
// is listed and is not A, otherwise a copy of A or another archive.
void fill_copies(GuiState& state, const std::optional<Uuid>& wanted)
{
    auto& dialog = state.compare_dialog;
    const auto reference = chosen_in(dialog.reference, dialog.references);
    const auto& statuses = state.archives_page.statuses;
    dialog.copies = reference ? copy_candidates(statuses, *reference) : dialog.references;
    const auto copy = reference ? default_copy(statuses, *reference, wanted) : wanted;
    dialog.filling = true;
    fill(state, dialog.copy, dialog.copies, copy);
    dialog.filling = false;
    show_refusal(state);
}

// Both lists as Archives read them last, A and B as given where listed.
void fill_both(GuiState& state, const std::optional<Uuid>& reference, const std::optional<Uuid>& copy)
{
    auto& dialog = state.compare_dialog;
    dialog.references.clear();
    for (const auto& status : state.archives_page.statuses) {
        dialog.references.push_back(status.archive.archive_uuid);
    }
    dialog.filling = true;
    fill(state, dialog.reference, dialog.references, reference);
    dialog.filling = false;
    fill_copies(state, copy);
}

GtkWidget* combo_row(const std::string& title)
{
    GtkWidget* row = adw_combo_row_new();
    adw_preferences_row_set_use_markup(ADW_PREFERENCES_ROW(row), FALSE);
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), title.c_str());
    // The archive chosen with its volume label, not cut after a few letters.
    show_whole_items(row);
    return row;
}

} // namespace

void compare_with(GuiState& state, const Uuid& reference, const std::optional<Uuid>& copy)
{
    if (!state.window) {
        return;
    }
    if (!state.session) {
        show_message(state, fixed_message("Open a repository first (Settings)."));
        return;
    }
    close_dialogs(state);
    auto& dialog = state.compare_dialog;
    const auto& text = state.text;
    AdwDialog* shown = adw_dialog_new();
    dialog.dialog = GTK_WIDGET(shown);
    adw_dialog_set_title(shown, text("Compare Archives").c_str());
    adw_dialog_set_content_width(shown, 620);

    GtkWidget* view = adw_toolbar_view_new();
    adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(view), adw_header_bar_new());
    GtkWidget* box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 18);
    gtk_widget_set_margin_start(box, 24);
    gtk_widget_set_margin_end(box, 24);
    gtk_widget_set_margin_top(box, 6);
    gtk_widget_set_margin_bottom(box, 12);
    gtk_box_append(GTK_BOX(box),
        make_words(text("Comparison checks that two registered archives hold the same entries with the same content, "
                        "using their manifests in the repository; no media is read. Choose the reference archive as A "
                        "and the copy to check as B."),
            "dim-label"));
    GtkWidget* group = adw_preferences_group_new();
    dialog.reference = combo_row(text("Reference archive (A)"));
    dialog.copy = combo_row(text("Copy to check (B)"));
    adw_action_row_set_subtitle(
        ADW_ACTION_ROW(dialog.copy), text("The copies of A come first.").c_str());
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(group), dialog.reference);
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(group), dialog.copy);
    gtk_box_append(GTK_BOX(box), group);
    dialog.refusal = make_line_box("va-error", "dialog-warning-symbolic", "error");
    dialog.refusal_words = make_words("");
    gtk_widget_set_hexpand(dialog.refusal_words, TRUE);
    gtk_box_append(GTK_BOX(dialog.refusal), dialog.refusal_words);
    gtk_widget_set_visible(dialog.refusal, FALSE);
    gtk_box_append(GTK_BOX(box), dialog.refusal);
    adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(view), box);

    GtkWidget* buttons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_widget_set_margin_start(buttons, 24);
    gtk_widget_set_margin_end(buttons, 24);
    gtk_widget_set_margin_top(buttons, 12);
    gtk_widget_set_margin_bottom(buttons, 18);
    GtkWidget* cancel = gtk_button_new_with_label(text("Cancel").c_str());
    g_signal_connect_swapped(cancel, "clicked", G_CALLBACK(adw_dialog_close), shown);
    gtk_box_append(GTK_BOX(buttons), cancel);
    GtkWidget* gap = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_hexpand(gap, TRUE);
    gtk_box_append(GTK_BOX(buttons), gap);
    dialog.start = gtk_button_new_with_label(text("Compare").c_str());
    gtk_widget_add_css_class(dialog.start, "suggested-action");
    g_signal_connect(dialog.start, "clicked", G_CALLBACK(+[](GtkButton*, gpointer data) {
        auto& state = *static_cast<GuiState*>(data);
        guarded(
            [&] {
                auto& dialog = state.compare_dialog;
                const auto reference = chosen_in(dialog.reference, dialog.references);
                const auto copy = chosen_in(dialog.copy, dialog.copies);
                if (comparison_refusal(state.text, reference, copy)) {
                    show_refusal(state);
                    return;
                }
                // Forgotten at once (as close_dialogs does): it closes once
                // its animation has ended, and nothing may fill it meanwhile.
                GtkWidget* closing = std::exchange(state.compare_dialog, {}).dialog;
                adw_dialog_close(ADW_DIALOG(closing));
                start_comparison(state, *reference, *copy);
            },
            [&](const Report& report) { show_message(state, report); });
    }), &state);
    gtk_box_append(GTK_BOX(buttons), dialog.start);
    adw_toolbar_view_add_bottom_bar(ADW_TOOLBAR_VIEW(view), buttons);
    adw_dialog_set_child(shown, view);
    adw_dialog_set_default_widget(shown, dialog.start);

    fill_both(state, reference, copy);
    g_signal_connect(dialog.reference, "notify::selected", G_CALLBACK(+[](GObject*, GParamSpec*, gpointer data) {
        auto& state = *static_cast<GuiState*>(data);
        auto& dialog = state.compare_dialog;
        if (dialog.filling) {
            return;
        }
        // B's list for the new A, B kept where it can be.
        guarded([&] { fill_copies(state, chosen_in(dialog.copy, dialog.copies)); },
            [&](const Report& report) { show_message(state, report); });
    }), &state);
    g_signal_connect(dialog.copy, "notify::selected", G_CALLBACK(+[](GObject*, GParamSpec*, gpointer data) {
        auto& state = *static_cast<GuiState*>(data);
        if (!state.compare_dialog.filling) {
            guarded([&] { show_refusal(state); }, [&](const Report& report) { show_message(state, report); });
        }
    }), &state);
    g_signal_connect(shown, "closed", G_CALLBACK(+[](AdwDialog* closed, gpointer data) {
        auto& state = *static_cast<GuiState*>(data);
        if (state.compare_dialog.dialog == GTK_WIDGET(closed)) {
            state.compare_dialog = {};
        }
    }), &state);
    adw_dialog_present(shown, GTK_WIDGET(state.window));
    gtk_widget_grab_focus(dialog.start);
}

void refresh_compare_dialog(GuiState& state)
{
    auto& dialog = state.compare_dialog;
    if (!dialog.dialog) {
        return;
    }
    fill_both(state, chosen_in(dialog.reference, dialog.references), chosen_in(dialog.copy, dialog.copies));
}

} // namespace va::gui
