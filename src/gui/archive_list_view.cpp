// Archive List (§B17): never a bare "integrity status", but the last FULL
// verify and the last verify of any mode with dates, plus OVERDUE (R-21).
// Above the archives, the registrations, verifications and relocations that
// have not finished, with their progress, wherever they were started: a
// registration of a whole volume from the command line takes many hours, and
// this is where the operator sees how far it is. A verification or
// registration there that has stopped can be resumed (decision 0022, point
// 7), wherever it was started. A registration that has stopped can be
// abandoned (decision 0025), as when its media failed, and so can a stopped
// relocation (decision 0026). An older version's registration, whose plan
// never says when it worked, can be resumed and abandoned once it reads as
// one that may have stopped; its Resume asks first, as that version may
// still run it.
#include "gui/gui_app.h"

#include "app/device_alerts.h"
#include "app/local_time.h"
#include "app/plain_words.h"
#include "core/engine/abandon_registration.h"
#include "core/engine/abandon_relocation.h"
#include "core/engine/abandon_verification.h"
#include "core/engine/archive_status.h"
#include "platform/file_system.h"
#include "platform/process_table.h"

#include <chrono>
#include <exception>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>

namespace va::gui {
namespace {

// How often the unfinished operations are read while the list is shown. The
// operations checkpoint every 1000 files or minute, so a shorter interval
// would show nothing new.
constexpr guint poll_seconds = 5;

// A tick of that timer this much later than the one before means that
// nothing in this window ran meanwhile: the computer slept, and the
// operations on it slept too. Where the clock goes on while the computer
// sleeps (Windows), each operation's next step spans the sleep, which is no
// slow work (OperationWatch::polls_held_up). Where it stops (Linux), a sleep
// is not seen, and need not be: the steps' gaps leave it out as well. Far
// longer than a busy moment ever holds the timer up.
constexpr auto held_up_after = std::chrono::minutes(1);

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

// A line of the table that says what is wrong with an archive's latest
// manifest and how to fix it (decision 0028): red, as the table's warnings
// are, but for a retired archive whose latest manifest is lost (`warning`
// false), which needs nothing of the operator any more (point 9); wrapping
// rather than widening the table, and selectable, so that the manifest's path
// can be copied.
GtkWidget* problem_cell(const std::string& text, bool warning)
{
    GtkWidget* label = gtk_label_new(nullptr);
    gchar* escaped = g_markup_escape_text(text.c_str(), -1);
    const std::string markup =
        warning ? std::string("<span foreground=\"#c01c28\">") + escaped + "</span>" : std::string(escaped);
    g_free(escaped);
    gtk_label_set_markup(GTK_LABEL(label), markup.c_str());
    gtk_label_set_xalign(GTK_LABEL(label), 0.0f);
    gtk_label_set_selectable(GTK_LABEL(label), TRUE);
    gtk_label_set_wrap(GTK_LABEL(label), TRUE);
    gtk_label_set_wrap_mode(GTK_LABEL(label), PANGO_WRAP_WORD_CHAR);
    gtk_label_set_max_width_chars(GTK_LABEL(label), 90);
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

void ask_to_abandon(GuiState& state, const std::string& key);
void resume_from_list(GuiState& state, const std::string& key);
void ask_to_resume(GuiState& state, const std::string& key);

// What a button of a row of the operations grid does when clicked: `act` on
// the row's operation, named by its key.
struct OperationAction {
    std::string key;
    void (*act)(GuiState&, const std::string&);
};

// A button of a row of the operations grid; what it does shows in a message
// of its own, or in the view it brings into view.
GtkWidget* operation_button(GuiState& state, const std::string& label, const std::string& key,
    void (*act)(GuiState&, const std::string&))
{
    GtkWidget* button = gtk_button_new_with_label(label.c_str());
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
                [&](const Report& report) {
                    gtk_label_set_text(GTK_LABEL(state.archive_list.status), report(state.text).c_str());
                });
        }),
        &state);
    return button;
}

// The watches belong to one repository; another one opened in Settings starts afresh.
void use_session_repository(GuiState& state)
{
    auto& poll = state.operations;
    const auto root = state.session ? state.session->context.repo.root : std::filesystem::path{};
    if (poll.repository != root) {
        poll.watches.clear();
        poll.error = nullptr;
        poll.archives_shown.clear();
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
                   : text("Registrations, verifications and relocations that have not finished, also those "
                          "started from the command line. The numbers move each time an operation saves its "
                          "progress: after every 1000 files, or once a minute when a file finishes; not while it "
                          "scans.")
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
        // A long root (a folder deep in a volume) wraps, anywhere in a name,
        // rather than pushing the state and the row's button off the window;
        // without the hyphens Pango would add, which would read as part of
        // the path.
        GtkWidget* root = operation_cell(shown.root);
        gtk_label_set_wrap(GTK_LABEL(root), TRUE);
        gtk_label_set_wrap_mode(GTK_LABEL(root), PANGO_WRAP_WORD_CHAR);
        gtk_label_set_max_width_chars(GTK_LABEL(root), 40);
        PangoAttrList* no_hyphens = pango_attr_list_new();
        pango_attr_list_insert(no_hyphens, pango_attr_insert_hyphens_new(FALSE));
        gtk_label_set_attributes(GTK_LABEL(root), no_hyphens);
        pango_attr_list_unref(no_hyphens);
        gtk_grid_attach(GTK_GRID(view.operations_grid), root, 1, row, 1, 1);
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
            // Wraps rather than widen the table: the details may end with why
            // a total is not known, a whole sentence.
            GtkWidget* details = operation_cell(shown.details);
            gtk_label_set_wrap(GTK_LABEL(details), TRUE);
            gtk_label_set_wrap_mode(GTK_LABEL(details), PANGO_WRAP_WORD_CHAR);
            gtk_label_set_max_width_chars(GTK_LABEL(details), 64);
            gtk_box_append(GTK_BOX(progress), details);
        }
        gtk_grid_attach(GTK_GRID(view.operations_grid), progress, 3, row, 1, 1);

        const bool stopped = app::has_stopped(shown.state_kind);
        const bool quiet = shown.state_kind == app::OperationState::Quiet ||
                           shown.state_kind == app::OperationState::Silent;
        GtkWidget* state_cell = operation_cell(shown.state, quiet, stopped);
        // A long state ("stopped; it cannot be resumed ...") wraps rather
        // than running off the window's edge. One width for every state, so
        // the rows are measured as tall as they are drawn.
        gtk_label_set_wrap(GTK_LABEL(state_cell), TRUE);
        gtk_label_set_width_chars(GTK_LABEL(state_cell), 32);
        gtk_label_set_max_width_chars(GTK_LABEL(state_cell), 32);
        // A verification's run, and the process the record names; a
        // verification that adds chunk hashes has both (decision 0033).
        std::string tooltip = shown.run_id.empty() ? std::string{} : text.format("Run: {}", shown.run_id);
        if (!shown.process.empty()) {
            tooltip += (tooltip.empty() ? "" : "\n") + shown.process;
        }
        if (!tooltip.empty()) {
            gtk_widget_set_tooltip_text(state_cell, tooltip.c_str());
        }
        gtk_grid_attach(GTK_GRID(view.operations_grid), state_cell, 4, row, 1, 1);

        // A stopped verification or registration can be resumed here, as on
        // the command line (decision 0022, point 7); a stopped registration
        // (decision 0025), relocation (decision 0026) or verification that
        // adds chunk hashes (decision 0033) can be given up on purpose; and so
        // can an older version's registration that may have stopped
        // (describe_operation), whose Resume asks first.
        if (shown.offers_resume || shown.abandonable) {
            GtkWidget* actions = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
            gtk_widget_set_valign(actions, GTK_ALIGN_START);
            if (shown.offers_resume) {
                gtk_box_append(GTK_BOX(actions), operation_button(state, text("Resume"), shown.key,
                                                     shown.resume_asks_first ? ask_to_resume : resume_from_list));
            }
            if (shown.abandonable) {
                gtk_box_append(
                    GTK_BOX(actions), operation_button(state, text("Abandon..."), shown.key, ask_to_abandon));
            }
            gtk_grid_attach(GTK_GRID(view.operations_grid), actions, 5, row, 1, 1);
        }
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
    std::string host;                                // this computer, for the processes the plans name
    std::vector<app::UnfinishedOperation> operations;
    std::string archives_mark;                       // ArchiveStore::archive_list_mark, read with them
    Report error;
    std::chrono::steady_clock::duration took{};
};

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
    app::follow(poll.watches, std::move(result.operations), std::chrono::steady_clock::now(),
        std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now()));
    // The archives changed since the table was drawn: an operation ended, or
    // began, also one that did both between two reads (a small folder
    // registered from the command line, a verification of seconds). The
    // table shows its new revision, status or last verify.
    if (result.archives_mark != poll.archives_shown) {
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
    auto result = std::make_unique<PollResult>(
        PollResult{&state, state.session->context.repo, {}, state.host.host, {}, {}, nullptr, {}});
    for (const auto& watch : poll.watches) {
        result->previous.push_back(watch.operation());
    }
    poll.busy = true;
    poll.started = now;
    try {
        poll.thread = std::thread([result = std::move(result), now]() mutable {
            try {
                // Whether the process that runs each registration's attempt
                // still runs (decision 0024). A plan that names this window's
                // process is of an attempt that has ended unless a job of this
                // window works on its root or archive (decision 0045). Asked as
                // such a plan is judged, so after the read: a job's root and
                // archive are known before its work starts, and so before its
                // operation writes a plan. The window's state outlives this
                // thread (OperationsPoll, which joins it, goes first), and
                // Jobs::runs_operation is safe on any thread.
                const auto processes = platform::make_process_table();
                const app::AttemptCheck attempts{processes.get(), result->host,
                    [state = result->state](const nlohmann::json& record) {
                        return state->jobs.runs_operation(record);
                    }};
                auto read = app::read_unfinished_operations(
                    result->layout, result->previous, app::volume_used_bytes, attempts);
                result->took = std::chrono::steady_clock::now() - now;
                if (read) {
                    result->operations = std::move(read->operations);
                    result->archives_mark = std::move(read->archives_mark);
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
    // Held up (the computer slept): told before the read this tick starts, so
    // the step that read sees is taken as one after a stop.
    auto& poll = state.operations;
    const auto now = std::chrono::steady_clock::now();
    if (now - poll.ticked > held_up_after) {
        for (auto& watch : poll.watches) {
            watch.polls_held_up();
        }
    }
    poll.ticked = now;
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
        // While the list was not shown it was not read: no hold-up.
        poll.ticked = std::chrono::steady_clock::now();
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

// ---- Abandoning a registration (decision 0025) ------------------------------

// What the operator was shown and confirmed, carried into the dialog's answer.
struct AbandonChoice {
    GuiState* state;
    AbandonRequest request;
    std::string label;
};

// The report of an abandon, in any language.
Report abandon_report(const AbandonableRegistration& abandoned)
{
    return [abandoned](const app::Text& text) {
        std::string body = text.format("Abandoned the unfinished registration of {} ({}), revision {}.",
            abandoned.label, abandoned.archive_uuid, abandoned.revision);
        if (abandoned.first_registration) {
            body += "\n" + text("The archive had no revision: it is no longer listed.");
            if (abandoned.media_copy) {
                body += "\n" + text("Note: the media may still hold .veriarchive/identity.json naming this archive; "
                                     "delete it before registering the folder or volume again, unless you register "
                                     "it with a media copy.");
            }
        }
        return body;
    };
}

// Why a registration is not abandoned (refused, or the repository failed).
Report not_abandoned(const AbandonFailure& failure)
{
    return [message = failure.message](const app::Text& text) {
        return text.format("Not abandoned: {}", text(message));
    };
}

Report cannot_open(const IoError& error)
{
    return [message = error.message](const app::Text& text) {
        return text.format("Cannot open the repository: {}", text(message));
    };
}

// Shows a report in a message of its own: the section the row was in may be
// gone once its operation is (abandoned, or resumed and finished).
void show_message(GuiState& state, const Report& report)
{
    if (!state.window) {
        return;
    }
    GtkAlertDialog* dialog = gtk_alert_dialog_new("%s", report(state.text).c_str());
    gtk_alert_dialog_show(dialog, state.window);
    g_object_unref(dialog);
}

// Whether the window can run a step of abandoning now, one at a time besides
// the jobs; otherwise says why not.
bool can_abandon(GuiState& state)
{
    if (state.session && !state.job.running() && state.archive_list.abandoning) {
        return true;
    }
    show_message(state, fixed_message(state.session ? "Another step of abandoning is running."
                                                    : "Open a repository first (Settings)."));
    return false;
}

// A step of abandoning (reading what it removes, or abandoning) runs as the
// window's step (Job), one at a time, with `doing` on its bar: counting or
// deleting the saved progress of millions of files takes a while. It never
// takes an operation that a job of the window works on
// (refused_while_a_job_reads). `work` runs on the Job's thread, `done` on the
// main thread once the bar is gone again.
void run_abandon_step(GuiState& state, const std::string& doing, Job::Work work,
    std::function<void(const Report&)> done)
{
    auto& view = state.archive_list;
    gtk_widget_set_visible(view.abandoning, TRUE);
    state.job.start(state.text, view.abandoning, std::move(work),
        [&state, done = std::move(done)](const Report& report) {
            guarded(
                [&] {
                    lock_language(state);
                    if (state.archive_list.abandoning) {
                        gtk_widget_set_visible(state.archive_list.abandoning, FALSE);
                    }
                    done(report);
                },
                [&](const Report& failure) { show_message(state, failure); });
        });
    gtk_progress_bar_set_text(GTK_PROGRESS_BAR(view.abandoning), doing.c_str());
    lock_language(state);
}

// Abandons the registration the operator confirmed, after the same checks
// again, in its own transaction (abandon_registration); reports what it did.
void start_abandon(GuiState& state, const AbandonRequest& request, const std::string& label)
{
    if (!can_abandon(state)) {
        return;
    }
    run_abandon_step(state, state.text.format("Abandoning the registration of {} ...", label),
        [layout = state.session->context.repo, host = state.host.host, request](
            CancelToken&, IProgressSink&) -> Report {
            auto store = ArchiveStore::open(layout);
            if (!store) {
                return cannot_open(store.error());
            }
            const auto processes = platform::make_process_table();
            AbandonServices services{*store, *processes, host};
            const auto abandoned = abandon_registration(services, request);
            return abandoned ? abandon_report(*abandoned) : not_abandoned(abandoned.error());
        },
        [&state](const Report& result) {
            refresh_archive_list(state);
            start_poll(state);
            show_message(state, result);
        });
}

// Asks the operator, with Cancel as the default, saying what abandoning the
// registration removes, as abandonable_registration read it.
void confirm_abandon(GuiState& state, const AbandonableRegistration& registration)
{
    const auto& text = state.text;
    const auto label = registration.label.empty() ? registration.archive_uuid.substr(0, 8) : registration.label;
    std::string detail = text.format("Root: {}", registration.root) + "\n\n" +
                         text.format("Its saved progress will be lost: {} ({}) hashed so far.",
                             app::count_of(text, registration.files, app::Counted::File),
                             format_bytes(registration.bytes)) +
                         " " +
                         text("It cannot be resumed afterwards. Recorded revisions, manifests and the verification "
                              "history are not changed.");
    if (registration.first_registration) {
        detail += "\n\n" + text("The archive has no revision yet: it is removed with its registration.");
    }

    GtkAlertDialog* dialog = gtk_alert_dialog_new("%s",
        text.format("Abandon the unfinished registration of {}?", label).c_str());
    gtk_alert_dialog_set_detail(dialog, detail.c_str());
    const auto cancel = text("Cancel");
    const auto abandon = text("Abandon");
    const char* buttons[] = {cancel.c_str(), abandon.c_str(), nullptr};
    gtk_alert_dialog_set_buttons(dialog, buttons);
    gtk_alert_dialog_set_cancel_button(dialog, 0);
    gtk_alert_dialog_set_default_button(dialog, 0);
    // The request names the registration shown: one that changed meanwhile
    // (resumed and finished, say) is not abandoned in its place.
    auto* choice = new AbandonChoice{
        &state, {registration.archive_uuid, registration.revision, registration.created}, label};
    gtk_alert_dialog_choose(dialog, state.window, nullptr,
        +[](GObject* source, GAsyncResult* result, gpointer data) {
            std::unique_ptr<AbandonChoice> choice(static_cast<AbandonChoice*>(data));
            GError* error = nullptr;
            const int button = gtk_alert_dialog_choose_finish(GTK_ALERT_DIALOG(source), result, &error);
            if (error) {
                g_error_free(error); // dismissed, as when the window closed
            }
            g_object_unref(source);
            auto& state = *choice->state;
            if (button != 1 || !state.window) {
                return;
            }
            guarded([&] { start_abandon(state, choice->request, choice->label); },
                [&](const Report& report) { show_message(state, report); });
        },
        choice);
}

// ---- Abandoning a relocation (decision 0026) --------------------------------

struct RelocationAbandonChoice {
    GuiState* state;
    RelocationAbandonRequest request;
    std::string label;
};

Report relocation_abandon_report(const AbandonableRelocation& abandoned)
{
    return [abandoned](const app::Text& text) {
        return text.format("Abandoned the unfinished relocation of {} ({}).", abandoned.label, abandoned.archive_uuid);
    };
}

// Abandons the relocation the operator confirmed, after the same checks
// again, in its own transaction (abandon_relocation); reports what it did.
void start_relocation_abandon(GuiState& state, const RelocationAbandonRequest& request, const std::string& label)
{
    if (!can_abandon(state)) {
        return;
    }
    run_abandon_step(state, state.text.format("Abandoning the relocation of {} ...", label),
        [layout = state.session->context.repo, host = state.host.host, request](
            CancelToken&, IProgressSink&) -> Report {
            auto store = ArchiveStore::open(layout);
            if (!store) {
                return cannot_open(store.error());
            }
            const auto processes = platform::make_process_table();
            AbandonServices services{*store, *processes, host};
            const auto abandoned = abandon_relocation(services, request);
            return abandoned ? relocation_abandon_report(*abandoned) : not_abandoned(abandoned.error());
        },
        [&state](const Report& result) {
            refresh_archive_list(state);
            start_poll(state);
            show_message(state, result);
        });
}

// Asks the operator, with Cancel as the default, saying what abandoning the
// relocation removes, as abandonable_relocation read it.
void confirm_relocation_abandon(GuiState& state, const AbandonableRelocation& relocation)
{
    const auto& text = state.text;
    const auto label = relocation.label.empty() ? relocation.archive_uuid.substr(0, 8) : relocation.label;
    const std::string detail = text.format("New root: {}", relocation.root) + "\n\n" +
                               text.format("Its saved progress will be lost: {} ({}) matched so far.",
                                   app::count_of(text, relocation.files, app::Counted::File),
                                   format_bytes(relocation.bytes)) +
                               " " +
                               text("It cannot be resumed afterwards. The archive, its revisions, manifests and the "
                                    "verification history are not changed.");
    GtkAlertDialog* dialog = gtk_alert_dialog_new("%s",
        text.format("Abandon the unfinished relocation of {}?", label).c_str());
    gtk_alert_dialog_set_detail(dialog, detail.c_str());
    const auto cancel = text("Cancel");
    const auto abandon = text("Abandon");
    const char* buttons[] = {cancel.c_str(), abandon.c_str(), nullptr};
    gtk_alert_dialog_set_buttons(dialog, buttons);
    gtk_alert_dialog_set_cancel_button(dialog, 0);
    gtk_alert_dialog_set_default_button(dialog, 0);
    // The request names the relocation shown: one that changed meanwhile
    // (resumed and finished, say) is not abandoned in its place.
    auto* choice = new RelocationAbandonChoice{&state, {relocation.archive_uuid, relocation.run_id}, label};
    gtk_alert_dialog_choose(dialog, state.window, nullptr,
        +[](GObject* source, GAsyncResult* result, gpointer data) {
            std::unique_ptr<RelocationAbandonChoice> choice(static_cast<RelocationAbandonChoice*>(data));
            GError* error = nullptr;
            const int button = gtk_alert_dialog_choose_finish(GTK_ALERT_DIALOG(source), result, &error);
            if (error) {
                g_error_free(error); // dismissed, as when the window closed
            }
            g_object_unref(source);
            auto& state = *choice->state;
            if (button != 1 || !state.window) {
                return;
            }
            guarded([&] { start_relocation_abandon(state, choice->request, choice->label); },
                [&](const Report& report) { show_message(state, report); });
        },
        choice);
}

// The operator chose to abandon the relocation of a row: what abandoning it
// removes is read first, by the check that abandons it, as for a
// registration.
void ask_to_abandon_relocation(GuiState& state, const app::UnfinishedOperation& operation)
{
    const auto label = operation.label.empty() ? operation.archive_uuid.substr(0, 8) : operation.label;
    // The relocation shown: one that changed since is refused, not read in its place.
    const RelocationAbandonRequest request{operation.archive_uuid, operation.run_id};
    auto examined = std::make_shared<std::optional<AbandonableRelocation>>();
    run_abandon_step(state, state.text.format("Reading the relocation of {} ...", label),
        [layout = state.session->context.repo, host = state.host.host, request, examined](
            CancelToken&, IProgressSink&) -> Report {
            // Reads only, as the Archive List does (decision 0022).
            auto store = ArchiveStore::open_read_only(layout);
            if (!store) {
                return cannot_open(store.error());
            }
            const auto processes = platform::make_process_table();
            AbandonServices services{*store, *processes, host};
            auto relocation = abandonable_relocation(services, request);
            if (!relocation) {
                return not_abandoned(relocation.error());
            }
            *examined = std::move(*relocation);
            return {};
        },
        [&state, examined](const Report& refused) {
            if (refused) {
                show_message(state, refused);
            } else if (*examined) {
                confirm_relocation_abandon(state, **examined);
            }
        });
}

// ---- Abandoning a verification that adds chunk hashes (decision 0033) -------

struct VerificationAbandonChoice {
    GuiState* state;
    VerificationAbandonRequest request;
    std::string label;
};

Report verification_abandon_report(const AbandonableVerification& abandoned)
{
    return [abandoned](const app::Text& text) {
        return text.format("Abandoned the unfinished verification of {} ({}) that adds chunk hashes.",
            abandoned.label, abandoned.archive_uuid);
    };
}

// Abandons the verification the operator confirmed, after the same checks
// again, in its own transaction (abandon_verification); reports what it did.
void start_verification_abandon(
    GuiState& state, const VerificationAbandonRequest& request, const std::string& label)
{
    if (!can_abandon(state)) {
        return;
    }
    run_abandon_step(state, state.text.format("Abandoning the verification of {} ...", label),
        [layout = state.session->context.repo, host = state.host.host, request](
            CancelToken&, IProgressSink&) -> Report {
            auto store = ArchiveStore::open(layout);
            if (!store) {
                return cannot_open(store.error());
            }
            const auto processes = platform::make_process_table();
            AbandonServices services{*store, *processes, host};
            const auto abandoned = abandon_verification(services, request);
            return abandoned ? verification_abandon_report(*abandoned) : not_abandoned(abandoned.error());
        },
        [&state](const Report& result) {
            refresh_archive_list(state);
            start_poll(state);
            show_message(state, result);
        });
}

// Asks the operator, with Cancel as the default, saying what abandoning the
// verification removes, as abandonable_verification read it.
void confirm_verification_abandon(GuiState& state, const AbandonableVerification& verification)
{
    const auto& text = state.text;
    const auto label = verification.label.empty() ? verification.archive_uuid.substr(0, 8) : verification.label;
    const std::string detail = text.format("Root: {}", verification.root) + "\n\n" +
                               text.format("Its saved progress will be lost: {} ({}) matched so far.",
                                   app::count_of(text, verification.files, app::Counted::File),
                                   format_bytes(verification.bytes)) +
                               " " +
                               text("It cannot be resumed afterwards. The archive, its revisions, manifests and the "
                                    "verification history are not changed.");
    GtkAlertDialog* dialog = gtk_alert_dialog_new("%s",
        text.format("Abandon the unfinished verification of {} that adds chunk hashes?", label).c_str());
    gtk_alert_dialog_set_detail(dialog, detail.c_str());
    const auto cancel = text("Cancel");
    const auto abandon = text("Abandon");
    const char* buttons[] = {cancel.c_str(), abandon.c_str(), nullptr};
    gtk_alert_dialog_set_buttons(dialog, buttons);
    gtk_alert_dialog_set_cancel_button(dialog, 0);
    gtk_alert_dialog_set_default_button(dialog, 0);
    // The request names the run shown: one resumed meanwhile, under a run of
    // its own, is not abandoned in its place.
    auto* choice = new VerificationAbandonChoice{&state, {verification.archive_uuid, verification.run_id}, label};
    gtk_alert_dialog_choose(dialog, state.window, nullptr,
        +[](GObject* source, GAsyncResult* result, gpointer data) {
            std::unique_ptr<VerificationAbandonChoice> choice(static_cast<VerificationAbandonChoice*>(data));
            GError* error = nullptr;
            const int button = gtk_alert_dialog_choose_finish(GTK_ALERT_DIALOG(source), result, &error);
            if (error) {
                g_error_free(error); // dismissed, as when the window closed
            }
            g_object_unref(source);
            auto& state = *choice->state;
            if (button != 1 || !state.window) {
                return;
            }
            guarded([&] { start_verification_abandon(state, choice->request, choice->label); },
                [&](const Report& report) { show_message(state, report); });
        },
        choice);
}

// The operator chose to abandon the verification that adds chunk hashes of a
// row: what abandoning it removes is read first, by the check that abandons
// it, as for a registration.
void ask_to_abandon_verification(GuiState& state, const app::UnfinishedOperation& operation)
{
    const auto label = operation.label.empty() ? operation.archive_uuid.substr(0, 8) : operation.label;
    // The run shown: one that changed since is refused, not read in its place.
    const VerificationAbandonRequest request{operation.archive_uuid, operation.run_id};
    auto examined = std::make_shared<std::optional<AbandonableVerification>>();
    run_abandon_step(state, state.text.format("Reading the verification of {} ...", label),
        [layout = state.session->context.repo, host = state.host.host, request, examined](
            CancelToken&, IProgressSink&) -> Report {
            // Reads only, as the Archive List does (decision 0022).
            auto store = ArchiveStore::open_read_only(layout);
            if (!store) {
                return cannot_open(store.error());
            }
            const auto processes = platform::make_process_table();
            AbandonServices services{*store, *processes, host};
            auto verification = abandonable_verification(services, request);
            if (!verification) {
                return not_abandoned(verification.error());
            }
            *examined = std::move(*verification);
            return {};
        },
        [&state, examined](const Report& refused) {
            if (refused) {
                show_message(state, refused);
            } else if (*examined) {
                confirm_verification_abandon(state, **examined);
            }
        });
}

// The operator chose to abandon the registration, relocation or verification
// that adds chunk hashes `key` of the latest read. What abandoning it removes
// is read first, by the check that abandons it (abandonable_registration,
// abandonable_relocation, abandonable_verification, as the CLI shows it), not
// taken from the read the row came from: whether the archive goes, and the
// saved progress, which is nothing once its revision is recorded. A refusal
// shows at once (an attempt that runs, another registration by now);
// otherwise the question.
void ask_to_abandon(GuiState& state, const std::string& key)
{
    if (!can_abandon(state)) {
        return;
    }
    const auto& watches = state.operations.watches;
    const auto found = std::ranges::find(watches, key,
        [](const app::OperationWatch& watch) -> const std::string& { return watch.operation().key; });
    // Never an operation that a job of this window works on (decision 0045):
    // the check that abandons it would take this process's attempt for one
    // that has ended.
    if (!key.empty() && found != watches.end() &&
        refused_while_a_job_reads(state, found->operation().root, found->operation().archive_uuid)) {
        return;
    }
    if (!key.empty() && found != watches.end() && found->operation().kind == app::OperationKind::Relocation) {
        ask_to_abandon_relocation(state, found->operation());
        return;
    }
    if (!key.empty() && found != watches.end() && found->operation().kind == app::OperationKind::Verification &&
        found->operation().adds_chunk_hashes) {
        ask_to_abandon_verification(state, found->operation());
        return;
    }
    if (key.empty() || found == watches.end() || found->operation().kind != app::OperationKind::Registration) {
        const char* gone = key.starts_with("relocation:") ? "This relocation is no longer listed as unfinished."
                           : key.starts_with("run:")      ? "This verification is no longer listed as unfinished."
                                                          : "This registration is no longer listed as unfinished.";
        show_message(state, fixed_message(gone));
        return;
    }
    const auto& operation = found->operation();
    const auto label = operation.label.empty() ? operation.archive_uuid.substr(0, 8) : operation.label;
    // The registration shown: one that changed since (resumed and finished,
    // say) is refused, not read in its place.
    const AbandonRequest request{operation.archive_uuid, operation.revision, operation.started};
    auto examined = std::make_shared<std::optional<AbandonableRegistration>>();
    run_abandon_step(state, state.text.format("Reading the registration of {} ...", label),
        [layout = state.session->context.repo, host = state.host.host, request, examined](
            CancelToken&, IProgressSink&) -> Report {
            // Reads only, as the Archive List does (decision 0022).
            auto store = ArchiveStore::open_read_only(layout);
            if (!store) {
                return cannot_open(store.error());
            }
            const auto processes = platform::make_process_table();
            AbandonServices services{*store, *processes, host};
            auto registration = abandonable_registration(services, request);
            if (!registration) {
                return not_abandoned(registration.error());
            }
            *examined = std::move(*registration);
            return {};
        },
        [&state, examined](const Report& refused) {
            if (refused) {
                show_message(state, refused);
            } else if (*examined) {
                confirm_abandon(state, **examined);
            }
        });
}

// ---- Resuming a verification or registration (decision 0022, point 7) -------

// Whether the window can resume an operation now, as a job: not while a step
// of abandoning runs, which may be abandoning it; otherwise says why not.
bool can_resume(GuiState& state)
{
    if (state.session && !state.job.running()) {
        return true;
    }
    show_message(state, fixed_message(state.session ? "Another step of abandoning is running."
                                                    : "Open a repository first (Settings)."));
    return false;
}

// The verification or registration `key` of the latest read, as a copy (the
// view a resume brings into view may have the list read again), or nothing,
// saying so, where it is no longer listed as one Resume continues.
std::optional<app::UnfinishedOperation> listed_for_resume(GuiState& state, const std::string& key)
{
    const auto& watches = state.operations.watches;
    const auto found = std::ranges::find(watches, key,
        [](const app::OperationWatch& watch) -> const std::string& { return watch.operation().key; });
    if (key.empty() || found == watches.end() || found->operation().kind == app::OperationKind::Relocation) {
        show_message(state, fixed_message(key.starts_with("run:")
                                              ? "This verification is no longer listed as unfinished."
                                              : "This registration is no longer listed as unfinished."));
        return std::nullopt;
    }
    return found->operation();
}

// Continues `operation` exactly as its command-line resume does, as the
// window's one operation, its progress and report where an operation of its
// kind started in this window shows them; what is wrong with it by now (its
// media not attached, its archive at another revision) the resume itself says
// there, in the command line's words. `confirmed`: a registration that an
// older version left, which the operator confirmed in the question (as
// --confirm).
void resume_operation(GuiState& state, const app::UnfinishedOperation& operation, bool confirmed = false)
{
    if (operation.kind == app::OperationKind::Verification) {
        resume_verification(state, operation);
    } else {
        resume_registration(state, operation, confirmed);
    }
}

// The operator chose to resume the stopped verification or registration `key`
// of the latest read.
void resume_from_list(GuiState& state, const std::string& key)
{
    if (!can_resume(state)) {
        return;
    }
    if (const auto operation = listed_for_resume(state, key);
        operation && !refused_while_a_job_reads(state, operation->root, operation->archive_uuid)) {
        resume_operation(state, *operation);
    }
}

// What the operator is asked about, carried into the question's answer
// (ask_before_resuming).
struct ResumeChoice {
    GuiState* state;
    std::function<void(GuiState&)> resume;
};

// The operator chose Resume on the registration `key` of the latest read,
// whose row says that an older version left it and that it may have stopped
// (OperationRow::resume_asks_first): it asks first, and only once the
// operator chose Resume there does it resume as resume_from_list does, the
// resume confirmed as with --confirm.
void ask_to_resume(GuiState& state, const std::string& key)
{
    if (!can_resume(state)) {
        return;
    }
    const auto operation = listed_for_resume(state, key);
    if (!operation || refused_while_a_job_reads(state, operation->root, operation->archive_uuid)) {
        return;
    }
    const auto label = operation->label.empty() ? operation->archive_uuid.substr(0, 8) : operation->label;
    ask_before_resuming(state, label, operation->root, [asked = *operation](GuiState& state) {
        // A job may have started on it while the question was shown.
        if (can_resume(state) && !refused_while_a_job_reads(state, asked.root, asked.archive_uuid)) {
            resume_operation(state, asked, true);
        }
    });
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

void ask_before_resuming(GuiState& state, const std::string& label, const std::string& root,
    std::function<void(GuiState&)> resume)
{
    // What happens where the older version still runs the registration, and
    // what to make sure of first, with the root, so that the operator knows
    // which window or command to look for.
    const auto& text = state.text;
    const auto detail = text.format("Root: {}", root) + "\n\n" + text(app::older_version_resume_warning);
    GtkAlertDialog* dialog =
        gtk_alert_dialog_new("%s", text.format("Resume the unfinished registration of {}?", label).c_str());
    gtk_alert_dialog_set_detail(dialog, detail.c_str());
    const auto cancel = text("Cancel");
    const auto go_on = text("Resume");
    const char* buttons[] = {cancel.c_str(), go_on.c_str(), nullptr};
    gtk_alert_dialog_set_buttons(dialog, buttons);
    gtk_alert_dialog_set_cancel_button(dialog, 0);
    gtk_alert_dialog_set_default_button(dialog, 0);
    auto* choice = new ResumeChoice{&state, std::move(resume)};
    gtk_alert_dialog_choose(dialog, state.window, nullptr,
        +[](GObject* source, GAsyncResult* result, gpointer data) {
            std::unique_ptr<ResumeChoice> choice(static_cast<ResumeChoice*>(data));
            GError* error = nullptr;
            const int button = gtk_alert_dialog_choose_finish(GTK_ALERT_DIALOG(source), result, &error);
            if (error) {
                g_error_free(error); // dismissed, as when the window closed
            }
            g_object_unref(source);
            auto& state = *choice->state;
            if (button != 1 || !state.window) {
                return;
            }
            guarded([&] { choice->resume(state); }, [&](const Report& report) { show_message(state, report); });
        },
        choice);
}

void refresh_archive_list(GuiState& state)
{
    auto& view = state.archive_list;
    const auto& text = state.text;
    if (!view.grid) {
        return;
    }
    show_operations(state);
    clear_grid(view.grid);
    // Nothing shown yet: a read of the operations that comes back draws the
    // table again (apply_poll), so one that could not be drawn now is retried.
    state.operations.archives_shown.clear();
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
    // The mark before the archives: a change in between is drawn already and
    // only makes the next read draw the table once more, never the other way
    // round.
    const auto mark = store->archive_list_mark();
    const auto now = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());
    const auto repository_files = platform::make_file_system_access();
    const auto statuses = archive_statuses(
        *store, *repository_files, state.session->context.repo, state.session->context.config.policy, now);
    if (!statuses) {
        gtk_label_set_text(GTK_LABEL(view.status), text(statuses.error().message).c_str());
        return;
    }
    if (mark) {
        state.operations.archives_shown = *mark;
    }
    const auto count = app::count_of(text, statuses->size(), app::Counted::Archive);
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
            // The carrier as the latest revision recorded it, or the sample
            // identity in the screenshot mode.
            const auto identity = displayed_identity(status.header->identity, state.identity_shown);
            volume = identity.volume_label;
            capacity = format_bytes(identity.capacity_bytes);
            if (!identity.devices.empty()) {
                model = identity.devices.front().model;
            }
        }
        const bool overdue = status.full_overdue || status.any_overdue;
        // Decision 0044: a device alert of the last verification shows beside
        // it, as a failure does, until a later one gives none.
        const auto marks = app::alert_marks(text, app::archive_alerts(status));
        const bool failed = (status.last_any && status.last_any->result == "FAILED") || !marks.empty();
        auto last_verify = run_text(text, status.last_any, true);
        for (const auto& mark : marks) {
            last_verify += "\n" + mark;
        }
        gtk_grid_attach(GTK_GRID(view.grid), cell(display_label(status), true), 0, row, 1, 1);
        gtk_grid_attach(GTK_GRID(view.grid), cell(status.archive.archive_uuid.substr(0, 8)), 1, row, 1, 1);
        gtk_grid_attach(GTK_GRID(view.grid), cell(volume), 2, row, 1, 1);
        gtk_grid_attach(GTK_GRID(view.grid), cell(model), 3, row, 1, 1);
        gtk_grid_attach(GTK_GRID(view.grid), cell(capacity), 4, row, 1, 1);
        gtk_grid_attach(GTK_GRID(view.grid), cell(run_text(text, status.last_full, false)), 5, row, 1, 1);
        gtk_grid_attach(GTK_GRID(view.grid), cell(last_verify, false, failed), 6, row, 1, 1);
        gtk_grid_attach(GTK_GRID(view.grid), cell(overdue ? text("OVERDUE") : "", false, overdue), 7, row, 1, 1);
        gtk_grid_attach(GTK_GRID(view.grid), cell("r" + std::to_string(status.archive.current_revision)), 8, row, 1, 1);
        gtk_grid_attach(GTK_GRID(view.grid), cell(text(status.archive.status)), 9, row, 1, 1);
        ++row;
        // Decision 0028: an archive whose latest manifest cannot be used is
        // listed like the others, with what is wrong and how to fix it under
        // its row, from the volume on (its manifest names no volume now). It
        // stays one to choose in Verify Archive, which then records the
        // failure. A retired archive whose latest manifest is lost shows as
        // retired with its manifest lost (point 9).
        if (status.unusable_manifest) {
            const auto message = text(plain_message(*status.unusable_manifest));
            const auto problem =
                status.manifest_lost ? text.format("Manifest lost: {}", message) : text.format("Problem: {}", message);
            gtk_grid_attach(GTK_GRID(view.grid), problem_cell(problem, !status.manifest_lost), 2, row, 8, 1);
            ++row;
        }
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
    // Shown only while a registration is abandoned (decision 0025).
    view.abandoning = gtk_progress_bar_new();
    gtk_progress_bar_set_show_text(GTK_PROGRESS_BAR(view.abandoning), TRUE);
    gtk_widget_set_valign(view.abandoning, GTK_ALIGN_CENTER);
    gtk_widget_set_visible(view.abandoning, FALSE);
    gtk_box_append(GTK_BOX(toolbar), refresh);
    gtk_box_append(GTK_BOX(toolbar), view.status);
    gtk_box_append(GTK_BOX(toolbar), view.abandoning);
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
