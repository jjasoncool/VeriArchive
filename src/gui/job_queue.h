#pragma once
// The window's jobs (decision 0045): what each job reads, the queue of jobs
// that read one device, and which operation a job runs. Plain C++ without
// GTK, so the unit tests can check it.
//
// Jobs that read the same device run one after another, in the order they
// were started: a disk read in two places at once reads both slower than one
// after the other, and two reads of one archive could never run together. A
// job that reads no media (a comparison) waits for nothing.
#include "common/types.h"
#include "core/interfaces.h"

#include <nlohmann/json.hpp>

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace va::gui {

// The devices that reading `volume` reads, as keys that the queue compares:
// each device by its serial, else its WWN, else the system's name for it
// while it is attached; and the volume itself, by its filesystem id, so that
// two jobs of one volume wait for each other also where its devices tell
// nothing.
std::vector<std::string> device_keys(const VolumeInfo& volume);

// The devices that an archive's latest revision records as its carrier
// (its manifest header's identity, `identity` in the header JSON), as
// device_keys gives them, for a job that names the archive by its uuid and
// has not found its volume yet; and the archive itself.
std::vector<std::string> recorded_device_keys(const Uuid& archive_uuid, const nlohmann::json& header);

// One job's turn at its devices. Jobs wait in the order they asked; a job
// reads once no job before it in the queue reads or waits for one of its
// devices. Safe on any thread.
class DeviceQueue {
public:
    // Waits for the job's turn at `devices` (none: at once). `waiting` is told
    // once, before it waits, of the job ahead of it with one of its devices,
    // which reads it or waits for it too. Returns false, holding nothing,
    // where `cancel` was set first.
    bool take(std::uint64_t job, const std::vector<std::string>& devices, const CancelToken& cancel,
        const std::function<void(std::uint64_t reading)>& waiting = {});
    // The job is done with its devices, or gives up waiting.
    void give_back(std::uint64_t job);

private:
    struct Turn {
        std::uint64_t job = 0;
        std::vector<std::string> devices;
    };
    // The job ahead of `job` in the queue that has one of its devices, if any.
    std::optional<std::uint64_t> ahead_of(std::uint64_t job) const;

    std::mutex mutex_;
    std::condition_variable changed_;
    std::vector<Turn> turns_;  // in the order the jobs asked
};

// What a job reads: the root it was given, or the archive it names by its
// uuid, to tell the operation that a plan or record is of (judge_attempt).
struct JobTarget {
    std::optional<std::filesystem::path> root;
    std::optional<Uuid> archive_uuid;
};

// Whether `record` (a registration's plan, a verification's record) is of
// the operation of a job with `target`: the root it names is the job's root
// (written alike: the same path once made normal), or the archive it names
// is the job's archive.
bool runs_operation_of(const JobTarget& target, const nlohmann::json& record);

} // namespace va::gui
