// Resume and Abandon... of the unfinished operations that Jobs lists
// (decision 0022, point 7): a verification, registration or relocation
// (decision 0046) there that has stopped can be resumed, wherever it was
// started. A registration that has
// stopped can be abandoned (decision 0025), as when its media failed, and so
// can a stopped relocation (decision 0026) and a stopped verification, one
// that adds chunk hashes (decision 0033) or one that adds none (decision
// 0050). An older version's registration, whose
// plan never says when it worked, can be resumed and abandoned once it reads
// as one that may have stopped; its Resume asks first, as that version may
// still run it. A step of abandoning reads what it removes first, then asks,
// with Cancel as the default.
#include "gui/gui_app.h"

#include "app/plain_words.h"
#include "core/engine/abandon_registration.h"
#include "core/engine/abandon_relocation.h"
#include "core/engine/abandon_verification.h"
#include "platform/process_table.h"

#include <algorithm>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>

namespace va::gui {
namespace {

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

// Whether the window can run a step of abandoning now, one at a time besides
// the jobs; otherwise says why not.
bool can_abandon(GuiState& state)
{
    if (state.session && !state.job.running() && state.jobs_page.abandoning) {
        return true;
    }
    // The window's step is also the upgrade of the database and retiring an
    // archive (decision 0046); without the bar of Jobs' unfinished
    // operations (decision 0049), the page is not built.
    if (!state.session || state.job.running()) {
        show_message(state, fixed_message(state.session ? "Another step of the window is running; wait for it to end."
                                                        : "Open a repository first (Settings)."));
    }
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
    auto& view = state.jobs_page;
    gtk_widget_set_visible(view.abandoning, TRUE);
    state.job.start(state.text, view.abandoning, std::move(work),
        [&state, done = std::move(done)](const Report& report) {
            guarded(
                [&] {
                    lock_language(state);
                    if (state.jobs_page.abandoning) {
                        gtk_widget_set_visible(state.jobs_page.abandoning, FALSE);
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
            refresh_archives(state);
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

    // The request names the registration shown: one that changed meanwhile
    // (resumed and finished, say) is not abandoned in its place.
    ask(state.window, text.format("Abandon the unfinished registration of {}?", label), detail,
        {{"cancel", text("Cancel")}, {"abandon", text("Abandon"), true}},
        [choice = AbandonChoice{&state, {registration.archive_uuid, registration.revision, registration.created},
             label}](const std::string& answer) {
            auto& state = *choice.state;
            if (answer != "abandon" || !state.window) {
                return;
            }
            guarded([&] { start_abandon(state, choice.request, choice.label); },
                [&](const Report& report) { show_message(state, report); });
        });
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
            refresh_archives(state);
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
    // The request names the relocation shown: one that changed meanwhile
    // (resumed and finished, say) is not abandoned in its place.
    ask(state.window, text.format("Abandon the unfinished relocation of {}?", label), detail,
        {{"cancel", text("Cancel")}, {"abandon", text("Abandon"), true}},
        [choice = RelocationAbandonChoice{&state, {relocation.archive_uuid, relocation.run_id}, label}](
            const std::string& answer) {
            auto& state = *choice.state;
            if (answer != "abandon" || !state.window) {
                return;
            }
            guarded([&] { start_relocation_abandon(state, choice.request, choice.label); },
                [&](const Report& report) { show_message(state, report); });
        });
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
            // Reads only, as Archives does (decision 0022).
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

// ---- Abandoning a verification (decisions 0033, 0050) ------------------------

struct VerificationAbandonChoice {
    GuiState* state;
    VerificationAbandonRequest request;
    std::string label;
};

Report verification_abandon_report(const AbandonableVerification& abandoned)
{
    return [abandoned](const app::Text& text) {
        return abandoned.adds_chunk_hashes
                   ? text.format("Abandoned the unfinished verification of {} ({}) that adds chunk hashes.",
                         abandoned.label, abandoned.archive_uuid)
                   : text.format("Abandoned the stopped verification of {} ({}).", abandoned.label,
                         abandoned.archive_uuid);
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
            refresh_archives(state);
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
    // The request names the run shown: one resumed meanwhile, under a run of
    // its own, is not abandoned in its place.
    const auto question = verification.adds_chunk_hashes
                              ? text.format("Abandon the unfinished verification of {} that adds chunk hashes?", label)
                              : text.format("Abandon the stopped verification of {}?", label);
    ask(state.window, question, detail, {{"cancel", text("Cancel")}, {"abandon", text("Abandon"), true}},
        [choice = VerificationAbandonChoice{&state, {verification.archive_uuid, verification.run_id}, label}](
            const std::string& answer) {
            auto& state = *choice.state;
            if (answer != "abandon" || !state.window) {
                return;
            }
            guarded([&] { start_verification_abandon(state, choice.request, choice.label); },
                [&](const Report& report) { show_message(state, report); });
        });
}

// The operator chose to abandon the verification of a row, one that adds
// chunk hashes or one that adds none: what abandoning it removes is read
// first, by the check that abandons it, as for a registration.
void ask_to_abandon_verification(GuiState& state, const app::UnfinishedOperation& operation)
{
    const auto label = operation.label.empty() ? operation.archive_uuid.substr(0, 8) : operation.label;
    // The run shown: one that changed since is refused, not read in its place.
    const VerificationAbandonRequest request{operation.archive_uuid, operation.run_id};
    auto examined = std::make_shared<std::optional<AbandonableVerification>>();
    run_abandon_step(state, state.text.format("Reading the verification of {} ...", label),
        [layout = state.session->context.repo, host = state.host.host, request, examined](
            CancelToken&, IProgressSink&) -> Report {
            // Reads only, as Archives does (decision 0022).
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

} // namespace

// What abandoning it removes is read first, by the check that abandons it
// (abandonable_registration, abandonable_relocation, abandonable_verification,
// as the CLI shows it), not taken from the read the row came from: whether
// the archive goes, and the saved progress, which is nothing once its
// revision is recorded. A refusal shows at once (an attempt that runs,
// another registration by now); otherwise the question.
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
    if (!key.empty() && found != watches.end() && found->operation().kind == app::OperationKind::Verification) {
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
            // Reads only, as Archives does (decision 0022).
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

namespace {

// ---- Resuming a verification or registration (decision 0022, point 7) -------

// The verification, registration or relocation `key` of the latest read, as
// a copy (the view a resume brings into view may have the list read again),
// or nothing, saying so, where it is no longer listed.
std::optional<app::UnfinishedOperation> listed_for_resume(GuiState& state, const std::string& key)
{
    const auto& watches = state.operations.watches;
    const auto found = std::ranges::find(watches, key,
        [](const app::OperationWatch& watch) -> const std::string& { return watch.operation().key; });
    if (key.empty() || found == watches.end()) {
        const char* gone = key.starts_with("run:")          ? "This verification is no longer listed as unfinished."
                           : key.starts_with("relocation:") ? "This relocation is no longer listed as unfinished."
                                                            : "This registration is no longer listed as unfinished.";
        show_message(state, fixed_message(gone));
        return std::nullopt;
    }
    return found->operation();
}

// Continues `operation` exactly as its command-line resume does, as a job,
// shown where an operation of its kind started in this window shows it; what
// is wrong with it by now (its media not attached, its archive at another
// revision) the resume itself says there. `confirmed`: a registration that an
// older version left, which the operator confirmed in the question (as
// --confirm).
void resume_operation(GuiState& state, const app::UnfinishedOperation& operation, bool confirmed = false)
{
    switch (operation.kind) {
    case app::OperationKind::Verification:
        resume_verification(state, operation);
        return;
    case app::OperationKind::Relocation:
        resume_relocation(state, operation);
        return;
    case app::OperationKind::Registration:
        break;
    }
    resume_registration(state, operation, confirmed);
}

// What the operator is asked about, carried into the question's answer
// (ask_before_resuming).
struct ResumeChoice {
    GuiState* state;
    std::function<void(GuiState&)> resume;
};

} // namespace

void show_message(GuiState& state, const Report& report)
{
    // A message of its own: the row the operator chose may be gone once its
    // operation is (abandoned, or resumed and finished).
    if (!state.window) {
        return;
    }
    tell(state.window, state.text, report(state.text));
}

bool can_resume(GuiState& state)
{
    if (state.session && !state.job.running()) {
        return true;
    }
    show_message(state, fixed_message(state.session ? "Another step of the window is running; wait for it to end."
                                                    : "Open a repository first (Settings)."));
    return false;
}

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

// Only once the operator chose Resume in the question does it resume as
// resume_from_list does, the resume confirmed as with --confirm.
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

void ask_before_resuming(GuiState& state, const std::string& label, const std::string& root,
    std::function<void(GuiState&)> resume)
{
    // What happens where the older version still runs the registration, and
    // what to make sure of first, with the root, so that the operator knows
    // which window or command to look for.
    const auto& text = state.text;
    const auto detail = text.format("Root: {}", root) + "\n\n" + text(app::older_version_resume_warning);
    ask(state.window, text.format("Resume the unfinished registration of {}?", label), detail,
        {{"cancel", text("Cancel")}, {"resume", text("Resume")}},
        [choice = ResumeChoice{&state, std::move(resume)}](const std::string& answer) {
            auto& state = *choice.state;
            if (answer != "resume" || !state.window) {
                return;
            }
            guarded([&] { choice.resume(state); }, [&](const Report& report) { show_message(state, report); });
        });
}

} // namespace va::gui
