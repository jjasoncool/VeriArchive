// Verify Archive (§B17): FULL verification of a folder or volume, or of a
// registered archive found on the attached volumes, with progress, cancel and
// resume. The result always carries its mode (R-21) and the V1 guarantee (R-02).
// For a registered archive without chunk hashes, the verification can add them
// (decision 0033).
#include "gui/gui_app.h"

#include "app/device_alerts.h"
#include "app/plain_words.h"
#include "app/typed_path.h"
#include "app/verify_report.h"
#include "common/utf8.h"
#include "core/engine/add_chunk_hashes.h"
#include "core/engine/archive_status.h"
#include "core/engine/verify_engine.h"
#include "platform/file_system.h"
#include "platform/media_enumerator.h"
#include "platform/process_table.h"

#include <chrono>
#include <memory>

namespace va::gui {
namespace {

constexpr const char* guarantee =
    "VeriArchive detects accidental corruption and unintended change; tamper evidence (signed manifests) "
    "is not part of this version.";

std::string report(const app::Text& text, const VerifyOutcome& outcome)
{
    std::string body = text.format("FULL VERIFY {}", text(outcome.result)) + "\n\n" +
                       text.format("{} ({}) revision {}", outcome.label, outcome.archive_uuid, outcome.revision) +
                       "\n";
    // Decision 0044: a device alert comes right below the archive, beside the
    // result, which it does not change.
    for (const auto& line : app::health_alert_lines(text, outcome.devices)) {
        body += line + "\n";
    }
    if (outcome.read_compared) {
        if (const auto line = app::slower_alert_line(text, *outcome.read_compared)) {
            body += *line + "\n";
        }
    }
    if (!outcome.media_read) {
        // Decision 0028: failed on its manifest before the media was looked for.
        body += text("Media: not read, as the manifest of this revision cannot be used") + "\n";
    } else {
        body += text.format("Root: {}", path_to_utf8(outcome.root)) + "\n" +
                text.format("Identity: {}", text(to_string(outcome.identity.result))) + "\n";
        for (const auto& device : outcome.devices) {
            body += text.format("Device health: {}", app::device_health_words(text, device)) + "\n";
        }
        body += text.format("Coverage: {} of {} registered bytes read and matched",
                    format_bytes(outcome.coverage_bytes), format_bytes(outcome.registered_bytes)) +
                "\n";
        if (const auto speed = app::read_speed_words(text, outcome.read_speed, outcome.read_compared)) {
            body += text.format("Read speed: {}", *speed) + "\n";
        }
        body += text("Items:");
        for (const auto& [status, count] : outcome.counts) {
            body += "  " + text(status) + " " + std::to_string(count);
        }
        body += "\n";
    }
    for (const auto& failure : outcome.failure_classes) {
        body += text.format("Failure: {}", text(app::failure_class_words(failure))) + "\n";
    }
    for (const auto& warning : outcome.warnings) {
        body += text.format("Warning: {}", text(warning)) + "\n";
    }
    for (const auto& note : outcome.notes) {
        body += text.format("Note: {}", text(note)) + "\n";
    }
    if (!outcome.items.empty()) {
        body += "\n";
        for (const auto& item : outcome.items) {
            body += text(item.status) + "  " + item.path.p +
                    (item.detail.empty() ? "" : "  " + text.parenthesized(text(app::item_detail_words(item.detail)))) +
                    "\n";
        }
    }
    // Decision 0033: the revision that records the chunk hashes added, or why
    // there is none.
    if (const auto line = app::chunk_hashes_line(text, outcome, true); !line.empty()) {
        body += "\n" + line + "\n";
    }
    if (const auto advice = app::unbaselined_advice(text, outcome, app::ReportedIn::Window)) {
        body += "\n" + *advice + "\n";
    }
    if (outcome.result == "INCOMPLETE") {
        body += "\n" + text("The run was interrupted; use Resume to continue without re-reading verified files.") +
                "\n";
    }
    body += "\n";
    if (!outcome.resumed_from.empty()) {
        body += text.format("Resumed from run: {}", outcome.resumed_from) + "\n";
    }
    body += text.format("Run: {}", outcome.run_id) + "\n" + text(guarantee) + "\n";
    return body;
}

// Shows `report` below the buttons and keeps it, so a language change can
// show it again in the new language.
void show_report(GuiState& state, Report report)
{
    set_text(state.verify_view.report, report(state.text));
    state.verify_view.shown_report = std::move(report);
}

// The run that Resume continues (app::run_to_resume): the run interrupted
// last in this window, or the stopped verification that adds chunk hashes of
// the archive chosen in the drop-down (decision 0033).
std::optional<app::RunToResume> resume_target(const GuiState& state)
{
    const auto& view = state.verify_view;
    std::optional<app::RunToResume> interrupted_here;
    if (!view.incomplete_run.empty() && view.incomplete_archive) {
        interrupted_here = app::RunToResume{view.incomplete_run, view.incomplete_root, *view.incomplete_archive};
    }
    return app::run_to_resume(interrupted_here, view.chosen_adding);
}

// Resume, offered while there is a run to continue.
void offer_resume(GuiState& state)
{
    gtk_widget_set_sensitive(state.verify_view.resume, resume_target(state).has_value());
}

// Shows an unexpected error of a Verify handler (see guarded).
void show_error(GuiState& state, Report report)
{
    show_report(state, std::move(report));
}

// The archive as a job's title names it: its label, or the start of its uuid.
std::string archive_name(const GuiState& state, const Uuid& archive_uuid)
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

// Whether a verification started in the window fails metadata differences
// (R-51): the view's check box, as the command line's --strict-metadata, or
// the policy.
bool strict_metadata(const GuiState& state)
{
    return gtk_check_button_get_active(GTK_CHECK_BUTTON(state.verify_view.strict)) ||
           state.session->context.config.policy.strict_metadata;
}

// Runs `request` as a job (decision 0045), its progress and report shown in
// this view until it starts another. A run left INCOMPLETE is offered for
// Resume here.
void run_verification(GuiState& state, VerifyRequest request);

// What the view offers for the archive chosen in the drop-down (decision
// 0033). "Full verify, and add chunk hashes" is offered only for a registered
// archive to which a verification would add them, by its rule
// (why_chunk_hashes_cannot_be_added): one whose current revision has no chunk
// size, as its manifest says (R-14), and that is not retired. For any other
// choice it is cleared and cannot be chosen. And Resume continues the
// archive's verification that adds chunk hashes once it has stopped
// (app::resumable_adding_verification), also when another window or the
// command line ran it, as app::run_to_resume says. A verification that this
// window runs is never taken for one that has stopped.
void offer_for_chosen_archive(GuiState& state)
{
    auto& view = state.verify_view;
    if (!view.add_chunk_hashes) {
        return;
    }
    bool offered = false;
    view.chosen_adding.reset();
    const auto selected = chosen_archive(view.archive, view.archive_uuids, 1);
    if (state.session && selected) {
        if (auto store = ArchiveStore::open_read_only(state.session->context.repo)) {
            if (const auto archive = store->find_archive(*selected); archive && *archive) {
                const auto repository_files = platform::make_file_system_access();
                const auto refused = why_chunk_hashes_cannot_be_added(
                    *store, *repository_files, state.session->context.repo, **archive);
                offered = refused && !*refused;
                const auto processes = platform::make_process_table();
                const app::AttemptCheck attempts{processes.get(), state.host.host,
                    [&state](const nlohmann::json& record) { return state.jobs.runs_operation(record); }};
                const auto now = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());
                if (auto stopped = app::resumable_adding_verification(*store, *selected, attempts, now)) {
                    view.chosen_adding = std::move(*stopped);
                }
            }
        }
    }
    if (!offered) {
        gtk_check_button_set_active(GTK_CHECK_BUTTON(view.add_chunk_hashes), FALSE);
    }
    gtk_widget_set_sensitive(view.add_chunk_hashes, offered);
    if (view.resume) {
        offer_resume(state);
    }
}

void start_verification(GuiState& state, bool resume)
{
    auto& view = state.verify_view;
    if (!state.session) {
        show_report(state, fixed_message("Open a repository first (Settings)."));
        return;
    }
    VerifyRequest request;
    request.strict_metadata = strict_metadata(state);
    // R-111: the window has no --io; the config's override by serial applies.
    request.rotational_override = state.session->context.config.rotational_override;
    // A resumed run adds chunk hashes as the run it continues did.
    request.add_chunk_hashes = !resume && gtk_widget_get_sensitive(view.add_chunk_hashes) &&
                               gtk_check_button_get_active(GTK_CHECK_BUTTON(view.add_chunk_hashes));
    if (resume) {
        // Resume is offered only with a run to continue (offer_resume), and
        // never while a job of the window works on it.
        const auto target = resume_target(state);
        if (!target || refused_while_a_job_reads(state, target->root ? path_to_utf8(*target->root) : std::string{},
                           target->archive_uuid)) {
            return;
        }
        request.resume_run = target->run_id;
        request.root = target->root;
        if (!request.root) {
            request.archive_uuid = target->archive_uuid;
        }
    } else {
        const auto selected = gtk_drop_down_get_selected(GTK_DROP_DOWN(view.archive));
        if (selected != GTK_INVALID_LIST_POSITION && selected > 0 && selected <= view.archive_uuids.size()) {
            request.archive_uuid = view.archive_uuids[selected - 1];
        } else {
            request.root = entry_root(view.folder);
            if (request.root->empty()) {
                show_report(state,
                    fixed_message("Choose the folder or volume to verify, or pick a registered archive."));
                return;
            }
        }
    }
    run_verification(state, std::move(request));
}

void run_verification(GuiState& state, VerifyRequest request)
{
    auto& view = state.verify_view;
    show_report(state, fixed_message("Verifying (reading every registered byte) ..."));
    hide_alert(view.alert);
    // Filled by the worker; read by `done`, which runs only after the worker finished.
    struct Interrupted {
        std::string run_id;
        Uuid archive_uuid;
    };
    auto interrupted = std::make_shared<std::optional<Interrupted>>();
    // The worker returns its report unworded; it is worded when shown, in the
    // language in use then.
    JobSpec job;
    const auto name = request.root ? path_to_utf8(*request.root) : archive_name(state, *request.archive_uuid);
    if (request.resume_run) {
        job.title = [name](const app::Text& text) { return text.format("Resume the FULL verification of {}", name); };
    } else {
        job.title = [name](const app::Text& text) { return text.format("FULL verify {}", name); };
    }
    job.target = {request.root, request.root ? std::nullopt : request.archive_uuid};
    if (request.root) {
        job.devices = [root = *request.root] { return devices_of_root(root); };
    } else {
        job.devices = [layout = state.session->context.repo, archive = *request.archive_uuid] {
            return devices_of_archive(layout, archive);
        };
    }
    job.work = [&state, request, interrupted](JobContext& context) -> Report {
            // Decision 0029: a root on a network share, or one written in the
            // \\?\ form, is refused before anything of it is read. Asked here,
            // off the window's thread: on Linux it reads the root's filesystem.
            if (request.root) {
                if (auto refusal = app::root_not_taken(*request.root, path_to_utf8(*request.root))) {
                    return [refusal = std::move(*refusal)](const app::Text& text) {
                        return text.format("Not verified: {}", text(refusal));
                    };
                }
            }
            auto& session = *state.session;
            auto store = ArchiveStore::open(session.context.repo);
            if (!store) {
                return [message = store.error().message](const app::Text& text) {
                    return text.format("Cannot open the repository: {}", text(message));
                };
            }
            const auto archive_files = platform::make_file_system_access();
            const auto repository_files = platform::make_file_system_access();
            const auto media = platform::make_media_enumerator();
            const auto processes = platform::make_process_table();
            VerifyServices services{*archive_files, *repository_files, *media, *store, session.context.repo,
                session.context.repository_uuid, state.host, *session.writer, *processes};
            // Decision 0044: a device alert shows in the job as soon as the
            // devices were asked; the operator may stop it, or let it go on.
            auto asked = request;
            asked.devices_observed = context.devices_observed;
            auto outcome = verify_full(services, asked, context.cancel, context.progress);
            if (!outcome) {
                // Decision 0022, point 7: a chosen archive that more than one
                // attached volume holds. The verification names their roots;
                // this view says what to do in it.
                if (!request.resume_run && request.archive_uuid && !outcome.error().roots_found.empty()) {
                    const auto short_uuid = request.archive_uuid->substr(0, 8);
                    std::string label;
                    if (const auto archive = store->find_archive(*request.archive_uuid); archive && *archive) {
                        label = (*archive)->label;
                    }
                    std::string roots;
                    for (const auto& root : outcome.error().roots_found) {
                        roots += "\n  " + path_to_utf8(root);
                    }
                    return [label, short_uuid, roots](const app::Text& text) {
                        const auto archive_name = label.empty() ? short_uuid : label + text.parenthesized(short_uuid);
                        return text.format(
                            "Not verified: archive {} is on more than one attached volume, as a device and a "
                            "sector-by-sector clone of it would be:{}\nVeriArchive cannot tell which one to verify. "
                            "Type the folder of the one to verify above, or choose it with \"Choose...\", choose "
                            "\"The folder or volume above\" under \"Verify:\" and press \"Verify (FULL)\"; or detach "
                            "the other volumes and verify the archive again.",
                            archive_name, roots);
                    };
                }
                // Refused while another operation that creates the archive's
                // next revision is unfinished, it names the window's own ways
                // on, or says that that operation still runs, judged now as
                // the Archive List judges its row (decision 0033).
                const auto now = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());
                return [message = app::stop_message_in_window(
                            outcome.error(), *store, *processes, state.host.host, now)](const app::Text& text) {
                    return text.format("Not verified: {}", text(message));
                };
            }
            // The devices as the view names them: the sample identity in the
            // screenshot mode, whose pictures are published.
            outcome->devices = displayed_devices(outcome->devices, state.identity_shown);
            if (outcome->result == "INCOMPLETE") {
                *interrupted = Interrupted{outcome->run_id, outcome->archive_uuid};
            } else if (request.resume_run && !outcome->media_read) {
                // Decision 0028: a resume that failed on the manifest left the
                // run it was to resume as it was, so Resume stays offered for
                // that run, for once the manifest is restored.
                *interrupted = Interrupted{*request.resume_run, outcome->archive_uuid};
            }
            return [outcome = std::move(*outcome)](const app::Text& text) { return report(text, outcome); };
        };
    job.done = [&state, request, interrupted](std::uint64_t ended, const Report& result) {
            guarded(
                [&] {
                    auto& view = state.verify_view;
                    if (*interrupted) {
                        view.incomplete_run = (*interrupted)->run_id;
                        view.incomplete_root = request.root;
                        view.incomplete_archive = (*interrupted)->archive_uuid;
                    } else if (request.resume_run && *request.resume_run == view.incomplete_run) {
                        view.incomplete_run.clear();
                    }
                    // The view shows the report of the job it shows.
                    if (view.job == ended) {
                        show_report(state, result);
                    }
                    refresh_archive_list(state);
                    // A revision with chunk hashes may have been recorded, or
                    // a verification that adds them may have stopped.
                    offer_for_chosen_archive(state);
                },
                [&](Report error) { show_error(state, std::move(error)); });
        };
    view.job = state.jobs.start(state.text, std::move(job));
    job_changed(state, view.job);
}

} // namespace

void resume_verification(GuiState& state, const app::UnfinishedOperation& verification)
{
    // Exactly `verify --full <archive_uuid> --resume <run_id>`: strict
    // metadata as the policy sets it, whatever this view's box says, which is
    // not where Resume is clicked (decision 0022, point 7).
    gtk_stack_set_visible_child_name(GTK_STACK(state.stack), "verify");
    run_verification(state, app::verification_resume(verification, state.session->context.config));
}

void refresh_archive_choices(GuiState& state)
{
    std::vector<std::string> labels;
    std::vector<Uuid> uuids;
    if (state.session) {
        if (auto store = ArchiveStore::open(state.session->context.repo)) {
            const auto now = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());
            const auto repository_files = platform::make_file_system_access();
            // Labels only: no manifest is read, so one that cannot be used
            // keeps no other archive from being chosen.
            if (const auto statuses = archive_statuses(*store, *repository_files, state.session->context.repo,
                    state.session->context.config.policy, now, StatusSelection{std::nullopt, false})) {
                for (const auto& status : *statuses) {
                    labels.push_back(display_label(status) + "  " + status.archive.archive_uuid.substr(0, 8));
                    uuids.push_back(status.archive.archive_uuid);
                }
            }
        }
    }
    const auto set_choices = [](GtkWidget* drop_down, const std::vector<std::string>& items) {
        if (!drop_down) {
            return;
        }
        std::vector<const char*> pointers;
        for (const auto& item : items) {
            pointers.push_back(item.c_str());
        }
        pointers.push_back(nullptr);
        GtkStringList* model = gtk_string_list_new(pointers.data());
        gtk_drop_down_set_model(GTK_DROP_DOWN(drop_down), G_LIST_MODEL(model));
        g_object_unref(model);
    };

    std::vector<std::string> verify_items{state.text("The folder or volume above")};
    verify_items.insert(verify_items.end(), labels.begin(), labels.end());
    set_choices(state.verify_view.archive, verify_items);
    state.verify_view.archive_uuids = uuids;
    offer_for_chosen_archive(state);

    set_choices(state.history_view.archive, labels);
    state.history_view.archive_uuids = uuids;

    // The comparison keeps the two archives chosen when the list is reloaded
    // (after a registration, say). Otherwise B suggests the second archive, so
    // A and B start out different.
    auto& compare = state.compare_view;
    const auto reference = compare.reference ? chosen_archive(compare.reference, compare.archive_uuids, 0)
                                             : std::nullopt;
    const auto copy = compare.copy ? chosen_archive(compare.copy, compare.archive_uuids, 0) : std::nullopt;
    set_choices(compare.reference, labels);
    set_choices(compare.copy, labels);
    compare.archive_uuids = uuids;
    if (compare.copy && uuids.size() > 1) {
        gtk_drop_down_set_selected(GTK_DROP_DOWN(compare.copy), 1);
    }
    if (compare.reference) {
        choose_archive(compare.reference, uuids, 0, reference);
        choose_archive(compare.copy, uuids, 0, copy);
    }
}

GtkWidget* build_verify_view(GuiState& state)
{
    auto& view = state.verify_view;
    GtkWidget* box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    set_margins(box, 16);
    gtk_box_append(GTK_BOX(box), make_heading(state.text("Verify Archive")));
    gtk_box_append(GTK_BOX(box), make_label(state.text(
        "FULL verification re-reads every registered file from the medium and compares it with "
        "the baseline. Only FULL VERIFY PASSED means 100% of the registered content was read and matched.")));

    GtkWidget* folder_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    view.folder = gtk_entry_new();
    gtk_widget_set_hexpand(view.folder, TRUE);
    gtk_entry_set_placeholder_text(GTK_ENTRY(view.folder), state.text("Archive folder or volume root").c_str());
    GtkWidget* choose = gtk_button_new_with_label(state.text("Choose...").c_str());
    g_signal_connect(choose, "clicked", G_CALLBACK(+[](GtkButton*, gpointer data) {
        auto& state = *static_cast<GuiState*>(data);
        guarded(
            [&] {
                choose_folder(state.window, state.verify_view.folder,
                    state.text("Choose the archive folder or volume"));
            },
            [&](Report error) { show_error(state, std::move(error)); });
    }), &state);
    gtk_box_append(GTK_BOX(folder_row), view.folder);
    gtk_box_append(GTK_BOX(folder_row), choose);
    gtk_box_append(GTK_BOX(box), folder_row);

    GtkWidget* archive_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_append(GTK_BOX(archive_row), make_row_label(state.text("Verify:")));
    const auto folder_above = state.text("The folder or volume above");
    const char* initial[] = {folder_above.c_str(), nullptr};
    view.archive = gtk_drop_down_new_from_strings(initial);
    gtk_box_append(GTK_BOX(archive_row), view.archive);
    gtk_box_append(GTK_BOX(box), archive_row);

    view.strict = gtk_check_button_new_with_label(
        state.text("Strict metadata (timestamp and attribute differences fail the run)").c_str());
    gtk_box_append(GTK_BOX(box), view.strict);

    view.add_chunk_hashes = gtk_check_button_new_with_label(state.text("Full verify, and add chunk hashes").c_str());
    gtk_widget_set_tooltip_text(view.add_chunk_hashes,
        state.text("For a registered archive without chunk hashes, chosen above. Every file is read once; when every "
                   "entry matches, the next revision records the same entries with their chunk hashes. Any "
                   "difference records the verification alone.")
            .c_str());
    gtk_widget_set_sensitive(view.add_chunk_hashes, FALSE);
    gtk_box_append(GTK_BOX(box), view.add_chunk_hashes);
    g_signal_connect(view.archive, "notify::selected", G_CALLBACK(+[](GObject*, GParamSpec*, gpointer data) {
        auto& state = *static_cast<GuiState*>(data);
        guarded([&] { offer_for_chosen_archive(state); }, [&](Report error) { show_error(state, std::move(error)); });
    }), &state);

    GtkWidget* buttons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    view.start = gtk_button_new_with_label(state.text("Verify (FULL)").c_str());
    view.cancel = gtk_button_new_with_label(state.text("Cancel").c_str());
    view.resume = gtk_button_new_with_label(state.text("Resume interrupted run").c_str());
    gtk_widget_set_sensitive(view.cancel, FALSE);
    // A rebuilt window (language change) can still resume an interrupted run.
    gtk_widget_set_sensitive(view.resume, resume_target(state).has_value());
    g_signal_connect(view.start, "clicked",
        G_CALLBACK(+[](GtkButton*, gpointer data) {
            auto& state = *static_cast<GuiState*>(data);
            guarded([&] { start_verification(state, false); },
                [&](Report error) { show_error(state, std::move(error)); });
        }),
        &state);
    g_signal_connect(view.resume, "clicked",
        G_CALLBACK(+[](GtkButton*, gpointer data) {
            auto& state = *static_cast<GuiState*>(data);
            guarded([&] { start_verification(state, true); },
                [&](Report error) { show_error(state, std::move(error)); });
        }),
        &state);
    g_signal_connect(view.cancel, "clicked", G_CALLBACK(+[](GtkButton*, gpointer data) {
        auto& state = *static_cast<GuiState*>(data);
        state.jobs.stop(state.verify_view.job);
    }), &state);
    gtk_box_append(GTK_BOX(buttons), view.start);
    gtk_box_append(GTK_BOX(buttons), view.cancel);
    gtk_box_append(GTK_BOX(buttons), view.resume);
    gtk_box_append(GTK_BOX(box), buttons);

    view.alert = make_alert_label();
    gtk_box_append(GTK_BOX(box), view.alert);
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
