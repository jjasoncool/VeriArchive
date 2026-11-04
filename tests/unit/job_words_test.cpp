// What the window says of its jobs and in which order it lists them
// (decisions 0045 and 0046): the groups of the Jobs page and the header's
// popover, the line under a job's title, its state, and the header button's
// label. The jobs, titles, reports and progress below are sample data, worded
// as the views word theirs (ARCH-SAMPLE-*, no real archive or device).
#include "test_support.h"

#include "gui/job_words.h"

#include <string>
#include <vector>

namespace {

using va::app::Language;
using va::app::Text;
using va::gui::JobShown;
using va::gui::JobState;

JobShown sample_job(std::uint64_t id, JobState state, std::string title)
{
    JobShown job;
    job.id = id;
    job.state = state;
    job.title = [title = std::move(title)](const Text&) { return title; };
    return job;
}

va::gui::Report sample_report(std::string report)
{
    return [report = std::move(report)](const Text&) { return report; };
}

} // namespace

void job_words_tests()
{
    using va::gui::count_jobs;
    using va::gui::group_jobs;
    using va::gui::group_of;
    using va::gui::job_meta;
    using va::gui::JobGroup;
    using va::gui::jobs_summary;
    using va::gui::state_words;

    const Text english;
    const Text chinese(Language::TraditionalChinese);

    // Jobs in the order they were started (Jobs::shown): two ended, one
    // running, one waiting for the device the running one reads, one more
    // ended, and one stopping while it runs.
    std::vector<JobShown> jobs;
    jobs.push_back(sample_job(1, JobState::Ended, "FULL verify ARCH-SAMPLE-1"));
    jobs.back().report = sample_report("FULL VERIFY PASSED\n\nARCH-SAMPLE-1 (c0ffee00-0000-4000-8000-000000000001)");
    jobs.push_back(sample_job(2, JobState::Ended, "Compare ARCH-SAMPLE-1 with ARCH-SAMPLE-2"));
    jobs.back().report = sample_report("\n  \nARCHIVES DIFFER  \nA (reference): ARCH-SAMPLE-1");
    jobs.push_back(sample_job(3, JobState::Running, "FULL verify ARCH-SAMPLE-3"));
    jobs.back().bar.text = "Hashing: 1.2 GiB of 9.4 GiB (12%) · 210.0 MiB/s · about 1 hour left";
    jobs.push_back(sample_job(4, JobState::Waiting, "FULL verify ARCH-SAMPLE-4"));
    jobs.back().waiting_for = jobs[2].title;
    jobs.push_back(sample_job(5, JobState::Ended, "Register ARCH-SAMPLE-5"));
    jobs.push_back(sample_job(6, JobState::Running, "FULL verify ARCH-SAMPLE-6"));
    jobs.back().stopping = true;

    // The groups: running and waiting in the order started, ended newest first.
    CHECK(group_of(jobs[0]) == JobGroup::Ended);
    CHECK(group_of(jobs[2]) == JobGroup::Running);
    CHECK(group_of(jobs[3]) == JobGroup::Waiting);
    CHECK(group_of(jobs[5]) == JobGroup::Running);  // stopping, still running
    const auto all = group_jobs(jobs);
    CHECK(all.running.size() == 2 && all.running[0]->id == 3 && all.running[1]->id == 6);
    CHECK(all.waiting.size() == 1 && all.waiting[0]->id == 4);
    CHECK(all.ended.size() == 3 && all.ended[0]->id == 5 && all.ended[1]->id == 2 && all.ended[2]->id == 1);
    CHECK(!all.empty());
    // The popover's last ended ones only; every running and waiting one.
    const auto last_two = group_jobs(jobs, 2);
    CHECK(last_two.ended.size() == 2 && last_two.ended[0]->id == 5 && last_two.ended[1]->id == 2);
    CHECK(last_two.running.size() == 2 && last_two.waiting.size() == 1);
    CHECK(group_jobs(jobs, 0).ended.empty());
    CHECK(group_jobs({}).empty());

    // A waiting job that stops is still waiting; it counts as one.
    jobs[3].stopping = true;
    CHECK(group_of(jobs[3]) == JobGroup::Waiting);
    const auto counts = count_jobs(jobs);
    CHECK(counts.running == 2 && counts.waiting == 1 && counts.active() == 3);
    CHECK(count_jobs({}).active() == 0);

    // The line under a job's title.
    CHECK(job_meta(english, jobs[2]) == "Hashing: 1.2 GiB of 9.4 GiB (12%) · 210.0 MiB/s · about 1 hour left");
    CHECK(job_meta(english, jobs[5]) == "Stopping...");
    CHECK(job_meta(chinese, jobs[5]) == "停止中……");
    CHECK(job_meta(english, jobs[3]) == "Stopping...");
    jobs[3].stopping = false;
    CHECK(job_meta(english, jobs[3]) == "Waiting: FULL verify ARCH-SAMPLE-3 reads the same device");
    CHECK(job_meta(chinese, jobs[3]) == "等候中：FULL verify ARCH-SAMPLE-3 正在讀取同一個裝置");
    JobShown queued = sample_job(7, JobState::Waiting, "Register ARCH-SAMPLE-7");
    CHECK(job_meta(english, queued) == "Waiting for its turn");
    CHECK(job_meta(chinese, queued) == "等候中");
    // Ended: its result, the first line that holds text, without the
    // blanks around it; "Ended" where the report has none.
    CHECK(job_meta(english, jobs[0]) == "FULL VERIFY PASSED");
    CHECK(job_meta(english, jobs[1]) == "ARCHIVES DIFFER");
    CHECK(job_meta(english, jobs[4]) == "Ended");
    CHECK(job_meta(chinese, jobs[4]) == "已結束");
    // Before the first progress a running job's bar says it starts.
    JobShown starting = sample_job(8, JobState::Running, "FULL verify ARCH-SAMPLE-8");
    CHECK(job_meta(english, starting) == "Running");
    starting.bar.text = "Starting...";
    CHECK(job_meta(english, starting) == "Starting...");

    // The state words a job's details show, which say also that it ended;
    // a running job's as the Jobs page names its group.
    CHECK(state_words(english, jobs[0]) == "Ended: FULL VERIFY PASSED");
    CHECK(state_words(chinese, jobs[1]) == "已結束：ARCHIVES DIFFER");
    CHECK(state_words(english, jobs[4]) == "Ended");
    CHECK(state_words(english, jobs[2]) == "Running");
    CHECK(state_words(chinese, jobs[2]) == "執行中");
    CHECK(state_words(english, jobs[5]) == "Stopping...");
    CHECK(state_words(english, queued) == "Waiting for its turn");

    // The header button's label, in either language.
    CHECK(jobs_summary(english, {2, 1}) == "Jobs: 2 running, 1 waiting");
    CHECK(jobs_summary(chinese, {2, 1}) == "工作：2 個執行中、1 個等候中");
    CHECK(jobs_summary(english, {1, 0}) == "Jobs: 1 running");
    CHECK(jobs_summary(chinese, {1, 0}) == "工作：1 個執行中");
    CHECK(jobs_summary(english, {0, 1}) == "Jobs: 0 running, 1 waiting");
    CHECK(jobs_summary(english, {0, 0}) == "Jobs: none running");
    CHECK(jobs_summary(chinese, {0, 0}) == "工作：沒有執行中的工作");

    // The ring of the header's button: the mean of the running jobs' bars,
    // a bar that moves to and fro counting as 0; the jobs that wait or
    // ended do not count.
    {
        using va::gui::running_fraction;
        std::vector<JobShown> ring;
        CHECK(running_fraction(ring) == 0);
        ring.push_back(sample_job(1, JobState::Ended, "FULL verify ARCH-SAMPLE-1"));
        ring.back().bar.fraction = 1;
        ring.push_back(sample_job(2, JobState::Waiting, "FULL verify ARCH-SAMPLE-2"));
        CHECK(running_fraction(ring) == 0);
        ring.push_back(sample_job(3, JobState::Running, "FULL verify ARCH-SAMPLE-3"));
        ring.back().bar.fraction = 0.5;
        CHECK(running_fraction(ring) == 0.5);
        ring.push_back(sample_job(4, JobState::Running, "Register ARCH-SAMPLE-4"));
        ring.back().bar.pulse = true;
        ring.back().bar.fraction = 0.9;  // left from before; a pulsing bar has no end yet
        CHECK(running_fraction(ring) == 0.25);
        ring.back().bar.pulse = false;
        ring.back().bar.fraction = 1.5;  // never past the end
        CHECK(running_fraction(ring) == 0.75);
    }

    // The tone of a result, its style class and its icon: never told by
    // colour alone, but where nothing says the result.
    {
        using va::gui::comparison_tone;
        using va::gui::JobTone;
        using va::gui::tone_icon;
        using va::gui::tone_style;
        using va::gui::verification_tone;
        CHECK(verification_tone("PASSED") == JobTone::Success);
        CHECK(verification_tone("PASSED_WITH_WARNINGS") == JobTone::Warning);
        CHECK(verification_tone("FAILED") == JobTone::Error);
        CHECK(verification_tone("INCOMPLETE") == JobTone::Stopped);
        CHECK(verification_tone("") == JobTone::Error);
        CHECK(comparison_tone("IDENTICAL") == JobTone::Success);
        CHECK(comparison_tone("IDENTICAL_WITH_WARNINGS") == JobTone::Warning);
        CHECK(comparison_tone("DIFFERENT") == JobTone::Error);
        CHECK(std::string(tone_style(JobTone::Success)) == "success");
        CHECK(std::string(tone_style(JobTone::Warning)) == "warning");
        CHECK(std::string(tone_style(JobTone::Error)) == "error");
        CHECK(std::string(tone_style(JobTone::Stopped)) == "dim-label");
        CHECK(std::string(tone_style(JobTone::None)) == "dim-label");
        for (const auto tone : {JobTone::Success, JobTone::Warning, JobTone::Error, JobTone::Stopped}) {
            CHECK(tone_icon(tone) != nullptr);
        }
        CHECK(std::string(tone_icon(JobTone::Success)) != tone_icon(JobTone::Error));
        CHECK(tone_icon(JobTone::None) == nullptr);
        // A job's tone is None until its work says otherwise.
        CHECK(sample_job(9, JobState::Ended, "Register ARCH-SAMPLE-9").tone == JobTone::None);
    }
}
