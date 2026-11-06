// The unfinished registrations, verifications and relocations (decision
// 0022), wherever they were started, read from the repository every few
// seconds while Jobs or Archives is shown, each read on a thread of its own: a
// registration of a whole volume from the command line takes many hours, and
// Jobs is where the operator sees how far it is (decision 0049); Archives
// offers Resume in an archive's details and counts those that stopped. A read that took long
// spaces out the next ones, so the operation doing the work keeps the
// database most of the time.
#include "gui/gui_app.h"

#include "platform/process_table.h"

#include <chrono>
#include <exception>
#include <memory>
#include <string>
#include <utility>

namespace va::gui {
namespace {

// How often the unfinished operations are read while Jobs or Archives is shown. The
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
    // The archives changed since the list was drawn: an operation ended, or
    // began, also one that did both between two reads (a small folder
    // registered from the command line, a verification of seconds). The
    // list shows its new revision, status or last verify.
    // (refresh_archives also has the attached devices read anew.)
    if (result.archives_mark != poll.archives_shown) {
        refresh_archives(state);  // draws the operations too
    } else {
        show_operations(state);
    }
    // Devices names the volumes that registrations are working on.
    refresh_devices(state);
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

} // namespace

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

void show_poll_error(GuiState& state, Report report)
{
    use_session_repository(state);
    state.operations.error = std::move(report);
    show_operations(state);
}

// The result comes back to the main thread (the only one allowed to touch
// widgets) through the GLib main loop. Each read costs time in proportion to
// the files done so far (about 3 seconds for a verification a million files
// into its archive; a stopped one is counted only once, `previous`), so a
// read that took long spaces the timer's next ones out: reading keeps the
// database busy at most a tenth of the time, and the operation doing the
// work gets the rest.
void start_poll(GuiState& state, bool when_due)
{
    auto& poll = state.operations;
    if (!state.session || poll.busy) {
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    if (when_due && !read_due(std::chrono::seconds(poll_seconds), poll.took, poll.started, now)) {
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

void watch_operations(GuiState& state, GtkWidget* page)
{
    auto& poll = state.operations;
    if (poll.timer != 0 && poll.served != page) {
        g_source_remove(poll.timer);  // it served the page of a window that is gone
        poll.timer = 0;
    }
    if (poll.timer == 0) {
        poll.timer = g_timeout_add_seconds(poll_seconds, poll_tick, &state);
        // While the page was not shown it was not read: no hold-up.
        poll.ticked = std::chrono::steady_clock::now();
        poll.served = page;
    }
    start_poll(state);
}

void stop_watching(GuiState& state, GtkWidget* page)
{
    auto& poll = state.operations;
    if (poll.timer != 0 && poll.served == page) {
        g_source_remove(poll.timer);
        poll.timer = 0;
        poll.served = nullptr;
    }
}

} // namespace va::gui
