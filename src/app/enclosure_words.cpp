#include "app/enclosure_words.h"

namespace va::app {

EnclosureFacts enclosure_facts(const std::string& label, const VolumeInfo& recorded, const VolumeInfo& now,
    std::optional<ConfirmedDevice> confirmed)
{
    return EnclosureFacts{label, enclosure_serial(recorded), enclosure_serial(now), std::move(confirmed)};
}

DeviceReadVerdict judge_device_read(const DeviceIdentity& read, const std::optional<ConfirmedDevice>& confirmed)
{
    if (!confirmed) {
        return DeviceReadVerdict::NothingToCompare;
    }
    return same_device_identity(read, confirmed->device) ? DeviceReadVerdict::SameDevice
                                                         : DeviceReadVerdict::OtherDevice;
}

CarrierConfirmation confirmation_by_serial(const EnclosureFacts& facts, const DeviceIdentity& read)
{
    return CarrierConfirmation{
        CarrierConfirmation::How::DeviceSerial, read, facts.enclosure_recorded, facts.enclosure_now};
}

CarrierConfirmation confirmation_by_operator(const EnclosureFacts& facts, const std::optional<DeviceIdentity>& read)
{
    return CarrierConfirmation{CarrierConfirmation::How::Operator, read, facts.enclosure_recorded, facts.enclosure_now};
}

std::string enclosure_question_title(const Text& text, const std::string& where)
{
    return text.format("The enclosure of {} differs from the one recorded", where);
}

std::vector<std::string> enclosure_question_body(const Text& text, const EnclosureFacts& facts)
{
    std::vector<std::string> body{
        text.format("The filesystem ID of this volume is the one {} records, but the serial its enclosure reports "
                    "has changed: recorded {}, now {}.",
            facts.label, facts.enclosure_recorded, facts.enclosure_now)};
    if (facts.confirmed) {
        body.push_back(text.format("{} records the device's own serial {}. Reading the serial now tells whether this "
                                   "is the same device.",
            facts.label,
            facts.confirmed->device.serial.empty() ? facts.confirmed->device.wwn : facts.confirmed->device.serial));
        return body;
    }
    body.push_back(text("It may be the same device in another enclosure, or a sector-by-sector clone of it. A clone "
                        "holds exactly the same content and passes a FULL verify too, so the verification itself "
                        "cannot tell them apart."));
    body.push_back(text.format("{} records no serial of the device itself, so a serial read now has nothing to be "
                               "compared with: you compare it with the serial on the device's label. Only once you "
                               "confirm that it is the same device and the FULL verify passes is it recorded; when the "
                               "enclosure changes again, the serial tells.",
        facts.label));
    return body;
}

std::string device_identity_reader_note(const Text& text)
{
    return text("Needs administrator permission: Windows asks whether to allow the VeriArchive device identity reader "
                "of an unknown publisher. It reads only the model, serial and WWN that the device reports, and reads "
                "or writes none of the data on the device.");
}

std::string device_read_declined(const Text& text)
{
    return text("Administrator permission was not given, so the device's own serial was not read. Choose \"Confirm "
                "without reading the serial\" only if you are sure that you moved this device into the new enclosure "
                "yourself.");
}

std::string device_read_failed(const Text& text)
{
    // No advice to attach the device directly: taken out of its enclosure, it
    // is no changed enclosure (enclosure_changed), and only a relocation
    // accepts it.
    return text("The device's own serial cannot be read: this enclosure does not pass ATA commands on. Try another "
                "enclosure; when you are sure that it is the same device, you can also confirm it yourself.");
}

std::string operator_confirmation_title(const Text& text)
{
    return text("Confirm without reading the serial");
}

std::vector<std::string> operator_confirmation_body(const Text& text, const EnclosureFacts& facts)
{
    return {text.format("Do this only if you are sure that you moved the device of {} into the new enclosure yourself.",
                facts.label),
        text("VeriArchive verifies it in full once. Whatever the result, the history records that you confirmed it "
             "without reading its serial; only when the verification passes is the new enclosure recorded. A "
             "sector-by-sector clone passes the verification too, so VeriArchive cannot find such a mistake for "
             "you.")};
}

std::string device_read_title(const Text& text)
{
    return text("What the device reports");
}

std::vector<std::string> device_read_body(const Text& text, const EnclosureFacts& facts)
{
    return {text.format("{} records no serial of the device itself, so VeriArchive cannot tell by itself. Compare it "
                        "with the serial on the device's label: is this the device of {}, only in another enclosure?",
                facts.label, facts.label),
        text("Once you answer yes, it is verified in full once. Whatever the result, the history records that you "
             "confirmed it after reading its serial; only when the verification passes are the new enclosure and "
             "the device's own serial recorded.")};
}

std::string other_device_title(const Text& text, const EnclosureFacts& facts)
{
    return text.format("This is not the device of {}", facts.label);
}

std::vector<std::string> other_device_body(const Text& text, const EnclosureFacts& facts)
{
    return {text.format("Its filesystem is that of {} (the same filesystem ID), but the device differs: most likely a "
                        "sector-by-sector clone. You can register it as another archive (a copy of {}), then compare "
                        "the two with Compare.",
                facts.label, facts.label),
        text("Nothing was recorded.")};
}

std::string same_device_started(const Text& text, const DeviceIdentity& read)
{
    // A read matches by its WWN alone too (same_device_identity).
    return text.format("The device's own serial matches ({}). Once the FULL verify passes, the new enclosure is "
                       "recorded.",
        read.serial.empty() ? read.wwn : read.serial);
}

std::string confirmation_words(const Text& text, const CarrierConfirmation& confirmation)
{
    if (confirmation.how == CarrierConfirmation::How::DeviceSerial) {
        return text("the device's serial matches");
    }
    return confirmation.device ? text("confirmed by you after reading the serial")
                               : text("confirmed by you, serial not read");
}

std::string confirmed_identity_words(const Text& text, const CarrierConfirmation& confirmation)
{
    return text.format(
        "MIGRATED: the enclosure differs from the one recorded ({})", confirmation_words(text, confirmation));
}

std::string carrier_line(const Text& text, const VerifyOutcome& outcome, bool sentence)
{
    if (!outcome.carrier_confirmed) {
        return {};
    }
    if (outcome.carrier_revision > 0) {
        const auto how = confirmation_words(text, outcome.carrier_confirmed->confirmation);
        // Two calls: the compiler checks each literal template against its values.
        return sentence ? text.format("New enclosure recorded: revision {} records it ({})", outcome.carrier_revision,
                              how)
                        : text.format("new enclosure recorded: revision {} records it ({})", outcome.carrier_revision,
                              how);
    }
    if (!outcome.carrier_not_recorded.empty()) {
        const auto why = text(outcome.carrier_not_recorded);
        return sentence ? text.format("New enclosure not recorded: {}", why)
                        : text.format("new enclosure not recorded: {}", why);
    }
    return {};
}

nlohmann::json carrier_member(const VerifyOutcome& outcome)
{
    if (!outcome.carrier_confirmed) {
        return {{"carrier_confirmed", nullptr}};
    }
    auto confirmed =
        carrier_confirmation_json(outcome.carrier_confirmed->confirmation, outcome.carrier_confirmed->from_revision);
    confirmed["revision"] = outcome.carrier_revision > 0 ? nlohmann::json(outcome.carrier_revision) : nullptr;
    confirmed["not_recorded"] =
        outcome.carrier_not_recorded.empty() ? nlohmann::json(nullptr) : nlohmann::json(outcome.carrier_not_recorded);
    return {{"carrier_confirmed", std::move(confirmed)}};
}

} // namespace va::app
