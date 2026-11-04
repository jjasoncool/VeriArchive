#pragma once
// What the window says of its jobs (decision 0045), and in which order it
// lists them: the state of a job, the line under its title, the groups of the
// Jobs page and of the header's popover, and the label of the header's jobs
// button (decision 0046).
//
// Plain C++ without GTK, so the unit tests can check it.
#include "app/text.h"
#include "gui/job_shown.h"

#include <cstddef>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

namespace va::gui {

// The group a job is listed in: Running (also while it stops), Waiting for its
// turn (also while it stops), or Ended.
enum class JobGroup { Running, Waiting, Ended };

JobGroup group_of(const JobShown& job);

// The jobs of each group, as the Jobs page and the header's popover list
// them: those running and those waiting in the order they were started, which
// is the order in which the jobs of one device take their turn; those ended
// newest first, at most `ended_limit` of them (the popover shows the last 3).
struct GroupedJobs {
    std::vector<const JobShown*> running;
    std::vector<const JobShown*> waiting;
    std::vector<const JobShown*> ended;
    bool empty() const { return running.empty() && waiting.empty() && ended.empty(); }
};

GroupedJobs group_jobs(const std::vector<JobShown>& jobs,
    std::size_t ended_limit = std::numeric_limits<std::size_t>::max());

// How many jobs run (also those that stop) and how many wait for their turn.
struct JobCounts {
    std::size_t running = 0;
    std::size_t waiting = 0;
    std::size_t active() const { return running + waiting; }
};

JobCounts count_jobs(const std::vector<JobShown>& jobs);

// What a job is doing, worded: waiting for whom, working, stopping, or how
// it ended ("Ended: " and its result, the first line of its report that holds
// text). A job's details and its bar while it waits show it.
std::string state_words(const app::Text& text, const JobShown& job);

// The line under a job's title in the Jobs page and the popover, whose group
// says already whether it runs, waits or ended: while it works how far it is
// (what its bar says: done so far, the percentage, the speed and the time
// left); while it waits whom it waits for; that it stops; once it ended its
// result, or "Ended" where its report has none.
std::string job_meta(const app::Text& text, const JobShown& job);

// The tooltip and accessible label of the header's jobs button, whose count
// is that of the jobs that run and wait: "Jobs: 2 running, 1 waiting",
// "Jobs: 1 running" while none waits, "Jobs: none running" while none runs or
// waits.
std::string jobs_summary(const app::Text& text, const JobCounts& counts);

// How far the jobs that run are, together, as the ring of the header's jobs
// button shows it: the mean of their bars (0 for a bar that moves to and fro,
// whose end is not known yet); 0 while none runs.
double running_fraction(const std::vector<JobShown>& jobs);

// The tone of a FULL verification's result (PASSED, PASSED_WITH_WARNINGS,
// FAILED, INCOMPLETE), and of a comparison's (IDENTICAL,
// IDENTICAL_WITH_WARNINGS, DIFFERENT): Error for any other.
JobTone verification_tone(std::string_view result);
JobTone comparison_tone(std::string_view result);

// How the line under an ended job's title shows its tone: the theme's style
// class for it (success, warning, error; dim-label for Stopped and None) and
// an icon, so that the result is not told by colour alone (none for None).
const char* tone_style(JobTone tone);
const char* tone_icon(JobTone tone);

} // namespace va::gui
