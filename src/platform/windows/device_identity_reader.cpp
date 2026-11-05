#include "platform/windows/device_identity_reader.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <format>

namespace va::platform::windows {
namespace {

std::uint16_t word_at(std::span<const std::byte> data, std::size_t index)
{
    return static_cast<std::uint16_t>(std::to_integer<unsigned>(data[2 * index]) |
                                      (std::to_integer<unsigned>(data[2 * index + 1]) << 8));
}

// Words [first, first + count) as ATA text: each word's high byte first.
std::string ata_text(std::span<const std::byte> data, std::size_t first, std::size_t count)
{
    std::string text;
    for (std::size_t index = first; index < first + count; ++index) {
        const auto word = word_at(data, index);
        for (const unsigned byte : {static_cast<unsigned>(word >> 8), static_cast<unsigned>(word & 0xff)}) {
            if (byte == 0) {
                text += ' ';
            } else if (byte < 0x20 || byte > 0x7e) {
                text += '?';
            } else {
                text += static_cast<char>(byte);
            }
        }
    }
    const auto begin = text.find_first_not_of(' ');
    if (begin == std::string::npos) {
        return {};
    }
    return text.substr(begin, text.find_last_not_of(' ') - begin + 1);
}

// The digits of a device's number: "0" to "63", no sign, space or leading
// zero.
template <typename Character>
std::optional<unsigned> device_number(std::basic_string_view<Character> digits)
{
    if (digits.empty() || digits.size() > 2 || (digits.size() > 1 && digits.front() == '0')) {
        return std::nullopt;
    }
    unsigned number = 0;
    for (const Character digit : digits) {
        if (digit < '0' || digit > '9') {
            return std::nullopt;
        }
        number = number * 10 + static_cast<unsigned>(digit - '0');
    }
    if (number >= physical_drive_count) {
        return std::nullopt;
    }
    return number;
}

bool is_drive_letter(wchar_t character)
{
    return (character >= L'A' && character <= L'Z') || (character >= L'a' && character <= L'z');
}

bool allowed_in_name(wchar_t character)
{
    return character >= 0x20 && std::wstring_view(L"<>:\"/\\|?*").find(character) == std::wstring_view::npos;
}

bool ends_with_json(std::wstring_view name)
{
    if (name.size() <= 5) {
        return false;
    }
    std::wstring ending(name.substr(name.size() - 5));
    std::ranges::transform(ending, ending.begin(), [](wchar_t character) {
        return character >= L'A' && character <= L'Z' ? static_cast<wchar_t>(character - L'A' + L'a') : character;
    });
    return ending == L".json";
}

std::string text_member(const nlohmann::json& object, const char* name, bool& valid)
{
    const auto found = object.find(name);
    if (found == object.end() || !found->is_string()) {
        valid = false;
        return {};
    }
    return found->get<std::string>();
}

bool printable_ascii(std::string_view text, std::size_t longest)
{
    return text.size() <= longest &&
           std::ranges::all_of(text, [](char character) { return character >= 0x20 && character <= 0x7e; });
}

bool wwn_text(std::string_view text)
{
    return text.empty() || (text.size() == 16 && std::ranges::all_of(text, [](char character) {
        return (character >= '0' && character <= '9') || (character >= 'a' && character <= 'f');
    }));
}

IoError other(std::string message)
{
    return IoError{IoErrorKind::Other, 0, std::move(message)};
}

} // namespace

std::optional<AtaIdentity> decode_identify_data(std::span<const std::byte> data)
{
    if (data.size() != identify_data_size) {
        return std::nullopt;
    }
    if (std::ranges::all_of(data, [](std::byte byte) { return byte == std::byte{0}; })) {
        return std::nullopt;
    }
    // Word 255: where its low byte is the signature 0xA5, its high byte makes
    // the sum of all 512 bytes zero (ATA8-ACS, the integrity word).
    if ((word_at(data, 255) & 0xff) == 0xa5) {
        unsigned sum = 0;
        for (const std::byte byte : data) {
            sum += std::to_integer<unsigned>(byte);
        }
        if ((sum & 0xff) != 0) {
            return std::nullopt;
        }
    }
    AtaIdentity identity{ata_text(data, 27, 20), ata_text(data, 10, 10), ata_text(data, 23, 4), {}};
    const std::uint16_t wwn[4]{word_at(data, 108), word_at(data, 109), word_at(data, 110), word_at(data, 111)};
    const bool none = std::ranges::all_of(wwn, [](std::uint16_t word) { return word == 0; }) ||
                      std::ranges::all_of(wwn, [](std::uint16_t word) { return word == 0xffff; });
    if (!none) {
        identity.wwn = std::format("{:04x}{:04x}{:04x}{:04x}", wwn[0], wwn[1], wwn[2], wwn[3]);
    }
    if (identity.serial.empty() && identity.wwn.empty()) {
        return std::nullopt;
    }
    return identity;
}

std::array<std::uint8_t, 16> identify_device_cdb()
{
    std::array<std::uint8_t, 16> cdb{};
    cdb[0] = 0x85;                    // ATA PASS-THROUGH (16)
    cdb[1] = 4 << 1;                  // protocol 4: PIO data-in; no 48-bit registers
    cdb[2] = 0x0e;                    // T_DIR from the device, BYT_BLOK in blocks, T_LENGTH in the sector count
    cdb[6] = 1;                       // sector count: one block of 512 bytes
    cdb[14] = identify_device_command;
    return cdb;
}

std::optional<unsigned> physical_drive_number(std::string_view device_ref)
{
    constexpr std::string_view prefix = "\\\\.\\PhysicalDrive";
    if (!device_ref.starts_with(prefix)) {
        return std::nullopt;
    }
    return device_number(device_ref.substr(prefix.size()));
}

std::optional<unsigned> device_number_argument(std::wstring_view argument)
{
    return device_number(argument);
}

bool output_path_argument(std::wstring_view argument)
{
    if (argument.size() > 32000) {
        return false;
    }
    if (argument.starts_with(L"\\\\?\\")) {
        argument.remove_prefix(4);
    }
    if (argument.size() < 3 || !is_drive_letter(argument[0]) || argument[1] != L':' || argument[2] != L'\\') {
        return false;
    }
    argument.remove_prefix(3);
    std::wstring_view name;
    while (true) {
        const auto separator = argument.find(L'\\');
        name = argument.substr(0, separator);
        if (name.empty() || name == L"." || name == L".." || !std::ranges::all_of(name, allowed_in_name)) {
            return false;
        }
        if (separator == std::wstring_view::npos) {
            break;
        }
        argument.remove_prefix(separator + 1);
    }
    return ends_with_json(name);
}

std::wstring reader_parameters(unsigned device_number, std::wstring_view output_path)
{
    return std::to_wstring(device_number) + L" \"" + std::wstring(output_path) + L"\"";
}

std::string identity_result_json(const AtaIdentity& identity, ReaderHow how)
{
    return nlohmann::json{{"model", identity.model}, {"serial", identity.serial}, {"wwn", identity.wwn},
        {"firmware", identity.firmware}, {"how", how == ReaderHow::Ata ? "ata" : "sat"}}
        .dump();
}

std::string error_result_json(std::string_view error, std::uint32_t code)
{
    return nlohmann::json{{"error", error}, {"code", code}}.dump();
}

std::expected<DeviceIdentityRead, IoError> reader_result(
    std::string_view text, const std::string& device_ref, const Timestamp& read_at)
{
    if (text.size() > largest_reader_result) {
        return std::unexpected(other("the device identity reader wrote a result larger than it writes"));
    }
    const auto json = nlohmann::json::parse(text, nullptr, false);
    if (!json.is_object()) {
        return std::unexpected(other("the device identity reader wrote no result that it writes"));
    }
    if (const auto error = json.find("error"); error != json.end()) {
        const auto code = json.find("code");
        const std::uint32_t native = code != json.end() && code->is_number_unsigned()
                                         ? code->get<std::uint32_t>()
                                         : 0;
        // ERROR_FILE_NOT_FOUND, ERROR_PATH_NOT_FOUND, ERROR_DEV_NOT_EXIST,
        // ERROR_NO_SUCH_DEVICE, ERROR_DEVICE_NOT_CONNECTED, ERROR_DEVICE_REMOVED:
        // the device is no longer attached.
        const bool gone = native == 2 || native == 3 || native == 55 || native == 433 || native == 1167 ||
                          native == 1617;
        if (*error == "not-passed") {
            if (gone) {
                return std::unexpected(IoError{IoErrorKind::NotFound, static_cast<int>(native),
                    std::format("the device is no longer attached (error {})", native)});
            }
            // ERROR_SEM_TIMEOUT, WAIT_TIMEOUT, ERROR_OPERATION_ABORTED (the
            // request ended by a reset), ERROR_TIMEOUT.
            if (native == 121 || native == 258 || native == 995 || native == 1460) {
                return std::unexpected(IoError{IoErrorKind::Timeout, static_cast<int>(native),
                    std::format("the device did not answer IDENTIFY DEVICE in time (error {})", native)});
            }
            return std::unexpected(IoError{IoErrorKind::CommandNotPassed, static_cast<int>(native),
                "the device's enclosure passes no ATA command on to the device"});
        }
        if (*error == "open-failed") {
            return std::unexpected(IoError{gone ? IoErrorKind::NotFound : IoErrorKind::Other,
                static_cast<int>(native),
                std::format("the device identity reader could not open the device (error {})", native)});
        }
        return std::unexpected(other("the device identity reader wrote an error that it does not write"));
    }
    bool valid = true;
    DeviceIdentityRead read;
    read.device.model = text_member(json, "model", valid);
    read.device.serial = text_member(json, "serial", valid);
    read.device.wwn = text_member(json, "wwn", valid);
    const auto firmware = text_member(json, "firmware", valid);
    read.how = text_member(json, "how", valid);
    if (!valid || !printable_ascii(read.device.model, 40) || !printable_ascii(read.device.serial, 20) ||
        !printable_ascii(firmware, 8) || !wwn_text(read.device.wwn) ||
        (read.device.serial.empty() && read.device.wwn.empty()) || (read.how != "ata" && read.how != "sat")) {
        return std::unexpected(other("the device identity reader wrote an identity that it does not write"));
    }
    read.read_at = read_at;
    read.device_ref = device_ref;
    return read;
}

} // namespace va::platform::windows
