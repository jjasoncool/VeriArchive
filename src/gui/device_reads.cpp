#include "gui/device_reads.h"

#include "app/enclosure_words.h"
#include "core/engine/carrier_confirmation.h"

#include <algorithm>

namespace va::gui {

bool still_the_device(const PhysicalDeviceInfo& then, const PhysicalDeviceInfo& now)
{
    return !then.device_ref.empty() && then.device_ref == now.device_ref && then.model == now.model &&
           then.serial == now.serial && then.wwn == now.wwn && then.bus == now.bus &&
           then.capacity_bytes == now.capacity_bytes;
}

const KeptDeviceRead* kept_read_of(const KeptDeviceReads& reads, const PhysicalDeviceInfo& device)
{
    const auto found = reads.find(device.device_ref);
    return found != reads.end() && still_the_device(found->second.seen, device) ? &found->second : nullptr;
}

void keep_reads_of(KeptDeviceReads& reads, const std::vector<PhysicalDeviceInfo>& attached)
{
    std::erase_if(reads, [&](const auto& kept) {
        return std::ranges::none_of(
            attached, [&](const PhysicalDeviceInfo& device) { return still_the_device(kept.second.seen, device); });
    });
}

void note_confirmed(KeptDeviceReads& reads, const DeviceIdentity& device, const Uuid& archive)
{
    for (auto& [device_ref, kept] : reads) {
        if (same_device_identity(kept.read.device, device) &&
            std::ranges::find(kept.confirmed_of, archive) == kept.confirmed_of.end()) {
            kept.confirmed_of.push_back(archive);
        }
    }
}

bool offers_device_read(const PhysicalDeviceInfo& device, bool reader)
{
    return reader && device.enclosure_hides_identity && !device.device_ref.empty();
}

std::optional<ReadProblem> read_problem(const IoError& error)
{
    const auto failure = device_identity_failure(error);
    if (failure == DeviceIdentityFailure::Cancelled) {
        return std::nullopt;
    }
    return ReadProblem{failure, failure == DeviceIdentityFailure::Unreadable ? error.message : std::string{}};
}

std::string read_problem_words(const app::Text& text, const ReadProblem& problem, ReadShownIn where)
{
    const auto cause = text(problem.cause);
    switch (problem.failure) {
    case DeviceIdentityFailure::Declined:
        switch (where) {
        case ReadShownIn::Question:
            return app::device_read_declined(text);
        case ReadShownIn::Registration:
            return text("Administrator permission was not given, so the device's own serial is not recorded. You can "
                        "start without recording it, or go back.");
        case ReadShownIn::Devices:
            break;
        }
        return text("Administrator permission was not given, so the device's own serial was not read.");
    case DeviceIdentityFailure::NotPassed:
        switch (where) {
        case ReadShownIn::Question:
            return app::device_read_failed(text);
        case ReadShownIn::Registration:
            return text("The device's own serial cannot be read: this enclosure does not pass ATA commands on. You "
                        "can start without recording it, or register it later through another enclosure.");
        case ReadShownIn::Devices:
            break;
        }
        return text("The device's own serial cannot be read: this enclosure does not pass ATA commands on.");
    case DeviceIdentityFailure::NotAvailable:
        return text("This computer cannot read the device's own serial: the device identity reader is not available.");
    case DeviceIdentityFailure::Unreadable:
    case DeviceIdentityFailure::Cancelled:
        break;
    }
    switch (where) {
    case ReadShownIn::Question:
        return text.format("The device's own serial could not be read ({}). Try again, or try another enclosure; when "
                           "you are sure that it is the same device, you can also confirm it yourself.",
            cause);
    case ReadShownIn::Registration:
        return text.format("The device's own serial could not be read ({}). You can start without recording it, or "
                           "go back and try again.",
            cause);
    case ReadShownIn::Devices:
        break;
    }
    return text.format("The device's own serial could not be read: {}", cause);
}

std::string read_serial(const DeviceIdentity& device)
{
    return device.serial.empty() ? device.wwn : device.serial;
}

} // namespace va::gui
