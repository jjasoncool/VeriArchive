#include "app/history.h"

#include "core/engine/relocate_engine.h"

namespace va::app {

std::string history_note(const Text& text, const VerifyRunRecord& run)
{
    const auto part = relocation_part(run);
    if (!part) {
        return {};
    }
    if (part->interrupted_attempt) {
        return text("interrupted relocation attempt");
    }
    return text.format("relocation from r{}", part->from_revision);
}

} // namespace va::app
