// Compare Archives (decision 0020): checks that a copy (B) holds the same
// entries as its reference (A), using the manifests of both archives' current
// revisions in the repository. No media is read, so a comparison is quick; what
// it compares is what each registration read. Whether the media still hold it
// is what a FULL verification of each archive proves.
#include "gui/gui_app.h"

#include "app/local_time.h"
#include "app/plain_words.h"
#include "core/engine/compare_archives.h"
#include "platform/file_system.h"

namespace va::gui {
namespace {

// The report lists at most this many differences: a text view with millions of
// lines would make the window unusable. The counts above the list cover all.
constexpr std::size_t listed_differences = 2000;

constexpr const char* note =
    "This compares what each registration read. FULL verification of each archive proves that its "
    "media still holds it.";

// What the report shows of a comparison. Only the listed differences are kept,
// so a report that stays on screen holds little memory.
struct Shown {
    ArchiveComparison comparison;   // `differences` cut to the listed ones
    std::size_t differences = 0;    // how many there are in all
};

std::string report(const app::Text& text, const Shown& shown)
{
    const auto& comparison = shown.comparison;
    std::string body = text.format("ARCHIVES {}", text(comparison.result)) + "\n";
    if (comparison.result == "IDENTICAL") {
        body += text("Every entry of A is in B with the same type and content, and B has no other entries.") +
                "\n";
    } else if (comparison.result == "IDENTICAL_WITH_WARNINGS") {
        body += text("No entry differs, but the entries listed below are not proven the same.") + "\n";
    }
    body += "\n" +
            text.format("A (reference): {} ({}) revision {}, recorded {}", comparison.label_a,
                comparison.archive_a, comparison.revision_a, app::local_time(comparison.created_a, text)) +
            "\n" +
            text.format("B (copy): {} ({}) revision {}, recorded {}", comparison.label_b, comparison.archive_b,
                comparison.revision_b, app::local_time(comparison.created_b, text)) +
            "\n" +
            text.format("Entries: {} in A, {} in B, {} the same", comparison.entries_a, comparison.entries_b,
                comparison.same) +
            "\n" + text.format("Differences: {}", shown.differences);
    for (const auto& [status, count] : comparison.counts) {
        body += "  " + text(status) + " " + std::to_string(count);
    }
    body += "\n";
    if (!comparison.differences.empty()) {
        body += "\n";
        for (const auto& difference : comparison.differences) {
            // "path:stream" names an alternate data stream, as Windows writes it.
            body += text(difference.status) + "  " + difference.path +
                    (difference.stream.empty() ? "" : ":" + difference.stream) +
                    (difference.detail.empty() ? ""
                                               : "  " + text.parenthesized(text(app::item_detail_words(difference.detail)))) +
                    "\n";
        }
        if (shown.differences > comparison.differences.size()) {
            const auto more = shown.differences - comparison.differences.size();
            // Two calls: the compiler checks each literal template against its value.
            body += (more == 1 ? text.format("... and {} more difference, not listed here", more)
                               : text.format("... and {} more differences, not listed here", more)) +
                    "\n";
        }
    }
    body += "\n" + text(note) + "\n";
    return body;
}

std::string failure_report(const app::Text& text, const CompareFailure& failure)
{
    std::string body = text.format("Not compared: {}", text(failure.message)) + "\n";
    // Cancelling is the user's choice, not a fault of an archive or the repository.
    if (failure.failure_class != "Cancelled") {
        body += text.format("Failure: {}", text(app::failure_class_words(failure.failure_class))) + "\n";
    }
    return body;
}

// Shows `report` below the buttons and keeps it, so a language change can
// show it again in the new language.
void show_report(GuiState& state, Report report)
{
    set_text(state.compare_view.report, report(state.text));
    state.compare_view.shown_report = std::move(report);
}

// Shows an unexpected error of a Compare handler (see guarded).
void show_error(GuiState& state, Report report)
{
    show_report(state, std::move(report));
}

// An archive as a job's title names it: its label, or the start of its uuid.
std::string label_of(const GuiState& state, const Uuid& archive_uuid)
{
    if (state.session) {
        if (auto store = ArchiveStore::open_read_only(state.session->context.repo)) {
            if (const auto archive = store->find_archive(archive_uuid); archive && *archive &&
                                                                        !(*archive)->label.empty()) {
                return (*archive)->label;
            }
        }
    }
    return archive_uuid.substr(0, 8);
}

void start_comparison(GuiState& state)
{
    auto& view = state.compare_view;
    if (!state.session) {
        show_report(state, fixed_message("Open a repository first (Settings)."));
        return;
    }
    const auto reference = chosen_archive(view.reference, view.archive_uuids, 0);
    const auto copy = chosen_archive(view.copy, view.archive_uuids, 0);
    if (!reference || !copy) {
        show_report(state, fixed_message("Choose the two archives to compare."));
        return;
    }
    // The engine allows it, but here it is almost surely a slip: "identical"
    // would then say nothing about the copy the user meant to check.
    if (*reference == *copy) {
        show_report(state, fixed_message("A and B are the same archive; choose the copy to check as B."));
        return;
    }

    show_report(state, fixed_message("Comparing the manifests of A and B ..."));
    // The worker returns its report unworded; it is worded when shown, in the
    // language in use then. A comparison reads no media: it waits for no
    // device (decision 0045).
    JobSpec job;
    job.title = [a = label_of(state, *reference), b = label_of(state, *copy)](const app::Text& text) {
        return text.format("Compare {} with {}", a, b);
    };
    job.counting = Counting::Entries;
    job.work = [&state, a = *reference, b = *copy](JobContext& context) -> Report {
            auto& session = *state.session;
            auto store = ArchiveStore::open(session.context.repo);
            if (!store) {
                return [message = store.error().message](const app::Text& text) {
                    return text.format("Cannot open the repository: {}", text(message));
                };
            }
            const auto repository_files = platform::make_file_system_access();
            auto comparison = compare_archives(
                *store, *repository_files, session.context.repo, a, b, context.cancel, context.progress);
            if (!comparison) {
                return [failure = comparison.error()](const app::Text& text) { return failure_report(text, failure); };
            }
            Shown shown{std::move(*comparison), 0};
            auto& differences = shown.comparison.differences;
            shown.differences = differences.size();
            if (differences.size() > listed_differences) {
                differences.resize(listed_differences);
            }
            return [shown = std::move(shown)](const app::Text& text) { return report(text, shown); };
        };
    job.done = [&state](std::uint64_t ended, const Report& result) {
        guarded(
            [&] {
                // The view shows the report of the job it shows.
                if (state.compare_view.job == ended) {
                    show_report(state, result);
                }
            },
            [&](Report error) { show_error(state, std::move(error)); });
    };
    view.job = state.jobs.start(state.text, std::move(job));
    job_changed(state, view.job);
}

} // namespace

GtkWidget* build_compare_view(GuiState& state)
{
    auto& view = state.compare_view;
    const auto& text = state.text;
    GtkWidget* box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    set_margins(box, 16);
    gtk_box_append(GTK_BOX(box), make_heading(text("Compare Archives")));
    gtk_box_append(GTK_BOX(box), make_label(text(
        "Comparison checks that two registered archives hold the same entries with the same content, "
        "using their manifests in the repository; no media is read. Choose the reference archive as A "
        "and the copy to check as B.")));

    // The labels share a column, so the two drop-downs line up.
    GtkWidget* choices = gtk_grid_new();
    gtk_grid_set_column_spacing(GTK_GRID(choices), 8);
    gtk_grid_set_row_spacing(GTK_GRID(choices), 8);
    const char* initial[] = {nullptr};
    view.reference = gtk_drop_down_new_from_strings(initial);
    view.copy = gtk_drop_down_new_from_strings(initial);
    gtk_grid_attach(GTK_GRID(choices), make_row_label(text("Reference archive (A):")), 0, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(choices), view.reference, 1, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(choices), make_row_label(text("Copy to check (B):")), 0, 1, 1, 1);
    gtk_grid_attach(GTK_GRID(choices), view.copy, 1, 1, 1, 1);
    gtk_box_append(GTK_BOX(box), choices);

    GtkWidget* buttons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    view.start = gtk_button_new_with_label(text("Compare").c_str());
    view.cancel = gtk_button_new_with_label(text("Cancel").c_str());
    gtk_widget_set_sensitive(view.cancel, FALSE);
    g_signal_connect(view.start, "clicked",
        G_CALLBACK(+[](GtkButton*, gpointer data) {
            auto& state = *static_cast<GuiState*>(data);
            guarded([&] { start_comparison(state); }, [&](Report error) { show_error(state, std::move(error)); });
        }),
        &state);
    g_signal_connect(view.cancel, "clicked", G_CALLBACK(+[](GtkButton*, gpointer data) {
        auto& state = *static_cast<GuiState*>(data);
        state.jobs.stop(state.compare_view.job);
    }), &state);
    gtk_box_append(GTK_BOX(buttons), view.start);
    gtk_box_append(GTK_BOX(buttons), view.cancel);
    gtk_box_append(GTK_BOX(box), buttons);

    view.progress = gtk_progress_bar_new();
    gtk_progress_bar_set_show_text(GTK_PROGRESS_BAR(view.progress), TRUE);
    gtk_box_append(GTK_BOX(box), view.progress);

    view.report = make_text_view();
    GtkWidget* scroller = gtk_scrolled_window_new();
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroller), view.report);
    gtk_widget_set_vexpand(scroller, TRUE);
    gtk_box_append(GTK_BOX(box), scroller);
    return box;
}

} // namespace va::gui
