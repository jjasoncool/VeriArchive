// Register Archive (§B17): choose a folder, register it (or a new revision),
// with progress, cancel and resume for an interrupted registration.
#include "gui/gui_app.h"

#include "common/utf8.h"
#include "core/engine/register_engine.h"
#include "platform/file_system.h"
#include "platform/media_enumerator.h"

namespace va::gui {
namespace {

std::string report(const app::Text& text, const RegisterOutcome& outcome)
{
    std::string body =
        text.format("Registered {} ({}) revision {}", outcome.label, outcome.archive_uuid, outcome.revision) +
        "\n\n" +
        text.format("{} files, {} empty directories, {} links, {} hashed", outcome.counts.files,
            outcome.counts.dirs, outcome.counts.links, format_bytes(outcome.counts.bytes)) +
        "\n" + text.format("Manifest: {}", path_to_utf8(outcome.manifest)) + "\n";
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

void set_running(GuiState& state, bool running)
{
    auto& view = state.register_view;
    gtk_widget_set_sensitive(view.start, !running);
    gtk_widget_set_sensitive(view.cancel, running);
    lock_language(state, running);
}

// Shows an unexpected error of a Register handler (see guarded). A start that
// failed leaves the buttons as they are when nothing runs.
void show_error(GuiState& state, Report report)
{
    if (!state.job.running()) {
        set_running(state, false);
    }
    show_report(state, std::move(report));
}

void start_registration(GuiState& state)
{
    auto& view = state.register_view;
    if (!state.session || state.job.running()) {
        show_report(state, fixed_message(state.session ? "Another operation is running."
                                                        : "Open a repository first (Settings)."));
        return;
    }
    RegisterRequest request;
    request.root = entry_path(view.folder);
    const std::string label = gtk_editable_get_text(GTK_EDITABLE(view.label));
    if (!label.empty()) {
        request.label = label;
    }
    request.new_revision = gtk_check_button_get_active(GTK_CHECK_BUTTON(view.new_revision));
    request.force_new_archive = gtk_check_button_get_active(GTK_CHECK_BUTTON(view.force_new_archive));
    request.write_media_copy = gtk_check_button_get_active(GTK_CHECK_BUTTON(view.media_copy));
    request.resume = gtk_check_button_get_active(GTK_CHECK_BUTTON(view.resume));
    if (request.root.empty()) {
        show_report(state, fixed_message("Choose the folder to register."));
        return;
    }

    set_running(state, true);
    show_report(state, [root = path_to_utf8(request.root)](const app::Text& text) {
        return text.format("Registering {} ...", root);
    });
    // The worker returns its report unworded; it is worded when shown, in the
    // language in use then.
    state.job.start(state.text, view.progress,
        [&state, request](CancelToken& cancel, IProgressSink& progress) -> Report {
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
            RegisterServices services{*archive_files, *repository_files, *session.writer, *session.writer,
                *media, *store, session.context.repo, state.host};
            auto outcome = register_archive(services, request, cancel, progress);
            if (!outcome) {
                return [message = outcome.error().message](const app::Text& text) {
                    return text.format("Not registered: {}", text(message));
                };
            }
            return [outcome = std::move(*outcome)](const app::Text& text) { return report(text, outcome); };
        },
        [&state](const Report& result) {
            guarded(
                [&] {
                    set_running(state, false);
                    gtk_progress_bar_set_text(GTK_PROGRESS_BAR(state.register_view.progress),
                        state.text("Done").c_str());
                    show_report(state, result);
                    refresh_archive_list(state);
                    refresh_archive_choices(state);
                },
                [&](Report error) { show_error(state, std::move(error)); });
        });
}

} // namespace

GtkWidget* build_register_view(GuiState& state)
{
    auto& view = state.register_view;
    GtkWidget* box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    set_margins(box, 16);
    gtk_box_append(GTK_BOX(box), make_heading(state.text("Register Archive")));
    gtk_box_append(GTK_BOX(box), make_label(state.text(
        "Registration reads every file of the folder and records its fingerprints (BLAKE3 and "
        "SHA-256) as the baseline that later verifications compare against. Nothing is written to "
        "the archive unless you ask for a media copy.")));

    GtkWidget* folder_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    view.folder = gtk_entry_new();
    gtk_widget_set_hexpand(view.folder, TRUE);
    gtk_entry_set_placeholder_text(GTK_ENTRY(view.folder), state.text("Archive folder or volume root").c_str());
    GtkWidget* choose = gtk_button_new_with_label(state.text("Choose...").c_str());
    g_signal_connect(choose, "clicked", G_CALLBACK(+[](GtkButton*, gpointer data) {
        auto& state = *static_cast<GuiState*>(data);
        guarded(
            [&] {
                choose_folder(state.window, state.register_view.folder, state.text("Choose the archive folder"));
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
        state.text("New revision of the archive registered at this folder").c_str());
    view.force_new_archive = gtk_check_button_new_with_label(
        state.text("Register as a separate archive even though this folder is registered").c_str());
    view.media_copy = gtk_check_button_new_with_label(
        state.text("Also write a manifest copy to the media (.veriarchive)").c_str());
    view.resume = gtk_check_button_new_with_label(
        state.text("Resume an interrupted registration of this folder").c_str());
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
    g_signal_connect(view.cancel, "clicked",
        G_CALLBACK(+[](GtkButton*, gpointer data) { static_cast<GuiState*>(data)->job.cancel(); }), &state);
    gtk_box_append(GTK_BOX(buttons), view.start);
    gtk_box_append(GTK_BOX(buttons), view.cancel);
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
