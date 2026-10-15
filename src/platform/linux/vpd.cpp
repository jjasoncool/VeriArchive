#include "platform/linux/vpd.h"

namespace va::platform::linux_vpd {

// The Unit Serial Number page as SCSI Primary Commands (SPC) defines it:
//   byte 0     peripheral qualifier and device type
//   byte 1     page code, 0x80
//   bytes 2-3  page length: the number of bytes after these four, big-endian
//   bytes 4..  the serial number in printable ASCII, right-aligned (so it may
//              start with spaces) and possibly ended by NUL bytes.
// sysfs shows the page exactly as the device returned it, header included.
std::string unit_serial(std::string_view page)
{
    constexpr std::size_t header_size = 4;
    constexpr unsigned char page_code = 0x80;
    // Bytes are compared as unsigned char: `char` is signed on x86, so 0x80
    // read as `char` is -128.
    const auto byte = [&](std::size_t index) { return static_cast<unsigned char>(page[index]); };
    if (page.size() < header_size || byte(1) != page_code) {
        return {};
    }
    const std::size_t length = (std::size_t{byte(2)} << 8) | byte(3);
    if (length > page.size() - header_size) {
        return {}; // the page was cut short
    }

    auto serial = page.substr(header_size, length);
    constexpr std::string_view padding{" \0", 2}; // a space and a NUL
    const auto first = serial.find_first_not_of(padding);
    if (first == std::string_view::npos) {
        return {};
    }
    serial = serial.substr(first, serial.find_last_not_of(padding) - first + 1);

    // Anything but printable ASCII is not a serial number; it would also not be
    // valid UTF-8 in the media evidence JSON (R-96).
    for (const char character : serial) {
        const auto code = static_cast<unsigned char>(character);
        if (code < 0x20 || code > 0x7e) {
            return {};
        }
    }
    return std::string(serial);
}

bool is_ata_vendor(std::string_view vendor)
{
    // The vendor field of a SCSI INQUIRY is 8 characters padded with spaces;
    // sysfs adds a newline.
    constexpr std::string_view padding = " \t\r\n";
    const auto first = vendor.find_first_not_of(padding);
    if (first == std::string_view::npos) {
        return false;
    }
    return vendor.substr(first, vendor.find_last_not_of(padding) - first + 1) == "ATA";
}

} // namespace va::platform::linux_vpd
