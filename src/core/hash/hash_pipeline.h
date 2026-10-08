#pragma once
// Hash pipeline (R-110): an ordered work queue feeds reader threads; each reader
// passes filled buffers through a bounded queue to its own hash thread, so
// reading and hashing overlap without one thread per file. One read pass yields
// BLAKE3 and SHA-256 together (R-101). Results are delivered on the calling
// thread, so callers never need locks.
#include "core/interfaces.h"

#include <functional>
#include <optional>
#include <span>
#include <string>

namespace va {

struct HashJob {
    std::size_t id = 0;                 // caller's identifier, returned in the result
    std::filesystem::path native;       // file to read
};

struct HashResult {
    std::size_t id = 0;
    std::string b3;
    std::string sha256;
    std::uint64_t size = 0;             // bytes actually read and hashed
    std::optional<IoError> error;       // set when the file could not be read completely
};

struct HashPipelineOptions {
    std::size_t readers = 1;            // 1 for rotational media, min(8, cores) otherwise
    std::size_t buffer_size = 8u << 20; // 8 MiB
    std::size_t buffers_per_reader = 8;
    bool uncached = true;               // R-100
};

// Reader count for a device: one for HDDs and for devices of unknown type
// (avoids seek amplification; USB bridges often hide the type), min(8, cores)
// for SSDs (R-110, R-111).
std::size_t default_reader_count(std::optional<bool> rotational);

// Hashes every job. `on_result` runs on the calling thread once per finished
// file; returning false stops the pipeline (e.g. the device disappeared).
// Returns Cancelled when stopped by `cancel` or by `on_result`.
std::expected<void, IoError> hash_files(
    IFileSystemAccess& files,
    std::span<const HashJob> jobs,
    const HashPipelineOptions& options,
    CancelToken& cancel,
    IProgressSink& progress,
    const std::function<bool(HashResult&&)>& on_result);

} // namespace va
