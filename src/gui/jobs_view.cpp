// Jobs (decision 0045): every registration, verification and comparison
// started in the window, newest first, each with what it does, how far it
// is, its device alert, Stop while it waits or works, and Details, a window
// of its own with its progress and, once it ended, its report. The view that
// started a job shows it too, as it did when the window ran one operation.
#include "gui/gui_app.h"

#include "common/utf8.h"
#include "platform/media_enumerator.h"

#include <nlohmann/json.hpp>

namespace va::gui {
namespace {

// What a job is doing, worded: waiting for whom, working, stopping, or how
// it ended (its report's first line that holds text: its result).
std::string state_words(const app::Text& text, const JobShown& job)
{
    if (job.state == JobState::Ended) {
        const auto report = job.report ? job.report(text) : std::string{};
        const auto first = report.find_first_not_of("\n ");
        const auto line =
            first == std::string::npos ? std::string{} : report.substr(first, report.find('\n', first) - first);
        return line.empty() ? text("Ended") : text.format("Ended: {}", line);
    }
    if (job.stopping) {
        return text("Stopping...");
    }
    if (job.state == JobState::Waiting) {
        return job.waiting_for ? text.format("Waiting: {} reads the same device", job.waiting_for(text))
                               : text("Waiting for its turn");
    }
    return text("running");
}

// A job's progress on `bar`: while it waits, its state; once it ended, as
// far as it came, and Done.
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

void show_job_alert(const JobShown& job, GtkWidget* label)
{
    if (job.alert.empty()) {
        hide_alert(label);
    } else {
        show_alert(label, job.alert);
    }
}

void update_row(GuiState& state, const JobShown& job)
{
    const auto found = state.jobs_view.rows.find(job.id);
    if (found == state.jobs_view.rows.end()) {
        return;
    }
    const auto& row = found->second;
    const auto& text = state.text;
    gchar* escaped = g_markup_escape_text(job.title ? job.title(text).c_str() : "", -1);
    gtk_label_set_markup(GTK_LABEL(row.title), (std::string("<b>") + escaped + "</b>").c_str());
    g_free(escaped);
    gtk_label_set_text(GTK_LABEL(row.state), state_words(text, job).c_str());
    show_bar(text, job, row.bar);
    show_job_alert(job, row.alert);
    gtk_widget_set_sensitive(row.stop, job.state != JobState::Ended && !job.stopping);
}

void update_details(GuiState& state, const JobShown& job)
{
    const auto found = state.jobs_view.details.find(job.id);
    if (found == state.jobs_view.details.end()) {
        return;
    }
    const auto& details = found->second;
    const auto& text = state.text;
    gtk_label_set_text(GTK_LABEL(details.state), state_words(text, job).c_str());
    show_bar(text, job, details.bar);
    show_job_alert(job, details.alert);
    gtk_widget_set_sensitive(details.stop, job.state != JobState::Ended && !job.stopping);
    set_text(details.report, job.state == JobState::Ended && job.report
                                 ? job.report(text)
                                 : text("The report shows here once the job has ended."));
}

// The view that started `job`, as it showed its one operation before.
void update_view(GuiState& state, const JobShown& job)
{
    const auto show = [&](std::uint64_t shown, GtkWidget* bar, GtkWidget* alert, GtkWidget* cancel) {
        if (shown != job.id || !bar) {
            return;
        }
        show_bar(state.text, job, bar);
        if (alert) {
            show_job_alert(job, alert);
        }
        gtk_widget_set_sensitive(cancel, job.state != JobState::Ended && !job.stopping);
    };
    show(state.register_view.job, state.register_view.progress, state.register_view.alert,
        state.register_view.cancel);
    show(state.verify_view.job, state.verify_view.progress, state.verify_view.alert, state.verify_view.cancel);
    show(state.compare_view.job, state.compare_view.progress, nullptr, state.compare_view.cancel);
}

struct JobButton {
    GuiState* state;
    std::uint64_t job;
};

void connect_stop(GtkWidget* button, GuiState& state, std::uint64_t job)
{
    g_signal_connect_data(button, "clicked",
        G_CALLBACK(+[](GtkButton*, gpointer data) {
            auto* pressed = static_cast<JobButton*>(data);
            pressed->state->jobs.stop(pressed->job);
        }),
        new JobButton{&state, job}, +[](gpointer data, GClosure*) { delete static_cast<JobButton*>(data); },
        GConnectFlags{});
}

void open_details(GuiState& state, std::uint64_t id)
{
    const auto* job = state.jobs.find(id);
    if (!job) {
        return;
    }
    auto& open = state.jobs_view.details;
    if (const auto found = open.find(id); found != open.end()) {
        gtk_window_present(GTK_WINDOW(found->second.window));
        return;
    }
    const auto& text = state.text;
    GuiState::JobDetails details;
    details.window = gtk_window_new();
    gtk_window_set_title(GTK_WINDOW(details.window), text("Job details").c_str());
    gtk_window_set_transient_for(GTK_WINDOW(details.window), state.window);
    gtk_window_set_destroy_with_parent(GTK_WINDOW(details.window), TRUE);
    gtk_window_set_default_size(GTK_WINDOW(details.window), 820, 560);

    GtkWidget* box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    set_margins(box, 16);
    gtk_box_append(GTK_BOX(box), make_heading(job->title ? job->title(text) : std::string{}));
    details.state = make_label("", true);
    gtk_box_append(GTK_BOX(box), details.state);
    details.alert = make_alert_label();
    gtk_box_append(GTK_BOX(box), details.alert);
    details.bar = gtk_progress_bar_new();
    gtk_progress_bar_set_show_text(GTK_PROGRESS_BAR(details.bar), TRUE);
    gtk_box_append(GTK_BOX(box), details.bar);
    details.report = make_text_view();
    GtkWidget* scroller = gtk_scrolled_window_new();
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroller), details.report);
    gtk_widget_set_vexpand(scroller, TRUE);
    gtk_box_append(GTK_BOX(box), scroller);

    GtkWidget* buttons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    details.stop = gtk_button_new_with_label(text("Stop").c_str());
    connect_stop(details.stop, state, id);
    GtkWidget* close = gtk_button_new_with_label(text("Close").c_str());
    g_signal_connect_swapped(close, "clicked", G_CALLBACK(gtk_window_destroy), details.window);
    gtk_box_append(GTK_BOX(buttons), details.stop);
    gtk_box_append(GTK_BOX(buttons), close);
    gtk_box_append(GTK_BOX(box), buttons);
    gtk_window_set_child(GTK_WINDOW(details.window), box);

    // Closed, by its button, by the system's, or with the main window: the
    // window no longer shows the job.
    g_signal_connect_data(details.window, "destroy",
        G_CALLBACK(+[](GtkWidget*, gpointer data) {
            auto* closed = static_cast<JobButton*>(data);
            closed->state->jobs_view.details.erase(closed->job);
        }),
        new JobButton{&state, id}, +[](gpointer data, GClosure*) { delete static_cast<JobButton*>(data); },
        GConnectFlags{});
    open[id] = details;
    update_details(state, *job);
    gtk_window_present(GTK_WINDOW(details.window));
}

// The rows of every job, newest first.
void show_rows(GuiState& state)
{
    auto& view = state.jobs_view;
    if (!view.list) {
        return;
    }
    while (GtkWidget* child = gtk_widget_get_first_child(view.list)) {
        gtk_box_remove(GTK_BOX(view.list), child);
    }
    view.rows.clear();
    const auto& jobs = state.jobs.shown();
    gtk_widget_set_visible(view.empty, jobs.empty());
    gtk_widget_set_sensitive(view.clear, jobs.size() > state.jobs.active_count());
    const auto& text = state.text;
    for (auto job = jobs.rbegin(); job != jobs.rend(); ++job) {
        GuiState::JobRow row;
        GtkWidget* box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
        GtkWidget* top = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        row.title = gtk_label_new(nullptr);
        gtk_label_set_xalign(GTK_LABEL(row.title), 0.0f);
        gtk_label_set_selectable(GTK_LABEL(row.title), TRUE);
        gtk_widget_set_hexpand(row.title, TRUE);
        gtk_label_set_ellipsize(GTK_LABEL(row.title), PANGO_ELLIPSIZE_MIDDLE);
        row.stop = gtk_button_new_with_label(text("Stop").c_str());
        connect_stop(row.stop, state, job->id);
        GtkWidget* details = gtk_button_new_with_label(text("Details").c_str());
        g_signal_connect_data(details, "clicked",
            G_CALLBACK(+[](GtkButton*, gpointer data) {
                auto* pressed = static_cast<JobButton*>(data);
                guarded([&] { open_details(*pressed->state, pressed->job); }, [](const Report&) {});
            }),
            new JobButton{&state, job->id}, +[](gpointer data, GClosure*) { delete static_cast<JobButton*>(data); },
            GConnectFlags{});
        gtk_box_append(GTK_BOX(top), row.title);
        gtk_box_append(GTK_BOX(top), row.stop);
        gtk_box_append(GTK_BOX(top), details);
        gtk_box_append(GTK_BOX(box), top);
        row.state = make_label("", true);
        gtk_box_append(GTK_BOX(box), row.state);
        row.bar = gtk_progress_bar_new();
        gtk_progress_bar_set_show_text(GTK_PROGRESS_BAR(row.bar), TRUE);
        gtk_box_append(GTK_BOX(box), row.bar);
        row.alert = make_alert_label();
        gtk_box_append(GTK_BOX(box), row.alert);
        gtk_box_append(GTK_BOX(box), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL));
        gtk_box_append(GTK_BOX(view.list), box);
        view.rows[job->id] = row;
        update_row(state, *job);
    }
}

} // namespace

GtkWidget* build_jobs_view(GuiState& state)
{
    auto& view = state.jobs_view;
    const auto& text = state.text;
    GtkWidget* box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    set_margins(box, 16);
    gtk_box_append(GTK_BOX(box), make_heading(text("Jobs")));
    gtk_box_append(GTK_BOX(box),
        make_label(text("Each registration, verification and comparison started in this window runs as a job. Jobs "
                        "that read the same device wait for one another; the others run at once. A job stopped "
                        "keeps what it did: it can be resumed without reading again what it read.")));
    view.clear = gtk_button_new_with_label(text("Clear ended jobs").c_str());
    gtk_widget_set_halign(view.clear, GTK_ALIGN_START);
    g_signal_connect(view.clear, "clicked", G_CALLBACK(+[](GtkButton*, gpointer data) {
        static_cast<GuiState*>(data)->jobs.clear_ended();
    }), &state);
    gtk_box_append(GTK_BOX(box), view.clear);
    view.empty = make_label(text("No jobs yet."));
    gtk_box_append(GTK_BOX(box), view.empty);
    view.list = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    GtkWidget* scroller = gtk_scrolled_window_new();
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroller), view.list);
    gtk_widget_set_vexpand(scroller, TRUE);
    gtk_box_append(GTK_BOX(box), scroller);
    show_rows(state);
    return box;
}

void job_changed(GuiState& state, std::uint64_t job)
{
    if (!state.window) {
        return;  // shutting down
    }
    if (job == 0) {
        show_rows(state);
        for (const auto& shown : state.jobs.shown()) {
            update_details(state, shown);
        }
    } else if (const auto* shown = state.jobs.find(job)) {
        update_row(state, *shown);
        update_details(state, *shown);
        update_view(state, *shown);
        // Ended: its row's Clear is offered.
        if (state.jobs_view.clear) {
            gtk_widget_set_sensitive(
                state.jobs_view.clear, state.jobs.shown().size() > state.jobs.active_count());
        }
    }
    lock_language(state);
}

void close_job_details(GuiState& state)
{
    // Destroying a window erases it from the map (its "destroy" handler).
    while (!state.jobs_view.details.empty()) {
        gtk_window_destroy(GTK_WINDOW(state.jobs_view.details.begin()->second.window));
    }
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
        GtkAlertDialog* dialog = gtk_alert_dialog_new("%s",
            state.text("A job of this window is working on this operation. Stop it in Jobs first, or wait for it to "
                       "end.")
                .c_str());
        gtk_alert_dialog_show(dialog, state.window);
        g_object_unref(dialog);
    }
    return read;
}

} // namespace va::gui
