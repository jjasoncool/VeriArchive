// Linux process table (decision 0024), read from /proc. A process is told
// from a later process given the same id by its start: the clock ticks since
// boot at which it started (/proc/<pid>/stat, field 22), with the boot's id
// (/proc/sys/kernel/random/boot_id), as the ticks start again at each boot.
// Every user may read both.
#include "platform/process_table.h"
#include "platform/linux/posix_util.h"

#include <cerrno>
#include <climits>
#include <cstddef>
#include <fcntl.h>
#include <sstream>
#include <string>
#include <unistd.h>

namespace va::platform {
namespace {

using posix::UniqueFd;
using posix::error_from_errno;

// The whole of a small file under /proc; nothing when it does not exist (the
// process has gone).
std::expected<std::optional<std::string>, IoError> read_proc(const std::string& file)
{
    UniqueFd descriptor(::open(file.c_str(), O_RDONLY | O_CLOEXEC));
    if (!descriptor.valid()) {
        const int code = errno;
        if (code == ENOENT || code == ESRCH) {
            return std::optional<std::string>{};
        }
        return std::unexpected(error_from_errno(code, "open " + file));
    }
    std::string text;
    char buffer[4096];
    for (;;) {
        const auto count = ::read(descriptor.get(), buffer, sizeof(buffer));
        if (count < 0) {
            const int code = errno;
            if (code == EINTR) {
                continue;
            }
            // A process that ends while its file is read.
            if (code == ESRCH) {
                return std::optional<std::string>{};
            }
            return std::unexpected(error_from_errno(code, "read " + file));
        }
        if (count == 0) {
            return std::optional<std::string>(std::move(text));
        }
        text.append(buffer, static_cast<std::size_t>(count));
    }
}

IoError unreadable(const std::string& file)
{
    return {IoErrorKind::Other, 0, "unexpected contents of " + file};
}

std::expected<std::optional<ProcessIdentity>, IoError> identify(std::uint64_t id)
{
    const auto file = "/proc/" + std::to_string(id) + "/stat";
    const auto stat = read_proc(file);
    if (!stat) {
        return std::unexpected(stat.error());
    }
    if (!*stat) {
        return std::optional<ProcessIdentity>{};
    }
    // "<pid> (<program>) <state> <ppid> ...": the program's name may hold
    // spaces and parentheses, so the fields after it start at the last ')'.
    const auto& text = **stat;
    const auto open = text.find('(');
    const auto close = text.rfind(')');
    if (open == std::string::npos || close == std::string::npos || close < open) {
        return std::unexpected(unreadable(file));
    }
    std::istringstream fields(text.substr(close + 1));
    std::string state;
    fields >> state; // field 3
    std::string skipped;
    for (int field = 4; field < 22; ++field) {
        fields >> skipped;
    }
    std::string started; // field 22: clock ticks since boot
    fields >> started;
    if (!fields || state.empty() || started.empty()) {
        return std::unexpected(unreadable(file));
    }
    // A process that has ended but whose parent has not yet collected it
    // (a zombie), or one being torn down.
    if (state == "Z" || state == "X" || state == "x") {
        return std::optional<ProcessIdentity>{};
    }
    const std::string boot_file = "/proc/sys/kernel/random/boot_id";
    const auto boot = read_proc(boot_file);
    if (!boot) {
        return std::unexpected(boot.error());
    }
    std::string boot_id = boot->value_or("");
    while (!boot_id.empty() && (boot_id.back() == '\n' || boot_id.back() == ' ')) {
        boot_id.pop_back();
    }
    if (boot_id.empty()) {
        return std::unexpected(unreadable(boot_file));
    }
    return std::optional<ProcessIdentity>(
        ProcessIdentity{id, boot_id + ":" + started, text.substr(open + 1, close - open - 1)});
}

class LinuxProcessTable final : public IProcessTable {
public:
    std::expected<ProcessIdentity, IoError> current() override
    {
        const auto self = identify(static_cast<std::uint64_t>(::getpid()));
        if (!self) {
            return std::unexpected(self.error());
        }
        if (!*self) {
            return std::unexpected(IoError{IoErrorKind::NotFound, 0, "this process is not in /proc"});
        }
        return **self;
    }

    std::expected<std::optional<ProcessIdentity>, IoError> find(std::uint64_t id) override
    {
        // Process ids are positive and fit in a pid_t.
        if (id == 0 || id > static_cast<std::uint64_t>(INT_MAX)) {
            return std::optional<ProcessIdentity>{};
        }
        return identify(id);
    }
};

} // namespace

std::unique_ptr<IProcessTable> make_process_table()
{
    return std::make_unique<LinuxProcessTable>();
}

} // namespace va::platform
