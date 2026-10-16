// Verify Archive (§B17): FULL verification of a folder or volume, or of a
// registered archive found on the attached volumes, with progress, cancel and
// resume. The result always carries its mode (R-21) and the V1 guarantee (R-02).
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

std::string report(const app::Text& text, const VerifyOutcome& outcome)
{
    std::string body =
        text.format("FULL VERIFY {}", text(outcome.result)) + "\n\n" +
        text.format("{} ({}) revision {}", outcome.label, outcome.archive_uuid, outcome.revision) + "\n" +
        text.format("Root: {}", path_to_utf8(outcome.root)) + "\n" +
        text.format("Identity: {}", text(to_string(outcome.identity.result))) + "\n" +
        text.format("Coverage: {} of {} registered bytes read and matched", format_bytes(outcome.coverage_bytes),
            format_bytes(outcome.registered_bytes)) +
        "\n" + text("Items:");
    for (const auto& [status, count] : outcome.counts) {
        body += "  " + text(status) + " " + std::to_string(count);
    }
    body += "\n";
    for (const auto& failure : outcome.failure_classes) {
        body += text.format("Failure: {}", text(failure)) + "\n";
    }
    for (const auto& warning : outcome.warnings) {
        body += text.format("Warning: {}", text(warning)) + "\n";
    }
    if (!outcome.items.empty()) {
        body += "\n";
        for (const auto& item : outcome.items) {
            body += text(item.status) + "  " + item.path.p +
                    (item.detail.empty() ? "" : "  " + text.parenthesized(text(item.detail))) + "\n";
        }
    }
    if (outcome.result == "INCOMPLETE") {
        body += "\n" + text("The run was interrupted; use Resume to continue without re-reading verified files.") +
                "\n";
    }
    body += "\n" + text.format("Run: {}", outcome.run_id) + "\n" + text(guarantee) + "\n";
    return body;
}

// Shows `report` below the buttons and keeps it, so a language change can
// show it again in the new language.
void show_report(GuiState& state, Report report)
{
    set_text(state.verify_view.report, report(state.text));
    state.verify_view.shown_report = std::move(report);
}

void set_running(GuiState& state, bool running)
{
    auto& view = state.verify_view;
    gtk_widget_set_sensitive(view.start, !running);
    gtk_widget_set_sensitive(view.cancel, running);
    gtk_widget_set_sensitive(view.resume, !running && !view.incomplete_run.empty());
    lock_language(state, running);
}

// Shows an unexpected error of a Verify handler (see guarded). A start that
// failed leaves the buttons as they are when nothing runs.
void show_error(GuiState& state, Report report)
{
    if (!state.job.running()) {
        set_running(state, false);
    }
    show_report(state, std::move(report));
}

void start_verification(GuiState& state, bool resume)
{
    auto& view = state.verify_view;
    if (!state.session || state.job.running()) {
        show_report(state, fixed_message(state.session ? "Another operation is running."
                                                        : "Open a repository first (Settings)."));
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
            request.root = entry_root(view.folder);
            if (request.root->empty()) {
                show_report(state,
                    fixed_message("Choose the folder or volume to verify, or pick a registered archive."));
                return;
            }
        }
    }

    set_running(state, true);
    show_report(state, fixed_message("Verifying (reading every registered byte) ..."));
    // Filled by the worker; read by `done`, which runs only after the worker finished.
    struct Interrupted {
        std::string run_id;
        Uuid archive_uuid;
    };
    auto interrupted = std::make_shared<std::optional<Interrupted>>();
    // The worker returns its report unworded; it is worded when shown, in the
    // language in use then.
    state.job.start(state.text, view.progress,
        [&state, request, interrupted](CancelToken& cancel, IProgressSink& progress) -> Report {
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
            VerifyServices services{*archive_files, *repository_files, *media, *store, session.context.repo,
                session.context.repository_uuid, state.host};
            auto outcome = verify_full(services, request, cancel, progress);
            if (!outcome) {
                return [message = outcome.error().message](const app::Text& text) {
                    return text.format("Not verified: {}", text(message));
                };
            }
            if (outcome->result == "INCOMPLETE") {
                *interrupted = Interrupted{outcome->run_id, outcome->archive_uuid};
            }
            return [outcome = std::move(*outcome)](const app::Text& text) { return report(text, outcome); };
        },
        [&state, request, interrupted](const Report& result) {
            guarded(
                [&] {
                    auto& view = state.verify_view;
                    if (*interrupted) {
                        view.incomplete_run = (*interrupted)->run_id;
                        view.incomplete_root = request.root;
                        view.incomplete_archive = (*interrupted)->archive_uuid;
                    } else {
                        view.incomplete_run.clear();
                    }
                    set_running(state, false);
                    gtk_progress_bar_set_text(GTK_PROGRESS_BAR(view.progress), state.text("Done").c_str());
                    show_report(state, result);
                    refresh_archive_list(state);
                },
                [&](Report error) { show_error(state, std::move(error)); });
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
            const auto repository_files = platform::make_file_system_access();
            if (const auto statuses = archive_statuses(*store, *repository_files, state.session->context.repo,
                    state.session->context.config.policy, now)) {
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

    GtkWidget* buttons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    view.start = gtk_button_new_with_label(state.text("Verify (FULL)").c_str());
    view.cancel = gtk_button_new_with_label(state.text("Cancel").c_str());
    view.resume = gtk_button_new_with_label(state.text("Resume interrupted run").c_str());
    gtk_widget_set_sensitive(view.cancel, FALSE);
    // A rebuilt window (language change) can still resume an interrupted run.
    gtk_widget_set_sensitive(view.resume, !view.incomplete_run.empty());
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
