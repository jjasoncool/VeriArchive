#include "core/engine/registration_attempt.h"

namespace va {
namespace {

// A member read without exceptions: another type (a damaged record) reads as missing.
std::string text_member(const nlohmann::json& object, const char* name)
{
    const auto found = object.find(name);
    return found != object.end() && found->is_string() ? found->get<std::string>() : std::string{};
}

} // namespace

void record_attempt(nlohmann::json& plan, const AttemptProcess& attempt)
{
    plan["process"] = nlohmann::json{{"id", attempt.process.id}, {"start", attempt.process.start},
        {"host", attempt.host}, {"program", attempt.process.program}};
}

std::optional<AttemptProcess> recorded_attempt(const nlohmann::json& plan)
{
    if (!plan.is_object()) {
        return std::nullopt;
    }
    const auto record = plan.find("process");
    if (record == plan.end() || !record->is_object()) {
        return std::nullopt;
    }
    const auto id = record->find("id");
    if (id == record->end() || !id->is_number_unsigned()) {
        return std::nullopt;
    }
    AttemptProcess attempt;
    attempt.process.id = id->get<std::uint64_t>();
    attempt.process.start = text_member(*record, "start");
    attempt.process.program = text_member(*record, "program");
    attempt.host = text_member(*record, "host");
    if (attempt.process.id == 0 || attempt.process.start.empty()) {
        return std::nullopt;
    }
    return attempt;
}

AttemptJudgement judge_attempt(const nlohmann::json& plan, IProcessTable& processes, std::string_view host,
    const std::function<bool()>& runs_an_operation)
{
    AttemptJudgement judgement;
    judgement.attempt = recorded_attempt(plan);
    // The attempt's last write says that it stopped (decision 0022), whether
    // its process still runs or not (the GUI's goes on after a cancel).
    if (plan.is_object() && !text_member(plan, "stopped").empty()) {
        judgement.state = AttemptState::Ended;
        return judgement;
    }
    if (!judgement.attempt || judgement.attempt->host != host) {
        return judgement; // Unknown
    }
    const auto& recorded = judgement.attempt->process;
    // This very process: the attempt has ended, unless this process runs an
    // operation now, which may be that registration; the process table then
    // finds this process running.
    if (const auto self = processes.current(); self && self->id == recorded.id && self->start == recorded.start &&
                                               !(runs_an_operation && runs_an_operation())) {
        judgement.state = AttemptState::Ended;
        return judgement;
    }
    const auto found = processes.find(recorded.id);
    if (!found) {
        return judgement; // Unknown: the process table cannot tell
    }
    // Gone, or the id belongs to a later process now.
    judgement.state = *found && (*found)->start == recorded.start ? AttemptState::Running : AttemptState::Ended;
    return judgement;
}

} // namespace va
