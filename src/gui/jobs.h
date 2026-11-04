#pragma once
// The window's jobs (decision 0045): every registration, verification and
// comparison started in the window runs as a job of its own, on a thread of
// its own, so that several run at once. A job that reads a device waits while
// an earlier job reads it (DeviceQueue): jobs of one device run one after
// another, jobs of different devices at once; a comparison reads no media and
// waits for nothing. Each job keeps the computer awake while it works
// (decision 0043), and one that ends while no window of the program is active
// is told in a notification. What the window shows of each job lives here, on
// the main thread; the views and the Jobs page show it.
#include "app/text.h"
#include "core/engine/device_alerts.h"
#include "core/interfaces.h"
#include "gui/displayed_identity.h"
#include "gui/gui_support.h"
#include "gui/job_queue.h"
#include "gui/job_shown.h"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <functional>
#include <list>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace va::gui {

// What a job's work is given, on its thread.
struct JobContext {
    CancelToken& cancel;
    IProgressSink& progress;
    // Shows the device alert of each device that the operation reads, in the
    // job, as soon as the operation tells them (decision 0044).
    DevicesObserved devices_observed;
    // Set by the work as it returns its report: the tone that the window
    // shows its result in once the job ended (decision 0046).
    JobTone tone = JobTone::None;
};

struct JobSpec {
    Report title;                                       // what the job is, worded when shown
    JobTarget target;                                   // the root or archive it reads (judge_attempt)
    // The devices it reads (device_keys), asked first, on its thread; unset:
    // it reads none and waits for nothing.
    std::function<std::vector<std::string>()> devices;
    Counting counting = Counting::Files;
    std::function<Report(JobContext&)> work;            // on the job's thread; its report, unworded
    std::function<void(std::uint64_t job, const Report& report)> done;  // on the main thread, once it ended
};

class Jobs {
public:
    Jobs() = default;
    Jobs(const Jobs&) = delete;
    Jobs& operator=(const Jobs&) = delete;
    ~Jobs();

    // Set on the main thread before the first job starts: whether a job that
    // ends while no window is active is told in a notification (off for the
    // screenshot mode and the smoke test), which media identity alerts name
    // (the screenshot mode's sample), and what to call once a job changed (0:
    // jobs were added or removed).
    void set_notifying(bool notifying) { notifying_ = notifying; }
    void set_identity_shown(IdentityShown shown) { identity_shown_ = shown; }
    void on_change(std::function<void(std::uint64_t job)> changed) { changed_ = std::move(changed); }

    // Starts a job, worded in `text`; returns its id, never 0. On the main thread.
    std::uint64_t start(const app::Text& text, JobSpec spec);
    // Asks a job to stop: one that waits ends at once, one that works stops as
    // a cancel stops it, resumable.
    void stop(std::uint64_t job);
    void stop_all();
    // Whether a job waits or works, and how many.
    bool active() const { return active_count() > 0; }
    std::size_t active_count() const;
    // The job `job`, as the window shows it; nothing for a job removed.
    const JobShown* find(std::uint64_t job) const;
    // Every job, in the order they were started.
    const std::vector<JobShown>& shown() const { return shown_; }
    // Removes every job that ended.
    void clear_ended();
    // Whether a job that waits or works reads the root or archive of
    // `record` (runs_operation_of). Safe on any thread: the Archive List asks
    // it on a thread of its own.
    bool runs_operation(const nlohmann::json& record) const;
    // Whether a job that waits or works reads `root` or `archive`, which a
    // step that abandons or resumes that operation must not then take over.
    bool reads(const std::optional<std::filesystem::path>& root, const std::optional<Uuid>& archive) const;
    // For the application's shutdown: stops every job and waits for it;
    // their `done` is then never called.
    void shut_down();

private:
    struct Thread {
        std::uint64_t id = 0;
        CancelToken cancel;
        std::thread thread;
    };
    class Progress;
    JobShown* shown_job(std::uint64_t job);
    void changed(std::uint64_t job) const;
    // Posts `change` to the main thread, where it is made to the job's JobShown.
    void post(std::uint64_t job, std::function<void(JobShown&)> change);
    void finish(std::uint64_t job, Report report, JobTone tone,
        std::function<void(std::uint64_t, const Report&)> done, app::Text text);

    std::uint64_t next_ = 1;
    std::vector<JobShown> shown_;
    std::list<Thread> threads_;               // main thread only
    DeviceQueue queue_;
    mutable std::mutex targets_mutex_;
    std::vector<std::pair<std::uint64_t, JobTarget>> targets_;  // of the jobs that wait or work
    std::function<void(std::uint64_t)> changed_;
    bool notifying_ = true;
    IdentityShown identity_shown_ = IdentityShown::Recorded;
    bool shutting_down_ = false;
};

} // namespace va::gui
