#include "app/retirement_report.h"

namespace va::app {

std::string retirement_explanation(const Text& text, const Retirement& retirement)
{
    if (!retirement.lost_manifest) {
        return {};
    }
    return text.format("Lost manifest of {} ({}), revision {}", retirement.label, retirement.archive_uuid,
               retirement.revision) +
           "\n  " + text(what_is_wrong_with(*retirement.lost_manifest)) + "\n" +
           text.format("Retiring the archive declares that manifest lost for good: do this only where no backup of the "
                       "repository, export of revision {} or media copy holds it, and the device that held the "
                       "archive is gone as well (broken, lost or wiped); otherwise put the manifest back with "
                       "`import`, or register the archive's next revision from its folder or volume with `register "
                       "<root_path> --lost-manifest {}`. The archive keeps its verification history and its hash "
                       "chain, but can no longer be verified or take a new revision. It no longer stops the "
                       "registration of a new folder or volume or a relocation: retired, it is no longer taken as a "
                       "possible owner of one, as identity matching cannot use its latest manifest.",
               retirement.revision, retirement.archive_uuid) +
           "\n";
}

} // namespace va::app
