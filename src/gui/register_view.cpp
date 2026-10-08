// Register Archive (§B17): choose a folder, register it (or a new revision),
// with progress, cancel and resume for an interrupted registration.
#include "gui/gui_app.h"

#include "common/utf8.h"
#include "core/engine/register_engine.h"
#include "platform/file_system.h"
#include "platform/media_enumerator.h"

namespace va::gui {
namespace {

std::string report(const RegisterOutcome& outcome)
{
    std::string text = "Registered " + outcome.label + " (" + outcome.archive_uuid + ") revision " +
                       std::to_string(outcome.revision) + "\n\n" + std::to_string(outcome.counts.files) +
                       " files, " + std::to_string(outcome.counts.dirs) + " empty directories, " +
                       std::to_string(outcome.counts.links) + " links, " + format_bytes(outcome.counts.bytes) +
                       " hashed\nManifest: " + path_to_utf8(outcome.manifest) + "\n";
    if (!outcome.unreadable.empty()) {
        text += "\nUnreadable at registration (recorded without hashes, never reported as verified):\n";
        for (const auto& entry : outcome.unreadable) {
            text += "  " + entry.unreadable + "  " + entry.path.p + "\n";
        }
    }
    for (const auto& mount_point : outcome.skipped_mount_points) {
        text += "Skipped mount point: " + mount_point.p + "\n";
    }
    for (const auto& warning : outcome.warnings) {
        text += "Warning: " + warning + "\n";
    }
    text += outcome.media_copy.empty()
                ? "\nThe baseline exists only in the repository: back up RepositoryRoot.\n"
                : "\nMedia copy: " + path_to_utf8(outcome.media_copy) + "\n";
    return text;
}

void set_running(GuiState& state, bool running)
{
    auto& view = state.register_view;
    gtk_widget_set_sensitive(view.start, !running);
    gtk_widget_set_sensitive(view.cancel, running);
}

void start_registration(GuiState& state)
{
    auto& view = state.register_view;
    if (!state.session || state.job.running()) {
        set_text(view.report, state.session ? "Another operation is running." : "Open a repository first (Settings).");
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
        set_text(view.report, "Choose the folder to register.");
        return;
    }

    set_running(state, true);
    set_text(view.report, "Registering " + path_to_utf8(request.root) + " ...");
    state.job.start(view.progress,
        [&state, request](CancelToken& cancel, IProgressSink& progress) -> std::string {
            auto& session = *state.session;
            auto store = ArchiveStore::open(session.context.repo);
            if (!store) {
                return "Cannot open the repository: " + store.error().message;
            }
            const auto archive_files = platform::make_file_system_access();
            const auto repository_files = platform::make_file_system_access();
            const auto media = platform::make_media_enumerator();
            RegisterServices services{*archive_files, *repository_files, *session.writer, *session.writer,
                *media, *store, session.context.repo, state.host};
            const auto outcome = register_archive(services, request, cancel, progress);
            if (!outcome) {
                return "Not registered: " + outcome.error().message;
            }
            return report(*outcome);
        },
        [&state](const std::string& text) {
            set_running(state, false);
            gtk_progress_bar_set_text(GTK_PROGRESS_BAR(state.register_view.progress), "Done");
            set_text(state.register_view.report, text);
            refresh_archive_list(state);
            refresh_archive_choices(state);
        });
}

} // namespace

GtkWidget* build_register_view(GuiState& state)
{
    auto& view = state.register_view;
    GtkWidget* box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    set_margins(box, 16);
    gtk_box_append(GTK_BOX(box), make_heading("Register Archive"));
    gtk_box_append(GTK_BOX(box), make_label(
        "Registration reads every file of the folder and records its fingerprints (BLAKE3 and "
        "SHA-256) as the baseline that later verifications compare against. Nothing is written to "
        "the archive unless you ask for a media copy."));

    GtkWidget* folder_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    view.folder = gtk_entry_new();
    gtk_widget_set_hexpand(view.folder, TRUE);
    gtk_entry_set_placeholder_text(GTK_ENTRY(view.folder), "Archive folder or volume root");
    GtkWidget* choose = gtk_button_new_with_label("Choose...");
    g_signal_connect(choose, "clicked", G_CALLBACK(+[](GtkButton*, gpointer data) {
        auto& state = *static_cast<GuiState*>(data);
        choose_folder(state.window, state.register_view.folder);
    }), &state);
    gtk_box_append(GTK_BOX(folder_row), view.folder);
    gtk_box_append(GTK_BOX(folder_row), choose);
    gtk_box_append(GTK_BOX(box), folder_row);

    view.label = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(view.label), "Label (optional, e.g. ARCH-0012)");
    gtk_box_append(GTK_BOX(box), view.label);

    view.new_revision = gtk_check_button_new_with_label("New revision of the archive registered at this folder");
    view.force_new_archive = gtk_check_button_new_with_label("Register as a separate archive even though this folder is registered");
    view.media_copy = gtk_check_button_new_with_label("Also write a manifest copy to the media (.veriarchive)");
    view.resume = gtk_check_button_new_with_label("Resume an interrupted registration of this folder");
    gtk_box_append(GTK_BOX(box), view.new_revision);
    gtk_box_append(GTK_BOX(box), view.force_new_archive);
    gtk_box_append(GTK_BOX(box), view.media_copy);
    gtk_box_append(GTK_BOX(box), view.resume);

    GtkWidget* buttons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    view.start = gtk_button_new_with_label("Register");
    view.cancel = gtk_button_new_with_label("Cancel");
    gtk_widget_set_sensitive(view.cancel, FALSE);
    g_signal_connect(view.start, "clicked",
        G_CALLBACK(+[](GtkButton*, gpointer data) { start_registration(*static_cast<GuiState*>(data)); }), &state);
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
