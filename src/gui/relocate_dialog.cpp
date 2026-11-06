// The relocation of an archive to a new carrier (decisions 0004, 0026, 0046):
// the operator copied the archive with their own tools, or put its device in
// another enclosure. Opened from the menu of an archive's details, or from
// its notice where its filesystem is on another device than recorded
// (MIGRATED), with the root found there. The dialog says what relocation
// does, lists the attached volumes, read off the window's thread (a fixed
// sample list in the screenshot mode), beside the new root, chosen, typed or
// pasted, and what the relocation will do. Start relocation checks the new
// root first, off the window's thread, as the relocation will judge it
// before it reads anything: as the command line refuses it
// (app::root_not_taken), its volume told, and no other archive's root on
// that volume overlapping it (decision 0001); a refusal shows in the dialog,
// which stays open. Then it runs as a job (decision 0045), queued behind
// every job of the new carrier's devices and of the archive, shown as a card
// in the archive's details; the job checks the new root again. The relocation
// reads the new carrier in full and records the next revision only when
// every entry matches. Its report shows in the job's details, the entries
// that differ where the new carrier does not hold the recorded content, and a
// toast says how it ended. A stopped relocation is resumed from the
// unfinished operations or the archive's details, as `archive relocate
// <uuid> <root> --resume` resumes it; where its new carrier is attached at
// another drive letter or mount point now, the dialog resumes it at the root
// the operator gives (Relocate to a new carrier... opens it so while the
// relocation is listed stopped).
#include "gui/gui_app.h"

#include "app/refusal_in_window.h"
#include "app/relocation_report.h"
#include "app/typed_path.h"
#include "common/utf8.h"
#include "core/engine/candidates.h"
#include "core/engine/relocate_engine.h"
#include "gui/archive_actions.h"
#include "gui/off_thread.h"
#include "platform/file_system.h"
#include "platform/media_enumerator.h"
#include "platform/process_table.h"

#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace va::gui {
namespace {

const ArchiveStatus* status_of(const GuiState& state, const Uuid& archive)
{
    const auto& statuses = state.archives_page.statuses;
    const auto found = std::ranges::find_if(
        statuses, [&](const ArchiveStatus& status) { return status.archive.archive_uuid == archive; });
    return found != statuses.end() ? &*found : nullptr;
}

// The archive as a job's title and its toast name it: as Archives lists it
// (display_label), or by the start of its uuid.
std::string label_of(const GuiState& state, const Uuid& archive)
{
    const auto* status = status_of(state, archive);
    return status ? display_label(*status) : archive.substr(0, 8);
}

// ---- The relocation, a job --------------------------------------------------

// Runs `request` as a job (decision 0045), shown as a card in the details of
// its archive. `typed`: the new root as the operator gave it, or as the record
// of a relocation resumed names it, which is checked as `archive relocate`
// checks it (app::root_not_taken).
void run_relocation(GuiState& state, RelocateRequest request, std::string typed)
{
    const auto archive = request.archive_uuid;
    const auto label = label_of(state, archive);
    const auto root = path_to_utf8(request.root);
    // How it ended, which the toast says; filled by the worker, read by
    // `done`, which runs only after the worker finished.
    struct Ended {
        std::optional<std::uint32_t> revision;
        std::string run_id;
        bool stopped = false;
    };
    auto ended = std::make_shared<Ended>();
    JobSpec job;
    job.keeps_progress = true;
    job.title = [label, root, resume = request.resume](const app::Text& text) {
        return relocation_title(text, label, root, resume);
    };
    job.target = {request.root, archive};
    // The new carrier's devices, and the archive itself: it waits behind a
    // verification of the archive by its uuid (recorded_device_keys), as two
    // operations of one archive never run together (decision 0045).
    job.devices = [new_root = request.root, archive] {
        auto devices = devices_of_root(new_root);
        devices.push_back("archive:" + archive);
        return devices;
    };
    job.counting = Counting::Files;
    job.work = [&state, request, typed, ended](JobContext& context) -> Report {
        // Not relocated, until it is.
        context.tone = JobTone::Error;
        // Decision 0029: a root on a network share, or one written in the
        // \\?\ form, is refused before anything of it is read, and one that
        // is no folder, as on the command line. Asked here, off the window's
        // thread: on Linux it reads the root's filesystem.
        if (auto refusal = app::root_not_taken(request.root, typed)) {
            return [refusal = std::move(*refusal)](const app::Text& text) {
                return text.format("Not relocated: {}", text(refusal));
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
        RelocateServices services{*archive_files, *repository_files, *session.writer, *media, *store,
            session.context.repo, session.context.repository_uuid, state.host, *processes};
        // Decision 0044: a device alert of the new carrier shows in the job as
        // soon as its devices were asked; the operator may stop it.
        auto asked = request;
        asked.devices_observed = context.devices_observed;
        auto outcome = relocate_archive(services, asked, context.cancel, context.progress);
        if (!outcome) {
            auto failure = std::move(outcome.error());
            // The new carrier does not hold the recorded content: each entry
            // that differs, as the command line lists them.
            if (failure.kind == RelocateFailure::Kind::Differs) {
                return [failure = std::move(failure)](const app::Text& text) {
                    return app::relocation_refusal_in_window(text, failure);
                };
            }
            // Stopped, or cut short by a device that went away: Resume
            // continues it.
            if (failure.kind == RelocateFailure::Kind::Incomplete) {
                context.tone = JobTone::Stopped;
                ended->stopped = true;
            }
            // Refused while another operation that creates the archive's
            // next revision is unfinished: the window's own ways on, or that
            // that operation still runs, judged now as Archives judges its
            // row (decisions 0026, 0033).
            const auto now = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());
            const bool runs = failure.refusing &&
                              app::runs_as_the_window_judges(*store, *failure.refusing, *processes, state.host.host, now);
            return [failure = std::move(failure), runs](const app::Text& text) {
                return text.format("Not relocated: {}", app::relocation_message_in_window(text, failure, runs));
            };
        }
        // The devices as the views name them: the sample identity in the
        // screenshot mode, whose pictures are published.
        outcome->devices = displayed_devices(outcome->devices, state.identity_shown);
        context.tone = verification_tone(outcome->result);
        ended->revision = outcome->revision;
        ended->run_id = outcome->run_id;
        return [outcome = std::move(*outcome), root = path_to_utf8(request.root)](const app::Text& text) {
            return app::relocation_report(text, outcome, root) + "\n" + text(guarantee) + "\n";
        };
    };
    job.done = [&state, archive, label, ended](std::uint64_t id, const Report&) {
        guarded(
            [&] {
                // The FULL verification that recorded the new revision, which
                // View result on its rows in Jobs and in its details opens.
                if (ended->revision && !ended->run_id.empty()) {
                    state.job_runs[id] = {ended->run_id, archive};
                }
                // Its new revision and carrier, or the relocation stopped, at
                // once; its run in History too.
                refresh_archives(state);
                refresh_history(state);
                start_poll(state);
                if (ended->revision) {
                    show_toast(state, relocated_toast(state.text, label, *ended->revision),
                        ToastAction{state.text("View archive"), [archive](GuiState& state) {
                                        show_page(state, "archives");
                                        select_archive(state, archive, true);
                                    }});
                } else {
                    show_toast(state, not_relocated_toast(state.text, label, ended->stopped),
                        ToastAction{state.text("Details"), [id](GuiState& state) { open_job_details(state, id); }});
                }
            },
            [&](const Report& error) { show_archives_status(state, error); });
    };
    const auto id = state.jobs.start(state.text, std::move(job));
    // Shown as a card in its archive's details, as a verification is; FULL
    // verify is not offered there meanwhile.
    state.verification_jobs[archive] = id;
    job_changed(state, id);
    if (state.archive_selected == archive) {
        show_details(state);
    }
}

// ---- The dialog -------------------------------------------------------------

using Dialog = GuiState::RelocateDialog;

bool is_open(const GuiState& state, std::uint64_t opened)
{
    return state.relocate_dialog.dialog && state.relocate_dialog.opened == opened;
}

std::string typed_root_text(const Dialog& dialog)
{
    return dialog.folder ? std::string(gtk_editable_get_text(GTK_EDITABLE(dialog.folder))) : std::string{};
}

// What the relocation will do (relocation_summary), and Start relocation once
// a new root is given.
void show_summary(GuiState& state)
{
    auto& dialog = state.relocate_dialog;
    if (!dialog.summary) {
        return;
    }
    const auto* status = status_of(state, dialog.archive);
    if (dialog.summary_row) {
        adw_preferences_group_remove(ADW_PREFERENCES_GROUP(dialog.summary), dialog.summary_row);
        dialog.summary_row = nullptr;
    }
    const auto typed = typed_root_text(dialog);
    if (status) {
        dialog.summary_row = make_fields_row(relocation_summary(state.text, *status, typed, state.identity_shown));
        adw_preferences_group_add(ADW_PREFERENCES_GROUP(dialog.summary), dialog.summary_row);
    }
    gtk_widget_set_sensitive(dialog.start, status && !typed.empty() && !dialog.checking_root);
}

// Why the new root is refused, under it; nothing hides it.
void show_problem(Dialog& dialog, const std::optional<std::string>& problem)
{
    if (!dialog.problem) {
        return;
    }
    gtk_label_set_text(
        GTK_LABEL(g_object_get_data(G_OBJECT(dialog.problem), "va-words")), problem ? problem->c_str() : "");
    gtk_widget_set_visible(dialog.problem, problem.has_value());
}

void clear_volume_rows(Dialog& dialog)
{
    for (GtkWidget* row : dialog.volume_rows) {
        adw_preferences_group_remove(ADW_PREFERENCES_GROUP(dialog.volumes), row);
    }
    dialog.volume_rows.clear();
    dialog.radios.clear();
}

GtkWidget* words_row(const std::string& words, const char* style = nullptr, bool spinning = false)
{
    GtkWidget* row = adw_action_row_new();
    adw_preferences_row_set_use_markup(ADW_PREFERENCES_ROW(row), FALSE);
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), words.c_str());
    if (style) {
        gtk_widget_add_css_class(row, style);
    }
    if (spinning) {
        GtkWidget* spinner = gtk_spinner_new();
        gtk_spinner_start(GTK_SPINNER(spinner));
        adw_action_row_add_prefix(ADW_ACTION_ROW(row), spinner);
    }
    return row;
}

void add_volume_row(Dialog& dialog, GtkWidget* row)
{
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(dialog.volumes), row);
    dialog.volume_rows.push_back(row);
}

// What a button of a volume's row is for: the volume's root, whose folders
// the folder chooser opens in.
struct FolderIn {
    GuiState* state;
    std::string root;
};

// A volume as Register Archive's first page lists it: chosen whole with its
// radio, where nothing on it would overlap the new root, or a folder in it.
GtkWidget* volume_row(GuiState& state, const VolumeRow& shown)
{
    auto& dialog = state.relocate_dialog;
    GtkWidget* row = adw_action_row_new();
    adw_preferences_row_set_use_markup(ADW_PREFERENCES_ROW(row), FALSE);
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), shown.title.c_str());
    std::string subtitle = shown.where;
    for (const auto& line : {shown.details, shown.note}) {
        if (!line.empty()) {
            subtitle += "\n" + line;
        }
    }
    adw_action_row_set_subtitle(ADW_ACTION_ROW(row), subtitle.c_str());
    gtk_widget_add_css_class(row, "numeric");

    GtkWidget* radio = gtk_check_button_new();
    gtk_widget_set_valign(radio, GTK_ALIGN_CENTER);
    if (!dialog.radios.empty()) {
        gtk_check_button_set_group(GTK_CHECK_BUTTON(radio), GTK_CHECK_BUTTON(dialog.radios.front()));
    }
    dialog.radios.push_back(radio);
    adw_action_row_add_prefix(ADW_ACTION_ROW(row), radio);
    gtk_accessible_update_property(GTK_ACCESSIBLE(radio), GTK_ACCESSIBLE_PROPERTY_LABEL,
        state.text.format("{} · {}", shown.title, shown.where).c_str(), -1);
    g_object_set_data_full(G_OBJECT(radio), "va-root", g_strdup(shown.root.c_str()), g_free);
    if (shown.selectable) {
        adw_action_row_set_activatable_widget(ADW_ACTION_ROW(row), radio);
        g_signal_connect(radio, "toggled", G_CALLBACK(+[](GtkCheckButton* toggled, gpointer data) {
            auto& state = *static_cast<GuiState*>(data);
            auto& dialog = state.relocate_dialog;
            if (!gtk_check_button_get_active(toggled) || dialog.filling || !dialog.folder) {
                return;
            }
            // The volume chosen is the new root, in the folder's row.
            dialog.filling = true;
            gtk_editable_set_text(GTK_EDITABLE(dialog.folder),
                static_cast<const char*>(g_object_get_data(G_OBJECT(toggled), "va-root")));
            dialog.filling = false;
            guarded(
                [&] {
                    show_problem(dialog, std::nullopt);
                    show_summary(state);
                },
                [&](const Report& report) { show_message(state, report); });
        }), &state);
    } else {
        // Not as a whole: it holds roots of other archives (decision 0001).
        gtk_widget_set_sensitive(radio, FALSE);
    }
    if (!shown.folder_button.empty()) {
        GtkWidget* choose = gtk_button_new_with_label(shown.folder_button.c_str());
        gtk_widget_set_valign(choose, GTK_ALIGN_CENTER);
        gtk_widget_set_tooltip_text(choose, shown.folder_tooltip.c_str());
        gtk_accessible_update_property(
            GTK_ACCESSIBLE(choose), GTK_ACCESSIBLE_PROPERTY_LABEL, shown.folder_tooltip.c_str(), -1);
        g_signal_connect_data(choose, "clicked",
            G_CALLBACK(+[](GtkButton*, gpointer data) {
                const auto* folder = static_cast<const FolderIn*>(data);
                auto& state = *folder->state;
                guarded(
                    [&] {
                        if (state.window && state.relocate_dialog.folder) {
                            choose_folder(state.window, state.relocate_dialog.folder,
                                state.text("Choose the new root of the archive"), path_from_utf8(folder->root));
                        }
                    },
                    [&](const Report& report) { show_message(state, report); });
            }),
            new FolderIn{&state, shown.root}, +[](gpointer data, GClosure*) { delete static_cast<FolderIn*>(data); },
            GConnectFlags{});
        adw_action_row_add_suffix(ADW_ACTION_ROW(row), choose);
    }
    // The archive's own filesystem in a folder of it (MIGRATED): that folder
    // made the new root.
    if (!shown.own_button.empty()) {
        GtkWidget* use = gtk_button_new_with_label(shown.own_button.c_str());
        gtk_widget_set_valign(use, GTK_ALIGN_CENTER);
        gtk_widget_set_tooltip_text(use, shown.own_tooltip.c_str());
        gtk_accessible_update_property(
            GTK_ACCESSIBLE(use), GTK_ACCESSIBLE_PROPERTY_LABEL, shown.own_tooltip.c_str(), -1);
        g_signal_connect_data(use, "clicked",
            G_CALLBACK(+[](GtkButton*, gpointer data) {
                const auto* folder = static_cast<const FolderIn*>(data);
                auto& state = *folder->state;
                if (state.relocate_dialog.folder) {
                    gtk_editable_set_text(GTK_EDITABLE(state.relocate_dialog.folder), folder->root.c_str());
                }
            }),
            new FolderIn{&state, shown.own_root}, +[](gpointer data, GClosure*) { delete static_cast<FolderIn*>(data); },
            GConnectFlags{});
        adw_action_row_add_suffix(ADW_ACTION_ROW(row), use);
    }
    return row;
}

// Chooses the radio of the volume typed, where it is listed; no radio
// otherwise.
void choose_typed_volume(Dialog& dialog)
{
    const auto typed = typed_root_text(dialog);
    dialog.filling = true;
    for (GtkWidget* radio : dialog.radios) {
        const auto* root = static_cast<const char*>(g_object_get_data(G_OBJECT(radio), "va-root"));
        const bool chosen = gtk_widget_get_sensitive(radio) && root && typed == root;
        if (gtk_check_button_get_active(GTK_CHECK_BUTTON(radio)) != chosen) {
            gtk_check_button_set_active(GTK_CHECK_BUTTON(radio), chosen);
        }
    }
    dialog.filling = false;
}

// Lists the volumes that a read found, or why it found none.
void show_volumes(GuiState& state, VolumesRead& read)
{
    auto& dialog = state.relocate_dialog;
    const auto& text = state.text;
    clear_volume_rows(dialog);
    if (read.failure) {
        add_volume_row(dialog, words_row(read.failure(text), "error"));
        return;
    }
    if (read.unread) {
        add_volume_row(dialog, words_row(read.unread(text), "error"));
    }
    const auto names = archive_names(state.archives_page.statuses, state.identity_shown);
    const auto rows = volume_rows(text, read.volumes, read.candidates, read.pending, names,
        read.sample ? IdentityShown::Recorded : state.identity_shown, VolumeUse::Relocation, dialog.archive);
    if (rows.empty()) {
        add_volume_row(dialog, words_row(text("No volume is attached."), "dim-label"));
    }
    for (const auto& shown : rows) {
        add_volume_row(dialog, volume_row(state, shown));
    }
    choose_typed_volume(dialog);
}

// Reads the attached volumes and the archives on them off the window's
// thread; the screenshot mode lists its sample volumes, never this
// computer's.
void read_volumes(GuiState& state)
{
    auto& dialog = state.relocate_dialog;
    if (!state.session || !dialog.volumes) {
        return;
    }
    clear_volume_rows(dialog);
    add_volume_row(dialog, words_row(state.text("Reading the attached volumes..."), "dim-label", true));
    const auto layout = state.session->context.repo;
    const bool sample = state.identity_shown == IdentityShown::Sample;
    read_off_thread<VolumesRead>(state, [layout, sample] { return read_attached_volumes(layout, sample); },
        [opened = dialog.opened](GuiState& state, VolumesRead& read) {
            if (is_open(state, opened)) {
                show_volumes(state, read);
            }
        });
}

// What Start relocation finds of the new root, off the window's thread, as
// the relocation will judge it before it reads anything of it: refused as
// the command line refuses it (app::root_not_taken), its volume and root
// subpath there, and the archives that matching finds on volumes.
struct RootCheck {
    std::optional<std::string> refused;  // app::root_not_taken, an English sentence
    std::string unidentified;            // why its volume cannot be told, a platform sentence
    std::string no_subpath;              // why its root subpath cannot be told, an engine sentence
    std::optional<VolumeInfo> volume;
    std::string root_subpath;
    std::vector<ArchiveCandidate> candidates;
    std::vector<PendingRoot> pending;
    Report unread;                       // the archives cannot be read: the relocation reads them itself
    Report failure;
};

// The relocation, or its resume, that Start runs at the new root typed:
// exactly `archive relocate <uuid> <root> [--resume]`, the readers as the
// config's override by serial says (R-111: the window has no --io).
std::optional<RelocateRequest> chosen_request(GuiState& state)
{
    auto& dialog = state.relocate_dialog;
    const auto root = entry_root(dialog.folder);
    if (root.empty() || !state.session) {
        return std::nullopt;
    }
    RelocateRequest request;
    if (dialog.resuming) {
        request = app::relocation_resume(*dialog.resuming, state.session->context.config);
    } else {
        request.archive_uuid = dialog.archive;
        request.rotational_override = state.session->context.config.rotational_override;
    }
    request.root = root;
    return request;
}

// The new root checked: why it is refused, in the dialog, which stays open;
// otherwise the job, once the dialog is gone.
void root_checked(GuiState& state, RootCheck& check)
{
    auto& dialog = state.relocate_dialog;
    const auto& text = state.text;
    dialog.checking_root = false;
    if (dialog.checking) {
        gtk_spinner_stop(GTK_SPINNER(dialog.checking));
        gtk_widget_set_visible(dialog.checking, FALSE);
    }
    std::optional<std::string> refusal;
    if (check.failure) {
        refusal = check.failure(text);
    } else if (check.refused) {
        refusal = text(*check.refused);
    } else if (!check.unidentified.empty()) {
        refusal = text.format("cannot identify the volume of the new root: {}", text(check.unidentified));
    } else if (!check.no_subpath.empty()) {
        refusal = text(check.no_subpath);
    } else if (check.volume) {
        refusal = relocation_root_refusal(text, *check.volume, check.root_subpath, check.candidates, dialog.archive,
            archive_names(state.archives_page.statuses, state.identity_shown));
    }
    show_problem(dialog, refusal);
    show_summary(state);
    auto request = chosen_request(state);
    if (refusal || !request) {
        return;
    }
    const auto typed = typed_root_text(dialog);
    // Forgotten at once (as close_dialogs does): it closes once its
    // animation has ended, and nothing may fill it meanwhile.
    GtkWidget* closing = std::exchange(state.relocate_dialog, {}).dialog;
    adw_dialog_close(ADW_DIALOG(closing));
    run_relocation(state, std::move(*request), typed);
}

// Start relocation (or Resume relocation): the new root checked off the
// window's thread first, then the job.
void start_from_dialog(GuiState& state)
{
    auto& dialog = state.relocate_dialog;
    const auto typed = typed_root_text(dialog);
    const auto request = chosen_request(state);
    if (!request || dialog.checking_root) {
        return;
    }
    const auto archive = dialog.archive;
    const auto* status = status_of(state, archive);
    const std::string recorded = status && status->header ? status->header->root_hint : std::string{};
    // Not while a job of the window waits or works on the archive, or reads
    // at the new root.
    if (refused_while_the_archive_has_a_job(state, archive, recorded) ||
        refused_while_the_archive_has_a_job(state, archive, path_to_utf8(request->root))) {
        return;
    }
    // A resume: only the relocation shown, as Unfinished operations lists it
    // now (decision 0022, point 7); resumed, finished or abandoned meanwhile,
    // or running again, it is not taken.
    if (dialog.resuming) {
        const auto listed = listed_relocation(state.text, state.operations.watches, archive,
            app::OperationWatch::Clock::now(),
            std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now()));
        if (!listed || listed->operation.run_id != dialog.resuming->run_id) {
            show_message(state, fixed_message("This relocation is no longer listed as unfinished."));
            return;
        }
        if (!listed->resumable && status) {
            show_message(state, [note = relocation_choice(state.text, *status, listed).note](const app::Text&) {
                return note;
            });
            return;
        }
    }
    dialog.checking_root = true;
    show_problem(dialog, std::nullopt);
    show_summary(state);
    if (dialog.checking) {
        gtk_widget_set_visible(dialog.checking, TRUE);
        gtk_spinner_start(GTK_SPINNER(dialog.checking));
    }
    read_off_thread<RootCheck>(state,
        [root = request->root, typed, layout = state.session->context.repo] {
            RootCheck check;
            // Decision 0029: refused before anything of it is read, as on
            // the command line; on Linux this reads the root's filesystem.
            if (auto refusal = app::root_not_taken(root, typed)) {
                check.refused = std::move(*refusal);
                return check;
            }
            const auto media = platform::make_media_enumerator();
            const auto volume = media->volume_for_path(root);
            if (!volume) {
                check.unidentified = volume.error().message;
                return check;
            }
            const auto subpath = root_subpath_of(root, *volume);
            if (!subpath) {
                check.no_subpath = subpath.error().message;
                return check;
            }
            check.volume = *volume;
            check.root_subpath = *subpath;
            read_archives(layout, check.candidates, check.pending, check.unread);
            return check;
        },
        [opened = dialog.opened](GuiState& state, RootCheck& check) {
            if (is_open(state, opened)) {
                root_checked(state, check);
            }
        });
}

} // namespace

void open_relocation(GuiState& state, const Uuid& archive, const std::optional<std::string>& preset_root)
{
    if (!state.window) {
        return;
    }
    if (!state.session) {
        show_message(state, fixed_message("Open a repository first (Settings)."));
        return;
    }
    const auto* status = status_of(state, archive);
    if (!status) {
        return;
    }
    const auto& text = state.text;
    // One relocation of an archive at a time (decision 0026): a stopped one,
    // as the latest read of the unfinished operations lists it, is resumed.
    const auto listed = listed_relocation(text, state.operations.watches, archive, app::OperationWatch::Clock::now(),
        std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now()));
    // Offered only where it applies; the menu says why not.
    if (const auto choice = relocation_choice(text, *status, listed); !choice.offered) {
        show_message(state, [note = choice.note](const app::Text&) { return note; });
        return;
    }
    if (refused_while_the_archive_has_a_job(
            state, archive, status->header ? status->header->root_hint : std::string{})) {
        return;
    }
    close_dialogs(state);
    auto& dialog = state.relocate_dialog;
    dialog.archive = archive;
    dialog.opened = ++state.relocations_opened;
    if (listed) {
        dialog.resuming = listed->operation;
    }
    const auto title = dialog.resuming ? text("Resume the relocation") : text("Relocate to a new carrier");
    AdwDialog* shown = adw_dialog_new();
    dialog.dialog = GTK_WIDGET(shown);
    adw_dialog_set_title(shown, title.c_str());
    adw_dialog_set_content_width(shown, 680);
    adw_dialog_set_content_height(shown, 640);

    GtkWidget* view = adw_toolbar_view_new();
    GtkWidget* header = adw_header_bar_new();
    adw_header_bar_set_title_widget(
        ADW_HEADER_BAR(header), adw_window_title_new(title.c_str(), display_label(*status).c_str()));
    adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(view), header);
    GtkWidget* box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 18);
    gtk_widget_set_margin_start(box, 24);
    gtk_widget_set_margin_end(box, 24);
    gtk_widget_set_margin_top(box, 6);
    gtk_widget_set_margin_bottom(box, 18);
    gtk_box_append(GTK_BOX(box), make_words(relocation_intro(text), "dim-label"));
    if (dialog.resuming) {
        gtk_box_append(GTK_BOX(box), make_words(relocation_resuming(text, dialog.resuming->root)));
    }

    dialog.volumes = adw_preferences_group_new();
    adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(dialog.volumes), text("Attached volumes").c_str());
    GtkWidget* again = gtk_button_new_from_icon_name("view-refresh-symbolic");
    gtk_widget_add_css_class(again, "flat");
    gtk_widget_set_valign(again, GTK_ALIGN_CENTER);
    gtk_widget_set_tooltip_text(again, text("Read the attached volumes again").c_str());
    gtk_accessible_update_property(
        GTK_ACCESSIBLE(again), GTK_ACCESSIBLE_PROPERTY_LABEL, text("Read the attached volumes again").c_str(), -1);
    g_signal_connect(again, "clicked", G_CALLBACK(+[](GtkButton*, gpointer data) {
        auto& state = *static_cast<GuiState*>(data);
        guarded([&] { read_volumes(state); }, [&](const Report& report) { show_message(state, report); });
    }), &state);
    adw_preferences_group_set_header_suffix(ADW_PREFERENCES_GROUP(dialog.volumes), again);
    gtk_box_append(GTK_BOX(box), dialog.volumes);

    // The new root, chosen or typed: also a volume's GUID path, which the
    // folder chooser may not offer.
    GtkWidget* folders = adw_preferences_group_new();
    adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(folders), text("New root").c_str());
    adw_preferences_group_set_description(ADW_PREFERENCES_GROUP(folders),
        text("The folder or volume on the new carrier that holds the archive's files now. You can also type or paste "
             "its path, including a volume's GUID path.")
            .c_str());
    dialog.folder = adw_entry_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(dialog.folder), text("Folder or volume").c_str());
    GtkWidget* choose = gtk_button_new_with_label(text("Choose a folder...").c_str());
    gtk_widget_set_valign(choose, GTK_ALIGN_CENTER);
    gtk_widget_add_css_class(choose, "flat");
    g_signal_connect(choose, "clicked", G_CALLBACK(+[](GtkButton*, gpointer data) {
        auto& state = *static_cast<GuiState*>(data);
        guarded(
            [&] {
                if (state.window && state.relocate_dialog.folder) {
                    choose_folder(state.window, state.relocate_dialog.folder,
                        state.text("Choose the new root of the archive"));
                }
            },
            [&](const Report& report) { show_message(state, report); });
    }), &state);
    adw_entry_row_add_suffix(ADW_ENTRY_ROW(dialog.folder), choose);
    // The root found where its filesystem is (MIGRATED), or where the
    // relocation resumed stopped.
    if (preset_root) {
        gtk_editable_set_text(GTK_EDITABLE(dialog.folder), preset_root->c_str());
    } else if (dialog.resuming) {
        gtk_editable_set_text(GTK_EDITABLE(dialog.folder), dialog.resuming->root.c_str());
    }
    g_signal_connect(dialog.folder, "changed", G_CALLBACK(+[](GtkEditable*, gpointer data) {
        auto& state = *static_cast<GuiState*>(data);
        auto& dialog = state.relocate_dialog;
        if (dialog.filling) {
            return;
        }
        guarded(
            [&] {
                choose_typed_volume(dialog);
                show_problem(dialog, std::nullopt);
                show_summary(state);
            },
            [&](const Report& report) { show_message(state, report); });
    }), &state);
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(folders), dialog.folder);
    gtk_box_append(GTK_BOX(box), folders);
    // Why Start found the new root refused, as Register Archive says it.
    dialog.problem = make_line_box("va-error", "dialog-warning-symbolic", "error");
    GtkWidget* problem_words = make_words("", nullptr, true);
    gtk_widget_set_hexpand(problem_words, TRUE);
    gtk_box_append(GTK_BOX(dialog.problem), problem_words);
    g_object_set_data(G_OBJECT(dialog.problem), "va-words", problem_words);
    gtk_widget_set_visible(dialog.problem, FALSE);
    gtk_box_append(GTK_BOX(box), dialog.problem);

    dialog.summary = adw_preferences_group_new();
    adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(dialog.summary), text("What the relocation does").c_str());
    adw_preferences_group_set_description(ADW_PREFERENCES_GROUP(dialog.summary),
        relocation_reading(text, status->header ? status->header->revision : status->archive.current_revision).c_str());
    gtk_box_append(GTK_BOX(box), dialog.summary);

    GtkWidget* scroller = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroller), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_propagate_natural_height(GTK_SCROLLED_WINDOW(scroller), TRUE);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroller), box);
    gtk_widget_set_vexpand(scroller, TRUE);
    adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(view), scroller);

    GtkWidget* buttons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_widget_set_margin_start(buttons, 24);
    gtk_widget_set_margin_end(buttons, 24);
    gtk_widget_set_margin_top(buttons, 12);
    gtk_widget_set_margin_bottom(buttons, 18);
    GtkWidget* cancel = gtk_button_new_with_label(text("Cancel").c_str());
    g_signal_connect_swapped(cancel, "clicked", G_CALLBACK(adw_dialog_close), shown);
    gtk_box_append(GTK_BOX(buttons), cancel);
    GtkWidget* gap = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_hexpand(gap, TRUE);
    gtk_box_append(GTK_BOX(buttons), gap);
    dialog.checking = gtk_spinner_new();
    gtk_widget_set_visible(dialog.checking, FALSE);
    gtk_box_append(GTK_BOX(buttons), dialog.checking);
    dialog.start =
        gtk_button_new_with_label(dialog.resuming ? text("Resume relocation").c_str() : text("Start relocation").c_str());
    gtk_widget_add_css_class(dialog.start, "suggested-action");
    g_signal_connect(dialog.start, "clicked", G_CALLBACK(+[](GtkButton*, gpointer data) {
        auto& state = *static_cast<GuiState*>(data);
        guarded([&] { start_from_dialog(state); }, [&](const Report& report) { show_message(state, report); });
    }), &state);
    gtk_box_append(GTK_BOX(buttons), dialog.start);
    adw_toolbar_view_add_bottom_bar(ADW_TOOLBAR_VIEW(view), buttons);
    adw_dialog_set_child(shown, view);

    g_signal_connect(shown, "closed", G_CALLBACK(+[](AdwDialog* closed, gpointer data) {
        auto& state = *static_cast<GuiState*>(data);
        if (state.relocate_dialog.dialog == GTK_WIDGET(closed)) {
            state.relocate_dialog = {};
        }
    }), &state);
    show_summary(state);
    read_volumes(state);
    adw_dialog_present(shown, GTK_WIDGET(state.window));
    gtk_widget_grab_focus(dialog.folder);
}

void resume_relocation(GuiState& state, const app::UnfinishedOperation& relocation)
{
    // Exactly `archive relocate <uuid> <root> --resume`, the new root its
    // record names (decision 0022, point 7). Shown in its archive's details,
    // which a narrow window brings into view.
    select_archive(state, relocation.archive_uuid, true);
    run_relocation(state, app::relocation_resume(relocation, state.session->context.config), relocation.root);
}

} // namespace va::gui
