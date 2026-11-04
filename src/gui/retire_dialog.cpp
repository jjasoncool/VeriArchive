// Retiring an archive (decisions 0003, 0046), from the menu of its details:
// what retiring it would do is read first (retirement_of, read only), as the
// window's single step beside the jobs, then the window asks, with Cancel as
// the default: what retiring means (no longer expected to be verified, its
// history kept, no way back), and, for an archive whose latest manifest is
// lost (decision 0028, point 9), the command line's explanation with an
// explicit confirmation, without which Retire is not offered. Retire retires
// it as the window's step. Refused while a job of the window waits or works on
// the archive, and, in the window's words, while an operation that would give
// the archive a new revision is unfinished. Afterwards Archives lists it
// retired; it can still be verified, but takes no new revision.
#include "gui/gui_app.h"

#include "app/refusal_in_window.h"
#include "core/engine/retire_archive.h"
#include "gui/archive_actions.h"
#include "platform/file_system.h"
#include "platform/process_table.h"

#include <algorithm>
#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace va::gui {
namespace {

const ArchiveStatus* status_of(const GuiState& state, const Uuid& archive)
{
    const auto& statuses = state.archives_page.statuses;
    const auto found = std::ranges::find_if(
        statuses, [&](const ArchiveStatus& status) { return status.archive.archive_uuid == archive; });
    return found != statuses.end() ? &*found : nullptr;
}

// Whether the window can run a step now: one at a time, besides the jobs;
// otherwise says why not.
bool can_step(GuiState& state)
{
    if (!state.session) {
        show_message(state, fixed_message("Open a repository first (Settings)."));
        return false;
    }
    if (state.job.running()) {
        show_message(state, fixed_message("Another step of the window is running; wait for it to end."));
        return false;
    }
    // The bar of Archives' head shows the step: without it, the page is not
    // built (as before the repository's database was upgraded).
    return state.archives_page.abandoning != nullptr;
}

// Not while a job of the window waits or works on the archive (a relocation
// would record a revision over the retirement; a verification is told to
// stop first).
bool refused_while_busy(GuiState& state, const Uuid& archive)
{
    const auto* status = status_of(state, archive);
    return refused_while_the_archive_has_a_job(
        state, archive, status && status->header ? status->header->root_hint : std::string{});
}

// A step of retiring runs as the window's step (Job), with `doing` on the bar
// of Archives' head; `done` on the main thread once the bar is gone again.
void run_step(GuiState& state, const std::string& doing, Job::Work work, std::function<void(const Report&)> done)
{
    auto& page = state.archives_page;
    gtk_widget_set_visible(page.abandoning, TRUE);
    state.job.start(state.text, page.abandoning, std::move(work),
        [&state, done = std::move(done)](const Report& report) {
            guarded(
                [&] {
                    lock_language(state);
                    if (state.archives_page.abandoning) {
                        gtk_widget_set_visible(state.archives_page.abandoning, FALSE);
                    }
                    done(report);
                },
                [&](const Report& failure) { show_message(state, failure); });
        });
    gtk_progress_bar_set_text(GTK_PROGRESS_BAR(page.abandoning), doing.c_str());
    lock_language(state);
}

Report cannot_open(const IoError& error)
{
    return [message = error.message](const app::Text& text) {
        return text.format("Cannot open the repository: {}", text(message));
    };
}

// Why the archive is not retired, in the window's words: an unfinished
// operation that would give it a new revision, judged now as Archives judges
// its row, or the engine's message.
Report not_retired(ArchiveStore& store, const RetireFailure& failure, const std::string& host)
{
    bool runs = false;
    if (failure.refusing) {
        const auto processes = platform::make_process_table();
        runs = app::runs_as_the_window_judges(store, *failure.refusing, *processes, host,
            std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now()));
    }
    if (failure.kind == RetireFailure::Kind::Unconfirmed) {
        // Found lost after the question was asked without a confirmation.
        return fixed_message("Its latest manifest was found lost meanwhile: choose Retire archive... again, which "
                             "asks for the confirmation that this needs.");
    }
    return [failure, runs](const app::Text& text) {
        return text.format("Not retired: {}", app::retirement_message_in_window(text, failure, runs));
    };
}

// Retire: retires the archive as the window's step, after the same checks
// again (retire_archive), confirmed where the question asked for it.
void retire(GuiState& state, const Uuid& archive, const std::string& label, bool confirmed)
{
    if (!can_step(state) || refused_while_busy(state, archive)) {
        return;
    }
    run_step(state, state.text.format("Retiring {} ...", label),
        [layout = state.session->context.repo, host = state.host.host, archive, confirmed](
            CancelToken&, IProgressSink&) -> Report {
            auto store = ArchiveStore::open(layout);
            if (!store) {
                return cannot_open(store.error());
            }
            const auto repository_files = platform::make_file_system_access();
            const auto retired = retire_archive(*store, *repository_files, layout, RetireRequest{archive, confirmed});
            if (!retired) {
                return not_retired(*store, retired.error(), host);
            }
            return {};
        },
        [&state, label](const Report& refused) {
            refresh_archives(state);
            start_poll(state);
            if (refused) {
                show_message(state, refused);
                return;
            }
            show_toast(state, retired_toast(state.text, label));
        });
}

// What the question answers into.
struct RetireAnswer {
    GuiState* state;
    Uuid archive;
    std::string label;
    bool confirm = false;
};

// Asks, with Cancel as the default, what retirement_of read; for a lost
// manifest, Retire is offered once its confirmation is turned on.
void ask_retirement(GuiState& state, const Uuid& archive, const std::string& label, const Retirement& retirement)
{
    if (!state.window) {
        return;
    }
    const auto& text = state.text;
    const auto question = retire_question(text, label, retirement);
    AdwDialog* dialog = adw_alert_dialog_new(question.heading.c_str(), question.body.c_str());
    auto* alert = ADW_ALERT_DIALOG(dialog);
    adw_alert_dialog_add_response(alert, "cancel", text("Cancel").c_str());
    adw_alert_dialog_add_response(alert, "retire", question.retire.c_str());
    adw_alert_dialog_set_response_appearance(alert, "retire", ADW_RESPONSE_DESTRUCTIVE);
    adw_alert_dialog_set_default_response(alert, "cancel");
    adw_alert_dialog_set_close_response(alert, "cancel");
    if (question.confirm) {
        GtkWidget* rows = gtk_list_box_new();
        gtk_list_box_set_selection_mode(GTK_LIST_BOX(rows), GTK_SELECTION_NONE);
        gtk_widget_add_css_class(rows, "boxed-list");
        GtkWidget* confirmation = adw_switch_row_new();
        adw_preferences_row_set_use_markup(ADW_PREFERENCES_ROW(confirmation), FALSE);
        adw_preferences_row_set_title(ADW_PREFERENCES_ROW(confirmation), question.confirmation.c_str());
        gtk_list_box_append(GTK_LIST_BOX(rows), confirmation);
        adw_alert_dialog_set_extra_child(alert, rows);
        adw_alert_dialog_set_response_enabled(alert, "retire", FALSE);
        g_signal_connect(confirmation, "notify::active", G_CALLBACK(+[](GObject* row, GParamSpec*, gpointer data) {
            adw_alert_dialog_set_response_enabled(
                ADW_ALERT_DIALOG(data), "retire", adw_switch_row_get_active(ADW_SWITCH_ROW(row)));
        }), dialog);
    }
    g_signal_connect_data(dialog, "response",
        G_CALLBACK(+[](AdwAlertDialog*, const char* response, gpointer data) {
            const auto* answer = static_cast<const RetireAnswer*>(data);
            auto& state = *answer->state;
            if (!response || std::string_view(response) != "retire" || !state.window) {
                return;
            }
            const auto archive = answer->archive;
            const auto label = answer->label;
            const bool confirmed = answer->confirm;
            guarded([&] { retire(state, archive, label, confirmed); },
                [&](const Report& report) { show_message(state, report); });
        }),
        new RetireAnswer{&state, archive, label, question.confirm},
        +[](gpointer data, GClosure*) { delete static_cast<RetireAnswer*>(data); }, GConnectFlags{});
    adw_dialog_present(dialog, GTK_WIDGET(state.window));
}

} // namespace

void ask_to_retire(GuiState& state, const Uuid& archive)
{
    if (!state.window) {
        return;
    }
    const auto* status = status_of(state, archive);
    if (!status) {
        return;
    }
    if (const auto choice = retire_choice(state.text, *status); !choice.offered) {
        show_message(state, [note = choice.note](const app::Text&) { return note; });
        return;
    }
    if (!can_step(state) || refused_while_busy(state, archive)) {
        return;
    }
    const auto label = display_label(*status);
    // What retiring it would do, read first, read only; never taken from the
    // list's read, which may be old.
    auto examined = std::make_shared<std::optional<Retirement>>();
    run_step(state, state.text.format("Reading the archive {} ...", label),
        [layout = state.session->context.repo, host = state.host.host, archive, examined](
            CancelToken&, IProgressSink&) -> Report {
            auto store = ArchiveStore::open_read_only(layout);
            if (!store) {
                return cannot_open(store.error());
            }
            const auto repository_files = platform::make_file_system_access();
            auto retirement = retirement_of(*store, *repository_files, layout, archive);
            if (!retirement) {
                return not_retired(*store, retirement.error(), host);
            }
            *examined = std::move(*retirement);
            return {};
        },
        [&state, archive, label, examined](const Report& refused) {
            if (refused) {
                show_message(state, refused);
                return;
            }
            if (!*examined) {
                return;
            }
            if ((*examined)->already_retired) {
                refresh_archives(state);
                show_message(state, fixed_message("This archive is retired already."));
                return;
            }
            ask_retirement(state, archive, label, **examined);
        });
}

} // namespace va::gui
