#include "gui/report.h"

#include <utility>

namespace va::gui {

Report fixed_message(std::string english)
{
    return [english = std::move(english)](const app::Text& text) { return text(english); };
}

Report unexpected_error(std::string what)
{
    // The message goes through text() too: some exceptions carry a sentence the
    // catalog knows, such as "cannot initialise SHA-256".
    return [what = std::move(what)](const app::Text& text) {
        return text.format("Unexpected error: {}", text(what));
    };
}

} // namespace va::gui
