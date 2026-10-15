// Archive List (§B17): never a bare "integrity status", but the last FULL
// verify and the last verify of any mode with dates, plus OVERDUE (R-21).
// Above the archives, the registrations and verifications that have not
// finished, with their progress, wherever they were started: a registration
// of a whole volume from the command line takes many hours, and this is where
// the operator sees how far it is.
#include "gui/gui_app.h"

#include "app/local_time.h"
#include "core/engine/archive_status.h"

#include <algorithm>
#include <chrono>
#include <exception>
#include <memory>
#include <utility>

namespace va::gui {
namespace {

// How often the unfinished operations are read while the list is shown. The
// operations checkpoint every 1000 files or minute, so a shorter interval
// would show nothing new.
constexpr guint poll_seconds = 5;

// The most height the operations take before they scroll, so that a long list
// of them never pushes the archives out of the window: about three rows.
constexpr int operations_max_height = 240;

std::string run_text(const app::Text& text, const std::optional<VerifyRunRecord>& run, bool with_mode)
{
    if (!run) {
        return text("never");
    }
    return (with_mode ? text(run->mode) + " " : "") + text(run->result) + "\n" + app::local_time(run->finished, text);
}

GtkWidget* cell(const std::string& text, bool bold = false, bool warning = false)
{
    GtkWidget* label = gtk_label_new(nullptr);
    gchar* escaped = g_markup_escape_text(text.c_str(), -1);
    std::string markup = escaped;
    g_free(escaped);
    if (bold) {
        markup = "<b>" + markup + "</b>";
    }
    if (warning) {
        markup = "<span foreground=\"#c01c28\"><b>" + markup + "</b></span>";
    }
    gtk_label_set_markup(GTK_LABEL(label), markup.c_str());
    gtk_label_set_xalign(GTK_LABEL(label), 0.0f);
    gtk_label_set_selectable(GTK_LABEL(label), TRUE);
    return label;
}

// ---- Unfinished operations --------------------------------------------------

// A cell of the operations grid. Not selectable: the grid is drawn anew with
// every read, which would drop a selection every few seconds.
GtkWidget* operation_cell(const std::string& text, bool bold = false, bool warning = false)
{
    GtkWidget* label = cell(text, bold, warning);
    gtk_label_set_selectable(GTK_LABEL(label), FALSE);
    gtk_widget_set_valign(label, GTK_ALIGN_START);
    return label;
}

// The watches belong to one repository; another one opened in Settings starts afresh.
void use_session_repository(GuiState& state)
{
    auto& poll = state.operations;
    const auto root = state.session ? state.session->context.repo.root : std::filesystem::path{};
    if (poll.repository != root) {
        poll.watches.clear();
        poll.error = nullptr;
        poll.has_read = false;
        poll.repository = root;
    }
}

// Draws the section from the latest read, in the interface language; hidden
// when nothing is unfinished.
void show_operations(GuiState& state)
{
    auto& view = state.archive_list;
    if (!view.operations) {
        return;
    }
    const auto& text = state.text;
    use_session_repository(state);
    const auto& poll = state.operations;
    clear_grid(view.operations_grid);
    if (!state.session || (poll.watches.empty() && !poll.error)) {
        gtk_widget_set_visible(view.operations, FALSE);
        return;
    }
    gtk_widget_set_visible(view.operations, TRUE);
    gtk_label_set_text(GTK_LABEL(view.operations_note),
        poll.error ? poll.error(text).c_str()
                   : text("Registrations and verifications that have not finished, also those started from "
                          "the command line. The numbers move each time an operation saves its progress: "
                          "after every 1000 files, or once a minute when a file finishes; not while it scans.")
                         .c_str());

    const char* headers[] = {"Label", "Root", "Operation", "Progress", "Status"};
    for (int column = 0; column < 5; ++column) {
        gtk_grid_attach(GTK_GRID(view.operations_grid), operation_cell(text(headers[column]), true), column, 0, 1, 1);
    }
    const auto now = std::chrono::steady_clock::now();
    const auto wall_now = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());
    int row = 1;
    for (const auto& shown : app::describe_operations(text, poll.watches, now, wall_now)) {
        GtkWidget* label = operation_cell(shown.label, true);
        gtk_widget_set_tooltip_text(label, shown.archive_uuid.c_str());
        gtk_grid_attach(GTK_GRID(view.operations_grid), label, 0, row, 1, 1);
        gtk_grid_attach(GTK_GRID(view.operations_grid), operation_cell(shown.root), 1, row, 1, 1);
        gtk_grid_attach(GTK_GRID(view.operations_grid), operation_cell(shown.kind), 2, row, 1, 1);

        // The bar when the share is known, the numbers below it.
        GtkWidget* progress = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
        if (shown.fraction) {
            GtkWidget* bar = gtk_progress_bar_new();
            gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(bar), *shown.fraction);
            gtk_widget_set_size_request(bar, 260, -1);
            gtk_widget_set_halign(bar, GTK_ALIGN_START);
            gtk_box_append(GTK_BOX(progress), bar);
        }
        gtk_box_append(GTK_BOX(progress), operation_cell(shown.progress));
        if (!shown.details.empty()) {
            gtk_box_append(GTK_BOX(progress), operation_cell(shown.details));
        }
        gtk_grid_attach(GTK_GRID(view.operations_grid), progress, 3, row, 1, 1);

        const bool stopped = shown.state_kind == app::OperationState::Stopped;
        const bool quiet = shown.state_kind == app::OperationState::Quiet;
        GtkWidget* state_cell = operation_cell(shown.state, quiet, stopped);
        // A long state ("stopped; it cannot be resumed ...") wraps rather
        // than running off the window's edge. One width for every state, so
        // the rows are measured as tall as they are drawn.
        gtk_label_set_wrap(GTK_LABEL(state_cell), TRUE);
        gtk_label_set_width_chars(GTK_LABEL(state_cell), 32);
        gtk_label_set_max_width_chars(GTK_LABEL(state_cell), 32);
        if (!shown.run_id.empty()) {
            gtk_widget_set_tooltip_text(state_cell, text.format("Run: {}", shown.run_id).c_str());
        }
        gtk_grid_attach(GTK_GRID(view.operations_grid), state_cell, 4, row, 1, 1);
        ++row;
    }
}

// Shows why the operations could not be read, keeping the rows of the last read.
void show_poll_error(GuiState& state, Report report)
{
    use_session_repository(state);
    state.operations.error = std::move(report);
    show_operations(state);
}

// What one read found, handed from its thread to the main thread.
struct PollResult {
    GuiState* state;
    RepositoryLayout layout;
    std::vector<app::UnfinishedOperation> previous;  // the read before, for the counts that cannot change
    std::vector<app::UnfinishedOperation> operations;
    Report error;
    std::chrono::steady_clock::duration took{};
};

// Which operations the watches follow, and which of them were recorded as
// stopped: when this changes, an operation ended (or began), and the archives
// below show something new as well.
std::vector<std::pair<std::string, bool>> followed(const std::vector<app::OperationWatch>& watches)
{
    std::vector<std::pair<std::string, bool>> keys;
    for (const auto& watch : watches) {
        keys.emplace_back(watch.operation().key, watch.operation().recorded_stopped);
    }
    std::ranges::sort(keys);
    return keys;
}

// On the main thread: takes a read's result into the watches and shows them.
void apply_poll(GuiState& state, PollResult& result)
{
    // Shutting down, or another repository was opened while the read ran.
    if (!state.window || !state.session || state.session->context.repo.root != result.layout.root) {
        return;
    }
    use_session_repository(state);
    auto& poll = state.operations;
    poll.took = result.took;
    poll.error = result.error;
    if (result.error) {
        show_operations(state);
        return;
    }
    const auto before = followed(poll.watches);
    app::follow(poll.watches, std::move(result.operations), std::chrono::steady_clock::now(),
        std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now()));
    // An operation that finished (or was recorded as stopped) may have
    // changed its archive's row: the revision, the status, the last verify.
    // The first read of a repository has nothing to compare with.
    const bool changed = poll.has_read && followed(poll.watches) != before;
    poll.has_read = true;
    if (changed) {
        refresh_archive_list(state);  // draws the operations too
    } else {
        show_operations(state);
    }
}

// Reads the unfinished operations on a thread, unless a read is in flight.
// The result comes back to the main thread (the only one allowed to touch
// widgets) through the GLib main loop. `when_due`: called by the timer. Each
// read costs time in proportion to the files done so far (about 3 seconds
// for a verification a million files into its archive; a stopped one is
// counted only once, `previous`), so a read that took long spaces the timer's
// next ones out: reading keeps the database busy at most a tenth of the time,
// and the operation doing the work gets the rest.
void start_poll(GuiState& state, bool when_due = false)
{
    auto& poll = state.operations;
    if (!state.session || poll.busy) {
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    const auto spacing = 10 * poll.took;
    if (when_due && spacing > std::chrono::seconds(poll_seconds) && now - poll.started < spacing) {
        return;
    }
    if (poll.thread.joinable()) {
        poll.thread.join();  // the last read; its result has arrived already
    }
    auto result = std::make_unique<PollResult>(PollResult{&state, state.session->context.repo, {}, {}, nullptr, {}});
    for (const auto& watch : poll.watches) {
        result->previous.push_back(watch.operation());
    }
    poll.busy = true;
    poll.started = now;
    try {
        poll.thread = std::thread([result = std::move(result), now]() mutable {
            try {
                auto read = app::read_unfinished_operations(result->layout, result->previous);
                result->took = std::chrono::steady_clock::now() - now;
                if (read) {
                    result->operations = std::move(*read);
                } else {
                    result->error = [message = read.error().message](const app::Text& text) {
                        return text.format("Cannot read the unfinished operations: {}", text(message));
                    };
                }
            } catch (const std::exception& error) {
                log_failure(std::string("exception while reading the unfinished operations: ") + error.what());
                result->error = unexpected_error(error.what());
            }
            g_idle_add(
                +[](gpointer data) -> gboolean {
                    std::unique_ptr<PollResult> result(static_cast<PollResult*>(data));
                    auto& state = *result->state;
                    state.operations.busy = false;
                    guarded([&] { apply_poll(state, *result); },
                        [&](const Report& report) { show_poll_error(state, report); });
                    return G_SOURCE_REMOVE;
                },
                result.release());
        });
    } catch (...) {
        // No thread could be started (std::system_error): nothing is in flight.
        poll.busy = false;
        throw;
    }
}

gboolean poll_tick(gpointer data)
{
    auto& state = *static_cast<GuiState*>(data);
    if (!state.window) {
        // Shutting down: the timer ends here.
        state.operations.timer = 0;
        state.operations.served = nullptr;
        return G_SOURCE_REMOVE;
    }
    guarded([&] { start_poll(state, true); }, [&](const Report& report) { show_poll_error(state, report); });
    return G_SOURCE_CONTINUE;
}

// The list came on screen: read now, then every few seconds.
void watch_operations(GuiState& state, GtkWidget* list)
{
    auto& poll = state.operations;
    if (poll.timer != 0 && poll.served != list) {
        g_source_remove(poll.timer);  // it served the list of a window that is gone
        poll.timer = 0;
    }
    if (poll.timer == 0) {
        poll.timer = g_timeout_add_seconds(poll_seconds, poll_tick, &state);
        poll.served = list;
    }
    start_poll(state);
}

// The list left the screen (another view, or its window closed): no more reads.
void stop_watching(GuiState& state, GtkWidget* list)
{
    auto& poll = state.operations;
    if (poll.timer != 0 && poll.served == list) {
        g_source_remove(poll.timer);
        poll.timer = 0;
        poll.served = nullptr;
    }
}

GtkWidget* build_operations_section(GuiState& state)
{
    auto& view = state.archive_list;
    const auto& text = state.text;
    view.operations = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    GtkWidget* heading = gtk_label_new(nullptr);
    gchar* escaped = g_markup_escape_text(text("Unfinished operations").c_str(), -1);
    const std::string markup = std::string("<b>") + escaped + "</b>";
    g_free(escaped);
    gtk_label_set_markup(GTK_LABEL(heading), markup.c_str());
    gtk_label_set_xalign(GTK_LABEL(heading), 0.0f);
    gtk_box_append(GTK_BOX(view.operations), heading);
    view.operations_note = make_label("");
    gtk_box_append(GTK_BOX(view.operations), view.operations_note);
    view.operations_grid = gtk_grid_new();
    gtk_grid_set_column_spacing(GTK_GRID(view.operations_grid), 18);
    gtk_grid_set_row_spacing(GTK_GRID(view.operations_grid), 10);
    // As tall as its rows up to a limit, then it scrolls: many operations
    // never make the window taller or push the archives out of it.
    GtkWidget* scroller = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroller), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_propagate_natural_height(GTK_SCROLLED_WINDOW(scroller), TRUE);
    gtk_scrolled_window_set_max_content_height(GTK_SCROLLED_WINDOW(scroller), operations_max_height);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroller), view.operations_grid);
    gtk_box_append(GTK_BOX(view.operations), scroller);
    gtk_box_append(GTK_BOX(view.operations), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL));
    gtk_widget_set_visible(view.operations, FALSE);
    return view.operations;
}

} // namespace

void refresh_archive_list(GuiState& state)
{
    auto& view = state.archive_list;
    const auto& text = state.text;
    if (!view.grid) {
        return;
    }
    show_operations(state);
    clear_grid(view.grid);
    if (!state.session) {
        gtk_label_set_text(GTK_LABEL(view.status), text("No repository is open; see Settings.").c_str());
        return;
    }
    // The list only reads: a database that went missing is reported, not
    // created anew (decision 0022).
    auto store = ArchiveStore::open_read_only(state.session->context.repo);
    if (!store) {
        gtk_label_set_text(GTK_LABEL(view.status), text(store.error().message).c_str());
        return;
    }
    const auto now = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());
    const auto statuses = archive_statuses(*store, state.session->context.config.policy, now);
    if (!statuses) {
        gtk_label_set_text(GTK_LABEL(view.status), text(statuses.error().message).c_str());
        return;
    }
    // Two calls: the compiler checks each literal template against its value.
    const auto count = statuses->size() == 1 ? text.format("{} archive", statuses->size())
                                             : text.format("{} archives", statuses->size());
    gtk_label_set_text(GTK_LABEL(view.status), count.c_str());

    const char* headers[] = {"Label", "UUID", "Volume", "Model", "Capacity", "Last FULL verify",
        "Last verify", "Overdue", "Rev", "Status"};
    for (int column = 0; column < 10; ++column) {
        gtk_grid_attach(GTK_GRID(view.grid), cell(text(headers[column]), true), column, 0, 1, 1);
    }
    int row = 1;
    for (const auto& status : *statuses) {
        std::string volume;
        std::string model;
        std::string capacity;
        if (status.header) {
            volume = status.header->identity.volume_label;
            capacity = format_bytes(status.header->identity.capacity_bytes);
            if (!status.header->identity.devices.empty()) {
                model = status.header->identity.devices.front().model;
            }
        }
        const bool overdue = status.full_overdue || status.any_overdue;
        const bool failed = status.last_any && status.last_any->result == "FAILED";
        gtk_grid_attach(GTK_GRID(view.grid), cell(display_label(status), true), 0, row, 1, 1);
        gtk_grid_attach(GTK_GRID(view.grid), cell(status.archive.archive_uuid.substr(0, 8)), 1, row, 1, 1);
        gtk_grid_attach(GTK_GRID(view.grid), cell(volume), 2, row, 1, 1);
        gtk_grid_attach(GTK_GRID(view.grid), cell(model), 3, row, 1, 1);
        gtk_grid_attach(GTK_GRID(view.grid), cell(capacity), 4, row, 1, 1);
        gtk_grid_attach(GTK_GRID(view.grid), cell(run_text(text, status.last_full, false)), 5, row, 1, 1);
        gtk_grid_attach(GTK_GRID(view.grid), cell(run_text(text, status.last_any, true), false, failed), 6, row, 1, 1);
        gtk_grid_attach(GTK_GRID(view.grid), cell(overdue ? text("OVERDUE") : "", false, overdue), 7, row, 1, 1);
        gtk_grid_attach(GTK_GRID(view.grid), cell("r" + std::to_string(status.archive.current_revision)), 8, row, 1, 1);
        gtk_grid_attach(GTK_GRID(view.grid), cell(text(status.archive.status)), 9, row, 1, 1);
        ++row;
    }
}

GtkWidget* build_archive_list_view(GuiState& state)
{
    auto& view = state.archive_list;
    const auto& text = state.text;
    GtkWidget* box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    set_margins(box, 16);
    gtk_box_append(GTK_BOX(box), make_heading(text("Archives")));
    gtk_box_append(GTK_BOX(box), make_label(text(
        "Each archive shows when it was last read in full (FULL verify) and when it was last "
        "verified at all. A result is only as current as its date.")));

    GtkWidget* toolbar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget* refresh = gtk_button_new_with_label(text("Refresh").c_str());
    g_signal_connect(refresh, "clicked",
        G_CALLBACK(+[](GtkButton*, gpointer data) {
            auto& state = *static_cast<GuiState*>(data);
            guarded(
                [&] {
                    refresh_archive_list(state);
                    start_poll(state);
                },
                [&](const Report& report) {
                    gtk_label_set_text(GTK_LABEL(state.archive_list.status), report(state.text).c_str());
                });
        }),
        &state);
    view.status = make_label("");
    // The free width of the row, so the count stays on one line; a long
    // message (an error) still wraps.
    gtk_widget_set_hexpand(view.status, TRUE);
    gtk_box_append(GTK_BOX(toolbar), refresh);
    gtk_box_append(GTK_BOX(toolbar), view.status);
    gtk_box_append(GTK_BOX(box), toolbar);

    gtk_box_append(GTK_BOX(box), build_operations_section(state));

    view.grid = gtk_grid_new();
    gtk_grid_set_column_spacing(GTK_GRID(view.grid), 18);
    gtk_grid_set_row_spacing(GTK_GRID(view.grid), 8);
    GtkWidget* scroller = gtk_scrolled_window_new();
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroller), view.grid);
    gtk_widget_set_vexpand(scroller, TRUE);
    gtk_box_append(GTK_BOX(box), scroller);

    // The unfinished operations are read only while the list is on screen:
    // "map" when it is shown (also when the window first appears on it),
    // "unmap" when another view is chosen or the window closes, e.g. when a
    // language change replaces it.
    g_signal_connect(box, "map",
        G_CALLBACK(+[](GtkWidget* list, gpointer data) {
            auto& state = *static_cast<GuiState*>(data);
            guarded([&] { watch_operations(state, list); },
                [&](const Report& report) { show_poll_error(state, report); });
        }),
        &state);
    g_signal_connect(box, "unmap",
        G_CALLBACK(+[](GtkWidget* list, gpointer data) {
            auto& state = *static_cast<GuiState*>(data);
            guarded([&] { stop_watching(state, list); }, [](const Report&) {});
        }),
        &state);
    return box;
}

} // namespace va::gui
