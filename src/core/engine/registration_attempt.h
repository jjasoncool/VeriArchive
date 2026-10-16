#pragma once
// Which process runs an attempt of a registration (decision 0024). Every
// attempt records its process in the registration's plan (the setting
// "registration:<archive_uuid>", decision 0022) as the member "process":
//
//   {"id": 4242, "start": "<when it started>", "host": "<computer>", "program": "veriarchive-cli.exe"}
//
// `register --resume`, the Archive List and abandoning a registration
// (decision 0025) read it back to tell an attempt that still runs from one
// that has ended. A process id means something only on the computer that
// gave it out, so the record names the computer too (HostInfo::host). The
// record of an unfinished relocation (decision 0026) names its attempt's
// process the same way, with "stopped" the same way, and is judged the same.
#include "core/interfaces.h"

#include <nlohmann/json.hpp>

#include <chrono>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace va {

// The process that runs an attempt, as its plan records it.
struct AttemptProcess {
    ProcessIdentity process;
    std::string host;  // the computer it runs on
};

// Writes `attempt` into the plan, in place of the attempt before.
void record_attempt(nlohmann::json& plan, const AttemptProcess& attempt);
// The attempt a plan records; nothing for a plan written before decision
// 0024, or a record that cannot be read.
std::optional<AttemptProcess> recorded_attempt(const nlohmann::json& plan);

enum class AttemptState {
    Running,  // its process runs on this computer
    Ended,    // its plan says it stopped ("stopped", decision 0022), or its process has ended
    Unknown,  // nothing tells: the plan names no process (an older version), names one on another
              // computer, or this computer's process table cannot be read
};

struct AttemptJudgement {
    AttemptState state = AttemptState::Unknown;
    std::optional<AttemptProcess> attempt;  // the process the plan names, if it names one
};

// Judges the attempt that a registration's plan records, on the computer
// `host`. A process that has the recorded id but another start is a later
// process given the same id: the attempt's process has ended.
//
// A plan that names this very process is of an attempt that has ended while
// this process runs no operation: a process runs one registration at a time
// (the CLI one command, the GUI one operation at a time), so such a plan is
// of an attempt that this process cancelled, but whose stop mark it could not
// write. `runs_an_operation` says whether this process runs an operation now,
// which may be the registration that wrote the plan; it is asked only for a
// plan that names this process, as that plan is judged, so after the plan
// was read (the Archive List reads on a thread of its own, while the window
// may start an operation). Without it, this process runs none, as where the
// program judges an attempt for itself: only before it starts one (a resume),
// or while it runs no registration (abandoning, decision 0025).
AttemptJudgement judge_attempt(const nlohmann::json& plan, IProcessTable& processes, std::string_view host,
    const std::function<bool()>& runs_an_operation = {});

// How long a registration's plan may say nothing before an attempt that it
// does not name (judged Unknown) is no longer taken to work: the plan of an
// attempt that works is written every 30 seconds (decision 0022).
inline constexpr std::chrono::minutes attempt_silent_after{3};

} // namespace va
