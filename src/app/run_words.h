#pragma once
// The words that the window's views, the verification report and the command
// line give the runs of a verification and what they read (decisions 0046,
// 0048), one source for each: a run's result with its mode (R-21), how long
// it took, a size, a count, a device's bus, the guarantee (R-02); and how a
// view tells them apart: a tone for what a result or an item does, a name and
// its value. The window gives a tone its colour by a style class of its own
// (gui/archive_rows.h, tone_class) and shows a pill as a widget of its own;
// the verification report gives it a colour of its own and says it in words
// too (run_report_html.h).
//
// Plain C++ without GTK.
#include "app/text.h"
#include "core/repository/archive_store.h"

#include <cstdint>
#include <string>
#include <string_view>

namespace va::app {

// How a result, an archive or a mark is shown: in the colour for success,
// warnings or errors, or in none.
enum class Tone { Neutral, Success, Warning, Error };

// A few words with their tone: a result, or a mark beside it.
struct Pill {
    std::string words;
    Tone tone = Tone::Neutral;
    bool operator==(const Pill&) const = default;
};

// A name and its value, a line of a group of the details. A value that is a
// path (a root) wraps at any character, filling its lines; any other between
// words where it can. `code`: a value that is a run's id, a uuid, a serial,
// a WWN or a hash, which the verification report writes in monospace (the
// window shows it as any other).
struct Field {
    std::string name;
    std::string value;
    bool path = false;
    bool code = false;
    bool operator==(const Field&) const = default;
};

// What VeriArchive detects and what this version does not (R-02): every
// verify report, the result page of every run, the verification report of a
// run and Settings carry it (decision 0017). It names no version of the
// program.
inline constexpr const char* guarantee =
    "VeriArchive detects accidental corruption and unintended change; tamper evidence (signed manifests) "
    "is not part of this version.";

// A size as the views show it: "173.0 B", "1.2 GiB", "9.4 TiB".
std::string format_bytes(std::uint64_t bytes);

// A device's bus as the views show it: "usb" as USB, "sata" as SATA, "nvme" as
// NVMe, "scsi" as SCSI (the names the enumerators give); any other as given.
std::string bus_words(const std::string& bus);

// "152,408": a count with its thousands grouped, as both languages write it.
std::string grouped(std::uint64_t count);

// How long a run took, from its start to its end: "8 s", "5 min 37 s",
// "12 h 44 min"; empty where either is no timestamp.
std::string duration_words(const Text& text, std::string_view started, std::string_view finished);

// A run's result with its mode, as its report's first line says it: "FULL
// VERIFY PASSED" (完整驗證 通過) for a FULL verification, "QUICK PASSED" for a
// mode that V1 does not run.
std::string result_with_mode(const Text& text, const VerifyRunRecord& run);

// PASSED Success, PASSED_WITH_WARNINGS Warning, INCOMPLETE Neutral (it claims
// nothing), FAILED and anything else Error.
Tone result_tone(std::string_view result);

} // namespace va::app
