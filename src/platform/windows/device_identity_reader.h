#pragma once
// What the device identity reader (veriarchive-device-identity.exe, decision
// 0047) and the program that starts it share, in plain C++ without the
// Windows headers, so that it is tested on every system: how the reader asks
// a device for its own identity (ATA IDENTIFY DEVICE, alone or inside the
// SCSI command that USB bridges take) and decodes the answer, which arguments
// it takes, and the small JSON result it writes, which the program that
// started it reads back. The reader runs with administrator permission, so
// both sides take nothing but what is written here.
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

namespace va::platform::windows {

// The reader's file name, in the folder of the program that starts it.
inline constexpr std::wstring_view device_identity_reader_name = L"veriarchive-device-identity.exe";
// The devices that the reader takes the number of, as Windows names them:
// \\.\PhysicalDrive0 to \\.\PhysicalDrive63.
inline constexpr unsigned physical_drive_count = 64;
// The largest result the program that started the reader reads back.
inline constexpr std::size_t largest_reader_result = 4096;

// ATA IDENTIFY DEVICE: a command that returns the device's identification,
// 512 bytes, and changes nothing.
inline constexpr std::uint8_t identify_device_command = 0xEC;
inline constexpr std::size_t identify_data_size = 512;

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

// The number N of `device_ref` when it is "\\.\PhysicalDriveN", as the media
// enumerator names a device, with N below physical_drive_count, written
// without a sign, a space or a leading zero; nothing for any other text. The
// program that starts the reader gives it this number, never a path.
std::optional<unsigned> physical_drive_number(std::string_view device_ref);

// The reader's arguments: the number of the device (the N of
// \\.\PhysicalDriveN, as above), then the file to write its result to.
std::optional<unsigned> device_number_argument(std::wstring_view argument);
// Whether `argument` may name the reader's result file: a full path on a
// drive letter ("C:\...\name.json"), or in the \\?\ form of one, whose name
// ends with ".json" (in any case) after at least one character; no part of it
// empty, "." or "..", no character that Windows takes for something else in a
// name (< > : " / | ? * or a control character, so no stream of a file). The
// file must not be there yet: the reader makes it new.
bool output_path_argument(std::wstring_view argument);
// The reader's command line for the device numbered `device_number` and the
// result file `output_path` (an output_path_argument), the path in double
// quotes.
std::wstring reader_parameters(unsigned device_number, std::wstring_view output_path);

// How the reader read the identification: through ATA pass-through, as a
// SATA controller takes it ("ata"), or inside ATA PASS-THROUGH (16), as a
// USB bridge takes it ("sat").
enum class ReaderHow { Ata, Sat };

// The reader's result: {"model","serial","wwn","firmware","how"} once it read
// the identification; {"error": "open-failed", "code": <the Win32 error>}
// where the device cannot be opened; {"error": "not-passed", "code": <the
// Win32 error of the last try, 0 where the command came back empty>} where
// neither way gave an identification.
std::string identity_result_json(const AtaIdentity& identity, ReaderHow how);
std::string error_result_json(std::string_view error, std::uint32_t code);

// What the program that started the reader makes of its result `text`, the
// read of `device_ref` at `read_at`: the identity; else an IoError whose kind
// says why (IDeviceIdentityHelper). NotFound where the device is no longer
// attached (Win32 errors 2, 3, 55, 433, 1167, 1617), whether it could not be
// opened ("open-failed") or went during the command ("not-passed"); Timeout
// for "not-passed" where the command was not answered in time (Win32 errors
// 121, 258, 995, 1460), as a sleeping device spinning up may be;
// CommandNotPassed for any other "not-passed" (also code 0: the bridge
// answered with a SCSI status, or nothing); Other where the device cannot be
// opened otherwise, for an error the reader does not write, and for a result
// that is no JSON object, is larger than largest_reader_result, or holds a
// text that is not as the reader writes it (printable ASCII; model up to 40
// characters, serial up to 20, firmware up to 8, WWN empty or 16 lowercase
// hex digits; a serial or a WWN; how "ata" or "sat"). Never PermissionDenied
// or Cancelled: those come from the operator's answer and the wait, not from
// the reader.
std::expected<DeviceIdentityRead, IoError> reader_result(
    std::string_view text, const std::string& device_ref, const Timestamp& read_at);

} // namespace va::platform::windows
