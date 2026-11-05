// The export of an archive's manifest (R-87, decision 0046), from the menu of
// its details: a folder chosen with the system's folder chooser, into which
// the export writes a folder of its own, named for the archive, so that a
// second archive's identity.json never replaces the first's; never inside the
// root of an archive attached now, where its files would be entries that the
// next FULL verification of that archive reports as added: refused at once
// as the latest read of the attached devices found them, and again on the
// job's thread from a read of the folder's volume then. It runs as a job
// (decision 0045) with a bar of the bytes copied, which Stop cancels, writing
// nothing (R-94); it reads the repository alone, so like a comparison it
// waits for no device. Its details say the two files written, and a toast how
// it ended.
#include "gui/gui_app.h"

#include "app/archive_roots.h"
#include "common/utf8.h"
#include "core/engine/transfer.h"
#include "gui/archive_actions.h"
#include "platform/file_system.h"
#include "platform/media_enumerator.h"

#include <algorithm>
#include <cstdint>
#include <filesystem>
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

std::string label_of(const GuiState& state, const Uuid& archive)
{
    const auto* status = status_of(state, archive);
    return status ? display_label(*status) : archive.substr(0, 8);
}

} // namespace

std::vector<AttachedArchiveRoot> attached_roots(const GuiState& state)
{
    std::vector<AttachedArchiveRoot> roots;
    const auto* read = attached_now(state);
    if (!read) {
        return roots;
    }
    for (const auto& attachment : read->archives) {
        for (const auto& place : attachment.places) {
            roots.push_back({place.root, label_of(state, attachment.archive_uuid)});
        }
    }
    return roots;
}

namespace {

void start_export(GuiState& state, const Uuid& archive, const std::filesystem::path& folder)
{
    if (!state.session) {
        show_message(state, fixed_message("Open a repository first (Settings)."));
        return;
    }
    const auto* status = status_of(state, archive);
    if (!status) {
        return;
    }
    if (const auto refusal = export_refusal(state.text, folder, attached_roots(state))) {
        show_message(state, [words = *refusal](const app::Text&) { return words; });
        return;
    }
    const auto label = display_label(*status);
    const auto out = folder / path_from_utf8(export_folder_name(*status));
    // What the toast says once it ended.
    struct Ended {
        bool exported = false;
        bool stopped = false;
    };
    auto ended = std::make_shared<Ended>();
    JobSpec job;
    job.title = [label](const app::Text& text) { return export_title(text, label); };
    // The bytes of the manifest copied (R-94); it reads the repository
    // alone, so it waits for no device (decision 0045).
    job.counting = Counting::Bytes;
    job.work = [&state, archive, folder, out, label, ended](JobContext& context) -> Report {
        // Not exported, until it is.
        context.tone = JobTone::Error;
        auto& session = *state.session;
        auto store = ArchiveStore::open(session.context.repo);
        if (!store) {
            return [message = store.error().message](const app::Text& text) {
                return text.format("Cannot open the repository: {}", text(message));
            };
        }
        // Never into an archive's root, as its volume is now.
        const auto media = platform::make_media_enumerator();
        const auto repository_files = platform::make_file_system_access();
        if (const auto holding = app::roots_holding(*store, *repository_files, *media, session.context.repo, folder);
            !holding.empty()) {
            return [folder, root = holding.front()](const app::Text& text) {
                return export_refusal_in_root(text, folder, root);
            };
        }
        std::uint32_t revision = 0;
        if (const auto record = store->find_archive(archive); record && *record) {
            revision = (*record)->current_revision;
        }
        auto exported = export_archive(*store, *repository_files, *session.writer, session.context.repo, archive,
            out, context.cancel, context.progress);
        if (!exported) {
            if (exported.error().cancelled) {
                context.tone = JobTone::Stopped;
                ended->stopped = true;
            }
            return [message = exported.error().message](const app::Text& text) {
                return text.format("Not exported: {}", text(message));
            };
        }
        context.tone = JobTone::Success;
        ended->exported = true;
        return [label, revision, written = std::move(*exported)](const app::Text& text) {
            return export_report(text, label, revision, written);
        };
    };
    job.done = [&state, label, ended](std::uint64_t id, const Report&) {
        guarded(
            [&] {
                show_toast(state,
                    ended->exported ? exported_toast(state.text, label)
                                    : not_exported_toast(state.text, label, ended->stopped),
                    ToastAction{state.text("Details"), [id](GuiState& state) { open_job_details(state, id); }});
            },
            [&](const Report& error) { show_archives_status(state, error); });
    };
    const auto& text = state.text;
    const auto title = job.title(text);
    const auto id = state.jobs.start(text, std::move(job));
    job_changed(state, id);
    // Nowhere else in the window shows it until it ends: the toast leads to it.
    show_toast(state, text.format("{}: started", title),
        ToastAction{text("Details"), [id](GuiState& state) { open_job_details(state, id); }}, ToastKind::Started);
}

// What the folder chooser answers into.
struct ExportChoice {
    GuiState* state;
    Uuid archive;
};

} // namespace

void export_manifest(GuiState& state, const Uuid& archive)
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
    if (const auto choice = export_choice(state.text, *status); !choice.offered) {
        show_message(state, [note = choice.note](const app::Text&) { return note; });
        return;
    }
    GtkFileDialog* chooser = gtk_file_dialog_new();
    gtk_file_dialog_set_title(chooser, state.text.format("Choose the folder to export {} into", display_label(*status)).c_str());
    gtk_file_dialog_select_folder(chooser, state.window, nullptr,
        +[](GObject* source, GAsyncResult* result, gpointer data) {
            std::unique_ptr<ExportChoice> choice(static_cast<ExportChoice*>(data));
            GFile* folder = gtk_file_dialog_select_folder_finish(GTK_FILE_DIALOG(source), result, nullptr);
            g_object_unref(source);
            if (!folder) {
                return;  // cancelled
            }
            gchar* path = g_file_get_path(folder);
            g_object_unref(folder);
            if (!path) {
                return;
            }
            // GLib's file names are UTF-8 on Windows; on Linux the path's bytes.
            const auto chosen = path_from_utf8(path);
            g_free(path);
            auto& state = *choice->state;
            if (!state.window) {
                return;
            }
            const auto archive = choice->archive;
            guarded([&] { start_export(state, archive, chosen); },
                [&](const Report& report) { show_message(state, report); });
        },
        new ExportChoice{&state, archive});
}

} // namespace va::gui
