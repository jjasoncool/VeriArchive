// Register Archive (§B17): choose a folder or volume, register it (or a new
// revision), with progress, cancel and resume for an interrupted registration;
// a resume of a registration that an older version left asks first (decision
// 0022, point 7).
#include "gui/gui_app.h"

#include "app/device_alerts.h"
#include "app/plain_words.h"
#include "app/typed_path.h"
#include "common/utf8.h"
#include "core/engine/register_engine.h"
#include "platform/file_system.h"
#include "platform/media_enumerator.h"
#include "platform/platform_services.h"
#include "platform/process_table.h"

#include <chrono>
#include <memory>
#include <optional>

namespace va::gui {
namespace {

std::string report(const app::Text& text, const RegisterOutcome& outcome)
{
    std::string body =
        text.format("Registered {} ({}) revision {}", outcome.label, outcome.archive_uuid, outcome.revision) +
        "\n\n";
    // Decision 0044: a device alert comes first, below the result line.
    for (const auto& line : app::health_alert_lines(text, outcome.devices)) {
        body += line + "\n";
    }
    body += text.format("{}, {}, {}, {} hashed", app::count_of(text, outcome.counts.files, app::Counted::File),
                app::count_of(text, outcome.counts.dirs, app::Counted::EmptyDirectory),
                app::count_of(text, outcome.counts.links, app::Counted::Link), format_bytes(outcome.counts.bytes)) +
            "\n" + text.format("Manifest: {}", path_to_utf8(outcome.manifest)) + "\n";
    for (const auto& device : outcome.devices) {
        body += text.format("Device health: {}", app::device_health_words(text, device)) + "\n";
    }
    if (!outcome.unreadable.empty()) {
        body += "\n" + text("Unreadable at registration (recorded without hashes, never reported as verified):") +
                "\n";
        for (const auto& entry : outcome.unreadable) {
            body += "  " + text(entry.unreadable) + "  " + entry.path.p + "\n";
        }
    }
    for (const auto& mount_point : outcome.skipped_mount_points) {
        body += text.format("Skipped mount point: {}", mount_point.p) + "\n";
    }
    for (const auto& warning : outcome.warnings) {
        body += text.format("Warning: {}", text(warning)) + "\n";
    }
    // A registration over a lost manifest, resumed here (decision 0028, point 8).
    if (outcome.lost_revision && outcome.lost_revision_left_out) {
        body += text.format("Revision {}, whose manifest was lost, was left out of the index; its verifications "
                            "stay in the history.",
                    *outcome.lost_revision) +
                "\n";
    }
    body += outcome.media_copy.empty()
                ? "\n" + text("The baseline exists only in the repository: back up RepositoryRoot.") + "\n"
                : "\n" + text.format("Media copy: {}", path_to_utf8(outcome.media_copy)) + "\n";
    return body;
}

// Shows `report` below the buttons and keeps it, so a language change can
// show it again in the new language.
void show_report(GuiState& state, Report report)
{
    set_text(state.register_view.report, report(state.text));
    state.register_view.shown_report = std::move(report);
}

// Shows an unexpected error of a Register handler (see guarded).
void show_error(GuiState& state, Report report)
{
    show_report(state, std::move(report));
}

// Whether a registration started in the window writes a media copy (R-89):
// the view's check box, as the command line's --write-media-copy.
bool media_copy(const GuiState& state)
{
    return gtk_check_button_get_active(GTK_CHECK_BUTTON(state.register_view.media_copy));
}

// Runs `request` as a job (decision 0045), its progress and report shown in
// this view until it starts another. `typed`: for Resume in the Archive List
// (decision 0022, point 7), the root as its plan names it; its root is then
// checked as `register <root> --resume` checks it (app::root_not_taken), also
// that it is a folder.
void run_registration(GuiState& state, RegisterRequest request, std::optional<std::string> typed);

void start_registration(GuiState& state)
{
    auto& view = state.register_view;
    if (!state.session) {
        show_report(state, fixed_message("Open a repository first (Settings)."));
        return;
    }
    RegisterRequest request;
    request.root = entry_root(view.folder);
    const std::string label = gtk_editable_get_text(GTK_EDITABLE(view.label));
    if (!label.empty()) {
        request.label = label;
    }
    request.new_revision = gtk_check_button_get_active(GTK_CHECK_BUTTON(view.new_revision));
    request.force_new_archive = gtk_check_button_get_active(GTK_CHECK_BUTTON(view.force_new_archive));
    request.write_media_copy = media_copy(state);
    request.resume = gtk_check_button_get_active(GTK_CHECK_BUTTON(view.resume));
    // R-111: the window has no --io; the config's override by serial applies.
    request.rotational_override = state.session->context.config.rotational_override;
    if (request.root.empty()) {
        show_report(state, fixed_message("Choose the folder or volume to register."));
        return;
    }
    run_registration(state, std::move(request), std::nullopt);
}

void run_registration(GuiState& state, RegisterRequest request, std::optional<std::string> typed)
{
    auto& view = state.register_view;
    const auto root = path_to_utf8(request.root);
    show_report(state, [root](const app::Text& text) { return text.format("Registering {} ...", root); });
    hide_alert(view.alert);
    // Filled by the worker where a resume waits for the operator's word, as
    // it takes up a registration that an older version left, which may still
    // run it (decision 0022, point 7); read by `done`, which runs only after
    // the worker finished.
    auto unconfirmed = std::make_shared<std::optional<OlderVersionRegistration>>();
    // The worker returns its report unworded; it is worded when shown, in the
    // language in use then.
    JobSpec job;
    job.title = [root](const app::Text& text) { return text.format("Register {}", root); };
    job.target.root = request.root;
    job.devices = [root = request.root] { return devices_of_root(root); };
    job.work = [&state, request, typed, unconfirmed](JobContext& context) -> Report {
            // Not registered, until it is.
            context.tone = JobTone::Error;
            // Decision 0029: a root on a network share, or one written in the
            // \\?\ form, is refused before anything of it is read, and a
            // resumed one that is no folder too, as on the command line. Asked
            // here, off the window's thread: on Linux it reads the root's filesystem.
            if (auto refusal = app::root_not_taken(request.root, typed ? *typed : path_to_utf8(request.root))) {
                return [refusal = std::move(*refusal)](const app::Text& text) {
                    return text.format("Not registered: {}", text(refusal));
                };
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
            RegisterServices services{*archive_files, *repository_files, *session.writer, *session.writer,
                *media, *store, session.context.repo, state.host, *processes, platform::final_path};
            // Decision 0044: a device alert shows in the job as soon as the
            // devices were asked; the operator may stop it, or let it go on.
            auto asked = request;
            asked.devices_observed = context.devices_observed;
            auto outcome = register_archive(services, asked, context.cancel, context.progress);
            if (!outcome) {
                // A resume of a registration that an older version left,
                // which changed nothing: the window asks the operator, as the
                // command line asks for --confirm, and says why meanwhile.
                if (outcome.error().kind == RegisterFailure::Kind::Unconfirmed && outcome.error().older_version) {
                    *unconfirmed = outcome.error().older_version;
                    context.tone = JobTone::Warning;  // nothing went wrong: the question follows
                    return [](const app::Text& text) {
                        return text.format("Not registered: {}", text(app::older_version_resume_warning));
                    };
                }
                // Refused while another operation that creates the archive's
                // next revision is unfinished, it names the window's own ways
                // on, or says that that operation still runs, judged now as
                // the Archive List judges its row (decisions 0025, 0033).
                // Stopped, or cut short by a device that went away: it can be
                // resumed.
                if (outcome.error().kind == RegisterFailure::Kind::Incomplete) {
                    context.tone = JobTone::Stopped;
                }
                const auto now = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());
                return [message = app::register_message_in_window(
                            outcome.error(), *store, *processes, state.host.host, now)](const app::Text& text) {
                    return text.format("Not registered: {}", text(message));
                };
            }
            // The devices as the view names them: the sample identity in the
            // screenshot mode, whose pictures are published.
            outcome->devices = displayed_devices(outcome->devices, state.identity_shown);
            // Entries recorded without hashes, and warnings, ask to be read.
            context.tone = outcome->unreadable.empty() && outcome->warnings.empty() ? JobTone::Success
                                                                                    : JobTone::Warning;
            return [outcome = std::move(*outcome)](const app::Text& text) { return report(text, outcome); };
        };
    job.done = [&state, request, typed, unconfirmed](std::uint64_t ended, const Report& result) {
            guarded(
                [&] {
                    // The view shows the report of the job it shows.
                    if (state.register_view.job == ended) {
                        show_report(state, result);
                    }
                    refresh_archive_list(state);
                    refresh_archive_choices(state);
                    if (!*unconfirmed) {
                        return;
                    }
                    // Decision 0022, point 7: the question, then the same
                    // resume, confirmed as with --confirm, only once the
                    // operator chose Resume in it.
                    const auto& registration = **unconfirmed;
                    const auto label = registration.label.empty() ? registration.archive_uuid.substr(0, 8)
                                                                  : registration.label;
                    ask_before_resuming(state, label, registration.root, [request, typed](GuiState& state) {
                        // A job may have started on it while the question was shown.
                        if (!state.session) {
                            show_report(state, fixed_message("Open a repository first (Settings)."));
                            return;
                        }
                        if (refused_while_a_job_reads(state, path_to_utf8(request.root), {})) {
                            return;
                        }
                        auto confirmed = request;
                        confirmed.confirmed = true;
                        run_registration(state, std::move(confirmed), typed);
                    });
                },
                [&](Report error) { show_error(state, std::move(error)); });
        };
    view.job = state.jobs.start(state.text, std::move(job));
    job_changed(state, view.job);
}

} // namespace

void resume_registration(GuiState& state, const app::UnfinishedOperation& registration, bool confirmed)
{
    // Exactly `register <root> --resume`, the root its plan names: no media
    // copy, whatever this view's box says, which is not where Resume is
    // clicked (decision 0022, point 7). A resume with a media copy is started
    // here, with the resume box and the media copy box. With --confirm where
    // the operator confirmed it in the question about an older version's
    // registration.
    show_page(state, "register");
    run_registration(state, app::registration_resume(registration, state.session->context.config, confirmed),
        registration.root);
}

GtkWidget* build_register_view(GuiState& state)
{
    auto& view = state.register_view;
    GtkWidget* box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    set_margins(box, 16);
    gtk_box_append(GTK_BOX(box), make_label(state.text(
        "Registration reads every file of the folder or volume and records its fingerprints "
        "(BLAKE3 and SHA-256) as the baseline that later verifications compare against. Nothing is "
        "written to the archive unless you ask for a media copy.")));

    GtkWidget* folder_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    view.folder = gtk_entry_new();
    gtk_widget_set_hexpand(view.folder, TRUE);
    gtk_entry_set_placeholder_text(GTK_ENTRY(view.folder), state.text("Archive folder or volume root").c_str());
    GtkWidget* choose = gtk_button_new_with_label(state.text("Choose...").c_str());
    g_signal_connect(choose, "clicked", G_CALLBACK(+[](GtkButton*, gpointer data) {
        auto& state = *static_cast<GuiState*>(data);
        guarded(
            [&] {
                choose_folder(state.window, state.register_view.folder,
                    state.text("Choose the archive folder or volume"));
            },
            [&](Report error) { show_error(state, std::move(error)); });
    }), &state);
    gtk_box_append(GTK_BOX(folder_row), view.folder);
    gtk_box_append(GTK_BOX(folder_row), choose);
    gtk_box_append(GTK_BOX(box), folder_row);

    view.label = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(view.label), state.text("Label (optional, e.g. ARCH-0012)").c_str());
    gtk_box_append(GTK_BOX(box), view.label);

    view.new_revision = gtk_check_button_new_with_label(
        state.text("New revision of the archive registered at this folder or volume").c_str());
    view.force_new_archive = gtk_check_button_new_with_label(
        state.text("Register as a separate archive even though this folder or volume is registered").c_str());
    view.media_copy = gtk_check_button_new_with_label(
        state.text("Also write a manifest copy to the media (.veriarchive)").c_str());
    view.resume = gtk_check_button_new_with_label(
        state.text("Resume an interrupted registration of this folder or volume").c_str());
    gtk_box_append(GTK_BOX(box), view.new_revision);
    gtk_box_append(GTK_BOX(box), view.force_new_archive);
    gtk_box_append(GTK_BOX(box), view.media_copy);
    gtk_box_append(GTK_BOX(box), view.resume);

    GtkWidget* buttons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    view.start = gtk_button_new_with_label(state.text("Register").c_str());
    view.cancel = gtk_button_new_with_label(state.text("Cancel").c_str());
    gtk_widget_set_sensitive(view.cancel, FALSE);
    g_signal_connect(view.start, "clicked",
        G_CALLBACK(+[](GtkButton*, gpointer data) {
            auto& state = *static_cast<GuiState*>(data);
            guarded([&] { start_registration(state); }, [&](Report error) { show_error(state, std::move(error)); });
        }),
        &state);
    g_signal_connect(view.cancel, "clicked", G_CALLBACK(+[](GtkButton*, gpointer data) {
        auto& state = *static_cast<GuiState*>(data);
        state.jobs.stop(state.register_view.job);
    }), &state);
    gtk_box_append(GTK_BOX(buttons), view.start);
    gtk_box_append(GTK_BOX(buttons), view.cancel);
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
