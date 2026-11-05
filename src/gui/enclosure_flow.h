#pragma once
// What the window asks before a FULL verify of an archive whose device is
// seen behind another enclosure (decision 0047), as the approved design of
// the window shows it, GTK-free: the dialog of each step, its answers and
// what each answer leads to; what the details say while a verification that
// went on with a confirmation runs, and the toast once it ended.
//
// The steps. The enclosure question first: what changed
// (app::enclosure_question_body), the enclosure's serial as recorded and as
// seen now listed after it, and either that reading the device's own serial
// tells (the archive has a confirmed device) or that a sector-by-sector clone
// passes a FULL verify too and the operator compares a read serial with the
// device's label; the note of what Windows asks
// (app::device_identity_reader_note) where a read is offered (the device's
// enclosure hides its serial, and this computer has a device identity
// reader). Where none is (Linux, a reader missing, or a device that reports
// its own serial through its enclosure), it says why, and that a clone passes
// too; answers Cancel, Confirm without reading the serial... (without a
// confirmed device, or after a read that gave nothing, or where no read is
// offered), Read the device's own serial. While the reader waits for Windows'
// question and reads, only Cancel. A read of the confirmed device's serial
// verifies at once (by the device's serial), with no question; one of another
// device says "This is not the device of X" with the serial recorded and the
// one read, and offers to register it as another archive; with nothing to
// compare, "What the device reports" (model, serial, WWN, compare it with the
// label) with No, it is another device and It is the same device, start the
// FULL verify (an operator confirmation with the device read), neither
// suggested. Confirm without reading the serial... asks once more
// (app::operator_confirmation_*): a clone passes too, and the history records
// the confirmation. A read that gave nothing goes back to the question, which
// says why. Cancel is every step's default and its answer to Escape.
//
// The question always starts on its first step, and a read is always made
// anew in it, never taken from a read kept from before (device_reads.h):
// behind an enclosure that hides the device's own identity, all the system
// tells of a device is the enclosure's, so another device of the same model
// put in the same enclosure or dock since is described exactly alike, and only
// a read now tells which device the verification would accept.
//
// Plain C++ without GTK, so the unit tests can check it.
#include "app/enclosure_words.h"
#include "app/text.h"
#include "core/engine/carrier_confirmation.h"
#include "core/engine/verify_engine.h"
#include "gui/archive_rows.h"
#include "gui/device_reads.h"

#include <cstddef>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace va::gui {

enum class EnclosureStep { Question, Reading, OperatorConfirmation, DeviceRead, OtherDevice };

// Where the dialog is: its step, the device read (once read), and why the
// last read gave nothing.
struct EnclosureFlow {
    EnclosureStep step = EnclosureStep::Question;
    std::optional<DeviceIdentity> read;
    std::optional<ReadProblem> problem;
};

// The answers' ids.
namespace enclosure_answer {
inline constexpr std::string_view cancel = "cancel";
inline constexpr std::string_view read = "read";                    // Read the device's own serial
inline constexpr std::string_view without_serial = "without-serial";  // Confirm without reading the serial...
inline constexpr std::string_view confirmed = "confirmed";          // It is the same device (no serial read)
inline constexpr std::string_view same = "same";                    // It is the same device (after a read)
inline constexpr std::string_view other = "other";                  // No, it is another device
inline constexpr std::string_view register_it = "register";         // Register as another archive...
} // namespace enclosure_answer

struct DialogAnswer {
    std::string id;
    std::string label;
    bool suggested = false;
    bool reads = false;  // starts the device identity reader, which asks for administrator permission
    bool operator==(const DialogAnswer&) const = default;
};

// A step as the dialog shows it: its heading, paragraphs, fields (shown
// after the first `fields_after` paragraphs), why the last read gave nothing
// (in the colour of warnings), a dim note, a spinner while the reader reads,
// and its answers, the first of which (Cancel) is the default.
struct EnclosureDialogShown {
    std::string heading;
    std::vector<std::string> paragraphs;
    std::vector<Field> fields;
    std::size_t fields_after = 0;
    std::optional<std::string> problem;
    std::string note;
    bool reading = false;
    std::vector<DialogAnswer> answers;
};

// Whether the question offers a read of the device's own serial: Offered
// where offers_device_read (its enclosure hides it, the system names the
// device, this computer has a device identity reader); NoReader where this
// computer has none (Linux, a reader missing) or cannot name the device;
// ReportedByDevice where it has one but the device seen now reports its own
// serial through its enclosure (not enclosure_hides_identity), which a read
// would only repeat: a USB flash drive, or a bridge that passes the device's
// identity on.
enum class SerialRead { Offered, NoReader, ReportedByDevice };
SerialRead serial_read_offer(const PhysicalDeviceInfo& now, bool reader);

// `where`: the volume as the question names it (its mount point); `offer`:
// serial_read_offer of the device seen now.
EnclosureDialogShown enclosure_dialog(const app::Text& text, const app::EnclosureFacts& facts,
    const std::string& where, const EnclosureFlow& flow, SerialRead offer);

// What follows a read, or an answer.
struct EnclosureNext {
    enum class Then {
        Show,     // show `flow`
        Verify,   // verify with `confirmation`, the dialog closed
        Read,     // read the device's own serial, showing `flow` (Reading) meanwhile
        Register, // close, and register the volume as another archive, a copy of this one
        Close,    // close, nothing done
    };
    Then then = Then::Show;
    EnclosureFlow flow;
    std::optional<CarrierConfirmation> confirmation;
};

// After a read of the device's own serial: the confirmed device's serial
// verifies by it; another device's is not this archive's; with nothing to
// compare, the operator compares it; a read that gave nothing goes back to the
// question saying why; a cancelled one closes.
EnclosureNext after_read(const app::EnclosureFacts& facts, const std::expected<DeviceIdentityRead, IoError>& read);

// What an answer of `flow` leads to.
EnclosureNext answered(const app::EnclosureFacts& facts, const EnclosureFlow& flow, std::string_view answer);

// The details' notice while a verification that went on with `confirmation`
// waits or works: that it was taken, and that only a pass records the new
// enclosure (and the device's own serial read), the history the confirmation
// whatever the result.
std::string confirmed_running_notice(
    const app::Text& text, const std::string& label, const CarrierConfirmation& confirmation);

// The toast once such a verification ended: `base` (verified_toast), and what
// it recorded of the new enclosure, or that it recorded none (app::
// carrier_line); `base` alone for a run that went on with none, or stopped.
std::string confirmed_toast(const app::Text& text, const std::string& base, const VerifyOutcome& outcome);

// A FULL verify that stopped MIGRATED (with no confirmation: the latest read
// of the attached devices was older than the change of enclosure, or it was
// the verification of a folder or volume) where the archive's device is seen
// behind another enclosure at `where`: FULL verify in its details asks first.
std::string enclosure_stop_words(const app::Text& text, const std::string& where);

} // namespace va::gui
