// `media list`: every mounted volume with its filesystem identity and the
// physical devices behind it (M2). Needs no repository.
#include "cli/commands.h"

#include "common/utf8.h"
#include "core/identity/volume_json.h"
#include "platform/media_enumerator.h"

#include <format>
#include <iostream>

namespace va::cli {
namespace {

std::string format_bytes(std::uint64_t bytes)
{
    const char* units[] = {"B", "KiB", "MiB", "GiB", "TiB"};
    double value = static_cast<double>(bytes);
    int unit = 0;
    while (value >= 1024.0 && unit < 4) {
        value /= 1024.0;
        ++unit;
    }
    return std::format("{:.1f} {}", value, units[unit]);
}

void print_volume(const app::Text& text, const VolumeInfo& volume)
{
    std::cout << path_to_utf8(volume.mount_point) << "  " << volume.filesystem_type << "  \""
              << volume.volume_label << "\"  " << format_bytes(volume.capacity_bytes)
              << (volume.read_only ? "  " + text("read-only") : std::string{}) << '\n'
              << "  "
              << text.format("filesystem_uuid: {}  short: {}",
                     volume.filesystem_uuid.empty() ? "-" : volume.filesystem_uuid,
                     volume.filesystem_uuid_short.empty() ? "-" : volume.filesystem_uuid_short)
              << '\n';
    for (const auto& device : volume.devices) {
        std::cout << "  "
                  << text.format("device: {}  serial: {}  bus: {}",
                         device.model.empty() ? "?" : device.model,
                         device.serial.empty() ? text("(none)") : device.serial,
                         device.bus.empty() ? "?" : device.bus)
                  << "  " << format_bytes(device.capacity_bytes)
                  << (device.rotational ? (*device.rotational ? "  hdd" : "  ssd") : "") << '\n';
    }
}

} // namespace

int media_command(const Arguments& arguments)
{
    if (arguments.words.size() != 2 || arguments.words[1] != "list") {
        return report_failure(arguments, {exit_code::usage, "usage: media list"});
    }
    const auto enumerator = platform::make_media_enumerator();
    const auto volumes = enumerator->volumes();
    if (!volumes) {
        return report_failure(arguments, {exit_code::repository, volumes.error().message});
    }

    if (arguments.json) {
        auto list = nlohmann::json::array();
        for (const auto& volume : *volumes) {
            list.push_back(volume_to_json(volume));
        }
        print_json({{"volumes", std::move(list)}});
    } else {
        for (const auto& volume : *volumes) {
            print_volume(arguments.text, volume);
        }
    }
    return exit_code::passed;
}

} // namespace va::cli
