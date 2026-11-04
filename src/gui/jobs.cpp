#include "gui/jobs.h"

#include "app/device_alerts.h"
#include "app/keep_awake.h"
#include "common/utf8.h"

#include <algorithm>
#include <exception>
#include <utility>

namespace va::gui {

// A job's progress, worded on its thread (ProgressWording) and shown on the
// main thread, at most five times a second.
class Jobs::Progress final : public IProgressSink {
public:
    Progress(Jobs& jobs, std::uint64_t job, const app::Text& text, Counting counting)
        : jobs_(jobs), job_(job), wording_(text, counting)
    {
    }

    void on_progress(const va::Progress& progress) override
    {
        // The engine reporting progress must not see an exception: it would
        // leave the hash pipeline's threads running. The reason goes to the
        // failure log.
        try {
            if (auto bar = wording_.update(progress)) {
                jobs_.post(job_, [bar = std::move(*bar)](JobShown& shown) { shown.bar = bar; });
            }
        } catch (const std::exception& error) {
            log_failure(std::string("exception while showing progress: ") + error.what());
        }
    }

private:
    Jobs& jobs_;
    std::uint64_t job_;
    ProgressWording wording_;
};

Jobs::~Jobs()
{
    shut_down();
}

std::uint64_t Jobs::start(const app::Text& text, JobSpec spec)
{
    const auto id = next_++;
    JobShown shown;
    shown.id = id;
    shown.title = spec.title;
    shown.bar.text = text("Starting...");
    shown_.push_back(std::move(shown));
    {
        // Before its work starts, and so before its operation writes a plan
        // or a record that names this process (judge_attempt).
        const std::lock_guard lock(targets_mutex_);
        targets_.emplace_back(id, spec.target);
    }
    auto& thread = threads_.emplace_back();
    thread.id = id;
    try {
        thread.thread = std::thread([this, id, cancel = &thread.cancel, text, spec = std::move(spec),
                                        shown_as = identity_shown_]() mutable {
            Report report;
            JobTone tone = JobTone::None;
            try {
                const auto devices = spec.devices ? spec.devices() : std::vector<std::string>{};
                const bool turn = queue_.take(id, devices, *cancel, [this, id](std::uint64_t ahead) {
                    post(id, [this, ahead](JobShown& job) {
                        if (const auto* reading = find(ahead)) {
                            job.waiting_for = reading->title;
                        }
                    });
                });
                if (!turn) {
                    report = fixed_message("Stopped before it started, while it waited for the device.");
                    tone = JobTone::Stopped;
                } else {
                    post(id, [](JobShown& job) {
                        job.state = JobState::Running;
                        job.waiting_for = nullptr;
                    });
                    // The computer stays awake while the job works (decision 0043).
                    const auto awake = app::keep_awake(text);
                    Progress progress(*this, id, text, spec.counting);
                    // Decision 0044: each device alert, as the job shows it.
                    const DevicesObserved alerts = [this, id, text, shown_as](
                                                       const std::vector<PhysicalDeviceInfo>& read) {
                        std::string alert;
                        for (const auto& line : app::health_alert_lines(text, displayed_devices(read, shown_as))) {
                            alert += (alert.empty() ? "" : "\n") + line;
                        }
                        if (!alert.empty()) {
                            post(id, [alert](JobShown& job) { job.alert = alert; });
                        }
                    };
                    JobContext context{*cancel, progress, alerts, JobTone::None};
                    report = spec.work(context);
                    tone = context.tone;
                }
            } catch (const std::exception& error) {
                log_failure(std::string("exception in a job: ") + error.what());
                report = unexpected_error(error.what());
                tone = JobTone::Error;
            }
            queue_.give_back(id);
            {
                const std::lock_guard lock(targets_mutex_);
                std::erase_if(targets_, [id](const auto& target) { return target.first == id; });
            }
            finish(id, std::move(report), tone, std::move(spec.done), text);
        });
    } catch (...) {
        // No thread could be started (std::system_error): nothing runs.
        threads_.pop_back();
        shown_.pop_back();
        const std::lock_guard lock(targets_mutex_);
        std::erase_if(targets_, [id](const auto& target) { return target.first == id; });
        throw;
    }
    changed(0);
    return id;
}

void Jobs::stop(std::uint64_t job)
{
    for (auto& thread : threads_) {
        if (thread.id == job) {
            thread.cancel.cancel();
            if (auto* shown = shown_job(job)) {
                shown->stopping = true;
            }
            changed(job);
        }
    }
}

void Jobs::stop_all()
{
    for (auto& thread : threads_) {
        stop(thread.id);
    }
}

std::size_t Jobs::active_count() const
{
    return static_cast<std::size_t>(
        std::ranges::count_if(shown_, [](const JobShown& job) { return job.state != JobState::Ended; }));
}

const JobShown* Jobs::find(std::uint64_t job) const
{
    const auto found = std::ranges::find(shown_, job, &JobShown::id);
    return found == shown_.end() ? nullptr : &*found;
}

JobShown* Jobs::shown_job(std::uint64_t job)
{
    const auto found = std::ranges::find(shown_, job, &JobShown::id);
    return found == shown_.end() ? nullptr : &*found;
}

void Jobs::clear_ended()
{
    std::erase_if(shown_, [](const JobShown& job) { return job.state == JobState::Ended; });
    changed(0);
}

bool Jobs::runs_operation(const nlohmann::json& record) const
{
    const std::lock_guard lock(targets_mutex_);
    return std::ranges::any_of(
        targets_, [&](const auto& target) { return runs_operation_of(target.second, record); });
}

bool Jobs::reads(const std::optional<std::filesystem::path>& root, const std::optional<Uuid>& archive) const
{
    nlohmann::json record = nlohmann::json::object();
    if (root) {
        record["root"] = path_to_utf8(*root);
    }
    if (archive) {
        record["archive_uuid"] = *archive;
    }
    return runs_operation(record);
}

void Jobs::shut_down()
{
    shutting_down_ = true;
    for (auto& thread : threads_) {
        thread.cancel.cancel();
    }
    for (auto& thread : threads_) {
        if (thread.thread.joinable()) {
            thread.thread.join();
        }
    }
    threads_.clear();
}

void Jobs::changed(std::uint64_t job) const
{
    if (changed_ && !shutting_down_) {
        changed_(job);
    }
}

void Jobs::post(std::uint64_t job, std::function<void(JobShown&)> change)
{
    struct Change {
        Jobs* jobs;
        std::uint64_t job;
        std::function<void(JobShown&)> change;
    };
    g_idle_add(
        +[](gpointer data) -> gboolean {
            std::unique_ptr<Change> change(static_cast<Change*>(data));
            auto& jobs = *change->jobs;
            if (jobs.shutting_down_) {
                return G_SOURCE_REMOVE;
            }
            try {
                if (auto* shown = jobs.shown_job(change->job)) {
                    change->change(*shown);
                    jobs.changed(change->job);
                }
            } catch (const std::exception& error) {
                log_failure(std::string("exception while showing a job: ") + error.what());
            }
            return G_SOURCE_REMOVE;
        },
        new Change{this, job, std::move(change)});
}

void Jobs::finish(std::uint64_t job, Report report, JobTone tone,
    std::function<void(std::uint64_t, const Report&)> done, app::Text text)
{
    struct Finished {
        Jobs* jobs;
        std::uint64_t job;
        Report report;
        JobTone tone;
        std::function<void(std::uint64_t, const Report&)> done;
        app::Text text;
    };
    g_idle_add(
        +[](gpointer data) -> gboolean {
            std::unique_ptr<Finished> finished(static_cast<Finished*>(data));
            auto& jobs = *finished->jobs;
            if (jobs.shutting_down_) {
                return G_SOURCE_REMOVE;  // shut_down joins the thread
            }
            // The thread posted this last: it ends at once.
            for (auto thread = jobs.threads_.begin(); thread != jobs.threads_.end(); ++thread) {
                if (thread->id == finished->job) {
                    if (thread->thread.joinable()) {
                        thread->thread.join();
                    }
                    jobs.threads_.erase(thread);
                    break;
                }
            }
            try {
                if (auto* shown = jobs.shown_job(finished->job)) {
                    shown->state = JobState::Ended;
                    shown->waiting_for = nullptr;
                    shown->report = finished->report;
                    shown->tone = finished->tone;
                }
                if (finished->done) {
                    finished->done(finished->job, finished->report);
                }
                if (jobs.notifying_ && finished->report) {
                    notify_if_away(finished->report(finished->text), "job-" + std::to_string(finished->job));
                }
            } catch (const std::exception& error) {
                log_failure(std::string("exception after a job: ") + error.what());
            }
            jobs.changed(finished->job);
            return G_SOURCE_REMOVE;
        },
        new Finished{this, job, std::move(report), tone, std::move(done), std::move(text)});
}

} // namespace va::gui
