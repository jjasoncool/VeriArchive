#pragma once
// The reads that the window's dialogs make off its thread (OffThreadReads):
// Register Archive and the relocation dialog read the attached volumes, which
// a slow or sleeping device may hold up, and what lies on them.
#include "gui/gui_app.h"

#include <atomic>
#include <exception>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace va::gui {

// Runs `read` on a thread of its own (OffThreadReads); its result comes back
// to the main thread, the only one allowed to touch widgets, through the GLib
// main loop, where `done` takes it, unless the window is gone. An exception
// of the read is its result's `failure` (a Report).
template <typename Result>
void read_off_thread(GuiState& state, std::function<Result()> read, std::function<void(GuiState&, Result&)> done)
{
    struct Posted {
        GuiState* state;
        Result result;
        std::function<void(GuiState&, Result&)> done;
    };
    state.reads.prune();
    auto finished = std::make_shared<std::atomic<bool>>(false);
    auto posted = std::make_unique<Posted>(Posted{&state, Result{}, std::move(done)});
    std::thread thread([read = std::move(read), posted = std::move(posted), finished]() mutable {
        try {
            posted->result = read();
        } catch (const std::exception& error) {
            log_failure(std::string("exception while reading for a dialog: ") + error.what());
            posted->result.failure = unexpected_error(error.what());
        }
        g_idle_add(
            +[](gpointer data) -> gboolean {
                std::unique_ptr<Posted> posted(static_cast<Posted*>(data));
                auto& state = *posted->state;
                if (state.window) {
                    guarded([&] { posted->done(state, posted->result); },
                        [&](const Report& report) { show_message(state, report); });
                }
                return G_SOURCE_REMOVE;
            },
            posted.release());
        finished->store(true);
    });
    state.reads.reads.push_back({std::move(thread), std::move(finished)});
}

// The archives that matching finds on the volumes, from the latest
// revisions' manifests (load_candidate_pool), and the roots of the
// unfinished registrations; `unread` says why they could not be read.
void read_archives(const RepositoryLayout& layout, std::vector<ArchiveCandidate>& candidates,
    std::vector<PendingRoot>& pending, Report& unread);

// The attached volumes, with the space in use on each, and what lies on them.
struct VolumesRead {
    bool sample = false;                  // the screenshot mode's sample list
    std::vector<AttachedVolume> volumes;
    std::vector<ArchiveCandidate> candidates;
    std::vector<PendingRoot> pending;
    Report unread;                        // why the archives on them cannot be told
    Report failure;                       // why the volumes cannot be listed
};

// Reads them (on the thread that read_off_thread runs it on): in the
// screenshot mode (`sample`) the fixed sample list (sample_volumes), never
// this computer's volumes.
VolumesRead read_attached_volumes(const RepositoryLayout& layout, bool sample);

} // namespace va::gui
