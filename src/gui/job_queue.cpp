#include "gui/job_queue.h"

#include "common/utf8.h"

#include <algorithm>

namespace va::gui {
namespace {

void add_key(std::vector<std::string>& keys, std::string key)
{
    if (std::ranges::find(keys, key) == keys.end()) {
        keys.push_back(std::move(key));
    }
}

std::string text_of(const nlohmann::json& json, const char* key)
{
    const auto found = json.find(key);
    return found != json.end() && found->is_string() ? found->get<std::string>() : std::string{};
}

// A path as roots are compared: made normal, with '/' between its parts and
// without a separator at its end (but a volume root's). A job's root and the
// root its operation records are the same root, written alike.
std::string comparable_root(const std::filesystem::path& root)
{
    auto text = path_to_utf8(root.lexically_normal());
    std::ranges::replace(text, '\\', '/');
    while (text.size() > 1 && text.back() == '/' && !(text.size() == 3 && text[1] == ':')) {
        text.pop_back();
    }
    return text;
}

} // namespace

std::vector<std::string> device_keys(const VolumeInfo& volume)
{
    std::vector<std::string> keys;
    for (const auto& device : volume.devices) {
        if (!device.serial.empty()) {
            add_key(keys, "device:" + device.serial);
        } else if (!device.wwn.empty()) {
            add_key(keys, "device:" + device.wwn);
        } else if (!device.device_ref.empty()) {
            add_key(keys, "attached:" + device.device_ref);
        }
    }
    if (!volume.filesystem_uuid.empty()) {
        add_key(keys, "volume:" + volume.filesystem_uuid);
    }
    return keys;
}

std::vector<std::string> recorded_device_keys(const Uuid& archive_uuid, const nlohmann::json& header)
{
    std::vector<std::string> keys;
    const auto identity = header.is_object() ? header.find("identity") : header.end();
    if (header.is_object() && identity != header.end() && identity->is_object()) {
        if (const auto devices = identity->find("devices"); devices != identity->end() && devices->is_array()) {
            for (const auto& device : *devices) {
                if (!device.is_object()) {
                    continue;
                }
                const auto serial = text_of(device, "serial");
                const auto wwn = text_of(device, "wwn");
                if (!serial.empty()) {
                    add_key(keys, "device:" + serial);
                } else if (!wwn.empty()) {
                    add_key(keys, "device:" + wwn);
                }
            }
        }
        if (const auto filesystem = text_of(*identity, "filesystem_uuid"); !filesystem.empty()) {
            add_key(keys, "volume:" + filesystem);
        }
    }
    add_key(keys, "archive:" + archive_uuid);
    return keys;
}

std::optional<std::uint64_t> DeviceQueue::ahead_of(std::uint64_t job) const
{
    const auto mine = std::ranges::find(turns_, job, &Turn::job);
    if (mine == turns_.end()) {
        return std::nullopt;
    }
    for (auto turn = turns_.begin(); turn != mine; ++turn) {
        const bool shares = std::ranges::any_of(mine->devices, [&](const std::string& device) {
            return std::ranges::find(turn->devices, device) != turn->devices.end();
        });
        if (shares) {
            return turn->job;
        }
    }
    return std::nullopt;
}

bool DeviceQueue::take(std::uint64_t job, const std::vector<std::string>& devices, const CancelToken& cancel,
    const std::function<void(std::uint64_t reading)>& waiting)
{
    if (devices.empty()) {
        return !cancel.cancelled();
    }
    std::unique_lock lock(mutex_);
    turns_.push_back({job, devices});
    bool told = false;
    for (;;) {
        if (cancel.cancelled()) {
            std::erase_if(turns_, [&](const Turn& turn) { return turn.job == job; });
            lock.unlock();
            changed_.notify_all();
            return false;
        }
        const auto ahead = ahead_of(job);
        if (!ahead) {
            return true;
        }
        if (!told && waiting) {
            told = true;
            lock.unlock();
            waiting(*ahead);
            lock.lock();
            continue;
        }
        // A cancel sets no condition: it is looked at five times a second.
        changed_.wait_for(lock, std::chrono::milliseconds(200));
    }
}

void DeviceQueue::give_back(std::uint64_t job)
{
    {
        const std::lock_guard lock(mutex_);
        std::erase_if(turns_, [&](const Turn& turn) { return turn.job == job; });
    }
    changed_.notify_all();
}

bool runs_operation_of(const JobTarget& target, const nlohmann::json& record)
{
    if (!record.is_object()) {
        return false;
    }
    if (const auto archive = text_of(record, "archive_uuid");
        target.archive_uuid && !archive.empty() && archive == *target.archive_uuid) {
        return true;
    }
    const auto root = text_of(record, "root");
    return target.root && !root.empty() && comparable_root(path_from_utf8(root)) == comparable_root(*target.root);
}

} // namespace va::gui
