#pragma once
// The window's words for a carrier seen behind another enclosure (decision
// 0047), and the facts and judgments behind them, GTK-free: the question
// that the FULL verify of such an archive asks first, what a read of the
// device's own serial tells, and the operator confirmation. The window builds
// its dialogs from these; each paragraph is one string.
//
// The enclosure question: identity matching found the archive's filesystem
// on another device than recorded (MIGRATED) behind an enclosure
// (enclosure_changed). Where the archive has a confirmed device
// (confirmed_device), reading the device's own serial tells: the same serial
// verifies at once, with no question (how = device_serial); another serial is
// not this archive's device, and no "same device" answer is offered. Where it
// has none, the serial read has nothing to be compared with: the operator
// compares it with the serial on the device's label, or confirms without
// reading it; either is an operator confirmation, recorded in the history
// with the run whatever its result.
//
// And what a verification that went on with a confirmation says of the new
// carrier once it has finished, in verify's text output and --json (always
// English, decision 0017), and in the window's report.
#include "app/text.h"
#include "core/engine/carrier_confirmation.h"
#include "core/engine/verify_engine.h"

#include <nlohmann/json.hpp>

#include <optional>
#include <string>
#include <vector>

namespace va::app {

// What the enclosure question shows: the archive's label, the enclosure's
// serial as the latest revision records it and as it is seen now
// (enclosure_serial), and the archive's confirmed device, if it has one.
struct EnclosureFacts {
    std::string label;
    std::string enclosure_recorded;
    std::string enclosure_now;
    std::optional<ConfirmedDevice> confirmed;
};

EnclosureFacts enclosure_facts(const std::string& label, const VolumeInfo& recorded, const VolumeInfo& now,
    std::optional<ConfirmedDevice> confirmed);

// What a read of the device's own serial tells, beside the archive's
// confirmed device.
enum class DeviceReadVerdict {
    SameDevice,        // the confirmed device's serial (or WWN): verify at once, by the serial
    OtherDevice,       // another device: not this archive's; no "same device" answer
    NothingToCompare,  // the archive has no confirmed device: the operator compares it with the label
};
DeviceReadVerdict judge_device_read(const DeviceIdentity& read, const std::optional<ConfirmedDevice>& confirmed);

// The confirmation a verification started from the question goes on with:
// by the device's serial (SameDevice), or by the operator, after reading the
// serial (`read`) or without it. For the enclosures the facts name.
CarrierConfirmation confirmation_by_serial(const EnclosureFacts& facts, const DeviceIdentity& read);
CarrierConfirmation confirmation_by_operator(const EnclosureFacts& facts, const std::optional<DeviceIdentity>& read);

// The question's heading, `where` the volume as the window names it (its
// mount point or label): "The enclosure of X differs from the one recorded".
std::string enclosure_question_title(const Text& text, const std::string& where);
// Its paragraphs: what changed; then, with a confirmed device, that reading
// the serial tells; without one, that a sector-by-sector clone passes a FULL
// verify too, and that the operator compares the serial with the label.
std::vector<std::string> enclosure_question_body(const Text& text, const EnclosureFacts& facts);
// What the device identity reader asks and reads (the system's question for
// administrator permission), shown beside every button that starts it.
std::string device_identity_reader_note(const Text& text);
// Administrator permission not given, or the serial could not be read
// through the enclosure: shown in the question, which stays open.
std::string device_read_declined(const Text& text);
std::string device_read_failed(const Text& text);

// The operator confirmation: its heading and its paragraphs, which say that a
// sector-by-sector clone passes the verification too, and that the history
// records the confirmation as the operator's, without the serial read,
// whatever the verification's result.
std::string operator_confirmation_title(const Text& text);
std::vector<std::string> operator_confirmation_body(const Text& text, const EnclosureFacts& facts);

// After a read with nothing to compare (NothingToCompare): what the device
// reports, and the question whether it is the archive's device.
std::string device_read_title(const Text& text);
std::vector<std::string> device_read_body(const Text& text, const EnclosureFacts& facts);
// After a read of another device (OtherDevice): "This is not the device of
// X", with what was recorded and what was read.
std::string other_device_title(const Text& text, const EnclosureFacts& facts);
std::vector<std::string> other_device_body(const Text& text, const EnclosureFacts& facts);
// After a read of the confirmed device (SameDevice): the verification starts
// with no question, and says so, naming the serial read (its WWN where it
// matched by that alone).
std::string same_device_started(const Text& text, const DeviceIdentity& read);

// How a recorded confirmation was given, in a few words: "the device's serial
// matches", "confirmed by you after reading the serial", "confirmed by you,
// serial not read".
std::string confirmation_words(const Text& text, const CarrierConfirmation& confirmation);
// The media identity of a run that went on with it, as the result page says
// it: "MIGRATED: the enclosure differs from the one recorded (...)".
std::string confirmed_identity_words(const Text& text, const CarrierConfirmation& confirmation);

// What a verification that went on with a confirmation says of the new
// carrier once it has finished, in `text`'s language: "new enclosure
// recorded: revision N records it (<confirmation_words>)", or "new enclosure
// not recorded: <why>" (it FAILED, or passed and could not record it), as
// verify's text output gives it after the items; with `sentence`, as the
// window's report gives it ("New enclosure recorded: ..."). Empty for a run
// that went on with none, or that has not finished.
std::string carrier_line(const Text& text, const VerifyOutcome& outcome, bool sentence = false);

// verify --json's "carrier_confirmed": the confirmation the run went on with
// (carrier_confirmation_json: how, device where read, the enclosures,
// from_revision), with `revision`, the one that records the new carrier, or
// null, and `not_recorded`, why a finished run records none, or null; null
// for a run that went on with none. As an object of that one member, which
// the run's object takes over.
nlohmann::json carrier_member(const VerifyOutcome& outcome);

} // namespace va::app
