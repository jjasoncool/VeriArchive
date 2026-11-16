// The devices and volumes attached now (decision 0046), read on a thread of
// its own while Archives or Devices is shown: when either comes on screen,
// when Refresh on either is pressed, and every 30 seconds while one of them
// is shown; and, since decision 0053, a moment after the system told of a
// device or volume attached or removed (platform::DeviceWatch, which says
// only that something changed). The core tells of none
// (core and common hold no system code), so reading again is the only
// way to see one. A read lists the volumes, asks each device for its health
// once (app::attached_devices: on Linux a udisks call of up to 3 seconds a
// device), reads the space in use on each volume, and matches the archives'
// candidates against the volumes (where_attached); the main thread then shows
// where each archive is attached in Archives, and the devices in Devices. A
// read that took long spaces out the next ones, as the reads of the
// unfinished operations are spaced (read_due); one that has not come back
// after a while has Devices say that a device may not be answering. While
// Devices is shown, the unfinished operations are read with the devices, for
// the volumes that registrations are working on.
#include "gui/gui_app.h"

#include "app/attached_devices.h"
#include "app/unfinished_operations.h"
#include "common/utf8.h"
#include "core/engine/candidates.h"
#include "platform/file_system.h"
#include "platform/media_enumerator.h"

#include <algorithm>
#include <chrono>
#include <exception>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace va::gui {
namespace {

// The RepositoryRoot of the repository open now, whose archives a read
// matches; none while none is open.
std::optional<std::filesystem::path> repository_open(const GuiState& state)
{
    return state.session ? std::optional(state.session->context.repo.root) : std::nullopt;
}

// What one read found, handed from its thread to the main thread.
struct DevicesResult {
    GuiState* state;
    std::optional<std::filesystem::path> repository;  // whose archives it matched
    std::optional<AttachedRead> read;                 // none where it failed
    Report error;
    std::chrono::steady_clock::duration took{};
};

// On the read's thread: the archives' candidates, from the repository's
// latest manifests (load_candidate_pool, which leaves out those that cannot
// be used), then the volumes and devices. In the screenshot mode, of this
// computer's volumes only those of the sample archives' folders
// (`sample_roots`) are read.
void read_attached(const std::optional<RepositoryLayout>& layout, bool sample,
    const std::vector<std::filesystem::path>& sample_roots, DevicesResult& result)
{
    std::vector<ArchiveCandidate> candidates;
    std::optional<std::string> unread;
    if (layout) {
        if (auto store = ArchiveStore::open_read_only(*layout); !store) {
            unread = store.error().message;
        } else {
            const auto repository_files = platform::make_file_system_access();
            if (auto pool = load_candidate_pool(*store, *repository_files, *layout)) {
                candidates = std::move(pool->candidates);
            } else {
                unread = pool.error().message;
            }
        }
    }
    const auto media = platform::make_media_enumerator();
    if (sample) {
        // SAMPLE DATA for the screenshot mode's pictures (sample_volumes):
        // Devices lists it, with the health it gives each sample device, in
        // place of this computer's devices, none of which is asked anything.
        app::AttachedDevices attached;
        std::vector<std::optional<std::uint64_t>> used;
        for (auto& volume : sample_volumes()) {
            attached.volumes.push_back(std::move(volume.volume));
            used.push_back(volume.used_bytes);
        }
        attached.devices = app::devices_of(attached.volumes);
        // Where the sample archives are is told on the volumes of their
        // folders, as normal use tells it, and no other volume of this
        // computer is listed or read: the views show nothing of those volumes
        // but the archives' roots, the sample archives' own folders, and the
        // sample identity (displayed_identity).
        std::vector<VolumeInfo> own;
        for (const auto& root : sample_roots) {
            auto volume = media->volume_for_path(root);
            if (volume && std::ranges::none_of(own, [&](const VolumeInfo& known) {
                    return known.mount_point == volume->mount_point;
                })) {
                own.push_back(std::move(*volume));
            }
        }
        result.read = attached_read(
            std::move(attached), used, candidates, layout.has_value(), std::move(unread), true, std::move(own));
        return;
    }
    auto attached = app::attached_devices(*media);
    if (!attached) {
        result.error = [message = attached.error().message](const app::Text& text) {
            return text.format("Cannot list the attached volumes: {}", text(message));
        };
        return;
    }
    std::vector<std::optional<std::uint64_t>> used;
    for (const auto& volume : attached->volumes) {
        used.push_back(volume.mount_point.empty() ? std::nullopt : app::volume_used_bytes(volume.mount_point));
    }
    result.read = attached_read(std::move(*attached), used, candidates, layout.has_value(), std::move(unread));
}

// On the main thread: takes a read's result and shows it.
void apply_read(GuiState& state, DevicesResult& result)
{
    if (!state.window) {
        return;  // shutting down
    }
    auto& poll = state.devices;
    const auto previous_took = poll.took;
    poll.took = result.took;
    if (result.repository != repository_open(state)) {
        // Another repository was opened while it ran: its archives are
        // matched anew.
        poll.again = true;
    } else {
        poll.repository = std::move(result.repository);
        poll.read = std::move(result.read);
        poll.error = std::move(result.error);
        // Decision 0047: a device's own serial read is kept only while the
        // device is attached as it was described then, and only while the
        // devices were watched without a pause much longer than the reads'
        // interval: unwatched, another device may have been put into its
        // enclosure, which the enclosure's description cannot tell.
        if (poll.applied != std::chrono::steady_clock::time_point{} &&
            poll.started - poll.applied > devices_read_interval + 10 * previous_took + devices_watch_pause) {
            state.device_reads.clear();
        }
        poll.applied = std::chrono::steady_clock::now();
        if (poll.read) {
            std::vector<PhysicalDeviceInfo> attached;
            for (const auto& device : poll.read->devices) {
                attached.push_back(device.device);
            }
            keep_reads_of(state.device_reads, attached);
        }
        // A read begun after a confirmed verification recorded an archive's
        // new carrier matched that carrier: the details show what it finds.
        std::erase_if(state.carriers_recorded, [&](const auto& recorded) { return recorded.second.at < poll.started; });
        show_attached(state);
        refresh_devices(state);
    }
    if (std::exchange(poll.again, false)) {
        read_devices(state, DevicesReadWhy::Refresh);
    }
}

// Says in Devices why the read could not be shown (an unexpected error of
// the main thread's part), in place of what it found.
void show_read_error(GuiState& state, const Report& report)
{
    state.devices.read.reset();
    state.devices.error = report;
    guarded([&] {
        show_attached(state);
        refresh_devices(state);
    }, [](const Report&) {});
}

gboolean devices_tick(gpointer data)
{
    auto& state = *static_cast<GuiState*>(data);
    if (!state.window) {
        // Shutting down: the timer ends here.
        state.devices.timer = 0;
        state.devices.served = nullptr;
        return G_SOURCE_REMOVE;
    }
    guarded([&] { read_devices(state, DevicesReadWhy::Due); }, [&](const Report& report) { show_read_error(state, report); });
    // Devices names the volumes that registrations are working on, from the
    // unfinished operations, which Archives reads every few seconds while it
    // is shown: while it is not, they are read as often as the devices.
    if (state.operations.timer == 0) {
        guarded([&] { start_poll(state, true); }, [&](const Report& report) { show_poll_error(state, report); });
    }
    return G_SOURCE_CONTINUE;
}

// A read that has not come back after devices_slow_after: Devices says that
// a device may not be answering (reading_words), unless it is back by then.
gboolean slow_read_tick(gpointer data)
{
    auto& state = *static_cast<GuiState*>(data);
    if (state.window && state.devices.busy) {
        guarded([&] { refresh_devices(state); }, [](const Report&) {});
    }
    return G_SOURCE_REMOVE;
}

// The folders of the sample archives listed in Archives, whose volumes alone
// the screenshot mode reads to tell where they are attached.
std::vector<std::filesystem::path> sample_roots_of(const GuiState& state)
{
    std::vector<std::filesystem::path> roots;
    for (const auto& status : state.archives_page.statuses) {
        if (status.header && !status.header->root_hint.empty()) {
            roots.push_back(path_from_utf8(status.header->root_hint));
        }
    }
    return roots;
}

} // namespace

void read_devices(GuiState& state, DevicesReadWhy why)
{
    auto& poll = state.devices;
    // Never while the window closes: once it is gone the program is ending,
    // and waits for a read in flight; while it asks whether to close with
    // jobs running, a read begun now would only hold its closing up.
    if (!state.window || state.shell.asking_to_close) {
        return;
    }
    if (poll.busy) {
        // What the operator asked for comes once the read in flight is back.
        poll.again = poll.again || why == DevicesReadWhy::Refresh;
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    if (why == DevicesReadWhy::Due && !read_due(devices_read_interval, poll.took, poll.started, now)) {
        return;
    }
    if (poll.thread.joinable()) {
        poll.thread.join();  // the last read; its result has arrived already
    }
    auto result = std::make_unique<DevicesResult>(DevicesResult{&state, repository_open(state), {}, nullptr, {}});
    const auto layout = state.session ? std::optional(state.session->context.repo) : std::nullopt;
    const bool sample = state.identity_shown == IdentityShown::Sample;
    auto sample_roots = sample ? sample_roots_of(state) : std::vector<std::filesystem::path>{};
    poll.busy = true;
    poll.started = now;
    try {
        poll.thread = std::thread(
            [result = std::move(result), layout, sample, sample_roots = std::move(sample_roots), now]() mutable {
                try {
                    read_attached(layout, sample, sample_roots, *result);
                } catch (const std::exception& error) {
                    log_failure(std::string("exception while reading the attached devices: ") + error.what());
                    result->read.reset();
                    result->error = unexpected_error(error.what());
                }
                result->took = std::chrono::steady_clock::now() - now;
                g_idle_add(
                    +[](gpointer data) -> gboolean {
                        std::unique_ptr<DevicesResult> result(static_cast<DevicesResult*>(data));
                        auto& state = *result->state;
                        auto& poll = state.devices;
                        poll.busy = false;
                        guarded([&] { apply_read(state, *result); },
                            [&](const Report& report) { show_read_error(state, report); });
                        // A language change chosen while it ran (change_language).
                        if (auto language = std::exchange(poll.language_after, std::nullopt)) {
                            if (!poll.busy && poll.thread.joinable()) {
                                poll.thread.join();  // past its last step: it has posted this
                            }
                            guarded([&] { change_language(state, *language); },
                                [&](const Report& report) { show_message(state, report); });
                        }
                        return G_SOURCE_REMOVE;
                    },
                    result.release());
            });
    } catch (...) {
        // No thread could be started (std::system_error): nothing is in flight.
        poll.busy = false;
        throw;
    }
    g_timeout_add_seconds(static_cast<guint>(devices_slow_after.count()) + 1, slow_read_tick, &state);
    refresh_devices(state);  // that it reads
}

void watch_devices(GuiState& state, GtkWidget* page)
{
    auto& poll = state.devices;
    if (poll.timer != 0 && poll.served != page) {
        g_source_remove(poll.timer);  // it served the other page, or a page of a window that is gone
        poll.timer = 0;
    }
    if (poll.timer == 0) {
        poll.timer = g_timeout_add_seconds(static_cast<guint>(devices_read_interval.count()), devices_tick, &state);
        poll.served = page;
    }
    read_devices(state, DevicesReadWhy::Shown);
    // Devices: the unfinished operations as well, for the volumes that
    // registrations are working on (devices_tick); Archives reads them itself.
    if (state.operations.timer == 0) {
        start_poll(state);
    }
}

void stop_watching_devices(GuiState& state, GtkWidget* page)
{
    auto& poll = state.devices;
    if (poll.timer != 0 && poll.served == page) {
        g_source_remove(poll.timer);
        poll.timer = 0;
        poll.served = nullptr;
        // Decision 0047: no longer watched, a device may be put into another
        // device's enclosure unseen: the reads of the devices' own serials
        // kept are dropped (device_reads.h).
        state.device_reads.clear();
    }
}

const AttachedRead* attached_now(const GuiState& state)
{
    const auto& poll = state.devices;
    return poll.read && poll.repository == repository_open(state) ? &*poll.read : nullptr;
}

} // namespace va::gui

namespace va::gui {
namespace {

// How often the changes that wait are looked at (DeviceChanges::due).
constexpr guint changes_tick_ms = 100;

gboolean changes_tick(gpointer data)
{
    auto& state = *static_cast<GuiState*>(data);
    auto& poll = state.devices;
    if (!state.window) {
        poll.changes_timer = 0;  // shutting down
        return G_SOURCE_REMOVE;
    }
    if (!poll.changes.due(std::chrono::steady_clock::now())) {
        return G_SOURCE_CONTINUE;
    }
    poll.changes.taken();
    poll.changes_timer = 0;
    // Only while Archives or Devices is shown (its timer runs): either reads
    // when it comes on screen.
    if (poll.timer != 0) {
        guarded([&] { read_devices(state, DevicesReadWhy::Refresh); },
            [&](const Report& report) { show_read_error(state, report); });
    }
    return G_SOURCE_REMOVE;
}

} // namespace

void device_change_told(GuiState& state)
{
    auto& poll = state.devices;
    if (!state.window) {
        return;
    }
    poll.changes.told(std::chrono::steady_clock::now());
    if (poll.changes_timer == 0) {
        poll.changes_timer = g_timeout_add(changes_tick_ms, changes_tick, &state);
    }
}

void start_device_watch(GuiState& state)
{
    // Told on the watch's thread: handed to the main thread, the only one
    // that touches the window's state.
    state.device_watch = std::make_unique<platform::DeviceWatch>([&state] {
        g_idle_add(
            +[](gpointer data) -> gboolean {
                auto& state = *static_cast<GuiState*>(data);
                guarded([&] { device_change_told(state); }, [](const Report&) {});
                return G_SOURCE_REMOVE;
            },
            &state);
    });
}

void stop_device_watch(GuiState& state)
{
    state.device_watch.reset();
}

} // namespace va::gui
