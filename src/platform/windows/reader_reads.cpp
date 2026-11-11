#include "platform/windows/reader_reads.h"

#include "core/smart/smart_data.h"

#include <cstddef>
#include <span>

namespace va::platform::windows {
namespace {

// Why nothing was read of a device, as a SMART read keeps it.
std::string problem_of(const IoError& error)
{
    switch (error.kind) {
    case IoErrorKind::CommandNotPassed:
        return "not-passed";
    case IoErrorKind::Timeout:
        return "no-answer";
    default:
        return "unreadable";
    }
}

} // namespace

std::vector<DeviceRead> device_reads(const std::vector<ReaderDevice>& devices,
    const std::vector<PhysicalDeviceInfo>& seen, const Timestamp& read_at)
{
    std::vector<DeviceRead> reads;
    for (std::size_t index = 0; index < devices.size() && index < seen.size(); ++index) {
        const auto& device = devices[index];
        DeviceRead read;
        read.device_ref = seen[index].device_ref;
        SmartRead smart;
        smart.read_at = read_at;
        smart.seen = seen[index];
        smart.seen.health.reset();

        if (!device.error.empty()) {
            const auto error = reader_error(device.error, device.code);
            read.identity = std::unexpected(error);
            if (error.kind != IoErrorKind::NotFound) {
                smart.problem = problem_of(error);
                smart.detail = error.message;
                read.smart = std::move(smart);
            }
            reads.push_back(std::move(read));
            continue;
        }

        DeviceIdentityRead identity;
        identity.how = device.how;
        identity.read_at = read_at;
        identity.device_ref = read.device_ref;
        if (device.how == "nvme") {
            const auto decoded = decode_nvme_identify(device.identify);
            if (decoded) {
                identity.device = DeviceIdentity{decoded->model, decoded->serial, ""};
                smart.firmware = decoded->firmware;
            }
        } else {
            const auto decoded = decode_identify_data(std::as_bytes(std::span(device.identify)));
            if (decoded) {
                identity.device = DeviceIdentity{decoded->model, decoded->serial, decoded->wwn};
                smart.firmware = decoded->firmware;
            }
        }
        if (identity.device.serial.empty() && identity.device.wwn.empty()) {
            const IoError error{IoErrorKind::Other, 0,
                "the device identity reader wrote an identification that does not decode"};
            read.identity = std::unexpected(error);
            smart.problem = "unreadable";
            smart.detail = error.message;
            read.smart = std::move(smart);
            reads.push_back(std::move(read));
            continue;
        }
        read.identity = identity;
        smart.device = identity.device;
        smart.how = device.how;
        smart.identify = device.identify;
        smart.data = device.smart;
        smart.thresholds = device.thresholds;
        if (smart.data.empty()) {
            const auto support = device.how == "nvme" ? std::nullopt : ata_smart_support(device.identify);
            const auto error = reader_error("not-passed", device.smart_code);
            if (support && !support->supported) {
                smart.problem = "smart-unsupported";
            } else if (support && !support->enabled) {
                smart.problem = "smart-off";
            } else if (error.kind == IoErrorKind::Timeout) {
                smart.problem = "no-answer";
                smart.detail = error.message;
            } else {
                smart.problem = "smart-not-passed";
                if (device.smart_code != 0) {
                    smart.detail = error.message;
                }
            }
        }
        read.smart = std::move(smart);
        reads.push_back(std::move(read));
    }
    return reads;
}

} // namespace va::platform::windows
