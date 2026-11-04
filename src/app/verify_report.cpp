#include "app/verify_report.h"

#include "common/shell_argument.h"
#include "common/utf8.h"

#include <algorithm>

namespace va::app {

std::optional<std::string> unbaselined_advice(const Text& text, const VerifyOutcome& outcome, ReportedIn where)
{
    const auto unbaselined = outcome.counts.find("Unbaselined");
    if (unbaselined == outcome.counts.end() || unbaselined->second == 0 || outcome.result != "PASSED_WITH_WARNINGS") {
        return std::nullopt;
    }
    // A retired archive takes no new revision (decision 0003): registering
    // one would be refused.
    if (std::ranges::find(outcome.identity.details, "retired") != outcome.identity.details.end()) {
        return std::nullopt;
    }
    const auto root = path_to_utf8(outcome.root);
    if (where == ReportedIn::CommandLine) {
        return text.format("The Unbaselined entries could not be read when the archive was registered, so they have "
                           "no baseline yet. To take one for them, register a new revision of the archive: "
                           "veriarchive-cli register {} --new-revision",
            shell_argument(root));
    }
    // Decision 0046: the details' menu opens Register Archive on its options
    // with the archive's root and a new revision chosen; the folder or volume
    // verified may be another spelling of that root, chosen there itself,
    // where the new revision is turned on. Either way Next leads to Confirm,
    // whose Start registration starts it.
    return text.format("The Unbaselined entries could not be read when the archive was registered, so they have no "
                       "baseline yet. To take one for them, register a new revision of the archive: choose \"{}\" in "
                       "the menu of its details in Archives (or choose its folder or volume ({}) in Register Archive "
                       "and turn on \"{}\"), then press \"{}\" and \"{}\".",
        text("Create a new revision..."), root, text("Create a new revision"), text("Next"), text("Start registration"));
}

} // namespace va::app
