// Jobs (decisions 0045, 0046 and 0049): every registration, verification and
// comparison started in the window, in the groups Running, Waiting and Ended,
// each with its title, how far it is, its device alert, its bar while it
// runs, Stop until it ended, View result once a verification ended having
// recorded a run (its result page), and Details, a dialog of its own with its
// progress and, once it ended, its report and View result too; an ended
// job's result shows in its tone. The header's jobs button shows how far the
// jobs that run are on a ring, counts the jobs that run or wait and lists
// them, with the last ended
// ones, in its popover. An archive's details in Archives show the job of its
// verification too, as a card of its row. Below the jobs, the unfinished
// operations (decision 0022), wherever they were started, with Resume and
// Abandon... where they have stopped (unfinished_actions.cpp): Jobs lists
// them since decision 0049, so that what was started and has not ended is in
// one place; a job's Stop keeps what it did, and Resume there goes on from
// it. The jobs button and the Jobs row count those that stopped.
#include "gui/gui_app.h"

#include "common/utf8.h"
#include "platform/media_enumerator.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <limits>
#include <string>
#include <string_view>
#include <utility>

namespace va::gui {
namespace {

// The ended jobs the header's popover lists, the last ones.
constexpr std::size_t popover_ended = 3;

// A job's progress on a bar that says it in words (its details, the view
// that started it): while it waits, its state; once it ended, as far as it
// came, and Done.
void show_bar(const app::Text& text, const JobShown& job, GtkWidget* bar)
{
    auto* progress = GTK_PROGRESS_BAR(bar);
    if (job.state == JobState::Ended) {
        gtk_progress_bar_set_fraction(progress, job.bar.pulse ? 0 : job.bar.fraction);
        gtk_progress_bar_set_text(progress, text("Done").c_str());
        return;
    }
    if (job.state == JobState::Waiting) {
        gtk_progress_bar_set_fraction(progress, 0);
        gtk_progress_bar_set_text(progress, state_words(text, job).c_str());
        return;
    }
    if (job.bar.pulse) {
        gtk_progress_bar_pulse(progress);
    } else {
        gtk_progress_bar_set_fraction(progress, job.bar.fraction);
    }
    gtk_progress_bar_set_text(progress, job.bar.text.c_str());
}

// A row's bar, whose line above says the words: shown while the job runs.
void show_row_bar(const JobShown& job, GtkWidget* bar)
{
    const bool running = group_of(job) == JobGroup::Running;
    gtk_widget_set_visible(bar, running);
    if (!running) {
        return;
    }
    if (job.bar.pulse) {
        gtk_progress_bar_pulse(GTK_PROGRESS_BAR(bar));
    } else {
        gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(bar), job.bar.fraction);
    }
}

void show_job_alert(const JobShown& job, GtkWidget* label)
{
    if (job.alert.empty()) {
        hide_alert(label);
    } else {
        show_alert(label, job.alert);
    }
}

std::string title_of(const app::Text& text, const JobShown& job)
{
    return job.title ? job.title(text) : std::string{};
}

// The line under a job's title in the tone of its result once it ended, after
// the tone's icon, so the result is not told by colour alone (decision 0046);
// dim before.
void show_tone(const GuiState::JobRow& row, const JobShown& job)
{
    const auto tone = job.state == JobState::Ended ? job.tone : JobTone::None;
    const std::string_view wanted = tone_style(tone);
    // Only what changes: a running job's row is updated with each progress.
    for (GtkWidget* widget : {row.meta, row.tone}) {
        for (const auto any : {JobTone::Success, JobTone::Warning, JobTone::Error, JobTone::None}) {
            if (tone_style(any) != wanted) {
                gtk_widget_remove_css_class(widget, tone_style(any));
            }
        }
        gtk_widget_add_css_class(widget, wanted.data());
    }
    const char* icon = tone_icon(tone);
    if (icon) {
        gtk_image_set_from_icon_name(GTK_IMAGE(row.tone), icon);
    }
    gtk_widget_set_visible(row.tone, icon != nullptr);
}

// Whether `job` ended having recorded a run, whose result page View result opens.
bool recorded_a_run(const GuiState& state, const JobShown& job)
{
    return job.state == JobState::Ended && state.job_runs.contains(job.id);
}

void update_row(GuiState& state, const GuiState::JobRow& row, const JobShown& job)
{
    const auto& text = state.text;
    const auto title = title_of(text, job);
    gtk_label_set_text(GTK_LABEL(row.title), title.c_str());
    gtk_widget_set_tooltip_text(row.title, title.c_str());
    gtk_label_set_text(GTK_LABEL(row.meta), job_meta(text, job).c_str());
    show_tone(row, job);
    show_row_bar(job, row.bar);
    show_job_alert(job, row.alert);
    // Stop does not ask first (decision 0045): a job stopped can be resumed.
    gtk_widget_set_visible(row.stop, job.state != JobState::Ended);
    gtk_widget_set_sensitive(row.stop, !job.stopping);
    gtk_widget_set_visible(row.result, recorded_a_run(state, job));
}

void update_details(GuiState& state, const JobShown& job)
{
    const auto found = state.jobs_page.details.find(job.id);
    if (found == state.jobs_page.details.end()) {
        return;
    }
    const auto& details = found->second;
    const auto& text = state.text;
    gtk_label_set_text(GTK_LABEL(details.state), state_words(text, job).c_str());
    show_bar(text, job, details.bar);
    show_job_alert(job, details.alert);
    // Until it ended, as on its row; then View result, where it recorded a run.
    gtk_widget_set_visible(details.stop, job.state != JobState::Ended);
    gtk_widget_set_sensitive(details.stop, !job.stopping);
    gtk_widget_set_visible(details.result, recorded_a_run(state, job));
    set_text(details.report, job.state == JobState::Ended && job.report
                                 ? job.report(text)
                                 : text("The report shows here once the job has ended."));
}

// The view that started `job`: an archive's details, its card. (Register
// Archive and the comparison dialog close once their job starts: Jobs and the
// header's popover show it.)
void update_view(GuiState& state, const JobShown& job)
{
    // The card shows the job in whichever group it is now.
    const auto& card = state.archives_page.card.rows;
    if (const auto found = card.find(job.id); found != card.end()) {
        update_row(state, found->second, job);
    }
}

struct JobButton {
    GuiState* state;
    std::uint64_t job;
};

// Connects `signal` of `widget` to `handler`, which gets the state and the job.
void connect_job(GtkWidget* widget, const char* signal, GCallback handler, GuiState& state, std::uint64_t job)
{
    g_signal_connect_data(widget, signal, handler, new JobButton{&state, job},
        +[](gpointer data, GClosure*) { delete static_cast<JobButton*>(data); }, GConnectFlags{});
}

void connect_stop(GtkWidget* button, GuiState& state, std::uint64_t job)
{
    connect_job(button, "clicked", G_CALLBACK(+[](GtkButton*, gpointer data) {
        auto* pressed = static_cast<JobButton*>(data);
        pressed->state->jobs.stop(pressed->job);
    }),
        state, job);
}

// View result of an ended verification: the result page of the run it
// recorded, over Archives, in place of the popover or of the job's details.
void connect_view_result(GtkWidget* button, GuiState& state, std::uint64_t job)
{
    connect_job(button, "clicked", G_CALLBACK(+[](GtkButton*, gpointer data) {
        auto* pressed = static_cast<JobButton*>(data);
        auto& state = *pressed->state;
        // Copies: closing the details takes this button away.
        const auto ended = pressed->job;
        guarded(
            [&] {
                const auto found = state.job_runs.find(ended);
                if (found == state.job_runs.end()) {
                    return;
                }
                const auto run = found->second;
                if (state.jobs_page.popover) {
                    gtk_popover_popdown(GTK_POPOVER(state.jobs_page.popover));
                }
                if (const auto open = state.jobs_page.details.find(ended); open != state.jobs_page.details.end()) {
                    adw_dialog_close(ADW_DIALOG(open->second.dialog));
                }
                open_result_of(state, run.archive_uuid, run.run_id);
            },
            [&](const Report& report) { show_message(state, report); });
    }),
        state, job);
}

// A job's row: its title, under it how far it is (job_meta) or, once it
// ended, its result in its tone, Stop and Details beside them, its device
// alert, its bar. `compact`: the popover's, whose title stays on one line.
GtkWidget* make_row(GuiState& state, GuiState::JobList& list, const JobShown& job, bool compact)
{
    const auto& text = state.text;
    GuiState::JobRow row;
    row.group = group_of(job);
    GtkWidget* list_row = gtk_list_box_row_new();
    gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(list_row), FALSE);
    gtk_list_box_row_set_selectable(GTK_LIST_BOX_ROW(list_row), FALSE);
    GtkWidget* grid = gtk_grid_new();
    gtk_grid_set_column_spacing(GTK_GRID(grid), 12);
    gtk_grid_set_row_spacing(GTK_GRID(grid), 6);
    gtk_widget_set_margin_start(grid, compact ? 12 : 14);
    gtk_widget_set_margin_end(grid, compact ? 12 : 14);
    gtk_widget_set_margin_top(grid, compact ? 8 : 12);
    gtk_widget_set_margin_bottom(grid, compact ? 8 : 12);

    GtkWidget* words = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_widget_set_hexpand(words, TRUE);
    gtk_widget_set_valign(words, GTK_ALIGN_CENTER);
    row.title = gtk_label_new(nullptr);
    gtk_label_set_xalign(GTK_LABEL(row.title), 0.0f);
    gtk_widget_add_css_class(row.title, "heading");
    if (compact) {
        gtk_label_set_ellipsize(GTK_LABEL(row.title), PANGO_ELLIPSIZE_MIDDLE);
    } else {
        gtk_label_set_wrap(GTK_LABEL(row.title), TRUE);
        gtk_label_set_wrap_mode(GTK_LABEL(row.title), PANGO_WRAP_WORD_CHAR);
    }
    row.meta = gtk_label_new(nullptr);
    gtk_label_set_xalign(GTK_LABEL(row.meta), 0.0f);
    gtk_label_set_wrap(GTK_LABEL(row.meta), TRUE);
    gtk_label_set_wrap_mode(GTK_LABEL(row.meta), PANGO_WRAP_WORD_CHAR);
    gtk_widget_set_hexpand(row.meta, TRUE);
    gtk_widget_add_css_class(row.meta, "numeric");
    if (compact) {
        gtk_widget_add_css_class(row.meta, "caption");
    }
    row.tone = gtk_image_new();
    gtk_widget_set_valign(row.tone, GTK_ALIGN_START);
    GtkWidget* meta_line = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_box_append(GTK_BOX(meta_line), row.tone);
    gtk_box_append(GTK_BOX(meta_line), row.meta);
    gtk_box_append(GTK_BOX(words), row.title);
    gtk_box_append(GTK_BOX(words), meta_line);

    GtkWidget* buttons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_set_valign(buttons, GTK_ALIGN_CENTER);
    row.stop = gtk_button_new_with_label(text("Stop").c_str());
    connect_stop(row.stop, state, job.id);
    row.result = gtk_button_new_with_label(text("View result").c_str());
    connect_view_result(row.result, state, job.id);
    row.details = gtk_button_new_with_label(text("Details").c_str());
    gtk_widget_add_css_class(row.details, "flat");
    connect_job(row.details, "clicked", G_CALLBACK(+[](GtkButton*, gpointer data) {
        auto* pressed = static_cast<JobButton*>(data);
        auto& state = *pressed->state;
        guarded(
            [&] {
                // From the popover: the dialog takes its place.
                if (state.jobs_page.popover) {
                    gtk_popover_popdown(GTK_POPOVER(state.jobs_page.popover));
                }
                open_job_details(state, pressed->job);
            },
            [](const Report&) {});
    }),
        state, job.id);
    gtk_box_append(GTK_BOX(buttons), row.stop);
    gtk_box_append(GTK_BOX(buttons), row.result);
    gtk_box_append(GTK_BOX(buttons), row.details);

    row.alert = make_alert_label();
    row.bar = gtk_progress_bar_new();
    gtk_widget_set_valign(row.bar, GTK_ALIGN_CENTER);

    gtk_grid_attach(GTK_GRID(grid), words, 0, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), buttons, 1, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), row.alert, 0, 1, 2, 1);
    gtk_grid_attach(GTK_GRID(grid), row.bar, 0, 2, 2, 1);
    gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(list_row), grid);
    // A screen reader names the job each button is for, as several are
    // listed. A job's title stays; a language change makes the rows again.
    const auto title = title_of(text, job);
    gtk_accessible_update_property(
        GTK_ACCESSIBLE(row.stop), GTK_ACCESSIBLE_PROPERTY_LABEL, text.format("Stop {}", title).c_str(), -1);
    gtk_accessible_update_property(GTK_ACCESSIBLE(row.details), GTK_ACCESSIBLE_PROPERTY_LABEL,
        text.format("Details of {}", title).c_str(), -1);
    gtk_accessible_update_property(GTK_ACCESSIBLE(row.result), GTK_ACCESSIBLE_PROPERTY_LABEL,
        text.format("Result of {}", title).c_str(), -1);
    update_row(state, row, job);
    list.rows[job.id] = row;
    return list_row;
}

// The jobs of `list` again, in their groups: the Jobs page every job, the
// popover (`compact`) those that run or wait and the last ended ones.
void show_list(GuiState& state, GuiState::JobList& list, bool compact)
{
    if (!list.box) {
        return;
    }
    while (GtkWidget* child = gtk_widget_get_first_child(list.box)) {
        gtk_box_remove(GTK_BOX(list.box), child);
    }
    list.rows.clear();
    const auto& text = state.text;
    const auto grouped =
        group_jobs(state.jobs.shown(), compact ? popover_ended : std::numeric_limits<std::size_t>::max());
    if (grouped.empty()) {
        GtkWidget* empty = gtk_label_new(text("No jobs yet.").c_str());
        gtk_widget_add_css_class(empty, "dim-label");
        set_margins(empty, compact ? 24 : 48);
        gtk_box_append(GTK_BOX(list.box), empty);
        return;
    }
    const std::pair<const char*, const std::vector<const JobShown*>*> groups[] = {
        {"Running", &grouped.running}, {"Waiting", &grouped.waiting}, {"Ended", &grouped.ended}};
    for (const auto& [name, jobs] : groups) {
        if (jobs->empty()) {
            continue;
        }
        if (compact) {
            GtkWidget* heading = gtk_label_new(text(name).c_str());
            gtk_label_set_xalign(GTK_LABEL(heading), 0.0f);
            gtk_widget_add_css_class(heading, "caption-heading");
            gtk_widget_add_css_class(heading, "dim-label");
            gtk_widget_set_margin_start(heading, 6);
            gtk_widget_set_margin_top(heading, 6);
            GtkWidget* rows = gtk_list_box_new();
            gtk_list_box_set_selection_mode(GTK_LIST_BOX(rows), GTK_SELECTION_NONE);
            gtk_widget_add_css_class(rows, "boxed-list");
            for (const auto* job : *jobs) {
                gtk_list_box_append(GTK_LIST_BOX(rows), make_row(state, list, *job, true));
            }
            gtk_box_append(GTK_BOX(list.box), heading);
            gtk_box_append(GTK_BOX(list.box), rows);
        } else {
            GtkWidget* group = adw_preferences_group_new();
            adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(group), text(name).c_str());
            for (const auto* job : *jobs) {
                adw_preferences_group_add(ADW_PREFERENCES_GROUP(group), make_row(state, list, *job, false));
            }
            gtk_box_append(GTK_BOX(list.box), group);
        }
    }
}

// Whether a list's row of `job` shows it as it is now, in its group; if so,
// the row shows the change. Otherwise the list is shown again: the job moved
// to another group, or into the list.
bool update_list(GuiState& state, GuiState::JobList& list, const JobShown& job)
{
    const auto found = list.rows.find(job.id);
    if (found == list.rows.end() || found->second.group != group_of(job)) {
        return false;
    }
    update_row(state, found->second, job);
    return true;
}

// "Clear ended jobs" while a job has ended.
void show_clear(GuiState& state)
{
    if (state.jobs_page.clear) {
        gtk_widget_set_sensitive(state.jobs_page.clear, state.jobs.shown().size() > state.jobs.active_count());
    }
}

// The count of jobs that run or wait, on the header's button and the Jobs
// row, hidden at 0; the button says them in words to a tooltip and a screen
// reader.
void show_counts(GuiState& state)
{
    const auto counts = count_jobs(state.jobs.shown());
    const auto number = std::to_string(counts.active());
    for (GtkWidget* badge : {state.jobs_page.count, state.shell.jobs_badge}) {
        if (badge) {
            gtk_label_set_text(GTK_LABEL(badge), number.c_str());
            gtk_widget_set_visible(badge, counts.active() > 0);
        }
    }
    if (GtkWidget* button = state.jobs_page.button) {
        const auto summary = jobs_summary(state.text, counts);
        gtk_widget_set_tooltip_text(button, summary.c_str());
        gtk_accessible_update_property(GTK_ACCESSIBLE(button), GTK_ACCESSIBLE_PROPERTY_LABEL, summary.c_str(), -1);
    }
    if (state.jobs_page.ring) {
        gtk_widget_queue_draw(state.jobs_page.ring);
    }
}

// The ring of the header's jobs button (decision 0046): a faint track in the
// colour of the button's words, and, over it from the top clockwise, how far
// the jobs that run are (running_fraction) in the theme's accent colour, which
// the style gives the ring itself (.va-ring), in the light and the dark style.
void draw_ring(GtkDrawingArea* area, cairo_t* cairo, int width, int height, gpointer data)
{
    const auto& state = *static_cast<const GuiState*>(data);
    const double fraction = running_fraction(state.jobs.shown());
    GdkRGBA words{};
    GdkRGBA accent{};
    gtk_widget_get_color(gtk_widget_get_parent(GTK_WIDGET(area)), &words);
    gtk_widget_get_color(GTK_WIDGET(area), &accent);
    constexpr double line = 2.5;
    const double radius = (std::min(width, height) - line) / 2.0;
    const double x = width / 2.0;
    const double y = height / 2.0;
    cairo_set_line_width(cairo, line);
    cairo_set_source_rgba(cairo, words.red, words.green, words.blue, words.alpha * 0.2);
    cairo_arc(cairo, x, y, radius, 0, 2 * G_PI);
    cairo_stroke(cairo);
    if (fraction > 0) {
        gdk_cairo_set_source_rgba(cairo, &accent);
        cairo_arc(cairo, x, y, radius, -G_PI / 2, -G_PI / 2 + 2 * G_PI * fraction);
        cairo_stroke(cairo);
    }
}

// ---- Unfinished operations --------------------------------------------------

// What a button of a row of the operations does when clicked: `act` on the
// row's operation, named by its key.
struct OperationAction {
    std::string key;
    void (*act)(GuiState&, const std::string&);
};

// A button of a row of the operations; what it does shows in a message of its
// own, or where it brings a job into view.
GtkWidget* operation_button(GuiState& state, const std::string& label, const std::string& key,
    void (*act)(GuiState&, const std::string&))
{
    GtkWidget* button = gtk_button_new_with_label(label.c_str());
    gtk_widget_set_valign(button, GTK_ALIGN_CENTER);
    g_object_set_data_full(G_OBJECT(button), "operation-action", new OperationAction{key, act},
        +[](gpointer data) { delete static_cast<OperationAction*>(data); });
    g_signal_connect(button, "clicked",
        G_CALLBACK(+[](GtkButton* clicked, gpointer data) {
            auto& state = *static_cast<GuiState*>(data);
            const auto* action =
                static_cast<const OperationAction*>(g_object_get_data(G_OBJECT(clicked), "operation-action"));
            guarded(
                [&] {
                    if (action) {
                        action->act(state, action->key);
                    }
                },
                [&](const Report& report) { show_message(state, report); });
        }),
        &state);
    return button;
}

// A label of an operation's row. Not selectable: a read that finds other
// operations draws the rows anew, which would drop a selection.
GtkWidget* operation_label(const std::string& text, const char* style = nullptr)
{
    GtkWidget* label = gtk_label_new(text.c_str());
    gtk_label_set_xalign(GTK_LABEL(label), 0.0f);
    gtk_label_set_wrap(GTK_LABEL(label), TRUE);
    gtk_label_set_wrap_mode(GTK_LABEL(label), PANGO_WRAP_WORD_CHAR);
    if (style) {
        gtk_widget_add_css_class(label, style);
    }
    return label;
}

// What makes up a row of the operations (OperationShown), besides the words
// and the bar that a read changes in place.
GuiState::OperationShown operation_shape(const app::OperationRow& shown)
{
    GuiState::OperationShown shape;
    shape.key = shown.key;
    shape.offers_resume = shown.offers_resume;
    shape.resume_asks_first = shown.resume_asks_first;
    shape.abandonable = shown.abandonable;
    shape.has_bar = shown.fraction.has_value();
    shape.has_details = !shown.details.empty();
    return shape;
}

bool same_shape(const GuiState::OperationShown& drawn, const GuiState::OperationShown& read)
{
    return drawn.key == read.key && drawn.offers_resume == read.offers_resume &&
           drawn.resume_asks_first == read.resume_asks_first && drawn.abandonable == read.abandonable &&
           drawn.has_bar == read.has_bar && drawn.has_details == read.has_details;
}

// Gives the row `drawn` the words, bar and state of `shown`, the same
// operation as a later read sees it.
void update_operation_row(const app::Text& text, const GuiState::OperationShown& drawn, const app::OperationRow& shown)
{
    const bool stopped = app::has_stopped(shown.state_kind);
    gtk_image_set_from_icon_name(GTK_IMAGE(drawn.icon), stopped ? "media-playback-pause-symbolic" : jobs_icon);
    gtk_label_set_text(GTK_LABEL(drawn.title), text.format("{} · {}", shown.label, shown.kind).c_str());
    gtk_widget_set_tooltip_text(drawn.title, shown.archive_uuid.c_str());
    gtk_label_set_text(GTK_LABEL(drawn.root), shown.root.c_str());
    if (drawn.bar && shown.fraction) {
        gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(drawn.bar), *shown.fraction);
    }
    gtk_label_set_text(GTK_LABEL(drawn.progress), shown.progress.c_str());
    if (drawn.details) {
        gtk_label_set_text(GTK_LABEL(drawn.details), shown.details.c_str());
    }
    const bool quiet =
        shown.state_kind == app::OperationState::Quiet || shown.state_kind == app::OperationState::Silent;
    gtk_label_set_text(GTK_LABEL(drawn.state), shown.state.c_str());
    if (stopped) {
        gtk_widget_add_css_class(drawn.state, "warning");
    } else {
        gtk_widget_remove_css_class(drawn.state, "warning");
    }
    if (quiet) {
        gtk_widget_add_css_class(drawn.state, "heading");
    } else {
        gtk_widget_remove_css_class(drawn.state, "heading");
    }
    // A verification's run, and the process the record names; a
    // verification that adds chunk hashes has both (decision 0033).
    std::string tooltip = shown.run_id.empty() ? std::string{} : text.format("Run: {}", shown.run_id);
    if (!shown.process.empty()) {
        tooltip += (tooltip.empty() ? "" : "\n") + shown.process;
    }
    gtk_widget_set_tooltip_text(drawn.state, tooltip.empty() ? nullptr : tooltip.c_str());
}

// One unfinished operation: its archive and kind, its root, its bar where
// the share done is known with what it did, its state, and Resume and
// Abandon... where it offers them. `drawn` keeps the widgets that a later
// read of the same operation changes.
GtkWidget* operation_row(GuiState& state, const app::OperationRow& shown, GuiState::OperationShown& drawn)
{
    const auto& text = state.text;
    GtkWidget* row = gtk_list_box_row_new();
    gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(row), FALSE);
    gtk_list_box_row_set_selectable(GTK_LIST_BOX_ROW(row), FALSE);
    GtkWidget* box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_widget_set_margin_start(box, 14);
    gtk_widget_set_margin_end(box, 14);
    gtk_widget_set_margin_top(box, 10);
    gtk_widget_set_margin_bottom(box, 10);

    drawn.icon = gtk_image_new();
    gtk_widget_set_valign(drawn.icon, GTK_ALIGN_START);
    gtk_widget_add_css_class(drawn.icon, "dim-label");
    gtk_box_append(GTK_BOX(box), drawn.icon);

    GtkWidget* words = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_widget_set_hexpand(words, TRUE);
    drawn.title = operation_label("", "heading");
    gtk_box_append(GTK_BOX(words), drawn.title);
    // A long root (a folder deep in a volume) wraps, anywhere in a name,
    // rather than pushing the state and the row's buttons off the window;
    // without the hyphens Pango would add, which would read as part of the
    // path.
    drawn.root = operation_label("", "dim-label");
    PangoAttrList* no_hyphens = pango_attr_list_new();
    pango_attr_list_insert(no_hyphens, pango_attr_insert_hyphens_new(FALSE));
    gtk_label_set_attributes(GTK_LABEL(drawn.root), no_hyphens);
    pango_attr_list_unref(no_hyphens);
    gtk_box_append(GTK_BOX(words), drawn.root);
    if (drawn.has_bar) {
        drawn.bar = gtk_progress_bar_new();
        gtk_widget_set_margin_top(drawn.bar, 4);
        gtk_box_append(GTK_BOX(words), drawn.bar);
    }
    drawn.progress = operation_label("", "numeric");
    gtk_box_append(GTK_BOX(words), drawn.progress);
    if (drawn.has_details) {
        // The details may end with why a total is not known, a whole sentence.
        drawn.details = operation_label("", "dim-label");
        gtk_widget_add_css_class(drawn.details, "caption");
        gtk_box_append(GTK_BOX(words), drawn.details);
    }
    drawn.state = operation_label("");
    gtk_box_append(GTK_BOX(words), drawn.state);
    gtk_box_append(GTK_BOX(box), words);
    update_operation_row(text, drawn, shown);

    // A stopped verification or registration can be resumed here, as on
    // the command line (decision 0022, point 7); a stopped registration
    // (decision 0025), relocation (decision 0026) or verification that
    // adds chunk hashes (decision 0033) can be given up on purpose; and so
    // can an older version's registration that may have stopped
    // (describe_operation), whose Resume asks first.
    if (shown.offers_resume || shown.abandonable) {
        GtkWidget* actions = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
        gtk_widget_set_valign(actions, GTK_ALIGN_CENTER);
        if (shown.offers_resume) {
            gtk_box_append(GTK_BOX(actions), operation_button(state, text("Resume"), shown.key,
                                                 shown.resume_asks_first ? ask_to_resume : resume_from_list));
        }
        if (shown.abandonable) {
            GtkWidget* abandon = operation_button(state, text("Abandon..."), shown.key, ask_to_abandon);
            gtk_widget_add_css_class(abandon, "destructive-action");
            gtk_box_append(GTK_BOX(actions), abandon);
        }
        gtk_box_append(GTK_BOX(box), actions);
    }
    gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), box);
    return row;
}

GtkWidget* build_operations(GuiState& state)
{
    auto& page = state.jobs_page;
    const auto& text = state.text;
    page.operations = adw_preferences_group_new();
    adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(page.operations), text("Unfinished operations").c_str());
    GtkWidget* box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    page.operations_note = make_label("");
    gtk_widget_add_css_class(page.operations_note, "dim-label");
    gtk_box_append(GTK_BOX(box), page.operations_note);
    // Shown only while a step of abandoning one of them runs (decisions
    // 0025, 0026, 0033).
    page.abandoning = gtk_progress_bar_new();
    gtk_progress_bar_set_show_text(GTK_PROGRESS_BAR(page.abandoning), TRUE);
    gtk_widget_set_visible(page.abandoning, FALSE);
    gtk_box_append(GTK_BOX(box), page.abandoning);
    page.operations_list = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(page.operations_list), GTK_SELECTION_NONE);
    gtk_widget_add_css_class(page.operations_list, "boxed-list");
    gtk_box_append(GTK_BOX(box), page.operations_list);
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(page.operations), box);
    gtk_widget_set_visible(page.operations, FALSE);
    return page.operations;
}

// The keyboard focus on a part of Jobs that is made anew goes to Clear ended
// jobs, as Archives' goes to its Refresh (keep_focus_from).
void keep_focus_in_jobs(GuiState& state, GtkWidget* part)
{
    if (!state.window || !part || !state.jobs_page.clear) {
        return;
    }
    GtkWidget* focus = gtk_root_get_focus(GTK_ROOT(state.window));
    if (focus && (focus == part || gtk_widget_is_ancestor(focus, part))) {
        gtk_widget_grab_focus(state.jobs_page.clear);
    }
}

// How many unfinished operations have stopped (decision 0049): on the jobs
// button and the Jobs row of the sidebar, hidden at 0.
void show_stopped(GuiState& state)
{
    const auto stopped = stopped_operations(state);
    const auto number = std::to_string(stopped);
    const auto words = stopped == 1 ? state.text("1 unfinished operation has stopped")
                                    : state.text.format("{} unfinished operations have stopped", stopped);
    if (state.jobs_page.stopped) {
        gtk_label_set_text(GTK_LABEL(state.jobs_page.stopped_count), number.c_str());
        gtk_widget_set_visible(state.jobs_page.stopped, stopped > 0);
        gtk_widget_set_tooltip_text(state.jobs_page.stopped, words.c_str());
    }
    if (GtkWidget* badge = state.shell.stopped_badge) {
        gtk_label_set_text(GTK_LABEL(badge), number.c_str());
        gtk_widget_set_visible(badge, stopped > 0);
        gtk_widget_set_tooltip_text(badge, words.c_str());
    }
}

} // namespace

GtkWidget* make_job_card(GuiState& state, GuiState::JobList& list, const JobShown& job)
{
    return make_row(state, list, job, false);
}

void open_job_details(GuiState& state, std::uint64_t id)
{
    const auto* job = state.jobs.find(id);
    if (!job || !state.window) {
        return;
    }
    auto& open = state.jobs_page.details;
    if (open.contains(id)) {
        return;  // shown already
    }
    const auto& text = state.text;
    GuiState::JobDetails details;
    AdwDialog* dialog = adw_dialog_new();
    details.dialog = GTK_WIDGET(dialog);
    adw_dialog_set_title(dialog, text("Job details").c_str());
    adw_dialog_set_content_width(dialog, 760);
    adw_dialog_set_content_height(dialog, 560);

    GtkWidget* view = adw_toolbar_view_new();
    adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(view), adw_header_bar_new());
    GtkWidget* box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_widget_set_margin_start(box, 18);
    gtk_widget_set_margin_end(box, 18);
    gtk_widget_set_margin_top(box, 6);
    gtk_widget_set_margin_bottom(box, 6);
    GtkWidget* title = make_label(title_of(text, *job), true);
    gtk_label_set_wrap_mode(GTK_LABEL(title), PANGO_WRAP_WORD_CHAR);
    gtk_widget_add_css_class(title, "title-4");
    gtk_box_append(GTK_BOX(box), title);
    details.state = make_label("", true);
    gtk_widget_add_css_class(details.state, "dim-label");
    gtk_box_append(GTK_BOX(box), details.state);
    details.alert = make_alert_label();
    gtk_box_append(GTK_BOX(box), details.alert);
    details.bar = gtk_progress_bar_new();
    gtk_progress_bar_set_show_text(GTK_PROGRESS_BAR(details.bar), TRUE);
    gtk_box_append(GTK_BOX(box), details.bar);
    details.report = make_text_view();
    set_margins(details.report, 8);
    GtkWidget* scroller = gtk_scrolled_window_new();
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroller), details.report);
    gtk_scrolled_window_set_has_frame(GTK_SCROLLED_WINDOW(scroller), TRUE);
    gtk_widget_set_vexpand(scroller, TRUE);
    gtk_box_append(GTK_BOX(box), scroller);
    adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(view), box);

    GtkWidget* buttons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    set_margins(buttons, 12);
    gtk_widget_set_margin_start(buttons, 18);
    gtk_widget_set_margin_end(buttons, 18);
    // Stop at the start, while the job waits or runs; Close at the end.
    details.stop = gtk_button_new_with_label(text("Stop").c_str());
    connect_stop(details.stop, state, id);
    details.result = gtk_button_new_with_label(text("View result").c_str());
    connect_view_result(details.result, state, id);
    GtkWidget* close = gtk_button_new_with_label(text("Close").c_str());
    gtk_widget_set_hexpand(close, TRUE);
    gtk_widget_set_halign(close, GTK_ALIGN_END);
    gtk_widget_add_css_class(close, "suggested-action");
    g_signal_connect_swapped(close, "clicked", G_CALLBACK(adw_dialog_close), dialog);
    gtk_box_append(GTK_BOX(buttons), details.stop);
    gtk_box_append(GTK_BOX(buttons), details.result);
    gtk_box_append(GTK_BOX(buttons), close);
    adw_toolbar_view_add_bottom_bar(ADW_TOOLBAR_VIEW(view), buttons);
    adw_dialog_set_child(dialog, view);
    adw_dialog_set_default_widget(dialog, close);
    // Close has the focus, not the title, whose text would show selected.
    adw_dialog_set_focus(dialog, close);

    // Closed, by its button, by Escape, or with the main window: the window
    // no longer shows the job; a dialog shown for the job since keeps its
    // entry.
    connect_job(details.dialog, "closed", G_CALLBACK(+[](AdwDialog* closed, gpointer data) {
        auto* shown = static_cast<JobButton*>(data);
        auto& open = shown->state->jobs_page.details;
        if (const auto found = open.find(shown->job); found != open.end() && found->second.dialog == GTK_WIDGET(closed)) {
            open.erase(found);
        }
    }),
        state, id);
    open[id] = details;
    update_details(state, *job);
    adw_dialog_present(dialog, GTK_WIDGET(state.window));
}

GtkWidget* build_jobs_page(GuiState& state)
{
    auto& page = state.jobs_page;
    const auto& text = state.text;
    GtkWidget* box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 24);
    set_margins(box, 24);
    GtkWidget* intro = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 18);
    GtkWidget* explanation =
        make_label(text("Each registration, verification, cross-check, relocation and export started in this window "
                        "runs as a job. Jobs that read the same device wait for one another; the others run at once. A "
                        "stopped registration, verification or relocation keeps what it did and is listed under "
                        "Unfinished operations, where Resume goes on without reading again what it read."));
    gtk_widget_set_hexpand(explanation, TRUE);
    gtk_widget_add_css_class(explanation, "dim-label");
    page.clear = gtk_button_new_with_label(text("Clear ended jobs").c_str());
    gtk_widget_set_valign(page.clear, GTK_ALIGN_START);
    g_signal_connect(page.clear, "clicked", G_CALLBACK(+[](GtkButton*, gpointer data) {
        auto& state = *static_cast<GuiState*>(data);
        state.jobs.clear_ended();
        // The runs of the jobs cleared: no row leads to them any more.
        std::erase_if(state.job_runs, [&](const auto& run) { return !state.jobs.find(run.first); });
    }), &state);
    gtk_box_append(GTK_BOX(intro), explanation);
    gtk_box_append(GTK_BOX(intro), page.clear);
    gtk_box_append(GTK_BOX(box), intro);
    page.page.box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 24);
    gtk_box_append(GTK_BOX(box), page.page.box);
    gtk_box_append(GTK_BOX(box), build_operations(state));

    GtkWidget* clamp = adw_clamp_new();
    adw_clamp_set_maximum_size(ADW_CLAMP(clamp), 900);
    adw_clamp_set_tightening_threshold(ADW_CLAMP(clamp), 700);
    adw_clamp_set_child(ADW_CLAMP(clamp), box);
    GtkWidget* scroller = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroller), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroller), clamp);
    gtk_widget_set_vexpand(scroller, TRUE);
    show_list(state, page.page, false);
    show_clear(state);
    // The unfinished operations are read only while the page is on screen,
    // as while Archives is (operations_poll.cpp).
    g_signal_connect(scroller, "map", G_CALLBACK(+[](GtkWidget* shown, gpointer data) {
        auto& state = *static_cast<GuiState*>(data);
        guarded([&] { watch_operations(state, shown); }, [&](const Report& report) { show_poll_error(state, report); });
    }), &state);
    g_signal_connect(scroller, "unmap", G_CALLBACK(+[](GtkWidget* hidden, gpointer data) {
        auto& state = *static_cast<GuiState*>(data);
        guarded([&] { stop_watching(state, hidden); }, [](const Report&) {});
    }), &state);
    return scroller;
}

GtkWidget* build_jobs_button(GuiState& state)
{
    auto& page = state.jobs_page;
    const auto& text = state.text;
    page.button = gtk_menu_button_new();
    // Flat, as the header bar's icon buttons are: a header bar gives a button
    // with more than an icon a background of its own.
    gtk_widget_add_css_class(page.button, "flat");
    // A ring, the count and the word Jobs, as the approved design draws it:
    // no icon alone, which the header's Reload would be taken for. The ring
    // says nothing a screen reader needs; the button's label says it all.
    GtkWidget* face = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    page.ring = GTK_WIDGET(g_object_new(GTK_TYPE_DRAWING_AREA, "accessible-role", GTK_ACCESSIBLE_ROLE_PRESENTATION,
        nullptr));
    gtk_drawing_area_set_content_width(GTK_DRAWING_AREA(page.ring), 16);
    gtk_drawing_area_set_content_height(GTK_DRAWING_AREA(page.ring), 16);
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(page.ring), draw_ring, &state, nullptr);
    gtk_widget_set_valign(page.ring, GTK_ALIGN_CENTER);
    gtk_widget_add_css_class(page.ring, "va-ring");
    gtk_box_append(GTK_BOX(face), page.ring);
    page.count = gtk_label_new("0");
    gtk_widget_add_css_class(page.count, "va-count");
    gtk_widget_add_css_class(page.count, "numeric");
    gtk_widget_set_valign(page.count, GTK_ALIGN_CENTER);
    gtk_widget_set_visible(page.count, FALSE);
    gtk_box_append(GTK_BOX(face), page.count);
    page.word = gtk_label_new(text("Jobs").c_str());
    gtk_box_append(GTK_BOX(face), page.word);
    // How many unfinished operations have stopped (decision 0049), which
    // Jobs lists: a pause sign and the number, hidden at 0.
    page.stopped = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 2);
    gtk_widget_add_css_class(page.stopped, "va-stopped");
    gtk_box_append(GTK_BOX(page.stopped), gtk_image_new_from_icon_name("media-playback-pause-symbolic"));
    page.stopped_count = gtk_label_new("0");
    gtk_widget_add_css_class(page.stopped_count, "numeric");
    gtk_box_append(GTK_BOX(page.stopped), page.stopped_count);
    gtk_widget_set_valign(page.stopped, GTK_ALIGN_CENTER);
    gtk_widget_set_visible(page.stopped, FALSE);
    gtk_box_append(GTK_BOX(face), page.stopped);
    gtk_menu_button_set_child(GTK_MENU_BUTTON(page.button), face);

    page.popover = gtk_popover_new();
    // Its right edge under the button's, near the window's: it opens into the
    // window, not past it; a narrow window makes it narrower (build_window).
    gtk_widget_set_halign(page.popover, GTK_ALIGN_END);
    GtkWidget* box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    page.popover_box = box;
    gtk_widget_set_size_request(box, jobs_popover_width, -1);
    page.popover_list.box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    GtkWidget* scroller = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroller), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_propagate_natural_height(GTK_SCROLLED_WINDOW(scroller), TRUE);
    gtk_scrolled_window_set_max_content_height(GTK_SCROLLED_WINDOW(scroller), 460);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroller), page.popover_list.box);
    gtk_box_append(GTK_BOX(box), scroller);
    GtkWidget* all = gtk_button_new_with_label(text("See all jobs").c_str());
    gtk_widget_set_halign(all, GTK_ALIGN_END);
    g_signal_connect(all, "clicked", G_CALLBACK(+[](GtkButton*, gpointer data) {
        auto& state = *static_cast<GuiState*>(data);
        guarded(
            [&] {
                gtk_popover_popdown(GTK_POPOVER(state.jobs_page.popover));
                show_page(state, "jobs");
            },
            [](const Report&) {});
    }), &state);
    gtk_box_append(GTK_BOX(box), all);
    gtk_popover_set_child(GTK_POPOVER(page.popover), box);
    gtk_menu_button_set_popover(GTK_MENU_BUTTON(page.button), page.popover);
    show_list(state, page.popover_list, true);
    show_counts(state);
    show_stopped(state);
    return page.button;
}

void job_changed(GuiState& state, std::uint64_t job)
{
    if (!state.window) {
        return;  // shutting down
    }
    auto& page = state.jobs_page;
    if (job == 0) {
        show_list(state, page.page, false);
        show_list(state, page.popover_list, true);
        for (const auto& shown : state.jobs.shown()) {
            update_details(state, shown);
        }
    } else if (const auto* shown = state.jobs.find(job)) {
        if (!update_list(state, page.page, *shown)) {
            show_list(state, page.page, false);
        }
        if (!update_list(state, page.popover_list, *shown)) {
            show_list(state, page.popover_list, true);
        }
        update_details(state, *shown);
        update_view(state, *shown);
    }
    // A result page offers Resume, Jobs lists an unfinished operation, and
    // the overview of Archives counts an archive to verify, while no job of
    // the window works on it: a job added or ended changes that.
    const auto* changed = job == 0 ? nullptr : state.jobs.find(job);
    if (job == 0 || (changed && changed->state == JobState::Ended)) {
        show_operations(state);
    }
    show_clear(state);
    show_counts(state);
    lock_language(state);
}

std::vector<std::uint64_t> close_job_details(GuiState& state)
{
    // The entries go first, as the window they belong to goes: a dialog
    // closes once its animation has ended, but at once where there is none
    // (animations off, a dialog not shown), and its "closed" handler then
    // looks for its entry while this goes through them.
    const auto details = std::exchange(state.jobs_page.details, {});
    std::vector<std::uint64_t> open;
    for (const auto& [job, shown] : details) {
        open.push_back(job);
        adw_dialog_force_close(ADW_DIALOG(shown.dialog));
    }
    return open;
}

std::vector<std::string> devices_of_root(const std::filesystem::path& root)
{
    const auto media = platform::make_media_enumerator();
    const auto volume = media->volume_for_path(root);
    return volume ? device_keys(*volume) : std::vector<std::string>{};
}

std::vector<std::string> devices_of_archive(const RepositoryLayout& layout, const Uuid& archive_uuid)
{
    // The carrier as the index's copy of the latest revision's header records
    // it: what the queue needs is only which device it is.
    auto store = ArchiveStore::open_read_only(layout);
    if (!store) {
        return {"archive:" + archive_uuid};
    }
    const auto archive = store->find_archive(archive_uuid);
    if (!archive || !*archive) {
        return {"archive:" + archive_uuid};
    }
    const auto revision = store->revision(archive_uuid, (*archive)->current_revision);
    if (!revision || !*revision) {
        return {"archive:" + archive_uuid};
    }
    return recorded_device_keys(archive_uuid, nlohmann::json::parse((*revision)->header_json, nullptr, false));
}

bool refused_while_a_job_reads(GuiState& state, const std::string& root, const Uuid& archive)
{
    const auto read = state.jobs.reads(root.empty() ? std::nullopt : std::optional(path_from_utf8(root)),
        archive.empty() ? std::nullopt : std::optional(archive));
    if (read) {
        tell(state.window, state.text,
            state.text("A job of this window is working on this operation. Stop it in Jobs first, or wait for it to "
                       "end."));
    }
    return read;
}

std::size_t stopped_operations(const GuiState& state)
{
    if (!state.session || state.operations.error) {
        return 0;
    }
    const auto rows = app::describe_operations(state.text, state.operations.watches, std::chrono::steady_clock::now(),
        std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now()));
    return static_cast<std::size_t>(
        std::ranges::count_if(rows, [](const app::OperationRow& row) { return app::has_stopped(row.state_kind); }));
}

void show_operations(GuiState& state)
{
    use_session_repository(state);
    show_details_operations(state);
    show_results_operations(state);
    show_stopped(state);
    auto& page = state.jobs_page;
    if (!page.operations) {
        return;
    }
    const auto& text = state.text;
    const auto& poll = state.operations;
    const auto clear = [&] {
        keep_focus_in_jobs(state, page.operations_list);
        gtk_list_box_remove_all(GTK_LIST_BOX(page.operations_list));
        page.operation_rows.clear();
    };
    if (!state.session || (poll.watches.empty() && !poll.error)) {
        clear();
        gtk_widget_set_visible(page.operations, FALSE);
        return;
    }
    gtk_widget_set_visible(page.operations, TRUE);
    adw_preferences_group_set_description(ADW_PREFERENCES_GROUP(page.operations),
        text("Kept in the repository, also once the window is closed: Resume goes on where one stopped, without "
             "reading again what it read; Abandon... deletes what it saved. Those started from the command line are "
             "listed too.")
            .c_str());
    gtk_label_set_text(GTK_LABEL(page.operations_note),
        poll.error ? poll.error(text).c_str()
                   : text("The numbers move each time an operation saves its progress: after every 1000 files, or "
                          "once a minute when a file finishes; not while it scans.")
                         .c_str());
    gtk_widget_set_visible(page.operations_note, true);
    const auto now = std::chrono::steady_clock::now();
    const auto wall_now = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());
    // An operation that a job of this window works on is that job's row
    // already, above: it is not listed twice. One that has stopped is,
    // whatever waits.
    std::vector<app::OperationRow> rows;
    for (auto& row : app::describe_operations(text, poll.watches, now, wall_now)) {
        const bool worked_on = state.jobs.reads(row.root.empty() ? std::nullopt : std::optional(path_from_utf8(row.root)),
            row.archive_uuid.empty() ? std::nullopt : std::optional(row.archive_uuid));
        if (app::has_stopped(row.state_kind) || !worked_on) {
            rows.push_back(std::move(row));
        }
    }
    // The same operations with the same buttons as drawn: only their words
    // and bars change, so the keyboard focus on a Resume or Abandon... stays
    // (a read every few seconds would take it away otherwise).
    const bool same = rows.size() == page.operation_rows.size() &&
                      std::ranges::equal(page.operation_rows, rows, [](const auto& drawn, const auto& shown) {
                          return same_shape(drawn, operation_shape(shown));
                      });
    if (same) {
        for (std::size_t index = 0; index < rows.size(); ++index) {
            update_operation_row(text, page.operation_rows[index], rows[index]);
        }
    } else {
        clear();
        for (const auto& shown : rows) {
            auto drawn = operation_shape(shown);
            gtk_list_box_append(GTK_LIST_BOX(page.operations_list), operation_row(state, shown, drawn));
            page.operation_rows.push_back(std::move(drawn));
        }
    }
    gtk_widget_set_visible(page.operations_list, !rows.empty());
    gtk_widget_set_visible(page.operations, !rows.empty() || poll.error);
}

} // namespace va::gui
