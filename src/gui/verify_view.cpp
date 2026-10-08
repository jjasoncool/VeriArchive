// Verify Archive (§B17): FULL verification of a folder or of a registered
// archive found on the attached volumes, with progress, cancel and resume. The
// result always carries its mode (R-21) and the V1 guarantee (R-02).
#include "gui/gui_app.h"

#include "common/utf8.h"
#include "core/engine/archive_status.h"
#include "core/engine/verify_engine.h"
#include "platform/file_system.h"
#include "platform/media_enumerator.h"

#include <chrono>
#include <memory>

namespace va::gui {
namespace {

constexpr const char* guarantee =
    "V1 detects accidental corruption and unintended change; tamper evidence (signed manifests) "
    "begins in V1.1.";

std::string report(const VerifyOutcome& outcome)
{
    std::string text = "FULL VERIFY " + outcome.result + "\n\n" + outcome.label + " (" +
                       outcome.archive_uuid + ") revision " + std::to_string(outcome.revision) +
                       "\nRoot: " + path_to_utf8(outcome.root) +
                       "\nIdentity: " + std::string(to_string(outcome.identity.result)) +
                       "\nCoverage: " + format_bytes(outcome.coverage_bytes) + " of " +
                       format_bytes(outcome.registered_bytes) + " registered bytes read and matched\nItems:";
    for (const auto& [status, count] : outcome.counts) {
        text += "  " + status + " " + std::to_string(count);
    }
    text += "\n";
    for (const auto& failure : outcome.failure_classes) {
        text += "Failure: " + failure + "\n";
    }
    for (const auto& warning : outcome.warnings) {
        text += "Warning: " + warning + "\n";
    }
    if (!outcome.items.empty()) {
        text += "\n";
        for (const auto& item : outcome.items) {
            text += item.status + "  " + item.path.p + (item.detail.empty() ? "" : "  (" + item.detail + ")") + "\n";
        }
    }
    if (outcome.result == "INCOMPLETE") {
        text += "\nThe run was interrupted; use Resume to continue without re-reading verified files.\n";
    }
    text += "\nRun: " + outcome.run_id + "\n" + guarantee + "\n";
    return text;
}

void set_running(GuiState& state, bool running)
{
    auto& view = state.verify_view;
    gtk_widget_set_sensitive(view.start, !running);
    gtk_widget_set_sensitive(view.cancel, running);
    gtk_widget_set_sensitive(view.resume, !running && !view.incomplete_run.empty());
}

void start_verification(GuiState& state, bool resume)
{
    auto& view = state.verify_view;
    if (!state.session || state.job.running()) {
        set_text(view.report, state.session ? "Another operation is running." : "Open a repository first (Settings).");
        return;
    }
    VerifyRequest request;
    request.strict_metadata = gtk_check_button_get_active(GTK_CHECK_BUTTON(view.strict)) ||
                              state.session->context.config.policy.strict_metadata;
    if (resume) {
        request.resume_run = view.incomplete_run;
        request.root = view.incomplete_root;
        if (!request.root) {
            request.archive_uuid = view.incomplete_archive;
        }
    } else {
        const auto selected = gtk_drop_down_get_selected(GTK_DROP_DOWN(view.archive));
        if (selected != GTK_INVALID_LIST_POSITION && selected > 0 && selected <= view.archive_uuids.size()) {
            request.archive_uuid = view.archive_uuids[selected - 1];
        } else {
            request.root = entry_path(view.folder);
            if (request.root->empty()) {
                set_text(view.report, "Choose the folder to verify, or pick a registered archive.");
                return;
            }
        }
    }

    set_running(state, true);
    set_text(view.report, "Verifying (reading every registered byte) ...");
    // Filled by the worker; read by `done`, which runs only after the worker finished.
    struct Interrupted {
        std::string run_id;
        Uuid archive_uuid;
    };
    auto interrupted = std::make_shared<std::optional<Interrupted>>();
    state.job.start(view.progress,
        [&state, request, interrupted](CancelToken& cancel, IProgressSink& progress) -> std::string {
            auto& session = *state.session;
            auto store = ArchiveStore::open(session.context.repo);
            if (!store) {
                return "Cannot open the repository: " + store.error().message;
            }
            const auto archive_files = platform::make_file_system_access();
            const auto repository_files = platform::make_file_system_access();
            const auto media = platform::make_media_enumerator();
            VerifyServices services{*archive_files, *repository_files, *media, *store, session.context.repo,
                session.context.repository_uuid, state.host};
            const auto outcome = verify_full(services, request, cancel, progress);
            if (!outcome) {
                return "Not verified: " + outcome.error().message;
            }
            if (outcome->result == "INCOMPLETE") {
                *interrupted = Interrupted{outcome->run_id, outcome->archive_uuid};
            }
            return report(*outcome);
        },
        [&state, request, interrupted](const std::string& text) {
            auto& view = state.verify_view;
            if (*interrupted) {
                view.incomplete_run = (*interrupted)->run_id;
                view.incomplete_root = request.root;
                view.incomplete_archive = (*interrupted)->archive_uuid;
            } else {
                view.incomplete_run.clear();
            }
            set_running(state, false);
            gtk_progress_bar_set_text(GTK_PROGRESS_BAR(view.progress), "Done");
            set_text(view.report, text);
            refresh_archive_list(state);
        });
}

} // namespace

void refresh_archive_choices(GuiState& state)
{
    std::vector<std::string> labels;
    std::vector<Uuid> uuids;
    if (state.session) {
        if (auto store = ArchiveStore::open(state.session->context.repo)) {
            const auto now = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());
            if (const auto statuses = archive_statuses(*store, state.session->context.config.policy, now)) {
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

    std::vector<std::string> verify_items{"The folder above"};
    verify_items.insert(verify_items.end(), labels.begin(), labels.end());
    set_choices(state.verify_view.archive, verify_items);
    state.verify_view.archive_uuids = uuids;

    set_choices(state.history_view.archive, labels);
    state.history_view.archive_uuids = uuids;
}

GtkWidget* build_verify_view(GuiState& state)
{
    auto& view = state.verify_view;
    GtkWidget* box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    set_margins(box, 16);
    gtk_box_append(GTK_BOX(box), make_heading("Verify Archive"));
    gtk_box_append(GTK_BOX(box), make_label(
        "FULL verification re-reads every registered file from the medium and compares it with "
        "the baseline. Only FULL VERIFY PASSED means 100% of the registered content was read and matched."));

    GtkWidget* folder_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    view.folder = gtk_entry_new();
    gtk_widget_set_hexpand(view.folder, TRUE);
    gtk_entry_set_placeholder_text(GTK_ENTRY(view.folder), "Archive folder or volume root");
    GtkWidget* choose = gtk_button_new_with_label("Choose...");
    g_signal_connect(choose, "clicked", G_CALLBACK(+[](GtkButton*, gpointer data) {
        auto& state = *static_cast<GuiState*>(data);
        choose_folder(state.window, state.verify_view.folder);
    }), &state);
    gtk_box_append(GTK_BOX(folder_row), view.folder);
    gtk_box_append(GTK_BOX(folder_row), choose);
    gtk_box_append(GTK_BOX(box), folder_row);

    GtkWidget* archive_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_append(GTK_BOX(archive_row), make_label("Verify:"));
    const char* initial[] = {"The folder above", nullptr};
    view.archive = gtk_drop_down_new_from_strings(initial);
    gtk_box_append(GTK_BOX(archive_row), view.archive);
    gtk_box_append(GTK_BOX(box), archive_row);

    view.strict = gtk_check_button_new_with_label("Strict metadata (timestamp and attribute differences fail the run)");
    gtk_box_append(GTK_BOX(box), view.strict);

    GtkWidget* buttons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    view.start = gtk_button_new_with_label("Verify (FULL)");
    view.cancel = gtk_button_new_with_label("Cancel");
    view.resume = gtk_button_new_with_label("Resume interrupted run");
    gtk_widget_set_sensitive(view.cancel, FALSE);
    gtk_widget_set_sensitive(view.resume, FALSE);
    g_signal_connect(view.start, "clicked",
        G_CALLBACK(+[](GtkButton*, gpointer data) { start_verification(*static_cast<GuiState*>(data), false); }),
        &state);
    g_signal_connect(view.resume, "clicked",
        G_CALLBACK(+[](GtkButton*, gpointer data) { start_verification(*static_cast<GuiState*>(data), true); }),
        &state);
    g_signal_connect(view.cancel, "clicked",
        G_CALLBACK(+[](GtkButton*, gpointer data) { static_cast<GuiState*>(data)->job.cancel(); }), &state);
    gtk_box_append(GTK_BOX(buttons), view.start);
    gtk_box_append(GTK_BOX(buttons), view.cancel);
    gtk_box_append(GTK_BOX(buttons), view.resume);
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
