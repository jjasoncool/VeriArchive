#include "gui/enclosure_flow.h"

namespace va::gui {
namespace {

// A value of the fields of a read: as read, or that the device reports none.
std::string reported(const app::Text& text, const std::string& value)
{
    return value.empty() ? text("not reported") : value;
}

EnclosureNext show(EnclosureFlow flow)
{
    return EnclosureNext{EnclosureNext::Then::Show, std::move(flow), std::nullopt};
}

EnclosureNext verify(EnclosureFlow flow, CarrierConfirmation confirmation)
{
    return EnclosureNext{EnclosureNext::Then::Verify, std::move(flow), std::move(confirmation)};
}

EnclosureNext close()
{
    return EnclosureNext{EnclosureNext::Then::Close, {}, std::nullopt};
}

} // namespace

SerialRead serial_read_offer(const PhysicalDeviceInfo& now, bool reader)
{
    if (offers_device_read(now, reader)) {
        return SerialRead::Offered;
    }
    return reader && !now.enclosure_hides_identity ? SerialRead::ReportedByDevice : SerialRead::NoReader;
}

EnclosureDialogShown enclosure_dialog(const app::Text& text, const app::EnclosureFacts& facts,
    const std::string& where, const EnclosureFlow& flow, SerialRead offer)
{
    EnclosureDialogShown shown;
    const DialogAnswer cancel{std::string(enclosure_answer::cancel), text("Cancel")};
    const bool reads = offer == SerialRead::Offered;
    switch (flow.step) {
    case EnclosureStep::Question: {
        shown.heading = app::enclosure_question_title(text, where);
        // What changed, then the two serials, as the design lists them.
        auto body = app::enclosure_question_body(text, facts);
        shown.fields = {{text("Recorded"), facts.enclosure_recorded}, {text("Seen now"), facts.enclosure_now}};
        shown.fields_after = 1;
        // Where no read is offered, nothing tells but the operator: neither
        // that a read tells, nor a read to compare with the label; a clone
        // passes too.
        if (reads) {
            shown.paragraphs = std::move(body);
        } else {
            shown.paragraphs = {body.front(), app::sector_clone_words(text)};
        }
        if (flow.problem) {
            shown.problem = read_problem_words(text, *flow.problem, ReadShownIn::Question);
        }
        switch (offer) {
        case SerialRead::Offered:
            shown.note = app::device_identity_reader_note(text);
            break;
        case SerialRead::NoReader:
            shown.note = text("This computer cannot read the device's own serial (the device identity reader is not "
                              "available), so only you can confirm that it is the same device.");
            break;
        case SerialRead::ReportedByDevice:
            shown.note = text("This device reports its own serial through its enclosure, so a read with administrator "
                              "permission would tell nothing more: only you can confirm that it is the same device.");
            break;
        }
        shown.answers.push_back(cancel);
        // Without a confirmed device a read leaves it to the operator anyway;
        // with one, only once a read gave nothing, or none can be made.
        if (!facts.confirmed || flow.problem || !reads) {
            shown.answers.push_back(
                {std::string(enclosure_answer::without_serial), text("Confirm without reading the serial...")});
        }
        if (reads) {
            shown.answers.push_back(
                {std::string(enclosure_answer::read), text("Read the device's own serial"), true, true});
        }
        break;
    }
    case EnclosureStep::Reading:
        shown.heading = text("Reading the device's own serial");
        shown.paragraphs.push_back(text("Answer the User Account Control prompt to allow "
                                        "veriarchive-device-identity.exe. Once it has read the serial, VeriArchive "
                                        "continues here."));
        shown.note = app::device_identity_reader_note(text);
        shown.reading = true;
        shown.answers.push_back(cancel);
        break;
    case EnclosureStep::OperatorConfirmation:
        shown.heading = app::operator_confirmation_title(text);
        shown.paragraphs = app::operator_confirmation_body(text, facts);
        shown.answers.push_back(cancel);
        shown.answers.push_back(
            {std::string(enclosure_answer::confirmed), text("It is the same device, start the FULL verify")});
        break;
    case EnclosureStep::DeviceRead: {
        shown.heading = app::device_read_title(text);
        const auto device = flow.read.value_or(DeviceIdentity{});
        shown.fields = {{text("Model"), reported(text, device.model)},
            {text("Device's own serial"), reported(text, device.serial)}, {text("WWN"), reported(text, device.wwn)}};
        shown.paragraphs = app::device_read_body(text, facts);
        // Neither answer is suggested: only the operator, comparing the
        // serial with the device's label, knows.
        shown.answers.push_back(cancel);
        shown.answers.push_back({std::string(enclosure_answer::other), text("No, it is another device")});
        shown.answers.push_back(
            {std::string(enclosure_answer::same), text("It is the same device, start the FULL verify")});
        break;
    }
    case EnclosureStep::OtherDevice:
        shown.heading = facts.confirmed ? app::other_device_title(text, facts) : text("Another device");
        if (facts.confirmed && flow.read) {
            shown.fields = {{text("Serial recorded"), read_serial(facts.confirmed->device)},
                {text("Serial read now"), read_serial(*flow.read)}};
        }
        shown.paragraphs = app::other_device_body(text, facts);
        shown.answers.push_back(cancel);
        shown.answers.push_back({std::string(enclosure_answer::register_it), text("Register as another archive..."), true});
        break;
    }
    return shown;
}

EnclosureNext after_read(const app::EnclosureFacts& facts, const std::expected<DeviceIdentityRead, IoError>& read)
{
    if (!read) {
        const auto problem = read_problem(read.error());
        if (!problem) {
            return close();
        }
        return show(EnclosureFlow{EnclosureStep::Question, std::nullopt, problem});
    }
    const auto& device = read->device;
    switch (app::judge_device_read(device, facts.confirmed)) {
    case app::DeviceReadVerdict::SameDevice:
        return verify(EnclosureFlow{EnclosureStep::Reading, device, std::nullopt},
            app::confirmation_by_serial(facts, device));
    case app::DeviceReadVerdict::OtherDevice:
        return show(EnclosureFlow{EnclosureStep::OtherDevice, device, std::nullopt});
    case app::DeviceReadVerdict::NothingToCompare:
        break;
    }
    return show(EnclosureFlow{EnclosureStep::DeviceRead, device, std::nullopt});
}

EnclosureNext answered(const app::EnclosureFacts& facts, const EnclosureFlow& flow, std::string_view answer)
{
    // Each answer only in the step that offers it.
    switch (flow.step) {
    case EnclosureStep::Question:
        if (answer == enclosure_answer::read) {
            return EnclosureNext{EnclosureNext::Then::Read, EnclosureFlow{EnclosureStep::Reading, std::nullopt,
                                                                std::nullopt},
                std::nullopt};
        }
        if (answer == enclosure_answer::without_serial) {
            return show(EnclosureFlow{EnclosureStep::OperatorConfirmation, std::nullopt, std::nullopt});
        }
        break;
    case EnclosureStep::OperatorConfirmation:
        if (answer == enclosure_answer::confirmed) {
            return verify(flow, app::confirmation_by_operator(facts, std::nullopt));
        }
        break;
    case EnclosureStep::DeviceRead:
        if (flow.read && answer == enclosure_answer::same) {
            return verify(flow, app::confirmation_by_operator(facts, flow.read));
        }
        if (answer == enclosure_answer::other) {
            return show(EnclosureFlow{EnclosureStep::OtherDevice, flow.read, std::nullopt});
        }
        break;
    case EnclosureStep::OtherDevice:
        if (answer == enclosure_answer::register_it) {
            return EnclosureNext{EnclosureNext::Then::Register, flow, std::nullopt};
        }
        break;
    case EnclosureStep::Reading:
        break;
    }
    return close();
}

std::string confirmed_running_notice(
    const app::Text& text, const std::string& label, const CarrierConfirmation& confirmation)
{
    const auto how = app::confirmation_words(text, confirmation);
    // A serial that the operator compared with the label is the archive's
    // confirmed device once it passes; one that matched is so already.
    if (confirmation.how == CarrierConfirmation::How::Operator && confirmation.device &&
        !read_serial(*confirmation.device).empty()) {
        return text.format("Confirmed as the device of {} ({}). Only once this FULL verify passes are the new "
                           "enclosure {} and the device's own serial {} recorded; whatever the result, the history "
                           "records how it was confirmed.",
            label, how, confirmation.enclosure_now, read_serial(*confirmation.device));
    }
    return text.format("Confirmed as the device of {} ({}). Only once this FULL verify passes is the new enclosure {} "
                       "recorded; whatever the result, the history records how it was confirmed.",
        label, how, confirmation.enclosure_now);
}

std::string confirmed_toast(const app::Text& text, const std::string& base, const VerifyOutcome& outcome)
{
    const auto line = app::carrier_line(text, outcome, true);
    return line.empty() ? base : text.format("{} · {}", base, line);
}

std::string enclosure_stop_words(const app::Text& text, const std::string& where)
{
    return text.format("Not verified: the enclosure of {} differs from the one recorded (MIGRATED). FULL verify in "
                       "the archive's details asks first whether it is the same device, then verifies it.",
        where);
}

} // namespace va::gui
