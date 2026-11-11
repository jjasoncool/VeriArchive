#pragma once
// What the device identity reader (veriarchive-device-identity.exe,
// decisions 0047 and 0052) and the program that starts it share, in plain C++
// without the Windows headers, so that it is tested on every system: how the
// reader asks a device for its own identity and its SMART (ATA commands, alone
// or inside the SCSI command that USB bridges take; the NVMe protocol through
// the storage driver), which arguments it takes, and the JSON result it
// writes, which the program that started it reads back. The reader runs with
// administrator permission, so both sides take nothing but what is written
// here. The reader writes the pages the devices return as they are; the
// program that started it decodes them, unprivileged.
#include "common/error.h"
#include "common/types.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace va::platform::windows {

// The reader's file name, in the folder of the program that starts it.
inline constexpr std::wstring_view device_identity_reader_name = L"veriarchive-device-identity.exe";
// The devices that the reader takes the number of, as Windows names them:
// \\.\PhysicalDrive0 to \\.\PhysicalDrive63.
inline constexpr unsigned physical_drive_count = 64;
// The largest result the program that started the reader reads back: each of
// 64 devices with its pages written out in hex.
inline constexpr std::size_t largest_reader_result = 1024 * 1024;

// ATA IDENTIFY DEVICE: a command that returns the device's identification,
// 512 bytes, and changes nothing.
inline constexpr std::uint8_t identify_device_command = 0xEC;
inline constexpr std::size_t identify_data_size = 512;
// ATA SMART (decision 0052): the command, its two reads (SMART READ DATA,
// SMART READ THRESHOLDS; each returns 512 bytes and changes nothing), and the
// signature its LBA registers carry.
inline constexpr std::uint8_t smart_command = 0xB0;
inline constexpr std::uint8_t smart_read_data = 0xD0;
inline constexpr std::uint8_t smart_read_thresholds = 0xD1;
inline constexpr std::uint8_t smart_lba_mid = 0x4F;
inline constexpr std::uint8_t smart_lba_high = 0xC2;
// NVMe (decision 0052): Identify Controller (4096 bytes) and the SMART /
// Health Information log (log page 0x02, 512 bytes), asked through the
// storage driver; each changes nothing.
inline constexpr std::size_t nvme_identify_data_size = 4096;
inline constexpr std::uint32_t nvme_identify_controller = 0x01;
inline constexpr std::uint32_t nvme_health_log = 0x02;
inline constexpr std::size_t nvme_health_log_size = 512;

// What a device says of itself in its identification, each text trimmed:
// model (words 27-46), serial (words 10-19), firmware revision (words 23-26)
// and WWN (words 108-111, 16 lowercase hex digits; empty where the device
// gives none).
struct AtaIdentity {
    std::string model;
    std::string serial;
    std::string firmware;
    std::string wwn;
};

// The identification in `data`, the 512 bytes that IDENTIFY DEVICE returns:
// 256 words, each sent low byte first, and in each text word the first
// character in the high byte. A NUL counts as a space; any other byte that is
// no printable ASCII becomes '?'. Nothing for data of another size, data that
// is all zeros (a bridge that passed the command on to nothing), data whose
// integrity word (255: 0xA5 in its low byte) does not sum to zero, or an
// identification with neither a serial nor a WWN.
std::optional<AtaIdentity> decode_identify_data(std::span<const std::byte> data);

// The 16 bytes of the SCSI command ATA PASS-THROUGH (16) of SAT (operation
// code 0x85) that carries IDENTIFY DEVICE to the device behind a USB bridge:
// protocol PIO data-in, one 512-byte block read from the device, the length
// in the sector count, every register but the command zero.
std::array<std::uint8_t, 16> identify_device_cdb();
// The same command carrying SMART with `feature` (smart_read_data or
// smart_read_thresholds): the feature, the LBA's signature (mid 0x4F, high
// 0xC2) and the command 0xB0; one 512-byte block read from the device.
std::array<std::uint8_t, 16> smart_cdb(std::uint8_t feature);

// The number N of `device_ref` when it is "\\.\PhysicalDriveN", as the media
// enumerator names a device, with N below physical_drive_count, written
// without a sign, a space or a leading zero; nothing for any other text. The
// program that starts the reader gives it this number, never a path.
std::optional<unsigned> physical_drive_number(std::string_view device_ref);

// The reader's arguments: the file to write its result to, then the number of
// each device to read (the N of \\.\PhysicalDriveN, as above), one to
// physical_drive_count of them, each once.
std::optional<unsigned> device_number_argument(std::wstring_view argument);
std::optional<std::vector<unsigned>> device_numbers_argument(std::span<const std::wstring_view> arguments);
// Whether `argument` may name the reader's result file: a full path on a
// drive letter ("C:\...\name.json"), or in the \\?\ form of one, whose name
// ends with ".json" (in any case) after at least one character; no part of it
// empty, "." or "..", no character that Windows takes for something else in a
// name (< > : " / | ? * or a control character, so no stream of a file). The
// file must not be there yet: the reader makes it new.
bool output_path_argument(std::wstring_view argument);
// The reader's command line for the result file `output_path` (an
// output_path_argument), in double quotes, and the devices numbered
// `device_numbers`.
std::wstring reader_parameters(std::span<const unsigned> device_numbers, std::wstring_view output_path);

// What the reader read of one device: how, and the pages as the device
// returned them; or why nothing (`error`: "open-failed", the device could not
// be opened; "not-passed", no identification came back; `code`: the Win32
// error of the last try, 0 where the command came back refused or empty).
// Where the identification was read but SMART was not, `smart_error`
// ("not-passed") and `smart_code` say why.
struct ReaderDevice {
    unsigned number = 0;
    std::string how;                       // "ata" | "sat" | "nvme"
    std::vector<std::uint8_t> identify;    // IDENTIFY DEVICE (512) or Identify Controller (4096)
    std::vector<std::uint8_t> smart;       // SMART READ DATA or the health log (512)
    std::vector<std::uint8_t> thresholds;  // SMART READ THRESHOLDS (512); ATA only, and may be missing
    std::string error;
    std::uint32_t code = 0;
    std::string smart_error;
    std::uint32_t smart_code = 0;
};

// The reader's result: {"devices": [...]}, one object for each device it was
// given, in that order: {"number", "how", "identify", "smart", "thresholds"}
// (the pages in lowercase hex; "smart_error" and "smart_code" in place of the
// SMART pages that were not read), or {"number", "error", "code"}.
std::string reader_result_json(const std::vector<ReaderDevice>& devices);

// What the program that started the reader takes of its result `text`, read
// for the devices numbered `asked`: one ReaderDevice for each, in that order;
// else why not (Other): a result that is no JSON object, is larger than
// largest_reader_result, names other devices than those asked, in another
// order, a how other than "ata", "sat" or "nvme", pages that are not lowercase
// hex of their size (an ATA identification 512 bytes, an NVMe one 4096; SMART
// pages 512), an error it does not write.
std::expected<std::vector<ReaderDevice>, IoError> reader_devices(
    std::string_view text, std::span<const unsigned> asked);

// Why the reader read nothing of a device, as an IoError whose kind says why
// (IDeviceIdentityHelper): NotFound where the device is no longer attached
// (Win32 errors 2, 3, 55, 433, 1167, 1617), whether it could not be opened
// ("open-failed") or went during the command ("not-passed"); Timeout for
// "not-passed" where the command was not answered in time (Win32 errors 121,
// 258, 995, 1460), as a sleeping device spinning up may be; CommandNotPassed
// for any other "not-passed" (also code 0: the bridge answered with a SCSI
// status, or nothing); Other where the device cannot be opened otherwise.
// Never PermissionDenied or Cancelled: those come from the operator's answer
// and the wait, not from the reader.
IoError reader_error(std::string_view error, std::uint32_t code);

} // namespace va::platform::windows
