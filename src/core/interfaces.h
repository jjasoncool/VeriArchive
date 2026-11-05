#pragma once
#include "common/archive_path.h"
#include "common/error.h"
#include "common/types.h"
#include <atomic>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace va {

struct ReadOptions {
    bool uncached = true;
    bool sequential = true;
};

class IByteSource {
public:
    virtual ~IByteSource() = default;

    virtual std::expected<std::size_t, IoError> read(
        std::uint64_t offset,
        std::span<std::byte> output) = 0;
    virtual std::uint64_t size() const = 0;
    virtual std::size_t alignment() const = 0;
};

// Special: FIFO, socket or device node (Linux). Never opened; recorded as an
// unreadable entry (decision 0014).
enum class EntryType { File, Directory, Link, Stream, Special };

struct DirEntry {
    std::filesystem::path native_name;
    EntryType type;
    std::uint64_t size;
    std::int64_t mtime_ns;
    std::optional<std::uint32_t> mode;
    std::optional<std::uint32_t> attrs;
    bool is_mount_point;
};

class IFileSystemAccess {
public:
    virtual ~IFileSystemAccess() = default;

    virtual std::expected<std::vector<DirEntry>, IoError> list(
        const std::filesystem::path& directory) = 0;
    virtual std::expected<std::unique_ptr<IByteSource>, IoError> open(
        const std::filesystem::path& file,
        ReadOptions options) = 0;
    virtual std::expected<std::string, IoError> read_link(
        const std::filesystem::path& link) = 0;
    virtual std::expected<void, IoError> drop_cache(
        const std::filesystem::path& file) = 0;
};

// A file being written. Bytes go to a temporary file next to the destination and
// become visible only when commit() has flushed them to stable storage and
// atomically replaced the destination (R-61). Destroying an uncommitted file
// deletes the temporary file, so a crash never leaves a half-written destination.
//
// flush() makes the bytes written so far durable without making them visible.
// A commit() with nothing written since then has only the rename left to do,
// short enough to make while the repository database is locked: a manifest
// gets its name in the transaction that records its revision (decision 0023).
class IOutputFile {
public:
    virtual ~IOutputFile() = default;

    virtual std::expected<void, IoError> write(std::span<const std::byte> data) = 0;
    virtual std::expected<void, IoError> flush() = 0;
    virtual std::expected<void, IoError> commit() = 0;
};

// Durable writes (decision 0011). Only the repository and manifests are written;
// archive media is never written (R-03).
class IFileWriter {
public:
    virtual ~IFileWriter() = default;

    virtual std::expected<std::unique_ptr<IOutputFile>, IoError> create(
        const std::filesystem::path& destination) = 0;
    virtual std::expected<void, IoError> remove(const std::filesystem::path& file) = 0;
    virtual std::expected<void, IoError> create_directories(const std::filesystem::path& directory) = 0;
};

class IMediaEnumerator {
public:
    virtual ~IMediaEnumerator() = default;

    virtual std::expected<std::vector<VolumeInfo>, IoError> volumes() = 0;
    virtual std::expected<VolumeInfo, IoError> volume_for_path(
        const std::filesystem::path& path) = 0;
    // What `device` (one of a volume's devices, by its device_ref) reports of
    // its own health, without elevation (decision 0044): asked once at the
    // start of a registration, a verification and a relocation, and by
    // `media list`, never by the enumeration itself. HealthStatus::Unknown
    // where the device or the system tells nothing; an error only where the
    // device cannot be asked at all.
    virtual std::expected<DeviceHealth, IoError> device_health(const PhysicalDeviceInfo& device) = 0;
};

class IRawDeviceAccess {
public:
    virtual ~IRawDeviceAccess() = default;

    virtual std::expected<std::unique_ptr<IByteSource>, IoError> open_device(
        const std::string& device_ref) = 0;
};

class CancelToken;  // below

// Reads a device's own identity (model, serial, WWN) where its enclosure, the
// USB bridge or dock between it and the computer, reports its own serial in
// place of the device's (decision 0047). Reading it needs administrator
// permission, which the main program never holds (R-08, R-12): on Windows the
// implementation starts a small program of its own, the device identity
// reader, which the operating system starts only once the operator allows it
// administrator permission; it sends the device ATA IDENTIFY DEVICE alone,
// reads nothing of the device's content and writes nothing to it. Asked only
// on the operator's request, for one device of a volume (its device_ref, as
// IMediaEnumerator gives it). It waits for the operator's answer and for the
// reader, so it is called off the window's thread; `cancel` stops the wait
// once the operator has answered.
//
// Its error's kind tells why nothing was read (device_identity_failure):
// NotSupported: no device identity reader on this platform; PermissionDenied:
// the operator declined administrator permission, and only that;
// CommandNotPassed: the device's enclosure passes no ATA command on to it;
// Cancelled: `cancel` stopped the wait; any other: the device could not be
// opened or read (NotFound where it is no longer attached, or another device
// took its place while the operator answered; Timeout where it did not
// answer in time), or the reader failed.
class IDeviceIdentityHelper {
public:
    virtual ~IDeviceIdentityHelper() = default;

    // Whether a read can be asked for at all, without asking for one (which
    // would ask the operator for administrator permission): false where this
    // platform has no device identity reader, or its program is missing (for
    // example removed by an antivirus program). The window offers no read
    // where it is false.
    virtual bool available() const = 0;

    virtual std::expected<DeviceIdentityRead, IoError> read_device_identity(
        const std::string& device_ref, CancelToken& cancel) = 0;
};

enum class DeviceIdentityFailure { NotAvailable, Declined, NotPassed, Unreadable, Cancelled };

// Why IDeviceIdentityHelper::read_device_identity read nothing, from its
// error's kind.
inline DeviceIdentityFailure device_identity_failure(const IoError& error)
{
    switch (error.kind) {
    case IoErrorKind::NotSupported:
        return DeviceIdentityFailure::NotAvailable;
    case IoErrorKind::PermissionDenied:
        return DeviceIdentityFailure::Declined;
    case IoErrorKind::CommandNotPassed:
        return DeviceIdentityFailure::NotPassed;
    case IoErrorKind::Cancelled:
        return DeviceIdentityFailure::Cancelled;
    default:
        return DeviceIdentityFailure::Unreadable;
    }
}

struct ChangeTrackerState {
    std::string journal_id;
    std::string cursor;
};

enum class ChangeTrackerVerdict { Unchanged, Changed, ContinuityLost, Unsupported };

class IChangeTracker {
public:
    virtual ~IChangeTracker() = default;

    virtual std::expected<ChangeTrackerState, IoError> snapshot(const VolumeInfo& volume) = 0;
    virtual std::expected<ChangeTrackerVerdict, IoError> compare(
        const VolumeInfo& volume,
        const ChangeTrackerState& previous,
        std::vector<ArchivePath>* changed_out) = 0;
};

class ISigner {
public:
    virtual ~ISigner() = default;

    virtual std::string key_id() const = 0;
    virtual std::expected<std::vector<std::byte>, IoError> sign(
        std::span<const std::byte> data) = 0;
};

struct Progress {
    std::uint64_t bytes_done;
    std::uint64_t bytes_total;
    std::uint64_t items_done;
    std::uint64_t items_total;
    std::string current_item;
};

class IProgressSink {
public:
    virtual ~IProgressSink() = default;
    virtual void on_progress(const Progress& progress) = 0;
};

class CancelToken {
public:
    void cancel() { cancelled_.store(true); }
    bool cancelled() const { return cancelled_.load(); }
    void reset() { cancelled_.store(false); } // before reusing the token for a new operation

private:
    std::atomic_bool cancelled_{false};
};

class IEntropySource {
public:
    virtual ~IEntropySource() = default;
    virtual std::expected<void, IoError> fill(std::span<std::byte> output) = 0;
};

// A process as the operating system of this computer knows it (decision
// 0024). Once a process has ended, its id may be given to a later process:
// `start` tells the two apart.
struct ProcessIdentity {
    std::uint64_t id = 0;   // the process id
    std::string start;      // when it started, in the operating system's own terms: the same in every
                            // reading while the process runs, another for a later process given the
                            // same id (Windows: its creation time; Linux: the boot's id and the clock
                            // ticks since that boot). Compared, never shown.
    std::string program;    // the file name of its program, for people; may be empty
};

// The processes running on this computer, read to tell whether the process
// that runs an attempt of a registration still runs (decision 0024). It reads
// only what the operating system shows an unprivileged process (R-08).
class IProcessTable {
public:
    virtual ~IProcessTable() = default;

    // This process.
    virtual std::expected<ProcessIdentity, IoError> current() = 0;
    // The process that has `id` now; nothing when no process has it, or the
    // process that had it has ended. An error when it cannot be told, such as
    // when the operating system refuses to show that process.
    virtual std::expected<std::optional<ProcessIdentity>, IoError> find(std::uint64_t id) = 0;
};

class NullChangeTracker final : public IChangeTracker {
public:
    std::expected<ChangeTrackerState, IoError> snapshot(const VolumeInfo&) override
    {
        return std::unexpected(IoError{
            IoErrorKind::NotSupported,
            0,
            "change tracking is not available",
        });
    }

    std::expected<ChangeTrackerVerdict, IoError> compare(
        const VolumeInfo&,
        const ChangeTrackerState&,
        std::vector<ArchivePath>*) override
    {
        return ChangeTrackerVerdict::Unsupported;
    }
};

class NullRawDeviceAccess final : public IRawDeviceAccess {
public:
    std::expected<std::unique_ptr<IByteSource>, IoError> open_device(
        const std::string&) override
    {
        return std::unexpected(IoError{
            IoErrorKind::NotSupported,
            0,
            "raw device access is not available",
        });
    }
};

// Where no device identity reader exists (Linux, decision 0047) and in the
// tests: nothing is read and nothing asks for administrator permission.
class NullDeviceIdentityHelper final : public IDeviceIdentityHelper {
public:
    bool available() const override { return false; }

    std::expected<DeviceIdentityRead, IoError> read_device_identity(
        const std::string&, CancelToken&) override
    {
        return std::unexpected(IoError{
            IoErrorKind::NotSupported,
            0,
            "reading a device's own identity is not available on this platform",
        });
    }
};

} // namespace va