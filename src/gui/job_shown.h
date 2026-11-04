#pragma once
// What the window shows of a job (decision 0045), and of the progress of any
// operation it runs: kept on the main thread, shown by the Jobs page, the
// header's jobs button, a job's details and the view that started the job.
//
// Plain C++ without GTK, so the unit tests can check what is worded from it
// (job_words.h).
#include "gui/report.h"

#include <cstdint>
#include <string>

namespace va::gui {

// A progress bar as an operation's progress shows it: moving to and fro while
// it scans, otherwise how far it is, with what it did and, hashing, the speed
// and the time left (app::ProgressEstimate), worded as on the command line.
struct BarState {
    bool pulse = false;
    double fraction = 0;
    std::string text;
};

enum class JobState { Waiting, Running, Ended };

// How an ended job's result is shown (decision 0046): passed or the same
// (Success), passed with warnings (Warning), failed, different or refused
// (Error), stopped before it was done (Stopped); None where its work says
// nothing of it.
enum class JobTone { None, Success, Warning, Error, Stopped };

struct JobShown {
    std::uint64_t id = 0;
    Report title;
    JobState state = JobState::Waiting;
    bool stopping = false;          // Stop was pressed; it ends soon
    Report waiting_for;             // while it waits: the title of the job ahead of it
    BarState bar;                   // its progress, as a bar shows it
    std::string alert;              // its device alerts, worded, one per line
    Report report;                  // once it ended
    JobTone tone = JobTone::None;   // once it ended
};

} // namespace va::gui
