#include "test_support.h"

#include "platform/linux/vpd.h"

#include <string>
#include <string_view>

namespace {

using va::platform::linux_vpd::is_ata_vendor;
using va::platform::linux_vpd::unit_serial;

// A VPD page as sysfs shows it: device type 0 (disk), the page code, the
// big-endian length of what follows, then `body`.
std::string vpd_page(std::string_view body, unsigned char code = 0x80)
{
    std::string page{'\0', static_cast<char>(code), static_cast<char>(body.size() >> 8),
        static_cast<char>(body.size() & 0xff)};
    page += body;
    return page;
}

void valid_pages()
{
    CHECK(unit_serial(vpd_page("WD-WCC4E1234567")) == "WD-WCC4E1234567");
    // libata answers for SATA devices with the 20-character ATA field, right-aligned.
    CHECK(unit_serial(vpd_page("     WD-WCC4E1234567")) == "WD-WCC4E1234567");
    CHECK(unit_serial(vpd_page("  S3Z9NB0K123456    ")) == "S3Z9NB0K123456");
    // A NUL ends the field; NULs and spaces may pad either side.
    CHECK(unit_serial(vpd_page(std::string_view("ZA1B2C3D\0\0\0\0", 12))) == "ZA1B2C3D");
    CHECK(unit_serial(vpd_page(std::string_view("\0 ZA1B2C3D \0", 12))) == "ZA1B2C3D");
    // Spaces inside the serial are kept; the matcher ignores them (serials_equal).
    CHECK(unit_serial(vpd_page("AB CD")) == "AB CD");

    // Bytes after the stated length are not part of the page.
    auto longer = vpd_page("SERIAL01");
    longer += "trailing";
    CHECK(unit_serial(longer) == "SERIAL01");

    // A length above 255 uses byte 2.
    const std::string long_serial(300, 'A');
    CHECK(unit_serial(vpd_page(long_serial)) == long_serial);
}

void invalid_pages()
{
    CHECK(unit_serial("").empty());
    CHECK(unit_serial(std::string_view("\0\x80\0", 3)).empty());     // shorter than the header
    CHECK(unit_serial(vpd_page("")).empty());                        // no serial
    CHECK(unit_serial(vpd_page("    ")).empty());                    // only padding
    CHECK(unit_serial(vpd_page(std::string_view("\0\0\0", 3))).empty());
    CHECK(unit_serial(vpd_page("SERIAL01", 0x83)).empty());          // another page

    // The length says 8 bytes follow, but only 5 do.
    auto truncated = vpd_page("SERIAL01");
    truncated.resize(truncated.size() - 3);
    CHECK(unit_serial(truncated).empty());

    // Not printable ASCII: a control character, a byte above 0x7e, a NUL inside.
    CHECK(unit_serial(vpd_page("AB\x01" "CD")).empty());
    CHECK(unit_serial(vpd_page("AB\xff" "CD")).empty());
    CHECK(unit_serial(vpd_page(std::string_view("AB\0CD", 5))).empty());
}

// Only SATA devices behind libata (vendor "ATA") take their serial from page
// 0x80; udev reports another serial for SAS, virtio-scsi and USB devices.
void ata_vendor()
{
    CHECK(is_ata_vendor("ATA     \n")); // as sysfs shows it
    CHECK(is_ata_vendor("ATA"));
    CHECK(!is_ata_vendor("Google  \n")); // a cloud virtual SCSI device
    CHECK(!is_ata_vendor("SEAGATE \n"));
    CHECK(!is_ata_vendor("ATAPI   \n"));
    CHECK(!is_ata_vendor("ata     \n"));
    CHECK(!is_ata_vendor(""));
    CHECK(!is_ata_vendor("        \n"));
}

} // namespace

// Decision 0021: without udev, SATA devices behind libata report their serial
// in VPD page 0x80 (/sys/block/<disk>/device/vpd_pg80).
void vpd_tests()
{
    valid_pages();
    invalid_pages();
    ata_vendor();
}
