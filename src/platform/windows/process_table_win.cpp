// Windows process table (decision 0024). A process is found by its id with
// OpenProcess and told from a later process given the same id by its creation
// time (GetProcessTimes), which Windows fixes when the process starts. Only
// PROCESS_QUERY_LIMITED_INFORMATION and SYNCHRONIZE are asked for, which need
// no privilege (R-08); a process whose owner refuses even the first cannot be
// told (an error).
#include "platform/process_table.h"
#include "platform/windows/win_util.h"

#include "common/utf8.h"

#include <string>
#include <vector>

namespace va::platform {
namespace {

using windows::UniqueHandle;
using windows::error_from_win32;

// The file name of the process's program; empty when Windows does not say.
std::string program_of(HANDLE process)
{
    std::vector<wchar_t> buffer(32768);
    DWORD length = static_cast<DWORD>(buffer.size());
    if (!QueryFullProcessImageNameW(process, 0, buffer.data(), &length)) {
        return {};
    }
    const std::filesystem::path program(std::wstring(buffer.data(), length));
    return path_to_utf8(program.filename());
}

std::expected<ProcessIdentity, IoError> identify(HANDLE process, DWORD id)
{
    FILETIME created{};
    FILETIME exited{};
    FILETIME kernel{};
    FILETIME user{};
    if (!GetProcessTimes(process, &created, &exited, &kernel, &user)) {
        return std::unexpected(error_from_win32(GetLastError(), "GetProcessTimes"));
    }
    // 100-nanosecond intervals since 1601-01-01 UTC, as one number.
    const auto ticks = (static_cast<std::uint64_t>(created.dwHighDateTime) << 32) | created.dwLowDateTime;
    return ProcessIdentity{id, std::to_string(ticks), program_of(process)};
}

class WindowsProcessTable final : public IProcessTable {
public:
    std::expected<ProcessIdentity, IoError> current() override
    {
        return identify(GetCurrentProcess(), GetCurrentProcessId());
    }

    std::expected<std::optional<ProcessIdentity>, IoError> find(std::uint64_t id) override
    {
        // 0 is the idle process, and Windows process ids fit in 32 bits.
        if (id == 0 || id > 0xFFFFFFFFu) {
            return std::optional<ProcessIdentity>{};
        }
        const auto process_id = static_cast<DWORD>(id);
        bool can_wait = true;
        UniqueHandle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, process_id));
        if (!process.valid() && GetLastError() == ERROR_ACCESS_DENIED) {
            // A process whose owner allows less: it is asked whether it runs instead.
            process.reset(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, process_id));
            can_wait = false;
        }
        if (!process.valid()) {
            const DWORD code = GetLastError();
            if (code == ERROR_INVALID_PARAMETER) {
                return std::optional<ProcessIdentity>{}; // no process has this id
            }
            return std::unexpected(error_from_win32(code, "OpenProcess"));
        }
        // A process that has ended keeps its object, and its id, while a
        // handle to it is open (its parent's, say): the object is signalled.
        if (can_wait) {
            const DWORD waited = WaitForSingleObject(process.get(), 0);
            if (waited == WAIT_OBJECT_0) {
                return std::optional<ProcessIdentity>{};
            }
            if (waited != WAIT_TIMEOUT) {
                return std::unexpected(error_from_win32(GetLastError(), "WaitForSingleObject"));
            }
        } else {
            DWORD exit_code = 0;
            if (!GetExitCodeProcess(process.get(), &exit_code)) {
                return std::unexpected(error_from_win32(GetLastError(), "GetExitCodeProcess"));
            }
            if (exit_code != STILL_ACTIVE) {
                return std::optional<ProcessIdentity>{};
            }
        }
        auto identity = identify(process.get(), process_id);
        if (!identity) {
            return std::unexpected(identity.error());
        }
        return std::optional<ProcessIdentity>(std::move(*identity));
    }
};

} // namespace

std::unique_ptr<IProcessTable> make_process_table()
{
    return std::make_unique<WindowsProcessTable>();
}

} // namespace va::platform
