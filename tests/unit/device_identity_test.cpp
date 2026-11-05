// Reading a device's own identity behind its enclosure (decision 0047): the
// ATA IDENTIFY DEVICE answer and the SAT command of the device identity
// reader, its arguments and its result as the window's side reads it back,
// the device identification page by which Windows tells that an enclosure
// hides the device's identity (all plain C++, tested on every system), why a
// read failed, and the platform's reader where nothing may ask for
// administrator permission. Every model, serial, WWN, identifier and path
// here is sample data made up for the test.
#include "test_support.h"

#include "core/interfaces.h"
#include "platform/device_identity_helper.h"
#include "platform/windows/device_id_descriptor.h"
#include "platform/windows/device_identity_reader.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace va::platform::windows;
using va::IoErrorKind;

// The 512 bytes that IDENTIFY DEVICE returns: 256 words, each sent low byte
// first; in each text word the first character is the high byte.
class IdentifyData {
public:
    IdentifyData() : bytes_(identify_data_size) {}

    IdentifyData& word(std::size_t index, std::uint16_t value)
    {
        bytes_[2 * index] = static_cast<std::byte>(value & 0xff);
        bytes_[2 * index + 1] = static_cast<std::byte>(value >> 8);
        return *this;
    }
    // `text` in words [first, first + count), padded with `pad`.
    IdentifyData& text(std::size_t first, std::size_t count, std::string_view text, char pad = ' ')
    {
        std::string padded(text);
        padded.resize(2 * count, pad);
        for (std::size_t index = 0; index < count; ++index) {
            word(first + index, static_cast<std::uint16_t>(
                                    (static_cast<unsigned char>(padded[2 * index]) << 8) |
                                    static_cast<unsigned char>(padded[2 * index + 1])));
        }
        return *this;
    }
    IdentifyData& model(std::string_view model) { return text(27, 20, model); }
    IdentifyData& serial(std::string_view serial) { return text(10, 10, serial); }
    IdentifyData& firmware(std::string_view firmware) { return text(23, 4, firmware); }
    IdentifyData& wwn(std::uint16_t a, std::uint16_t b, std::uint16_t c, std::uint16_t d)
    {
        return word(108, a).word(109, b).word(110, c).word(111, d);
    }
    // Word 255: the signature 0xA5 and the checksum that makes the sum of
    // all 512 bytes zero.
    IdentifyData& integrity()
    {
        bytes_[510] = std::byte{0xa5};
        bytes_[511] = std::byte{0};
        unsigned sum = 0;
        for (const std::byte byte : bytes_) {
            sum += std::to_integer<unsigned>(byte);
        }
        bytes_[511] = static_cast<std::byte>((256 - (sum & 0xff)) & 0xff);
        return *this;
    }
    std::vector<std::byte>& bytes() { return bytes_; }

private:
    std::vector<std::byte> bytes_;
};

IdentifyData sample_identify()
{
    IdentifyData data;
    data.word(0, 0x0040)
        .model("SAMPLE MODEL 0001")
        .serial("      SAMPLE0001")
        .firmware("SAMP01")
        .wwn(0x5000, 0x0000, 0x0000, 0x0001);
    return data;
}

void identify_data_is_decoded()
{
    auto data = sample_identify();
    const auto identity = decode_identify_data(data.bytes());
    CHECK(identity && identity->model == "SAMPLE MODEL 0001" && identity->serial == "SAMPLE0001" &&
          identity->firmware == "SAMP01" && identity->wwn == "5000000000000001");

    // The first character of each text word is its high byte, which the
    // device sends second: the words 0x4153 and 0x504d read "ASPM".
    IdentifyData order;
    order.word(10, ('A' << 8) | 'S').word(11, ('P' << 8) | 'M');
    const auto ordered = decode_identify_data(order.bytes());
    CHECK(ordered && ordered->serial == "ASPM" && order.bytes()[20] == std::byte{'S'});

    // NULs pad like spaces; any other byte that is no printable ASCII is '?'.
    IdentifyData padded;
    padded.text(10, 10, "SAMPLE0002", '\0').text(27, 20, std::string("SAMPLE\x01MODEL", 12));
    const auto nul_padded = decode_identify_data(padded.bytes());
    CHECK(nul_padded && nul_padded->serial == "SAMPLE0002" && nul_padded->model == "SAMPLE?MODEL");

    // A WWN of all zeros or all ones is none; a serial alone is an identity.
    auto no_wwn = sample_identify();
    no_wwn.wwn(0, 0, 0, 0);
    CHECK(decode_identify_data(no_wwn.bytes())->wwn.empty());
    no_wwn.wwn(0xffff, 0xffff, 0xffff, 0xffff);
    CHECK(decode_identify_data(no_wwn.bytes())->wwn.empty());
    // A WWN alone is one too.
    auto wwn_only = sample_identify();
    wwn_only.serial("");
    const auto by_wwn = decode_identify_data(wwn_only.bytes());
    CHECK(by_wwn && by_wwn->serial.empty() && by_wwn->wwn == "5000000000000001");
    // Neither: no identification.
    wwn_only.wwn(0, 0, 0, 0);
    CHECK(!decode_identify_data(wwn_only.bytes()));

    // The integrity word, where the device gives it, must sum to zero.
    auto checked = sample_identify();
    checked.integrity();
    CHECK(decode_identify_data(checked.bytes()));
    checked.bytes()[54] ^= std::byte{0x01};
    CHECK(!decode_identify_data(checked.bytes()));

    // A bridge that passed the command on to nothing: all zeros. Data of
    // another size is no identification.
    CHECK(!decode_identify_data(IdentifyData().bytes()));
    auto data_short = sample_identify().bytes();
    data_short.pop_back();
    CHECK(!decode_identify_data(data_short));
    CHECK(!decode_identify_data({}));
}

void identify_command_is_built()
{
    const std::array<std::uint8_t, 16> expected{0x85, 0x08, 0x0e, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0xec, 0};
    CHECK(identify_device_cdb() == expected);
    CHECK(identify_device_command == 0xec);
}

void device_numbers_are_strict()
{
    CHECK(physical_drive_number("\\\\.\\PhysicalDrive0") == 0u);
    CHECK(physical_drive_number("\\\\.\\PhysicalDrive7") == 7u);
    CHECK(physical_drive_number("\\\\.\\PhysicalDrive63") == 63u);
    for (const std::string_view refused : {"\\\\.\\PhysicalDrive64", "\\\\.\\PhysicalDrive07", "\\\\.\\PhysicalDrive",
             "\\\\.\\PhysicalDrive1a", "\\\\.\\PhysicalDrive-1", "\\\\.\\PhysicalDrive 1", "\\\\.\\PhysicalDrive100",
             "\\\\?\\PhysicalDrive1", "\\\\.\\physicaldrive1", "/dev/sda", "", "1"}) {
        CHECK(!physical_drive_number(refused));
    }
    CHECK(device_number_argument(L"0") == 0u);
    CHECK(device_number_argument(L"42") == 42u);
    for (const std::wstring_view refused : {L"64", L"063", L"+1", L"-1", L" 1", L"1 ", L"", L"1.0", L"0x1"}) {
        CHECK(!device_number_argument(refused));
    }
}

void result_paths_are_strict()
{
    for (const std::wstring_view accepted : {L"C:\\SAMPLE\\result.json", L"c:\\SAMPLE\\RESULT.JSON",
             L"\\\\?\\C:\\Users\\SAMPLE\\AppData\\Local\\Temp\\VeriArchive-device-identity-SAMPLE\\device-identity.json",
             L"D:\\範例\\結果.json", L"C:\\x.json"}) {
        CHECK(output_path_argument(accepted));
    }
    for (const std::wstring_view refused : {L"result.json", L"SAMPLE\\result.json", L"C:result.json",
             L"C:\\SAMPLE\\result.txt", L"C:\\SAMPLE\\.json", L"C:\\SAMPLE\\result.json\\", L"C:\\SAMPLE\\result:x.json",
             L"C:\\SAMPLE\\result.json:x", L"C:\\SAMPLE\\..\\result.json", L"C:\\SAMPLE\\.\\result.json",
             L"C:\\SAMPLE\\\\result.json", L"C:/SAMPLE/result.json", L"C:\\SAMPLE/result.json",
             L"\\\\server\\share\\result.json", L"\\\\.\\PhysicalDrive0", L"\\\\?\\UNC\\server\\share\\result.json",
             L"\\\\?\\Volume{00000000-0000-0000-0000-000000000000}\\result.json", L"C:\\SAMPLE\\re*sult.json",
             L"C:\\SAMPLE\\re?sult.json", L"C:\\SAMPLE\\\"result.json", L"C:\\SAMPLE\\re\x01sult.json", L"",
             L"1:\\result.json"}) {
        CHECK(!output_path_argument(refused));
    }
    CHECK(reader_parameters(3, L"C:\\SAMPLE\\result.json") == L"3 \"C:\\SAMPLE\\result.json\"");
}

const std::string sample_ref = "\\\\.\\PhysicalDrive3";
const std::string sample_time = "2026-11-05T08:00:00Z";

void results_are_read_back()
{
    AtaIdentity identity{"SAMPLE MODEL 0001", "SAMPLE0001", "SAMP01", "5000000000000001"};
    const auto read = reader_result(identity_result_json(identity, ReaderHow::Sat), sample_ref, sample_time);
    CHECK(read && read->device.model == "SAMPLE MODEL 0001" && read->device.serial == "SAMPLE0001" &&
          read->device.wwn == "5000000000000001" && read->how == "sat" && read->read_at == sample_time &&
          read->device_ref == sample_ref);
    const auto by_ata = reader_result(identity_result_json(identity, ReaderHow::Ata), sample_ref, sample_time);
    CHECK(by_ata && by_ata->how == "ata");

    // Not passed on: CommandNotPassed, which the window tells apart; also
    // where the bridge answered with a SCSI status (code 0).
    const auto not_passed = reader_result(error_result_json("not-passed", 1), sample_ref, sample_time);
    CHECK(!not_passed && not_passed.error().kind == IoErrorKind::CommandNotPassed &&
          va::device_identity_failure(not_passed.error()) == va::DeviceIdentityFailure::NotPassed);
    const auto refused_status = reader_result(error_result_json("not-passed", 0), sample_ref, sample_time);
    CHECK(!refused_status && refused_status.error().kind == IoErrorKind::CommandNotPassed);
    // The command failed as the device went (ERROR_DEVICE_NOT_CONNECTED,
    // ERROR_NO_SUCH_DEVICE, ERROR_DEVICE_REMOVED): NotFound, not the
    // enclosure's fault.
    for (const std::uint32_t code : {1167u, 433u, 1617u, 55u}) {
        const auto went = reader_result(error_result_json("not-passed", code), sample_ref, sample_time);
        CHECK(!went && went.error().kind == IoErrorKind::NotFound &&
              went.error().native_code == static_cast<int>(code));
    }
    // No answer in time (ERROR_SEM_TIMEOUT, WAIT_TIMEOUT, ERROR_OPERATION_ABORTED,
    // ERROR_TIMEOUT), as a sleeping device spinning up behind its bridge:
    // Timeout, a read that failed.
    for (const std::uint32_t code : {121u, 258u, 995u, 1460u}) {
        const auto late = reader_result(error_result_json("not-passed", code), sample_ref, sample_time);
        CHECK(!late && late.error().kind == IoErrorKind::Timeout &&
              va::device_identity_failure(late.error()) == va::DeviceIdentityFailure::Unreadable);
    }
    // Not opened: NotFound where the device is gone, Other otherwise (never
    // PermissionDenied, which is the operator's answer alone).
    const auto gone = reader_result(error_result_json("open-failed", 2), sample_ref, sample_time);
    CHECK(!gone && gone.error().kind == IoErrorKind::NotFound && gone.error().native_code == 2);
    const auto denied = reader_result(error_result_json("open-failed", 5), sample_ref, sample_time);
    CHECK(!denied && denied.error().kind == IoErrorKind::Other &&
          va::device_identity_failure(denied.error()) == va::DeviceIdentityFailure::Unreadable);
    const auto unknown = reader_result(error_result_json("SAMPLE-error", 0), sample_ref, sample_time);
    CHECK(!unknown && unknown.error().kind == IoErrorKind::Other);

    // Anything the reader does not write is refused.
    const auto refused = [](std::string_view text) {
        const auto result = reader_result(text, sample_ref, sample_time);
        return !result && result.error().kind == IoErrorKind::Other;
    };
    CHECK(refused(""));
    CHECK(refused("not json"));
    CHECK(refused("[]"));
    CHECK(refused(R"({"error":1})"));
    CHECK(refused(R"({"model":"SAMPLE","serial":"SAMPLE0001","wwn":"","firmware":"S1","how":"magic"})"));
    CHECK(refused(R"({"model":"SAMPLE","serial":"SAMPLE0001","wwn":"","how":"sat"})"));
    CHECK(refused(R"({"model":"SAMPLE","serial":"","wwn":"","firmware":"S1","how":"sat"})"));
    CHECK(refused(R"({"model":"SAMPLE","serial":"SAMPLE0001","wwn":"5000000000000ABC","firmware":"S1","how":"sat"})"));
    CHECK(refused(R"({"model":"SAMPLE","serial":"SAMPLE0001","wwn":"50000000","firmware":"S1","how":"sat"})"));
    CHECK(refused(R"({"model":"SAMPLE","serial":"SAMPLE\u0001","wwn":"","firmware":"S1","how":"sat"})"));
    CHECK(refused(R"({"model":"SAMPLE","serial":"SAMPLE-SERIAL-LONGER-THAN-20","wwn":"","firmware":"S1","how":"sat"})"));
    CHECK(refused(R"({"model":"SAMPLE","serial":"SAMPLE0001","wwn":"","firmware":"SAMPLE-FW-9","how":"sat"})"));
    CHECK(refused(R"({"model":"SAMPLE","serial":7,"wwn":"","firmware":"S1","how":"sat"})"));
    CHECK(refused(std::string(largest_reader_result + 1, ' ')));
    // A result within the limit is read, whatever space pads it.
    CHECK(reader_result(identity_result_json(identity, ReaderHow::Sat) + std::string(100, ' '), sample_ref,
        sample_time));
}

void failures_are_told_apart()
{
    using va::DeviceIdentityFailure;
    using va::IoError;
    CHECK(va::device_identity_failure(IoError{IoErrorKind::NotSupported, 0, ""}) == DeviceIdentityFailure::NotAvailable);
    CHECK(va::device_identity_failure(IoError{IoErrorKind::PermissionDenied, 1223, ""}) == DeviceIdentityFailure::Declined);
    CHECK(va::device_identity_failure(IoError{IoErrorKind::CommandNotPassed, 0, ""}) == DeviceIdentityFailure::NotPassed);
    CHECK(va::device_identity_failure(IoError{IoErrorKind::Cancelled, 0, ""}) == DeviceIdentityFailure::Cancelled);
    for (const auto kind : {IoErrorKind::NotFound, IoErrorKind::DeviceRemoved, IoErrorKind::MediaError,
             IoErrorKind::Timeout, IoErrorKind::Other}) {
        CHECK(va::device_identity_failure(IoError{kind, 0, ""}) == DeviceIdentityFailure::Unreadable);
    }

    // Where no reader exists, none is offered, nothing is read and nothing
    // asks.
    va::NullDeviceIdentityHelper none;
    CHECK(!none.available());
    va::CancelToken cancel;
    const auto nothing = none.read_device_identity(sample_ref, cancel);
    CHECK(!nothing && va::device_identity_failure(nothing.error()) == DeviceIdentityFailure::NotAvailable);

    // The platform's reader, asked so that it starts nothing: cancelled
    // before it asks, or for a device that has no number Windows names it
    // by. On Linux there is none.
    const auto platform = va::platform::make_device_identity_helper();
    cancel.cancel();
    const auto cancelled = platform->read_device_identity(sample_ref, cancel);
    cancel.reset();
    const auto no_device = platform->read_device_identity("/dev/SAMPLE", cancel);
#ifdef _WIN32
    CHECK(!cancelled && cancelled.error().kind == IoErrorKind::Cancelled);
    CHECK(!no_device && va::device_identity_failure(no_device.error()) == DeviceIdentityFailure::Unreadable);
#else
    CHECK(!platform->available());
    CHECK(!cancelled && cancelled.error().kind == IoErrorKind::NotSupported);
    CHECK(!no_device && no_device.error().kind == IoErrorKind::NotSupported);
#endif
}

// STORAGE_DEVICE_ID_DESCRIPTOR as the IOCTL writes it: version, size, the
// number of identifiers, then each STORAGE_IDENTIFIER (code set, type, size,
// next offset, association, its bytes), little-endian.
struct Identifier {
    std::uint32_t code_set = 1;  // binary
    std::uint32_t type = 3;      // NAA
    std::uint32_t association = 0;
    std::vector<std::uint8_t> bytes;
};

template <typename Value>
void put(std::vector<std::byte>& answer, Value value)
{
    const auto at = answer.size();
    answer.resize(at + sizeof(Value));
    std::memcpy(answer.data() + at, &value, sizeof(Value));
}

std::vector<std::byte> descriptor(const std::vector<Identifier>& identifiers, std::optional<std::uint32_t> count = {})
{
    std::vector<std::byte> answer;
    put<std::uint32_t>(answer, 1);
    put<std::uint32_t>(answer, 0);
    put<std::uint32_t>(answer, count.value_or(static_cast<std::uint32_t>(identifiers.size())));
    for (const auto& identifier : identifiers) {
        const auto size = static_cast<std::uint16_t>(identifier.bytes.size());
        const auto next = static_cast<std::uint16_t>((16 + size + 3) / 4 * 4);
        put(answer, identifier.code_set);
        put(answer, identifier.type);
        put(answer, size);
        put(answer, next);
        put(answer, identifier.association);
        for (const auto byte : identifier.bytes) {
            answer.push_back(static_cast<std::byte>(byte));
        }
        answer.resize(answer.size() + (next - 16 - size));
    }
    const auto size = static_cast<std::uint32_t>(answer.size());
    std::memcpy(answer.data() + 4, &size, sizeof(size));
    return answer;
}

// Sample identifiers: an NAA of each kind, an EUI-64, and the vendor's
// identifier a USB bridge gives (vendor, product, serial).
const std::vector<std::uint8_t> local_naa{0x30, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01};
const std::vector<std::uint8_t> registered_naa{0x50, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01};
const std::vector<std::uint8_t> extended_naa{0x60, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02};
const std::vector<std::uint8_t> eui64{0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03};
const std::vector<std::uint8_t> vendor{'S', 'A', 'M', 'P', 'L', 'E', ' ', ' ', 'B', 'R', 'I', 'D', 'G', 'E'};

void identification_pages_are_read()
{
    // What a USB bridge that hides the device gives: an NAA it made up.
    const auto made_up = descriptor({{1, 3, 0, local_naa}, {2, 1, 0, vendor}});
    CHECK(!names_device_worldwide(made_up));
    CHECK(enclosure_hides_identity("usb", false, std::span<const std::byte>(made_up)));
    // A vendor's identifier alone, or nothing at all.
    const auto vendor_only = descriptor({{2, 1, 0, vendor}});
    CHECK(!names_device_worldwide(vendor_only));
    const auto empty = descriptor({});
    CHECK(!names_device_worldwide(empty));
    CHECK(enclosure_hides_identity("usb", false, std::span<const std::byte>(empty)));
    // The system gave no page: nothing names the device.
    CHECK(enclosure_hides_identity("usb", false, std::nullopt));

    // Removable media (a USB flash memory, a card reader) have no enclosure:
    // their USB serial is their own, whatever their page or none.
    CHECK(!enclosure_hides_identity("usb", true, std::nullopt));
    CHECK(!enclosure_hides_identity("usb", true, std::span<const std::byte>(made_up)));
    CHECK(!enclosure_hides_identity("usb", true, std::span<const std::byte>(vendor_only)));

    // A bridge that passes the device's own WWN on: registered or extended
    // NAA, or an EUI-64, of the logical unit; the vendor's identifier first.
    for (const auto& bytes : {registered_naa, extended_naa}) {
        const auto passed = descriptor({{2, 1, 0, vendor}, {1, 3, 0, bytes}});
        CHECK(names_device_worldwide(passed));
        CHECK(!enclosure_hides_identity("usb", false, std::span<const std::byte>(passed)));
    }
    const auto by_eui = descriptor({{1, 2, 0, eui64}});
    CHECK(names_device_worldwide(by_eui));
    // A worldwide NAA of the bridge's port names the port, not the device.
    CHECK(!names_device_worldwide(descriptor({{1, 3, 1, registered_naa}})));
    // All zeros names nothing.
    CHECK(!names_device_worldwide(descriptor({{1, 3, 0, {0x50, 0, 0, 0, 0, 0, 0, 0}}})));
    CHECK(!names_device_worldwide(descriptor({{1, 2, 0, std::vector<std::uint8_t>(8, 0)}})));
    // An NAA too short for one, an EUI-64 of a size it never has.
    CHECK(!names_device_worldwide(descriptor({{1, 3, 0, {0x50, 0, 0, 1}}})));
    CHECK(!names_device_worldwide(descriptor({{1, 2, 0, {0, 0, 0, 0, 0, 0, 1}}})));

    // Only identifiers that fit in the answer are read.
    auto cut = descriptor({{1, 3, 0, registered_naa}});
    cut.resize(12 + 16 + 4);
    CHECK(!names_device_worldwide(cut));
    // More identifiers claimed than there are: those that are there are read.
    CHECK(names_device_worldwide(descriptor({{1, 3, 0, registered_naa}}, 1000)));
    CHECK(!names_device_worldwide(descriptor({{1, 3, 0, local_naa}}, 1000)));
    CHECK(!names_device_worldwide({}));
    CHECK(!names_device_worldwide(std::vector<std::byte>(11)));

    // Another bus never hides the identity, whatever the page.
    for (const std::string_view bus : {"sata", "nvme", "scsi", ""}) {
        CHECK(!enclosure_hides_identity(bus, false, std::span<const std::byte>(made_up)));
        CHECK(!enclosure_hides_identity(bus, false, std::nullopt));
    }
}

} // namespace

#ifdef _WIN32
void device_identity_windows_tests(); // device_identity_win_test.cpp
#endif

void device_identity_tests()
{
    identify_data_is_decoded();
    identify_command_is_built();
    device_numbers_are_strict();
    result_paths_are_strict();
    results_are_read_back();
    failures_are_told_apart();
    identification_pages_are_read();
#ifdef _WIN32
    device_identity_windows_tests();
#endif
}
