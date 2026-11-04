#include "gui/progress_wording.h"

#include "app/plain_words.h"

#include <algorithm>
#include <cmath>

namespace va::gui {

std::optional<BarState> ProgressWording::update(const Progress& progress, std::chrono::steady_clock::time_point now)
{
    estimate_.update(progress, now);
    const bool complete = counting_ == Counting::Bytes
                              ? progress.bytes_total > 0 && progress.bytes_done >= progress.bytes_total
                              : progress.items_total > 0 && progress.items_done == progress.items_total;
    if (!complete && last_ != std::chrono::steady_clock::time_point{} && now - last_ < std::chrono::milliseconds(200)) {
        return std::nullopt;
    }
    last_ = now;
    BarState bar;
    if (counting_ == Counting::Bytes) {
        // A file's bytes copied, its size known from the start (R-94): no
        // items, and no scan before.
        if (progress.bytes_total == 0) {
            bar.pulse = true;
            bar.text = text_.format("Copying: {}", app::format_size(progress.bytes_done));
            return bar;
        }
        const auto share = std::min(1.0, static_cast<double>(progress.bytes_done) /
                                             static_cast<double>(progress.bytes_total));
        bar.fraction = share;
        const auto percent = static_cast<int>(std::floor(share * 100));
        bar.text = text_.format("Copying: {}",
            text_.format("{} of {} ({}%)", app::format_size(progress.bytes_done), app::format_size(progress.bytes_total),
                complete ? 100 : std::min(percent, 99)));
        return bar;
    }
    if (progress.items_total == 0) {
        bar.pulse = true;
        bar.text = text_.format("Scanning: {}", app::count_of(text_, progress.items_done, app::Counted::Entry));
    } else if (counting_ == Counting::Entries) {
        bar.fraction = static_cast<double>(progress.items_done) / static_cast<double>(progress.items_total);
        // Two calls: the compiler checks each literal template against its values.
        bar.text = progress.items_total == 1
                       ? text_.format("Comparing: {} / {} entry", progress.items_done, progress.items_total)
                       : text_.format("Comparing: {} / {} entries", progress.items_done, progress.items_total);
    } else {
        bar.fraction = estimate_.fraction().value_or(0);
        bar.text = text_.format("Hashing: {}", app::describe_progress(text_, estimate_));
    }
    return bar;
}

} // namespace va::gui
