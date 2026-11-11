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

// Bytes as lowercase hex, two digits each.
std::string hex_text(std::span<const std::uint8_t> bytes)
{
    constexpr char digits[] = "0123456789abcdef";
    std::string text;
    text.reserve(bytes.size() * 2);
    for (const auto byte : bytes) {
        text += digits[byte >> 4];
        text += digits[byte & 0x0f];
    }
    return text;
}

// The page `name` of a device's result: its bytes, from lowercase hex of
// exactly `size` bytes; none where the member is not there; nothing where it
// is there otherwise.
std::optional<std::vector<std::uint8_t>> page_member(const nlohmann::json& object, const char* name, std::size_t size)
{
    const auto found = object.find(name);
    if (found == object.end()) {
        return std::vector<std::uint8_t>{};
    }
    if (!found->is_string()) {
        return std::nullopt;
    }
    const auto& text = found->get_ref<const std::string&>();
    if (text.size() != 2 * size) {
        return std::nullopt;
    }
    const auto digit = [](char character) -> int {
        if (character >= '0' && character <= '9') {
            return character - '0';
        }
        if (character >= 'a' && character <= 'f') {
            return character - 'a' + 10;
        }
        return -1;
    };
    std::vector<std::uint8_t> bytes(size);
    for (std::size_t index = 0; index < size; ++index) {
        const int high = digit(text[2 * index]);
        const int low = digit(text[2 * index + 1]);
        if (high < 0 || low < 0) {
            return std::nullopt;
        }
        bytes[index] = static_cast<std::uint8_t>((high << 4) | low);
    }
    return bytes;
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


std::optional<std::vector<unsigned>> device_numbers_argument(std::span<const std::wstring_view> arguments)
{
    if (arguments.empty() || arguments.size() > physical_drive_count) {
        return std::nullopt;
    }
    std::vector<unsigned> numbers;
    for (const auto argument : arguments) {
        const auto number = device_number(argument);
        if (!number || std::ranges::find(numbers, *number) != numbers.end()) {
            return std::nullopt;
        }
        numbers.push_back(*number);
    }
    return numbers;
}

std::wstring reader_parameters(std::span<const unsigned> device_numbers, std::wstring_view output_path)
{
    std::wstring parameters = L"\"" + std::wstring(output_path) + L"\"";
    for (const auto number : device_numbers) {
        parameters += L" " + std::to_wstring(number);
    }
    return parameters;
}

std::array<std::uint8_t, 16> smart_cdb(std::uint8_t feature)
{
    auto cdb = identify_device_cdb();
    cdb[4] = feature;                 // FEATURES (7:0)
    cdb[10] = smart_lba_mid;          // LBA (15:8)
    cdb[12] = smart_lba_high;         // LBA (23:16)
    cdb[14] = smart_command;
    return cdb;
}

std::string reader_result_json(const std::vector<ReaderDevice>& devices)
{
    auto list = nlohmann::json::array();
    for (const auto& device : devices) {
        nlohmann::json entry{{"number", device.number}};
        if (!device.error.empty()) {
            entry["error"] = device.error;
            entry["code"] = device.code;
            list.push_back(std::move(entry));
            continue;
        }
        entry["how"] = device.how;
        entry["identify"] = hex_text(device.identify);
        if (!device.smart.empty()) {
            entry["smart"] = hex_text(device.smart);
            if (!device.thresholds.empty()) {
                entry["thresholds"] = hex_text(device.thresholds);
            }
        } else {
            entry["smart_error"] = device.smart_error.empty() ? std::string("not-passed") : device.smart_error;
            entry["smart_code"] = device.smart_code;
        }
        list.push_back(std::move(entry));
    }
    return nlohmann::json{{"devices", std::move(list)}}.dump();
}

std::expected<std::vector<ReaderDevice>, IoError> reader_devices(
    std::string_view text, std::span<const unsigned> asked)
{
    if (text.size() > largest_reader_result) {
        return std::unexpected(other("the device identity reader wrote a result larger than it writes"));
    }
    const auto json = nlohmann::json::parse(text, nullptr, false);
    const auto list = json.is_object() ? json.find("devices") : json.end();
    if (!json.is_object() || list == json.end() || !list->is_array() || list->size() != asked.size()) {
        return std::unexpected(other("the device identity reader wrote no result that it writes"));
    }
    const auto refused = [] {
        return std::unexpected(other("the device identity reader wrote a result that it does not write"));
    };
    std::vector<ReaderDevice> devices;
    for (std::size_t index = 0; index < asked.size(); ++index) {
        const auto& entry = (*list)[index];
        if (!entry.is_object()) {
            return refused();
        }
        ReaderDevice device;
        const auto number = entry.find("number");
        if (number == entry.end() || !number->is_number_unsigned() || number->get<unsigned>() != asked[index]) {
            return refused();
        }
        device.number = asked[index];
        if (const auto error = entry.find("error"); error != entry.end()) {
            const auto code = entry.find("code");
            if (!error->is_string() || (*error != "open-failed" && *error != "not-passed") || code == entry.end() ||
                !code->is_number_unsigned()) {
                return refused();
            }
            device.error = error->get<std::string>();
            device.code = code->get<std::uint32_t>();
            devices.push_back(std::move(device));
            continue;
        }
        bool valid = true;
        device.how = text_member(entry, "how", valid);
        if (!valid || (device.how != "ata" && device.how != "sat" && device.how != "nvme")) {
            return refused();
        }
        const bool nvme = device.how == "nvme";
        auto identify = page_member(entry, "identify", nvme ? nvme_identify_data_size : identify_data_size);
        if (!identify || identify->empty()) {
            return refused();
        }
        device.identify = std::move(*identify);
        auto smart = page_member(entry, "smart", nvme ? nvme_health_log_size : identify_data_size);
        auto thresholds = page_member(entry, "thresholds", identify_data_size);
        if (!smart || !thresholds || (nvme && !thresholds->empty()) || (smart->empty() && !thresholds->empty())) {
            return refused();
        }
        device.smart = std::move(*smart);
        device.thresholds = std::move(*thresholds);
        if (device.smart.empty()) {
            const auto smart_error = entry.find("smart_error");
            const auto smart_code = entry.find("smart_code");
            if (smart_error == entry.end() || *smart_error != "not-passed" || smart_code == entry.end() ||
                !smart_code->is_number_unsigned()) {
                return refused();
            }
            device.smart_error = "not-passed";
            device.smart_code = smart_code->get<std::uint32_t>();
        }
        devices.push_back(std::move(device));
    }
    return devices;
}

IoError reader_error(std::string_view error, std::uint32_t code)
{
    // ERROR_FILE_NOT_FOUND, ERROR_PATH_NOT_FOUND, ERROR_DEV_NOT_EXIST,
    // ERROR_NO_SUCH_DEVICE, ERROR_DEVICE_NOT_CONNECTED, ERROR_DEVICE_REMOVED:
    // the device is no longer attached.
    const bool gone = code == 2 || code == 3 || code == 55 || code == 433 || code == 1167 || code == 1617;
    if (error == "not-passed") {
        if (gone) {
            return IoError{IoErrorKind::NotFound, static_cast<int>(code),
                std::format("the device is no longer attached (error {})", code)};
        }
        // ERROR_SEM_TIMEOUT, WAIT_TIMEOUT, ERROR_OPERATION_ABORTED (the
        // request ended by a reset), ERROR_TIMEOUT.
        if (code == 121 || code == 258 || code == 995 || code == 1460) {
            return IoError{IoErrorKind::Timeout, static_cast<int>(code),
                std::format("the device did not answer in time (error {})", code)};
        }
        return IoError{IoErrorKind::CommandNotPassed, static_cast<int>(code),
            "the device's enclosure passes no ATA command on to the device"};
    }
    if (error == "open-failed") {
        return IoError{gone ? IoErrorKind::NotFound : IoErrorKind::Other, static_cast<int>(code),
            std::format("the device identity reader could not open the device (error {})", code)};
    }
    return other("the device identity reader wrote an error that it does not write");
}

} // namespace va::platform::windows
