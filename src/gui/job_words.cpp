#include "gui/job_words.h"

#include <algorithm>

namespace va::gui {
namespace {

// The first line of the job's report that holds text: its result; empty
// before it ended and where the report holds none.
std::string result_line(const app::Text& text, const JobShown& job)
{
    const auto report = job.report ? job.report(text) : std::string{};
    const auto first = report.find_first_not_of("\n \t");
    if (first == std::string::npos) {
        return {};
    }
    auto line = report.substr(first, report.find('\n', first) - first);
    line.erase(line.find_last_not_of(" \t\r") + 1);
    return line;
}

std::string waiting_words(const app::Text& text, const JobShown& job)
{
    return job.waiting_for ? text.format("Waiting: {} reads the same device", job.waiting_for(text))
                           : text("Waiting for its turn");
}

} // namespace

JobGroup group_of(const JobShown& job)
{
    switch (job.state) {
    case JobState::Running:
        return JobGroup::Running;
    case JobState::Waiting:
        return JobGroup::Waiting;
    case JobState::Ended:
        break;
    }
    return JobGroup::Ended;
}

GroupedJobs group_jobs(const std::vector<JobShown>& jobs, std::size_t ended_limit)
{
    GroupedJobs grouped;
    for (const auto& job : jobs) {
        switch (group_of(job)) {
        case JobGroup::Running:
            grouped.running.push_back(&job);
            break;
        case JobGroup::Waiting:
            grouped.waiting.push_back(&job);
            break;
        case JobGroup::Ended:
            break;
        }
    }
    for (auto job = jobs.rbegin(); job != jobs.rend() && grouped.ended.size() < ended_limit; ++job) {
        if (group_of(*job) == JobGroup::Ended) {
            grouped.ended.push_back(&*job);
        }
    }
    return grouped;
}

JobCounts count_jobs(const std::vector<JobShown>& jobs)
{
    JobCounts counts;
    for (const auto& job : jobs) {
        const auto group = group_of(job);
        counts.running += group == JobGroup::Running ? 1 : 0;
        counts.waiting += group == JobGroup::Waiting ? 1 : 0;
    }
    return counts;
}

std::string state_words(const app::Text& text, const JobShown& job)
{
    if (job.state == JobState::Ended) {
        const auto line = result_line(text, job);
        return line.empty() ? text("Ended") : text.format("Ended: {}", line);
    }
    if (job.stopping) {
        return text("Stopping...");
    }
    if (job.state == JobState::Waiting) {
        return waiting_words(text, job);
    }
    return text("Running");
}

std::string job_meta(const app::Text& text, const JobShown& job)
{
    if (job.state == JobState::Ended) {
        const auto line = result_line(text, job);
        return line.empty() ? text("Ended") : line;
    }
    if (job.stopping) {
        return text("Stopping...");
    }
    if (job.state == JobState::Waiting) {
        return waiting_words(text, job);
    }
    return job.bar.text.empty() ? text("Running") : job.bar.text;
}

std::string jobs_summary(const app::Text& text, const JobCounts& counts)
{
    if (counts.active() == 0) {
        return text("Jobs: none running");
    }
    if (counts.waiting == 0) {
        return text.format("Jobs: {} running", counts.running);
    }
    return text.format("Jobs: {} running, {} waiting", counts.running, counts.waiting);
}

double running_fraction(const std::vector<JobShown>& jobs)
{
    double sum = 0;
    std::size_t running = 0;
    for (const auto& job : jobs) {
        if (group_of(job) == JobGroup::Running) {
            sum += job.bar.pulse ? 0 : std::clamp(job.bar.fraction, 0.0, 1.0);
            ++running;
        }
    }
    return running == 0 ? 0 : sum / static_cast<double>(running);
}

JobTone verification_tone(std::string_view result)
{
    if (result == "PASSED") {
        return JobTone::Success;
    }
    if (result == "PASSED_WITH_WARNINGS") {
        return JobTone::Warning;
    }
    // A run stopped, or cut short by a device that went away: it can be
    // resumed (decision 0022).
    if (result == "INCOMPLETE") {
        return JobTone::Stopped;
    }
    return JobTone::Error;
}

JobTone comparison_tone(std::string_view result)
{
    if (result == "IDENTICAL") {
        return JobTone::Success;
    }
    if (result == "IDENTICAL_WITH_WARNINGS") {
        return JobTone::Warning;
    }
    return JobTone::Error;
}

const char* tone_style(JobTone tone)
{
    switch (tone) {
    case JobTone::Success:
        return "success";
    case JobTone::Warning:
        return "warning";
    case JobTone::Error:
        return "error";
    case JobTone::Stopped:
    case JobTone::None:
        break;
    }
    return "dim-label";
}

const char* tone_icon(JobTone tone)
{
    switch (tone) {
    case JobTone::Success:
        return "object-select-symbolic";
    case JobTone::Warning:
        return "dialog-warning-symbolic";
    case JobTone::Error:
        return "dialog-error-symbolic";
    case JobTone::Stopped:
        return "media-playback-pause-symbolic";
    case JobTone::None:
        break;
    }
    return nullptr;
}

} // namespace va::gui
