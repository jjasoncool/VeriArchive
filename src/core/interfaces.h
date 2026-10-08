#pragma once
#include "common/archive_path.h"
#include "common/error.h"
#include "common/types.h"
#include <atomic>
#include <expected>
#include <memory>
#include <span>
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
class IOutputFile {
public:
    virtual ~IOutputFile() = default;

    virtual std::expected<void, IoError> write(std::span<const std::byte> data) = 0;
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
};

class IRawDeviceAccess {
public:
    virtual ~IRawDeviceAccess() = default;

    virtual std::expected<std::unique_ptr<IByteSource>, IoError> open_device(
        const std::string& device_ref) = 0;
};

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

private:
    std::atomic_bool cancelled_{false};
};

class IEntropySource {
public:
    virtual ~IEntropySource() = default;
    virtual std::expected<void, IoError> fill(std::span<std::byte> output) = 0;
};

class NullChangeTracker final : public IChangeTracker {
public:
    std::expected<ChangeTrackerState, IoError> snapshot(const VolumeInfo&) override
    {
        return std::unexpected(IoError{
            IoErrorKind::NotSupported,
            0,
            "change tracking is not available in V1",
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
            "raw device access requires the V1.1 helper",
        });
    }
};

} // namespace va