#include "app/verify_report.h"

#include "common/utf8.h"

#include <algorithm>

namespace va::app {

std::string command_line_argument(std::string_view argument)
{
    const auto plain = [](char character) {
        const auto byte = static_cast<unsigned char>(character);
        return byte >= 0x80 || (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') ||
               (character >= '0' && character <= '9') || std::string_view("-_./\\:").contains(character);
    };
    if (!argument.empty() && std::ranges::all_of(argument, plain)) {
        return std::string(argument);
    }
    std::string quoted = "\"" + std::string(argument);
    std::size_t backslashes = 0;
    while (backslashes < argument.size() && argument[argument.size() - 1 - backslashes] == '\\') {
        ++backslashes;
    }
    quoted.append(backslashes, '\\');
    return quoted + "\"";
}

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
            command_line_argument(root));
    }
    return text.format("The Unbaselined entries could not be read when the archive was registered, so they have no "
                       "baseline yet. To take one for them, register a new revision of the archive: in Register "
                       "Archive, choose its folder or volume ({}), tick \"{}\" and press \"{}\".",
        root, text("New revision of the archive registered at this folder or volume"), text("Register"));
}

} // namespace va::app
