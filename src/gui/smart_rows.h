#pragma once
// What Devices shows of a device's SMART (decision 0052), and the SMART alert
// that Archives marks an archive with: from the reads that the repository
// keeps (smart_reads), the latest of the device as the system describes it
// now, compared with the read of the same device before it.
//
// Plain C++ without GTK, so the unit tests can check it.
#include "app/run_words.h"
#include "app/text.h"
#include "core/interfaces.h"
#include "core/smart/smart_data.h"

#include <expected>
#include <optional>
#include <string>
#include <vector>

namespace va::gui {

using app::Field;
using app::Pill;

// A table of the details: its headings and its rows, each as many cells.
struct SmartTable {
    std::vector<std::string> headings;
    std::vector<std::vector<std::string>> rows;
    bool operator==(const SmartTable&) const = default;
};

// The SMART of a device in its card on Devices.
struct SmartSection {
    // "SMART alert" (Error), "SMART OK" (Success), "SMART cannot be read"
    // (Neutral), "SMART not read" (Neutral).
    Pill pill;
    // When the read shown was made and how: "Read 2026-11-10 14:03 (UTC+8) ·
    // through its enclosure (SAT)"; empty where none was.
    std::string read;
    std::vector<std::string> alerts;  // each alert of the latest read; then the advice
    std::vector<std::string> hints;   // its hints
    // Why the latest read gave nothing: alone where no read gave data,
    // after the earlier read shown otherwise ("Latest read, 2026-11-11 ...:
    // The device did not answer in time.").
    std::string problem;
    std::vector<Field> values;        // the values most looked at
    std::string attributes_title;     // "All SMART attributes (12)", "All NVMe health values (11)"
    SmartTable attributes;
    std::string history_title;        // "Reads (3)"
    SmartTable history;               // newest first
    bool operator==(const SmartSection&) const = default;
};

// The SMART section of `device`, from `reads` (oldest first), with its own
// identity where the window keeps a read of it (`own`, device_smart): behind an
// enclosure that hides its identity, without one, "SMART to read again" and
// why, and no read. `reader`: this
// computer can read SMART (the device identity reader is there), which the
// words of a device never read say.
SmartSection smart_section(
    const app::Text& text, const std::vector<SmartRead>& reads, const PhysicalDeviceInfo& device, bool reader,
    const DeviceIdentity* own = nullptr);

// The mark beside an archive's result in Archives: "⚠ SMART: Pending
// sectors increased (0 → 8)", with "(+1)" where the read gives more alerts.
std::string smart_alert_mark(const app::Text& text, const ArchiveSmartAlert& alert);

// The line of the archive's details: "⚠ SMART of its device, read
// 2026-11-10 14:03 (UTC+8): Pending sectors increased (0 → 8); 1 more alert.
// Devices shows all of it."
std::string smart_alert_line(const app::Text& text, const ArchiveSmartAlert& alert);

// The summary at the head of Devices: when SMART was last read, and how many
// of the attached devices alert: "SMART last read 2026-11-10 14:03 (UTC+8) ·
// 1 device alerts"; "SMART has not been read yet".
std::string smart_summary(const app::Text& text, const std::vector<SmartRead>& reads,
    const std::vector<PhysicalDeviceInfo>& devices, const std::vector<std::optional<DeviceIdentity>>& own = {});

} // namespace va::gui

namespace va::gui {

// What a toast says once a run of the reader over the attached devices,
// asked for with Read the SMART of all devices..., is back (`reads`, or why
// it read nothing); `all` the SMART reads kept since, oldest first: "SMART
// read of 3 of 4 devices. Devices with a SMART alert: 1."; nothing where it
// was cancelled (the window was closed).
std::optional<std::string> smart_run_words(const app::Text& text,
    const std::expected<std::vector<DeviceRead>, IoError>& reads, const std::vector<SmartRead>& all);

} // namespace va::gui
