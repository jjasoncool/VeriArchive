// The window's side of the device identity reader on Windows (decisions
// 0047 and 0052), everything but Windows' question for administrator permission: the
// reader of this build is started as this test runs, without that
// permission (__COMPAT_LAYER=RunAsInvoker, so that Windows starts it at all),
// for device 63 (\\.\PhysicalDrive63), which no computer here has, so that no
// device is opened; the device as this program describes it is a sample
// description. It tells whether a read can be asked for, what it read back,
// waits for the reader and can be cancelled, refuses a read whose device
// changed while the operator answered, and leaves no folder behind, also
// none that an earlier read left. Every path, folder, file and device here
// is sample data.
#include "test_support.h"

#include "common/utf8.h"
#include "platform/windows/device_identity_reader.h"
#include "platform/windows/device_identity_start.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#ifndef VERIARCHIVE_TEST_DEVICE_IDENTITY_READER
#error "VERIARCHIVE_TEST_DEVICE_IDENTITY_READER must name the reader of this build"
#endif

namespace {

using va::IoError;
using va::IoErrorKind;
using va::PhysicalDeviceInfo;
using va::platform::windows::DescribeDevice;
using va::platform::windows::ReaderStart;
using va::platform::windows::UniqueHandle;

const std::string sample_device = "\\\\.\\PhysicalDrive63";

std::filesystem::path reader_of_this_build()
{
    return va::path_from_utf8(VERIARCHIVE_TEST_DEVICE_IDENTITY_READER);
}

// A made-up description of device 63, as the media enumerator gives one.
PhysicalDeviceInfo sample_description(std::string serial = "SAMPLE-ENCLOSURE-0001")
{
    PhysicalDeviceInfo device;
    device.device_ref = sample_device;
    device.model = "SAMPLE ENCLOSURE";
    device.serial = std::move(serial);
    device.bus = "usb";
    device.capacity_bytes = 1'000'000'000;
    return device;
}

// Device 63 described alike every time.
std::optional<PhysicalDeviceInfo> described_alike(unsigned)
{
    return sample_description();
}

// The folders that reads made under the temporary folder and left there.
std::set<std::filesystem::path> result_folders()
{
    std::set<std::filesystem::path> folders;
    std::error_code error;
    const auto temporary = std::filesystem::temp_directory_path(error);
    for (std::filesystem::directory_iterator entry(temporary, error), end; !error && entry != end;
         entry.increment(error)) {
        if (entry->path().filename().native().starts_with(L"VeriArchive-device-identity-")) {
            folders.insert(entry->path());
        }
    }
    return folders;
}

// Starts `reader` as this test runs, as its user, never asking anyone.
std::expected<UniqueHandle, IoError> start_as_this_user(
    const std::filesystem::path& reader, const std::wstring& parameters)
{
    std::wstring command = L"\"" + reader.native() + L"\" " + parameters;
    SetEnvironmentVariableW(L"__COMPAT_LAYER", L"RunAsInvoker");
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION started{};
    const bool ok = CreateProcessW(reader.c_str(), command.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr,
        &startup, &started);
    const DWORD code = GetLastError();
    SetEnvironmentVariableW(L"__COMPAT_LAYER", nullptr);
    if (!ok) {
        return std::unexpected(IoError{IoErrorKind::Other, static_cast<int>(code), "CreateProcessW"});
    }
    CloseHandle(started.hThread);
    return UniqueHandle(started.hProcess);
}

// A sample folder as a read whose reader could not be ended leaves it, with
// a sample result in it.
std::filesystem::path left_folder(const wchar_t* name)
{
    const auto folder = std::filesystem::temp_directory_path() / name;
    std::filesystem::create_directories(folder);
    std::ofstream(folder / L"device-identity.json") << R"({"error":"SAMPLE-left-result","code":0})";
    return folder;
}

void reads_through_the_reader()
{
    const auto before = result_folders();
    va::CancelToken cancel;

    // A read can be asked for where the reader is there, not where it is
    // missing (as when an antivirus program removed it) or is a folder.
    CHECK(va::platform::windows::make_device_identity_helper(reader_of_this_build(), start_as_this_user,
        described_alike)
              ->available());
    CHECK(!va::platform::windows::make_device_identity_helper(
        reader_of_this_build().parent_path() / L"SAMPLE-missing-reader.exe", start_as_this_user, described_alike)
               ->available());
    CHECK(!va::platform::windows::make_device_identity_helper(
        reader_of_this_build().parent_path(), start_as_this_user, described_alike)
               ->available());

    // A folder that an earlier read left, with its result: the next read
    // removes it. One that a read still uses (held open, as each read holds
    // its own) is kept, with its result.
    const auto left = left_folder(L"VeriArchive-device-identity-SAMPLE-left");
    const auto busy = left_folder(L"VeriArchive-device-identity-SAMPLE-busy");
    UniqueHandle busy_held(CreateFileW(busy.c_str(), FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr));
    CHECK(busy_held.valid());

    // Device 63 is not there: the reader writes open-failed, which comes back
    // as NotFound, the read of a device that is gone. The reader was given
    // the device's number and a result file it takes, nothing else.
    std::wstring given;
    const auto helper = va::platform::windows::make_device_identity_helper(reader_of_this_build(),
        [&given](const std::filesystem::path& reader, const std::wstring& parameters) {
            given = parameters;
            return start_as_this_user(reader, parameters);
        },
        described_alike);
    const auto gone = helper->read_device_identity(sample_device, cancel);
    CHECK(!gone && gone.error().kind == IoErrorKind::NotFound && gone.error().native_code == 2);
    CHECK(given.starts_with(L"\"\\\\?\\") && given.ends_with(L"\\device-identity.json\" 63"));
    CHECK(given.size() > 5 && va::platform::windows::output_path_argument(given.substr(1, given.size() - 5)));

    // Decision 0052: one run for several devices, each given by its number
    // after the result file, each read back in that order; a device the
    // reader cannot open gives no SMART read either (it is gone).
    const auto both = helper->read_devices({sample_device, "\\\\.\\PhysicalDrive62"}, cancel);
    CHECK(given.ends_with(L"\\device-identity.json\" 63 62"));
    CHECK(both && both->size() == 2 && (*both)[0].device_ref == sample_device &&
          (*both)[1].device_ref == "\\\\.\\PhysicalDrive62");
    for (const auto& read : both ? *both : std::vector<va::DeviceRead>{}) {
        CHECK(!read.identity && read.identity.error().kind == IoErrorKind::NotFound && !read.smart);
    }
    // A name that is no device of this computer is refused alone: the others
    // are read.
    const auto mixed = helper->read_devices({"SAMPLE-not-a-device", sample_device}, cancel);
    CHECK(mixed && mixed->size() == 2 && !(*mixed)[0].identity &&
          (*mixed)[0].identity.error().kind == IoErrorKind::Other && given.ends_with(L"\" 63"));

    std::error_code error;
    CHECK(!std::filesystem::exists(left, error));
    CHECK(std::filesystem::exists(busy / L"device-identity.json", error));
    busy_held.reset();
    std::filesystem::remove_all(busy, error);

    // A reader that refused its arguments wrote nothing: the exit says so.
    const auto refused = va::platform::windows::make_device_identity_helper(reader_of_this_build(),
        [](const std::filesystem::path& reader, const std::wstring& parameters) {
            return start_as_this_user(reader, parameters.substr(0, parameters.size() - 2) + L"64");
        },
        described_alike);
    const auto nothing = refused->read_device_identity(sample_device, cancel);
    CHECK(!nothing && nothing.error().kind == IoErrorKind::Other &&
          nothing.error().message.find("exit 2") != std::string::npos);

    // Declined, as Windows' start says: PermissionDenied, passed on.
    const auto declined = va::platform::windows::make_device_identity_helper(reader_of_this_build(),
        [](const std::filesystem::path&, const std::wstring&) -> std::expected<UniqueHandle, IoError> {
            return std::unexpected(IoError{IoErrorKind::PermissionDenied, 1223, "declined (sample)"});
        },
        described_alike);
    const auto no = declined->read_device_identity(sample_device, cancel);
    CHECK(!no && va::device_identity_failure(no.error()) == va::DeviceIdentityFailure::Declined);

    // A reader that does not end: the wait stops once it is cancelled. (An
    // event that is never set stands for the reader's process.)
    const auto waiting = va::platform::windows::make_device_identity_helper(reader_of_this_build(),
        [](const std::filesystem::path&, const std::wstring&) -> std::expected<UniqueHandle, IoError> {
            return UniqueHandle(CreateEventW(nullptr, TRUE, FALSE, nullptr));
        },
        described_alike);
    std::thread stopper([&cancel] {
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        cancel.cancel();
    });
    const auto started = std::chrono::steady_clock::now();
    const auto cancelled = waiting->read_device_identity(sample_device, cancel);
    stopper.join();
    CHECK(!cancelled && cancelled.error().kind == IoErrorKind::Cancelled);
    CHECK(std::chrono::steady_clock::now() - started < std::chrono::seconds(10));
    cancel.reset();

    // A reader that is not there is not started.
    bool asked = false;
    const auto missing = va::platform::windows::make_device_identity_helper(
        reader_of_this_build().parent_path() / L"SAMPLE-missing-reader.exe",
        [&asked](const std::filesystem::path& reader, const std::wstring& parameters) {
            asked = true;
            return start_as_this_user(reader, parameters);
        },
        described_alike);
    const auto absent = missing->read_device_identity(sample_device, cancel);
    CHECK(!absent && absent.error().kind == IoErrorKind::NotFound && !asked);

    // A device that is gone before Windows would ask: nothing is started.
    asked = false;
    const auto unplugged = va::platform::windows::make_device_identity_helper(reader_of_this_build(),
        [&asked](const std::filesystem::path& reader, const std::wstring& parameters) {
            asked = true;
            return start_as_this_user(reader, parameters);
        },
        [](unsigned) -> std::optional<PhysicalDeviceInfo> { return std::nullopt; });
    const auto not_attached = unplugged->read_device_identity(sample_device, cancel);
    CHECK(!not_attached && not_attached.error().kind == IoErrorKind::NotFound && !asked);

    // Another device took the number while the operator answered (described
    // otherwise once the reader has ended), or the device went: whatever the
    // reader wrote is not taken for the device's.
    for (const bool went : {false, true}) {
        int descriptions = 0;
        const auto swapped = va::platform::windows::make_device_identity_helper(reader_of_this_build(),
            start_as_this_user,
            [&descriptions, went](unsigned) -> std::optional<PhysicalDeviceInfo> {
                if (++descriptions == 1) {
                    return sample_description();
                }
                if (went) {
                    return std::nullopt;
                }
                return sample_description("SAMPLE-ENCLOSURE-0002");
            });
        const auto other = swapped->read_device_identity(sample_device, cancel);
        CHECK(!other && other.error().kind == IoErrorKind::NotFound && descriptions == 2);
        CHECK(!other && other.error().message.find(went ? "no longer attached" : "changed") != std::string::npos);
    }

    // Every read removed its folder, and the earlier reads' folders went too.
    const auto after = result_folders();
    CHECK(std::ranges::includes(before, after));
    CHECK(!after.contains(left) && !after.contains(busy));
}

} // namespace

void device_identity_windows_tests()
{
    reads_through_the_reader();
}
