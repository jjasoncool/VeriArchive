#include "gui/off_thread.h"

#include "core/engine/candidates.h"
#include "platform/file_system.h"
#include "platform/media_enumerator.h"

namespace va::gui {

void read_archives(const RepositoryLayout& layout, std::vector<ArchiveCandidate>& candidates,
    std::vector<PendingRoot>& pending, Report& unread)
{
    auto store = ArchiveStore::open_read_only(layout);
    if (!store) {
        unread = [message = store.error().message](const app::Text& text) {
            return text.format("Cannot open the repository: {}", text(message));
        };
        return;
    }
    const auto repository_files = platform::make_file_system_access();
    if (auto pool = load_candidate_pool(*store, *repository_files, layout)) {
        candidates = std::move(pool->candidates);
    } else {
        unread = [message = pool.error().message](const app::Text& text) {
            return text.format("Cannot open the repository: {}", text(message));
        };
        return;
    }
    if (const auto registrations = store->pending_registrations()) {
        for (const auto& registration : *registrations) {
            pending.push_back({registration.archive_uuid, registration.label, registration.root});
        }
    } else {
        unread = [message = registrations.error().message](const app::Text& text) {
            return text.format("Cannot read the unfinished operations: {}", text(message));
        };
    }
}

VolumesRead read_attached_volumes(const RepositoryLayout& layout, bool sample)
{
    VolumesRead read;
    read.sample = sample;
    if (sample) {
        for (auto& volume : sample_volumes()) {
            read.volumes.push_back({std::move(volume.volume), volume.used_bytes});
        }
    } else {
        const auto media = platform::make_media_enumerator();
        auto listed = media->volumes();
        if (!listed) {
            read.failure = [message = listed.error().message](const app::Text& text) {
                return text.format("Cannot list the attached volumes: {}", text(message));
            };
            return read;
        }
        for (auto& volume : *listed) {
            const auto used = app::volume_used_bytes(volume.mount_point);
            read.volumes.push_back({std::move(volume), used});
        }
    }
    read_archives(layout, read.candidates, read.pending, read.unread);
    return read;
}

} // namespace va::gui
