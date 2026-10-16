#pragma once
// Hash pipeline (R-110): an ordered work queue feeds reader threads; each reader
// passes filled buffers through a bounded queue to its own hash thread, so
// reading and hashing overlap without one thread per file. One read pass yields
// BLAKE3 and SHA-256 together and, with a chunk size, the BLAKE3 of every chunk
// (R-101). Results are delivered on the calling thread, so callers never need
// locks.
#include "core/interfaces.h"

#include <functional>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace va {

struct HashJob {
    std::size_t id = 0;                 // caller's identifier, returned in the result
    std::filesystem::path native;       // file to read
    std::uint64_t size = 0;             // expected size (from the scan), for progress by bytes
};

struct HashResult {
    std::size_t id = 0;
    std::string b3;
    std::string sha256;
    // The BLAKE3 of each chunk in file order, for a file read to its end that
    // is larger than the chunk size. Empty for any other file: one no larger
    // than the chunk size is one chunk, the whole file, whose hash is b3
    // (R-31).
    std::vector<std::string> chunks;
    std::uint64_t size = 0;             // bytes actually read and hashed
    std::optional<IoError> error;       // set when the file could not be read completely
};

struct HashPipelineOptions {
    std::size_t readers = 1;            // 1 for rotational media, min(8, cores) otherwise
    std::size_t buffer_size = 8u << 20; // 8 MiB
    std::size_t buffers_per_reader = 8;
    bool uncached = true;               // R-100
    // 0: whole files only. Otherwise chunk n of a file is its bytes from
    // n * chunk_size up to the next multiple or the end of the file, counted
    // from the start of the file (R-31) whatever the buffers and reads.
    std::uint64_t chunk_size = 0;
};

// Reader count for a device: one for HDDs and for devices of unknown type
// (avoids seek amplification; USB bridges often hide the type), min(8, cores)
// for SSDs (R-110, R-111).
std::size_t default_reader_count(std::optional<bool> rotational);

// Whether the devices of `volume` are rotational, which default_reader_count
// turns into readers (R-110, R-111). `io_option` (--io hdd|ssd) wins. Without
// it, each device is as the config's io.rotational_override says for its
// serial (`overrides`, serial -> rotational; serials compared as identity
// matching compares them, serials_equal), else as the platform detected it.
// Rotational when any device is; not rotational when every device whose type
// is known is not; unknown when none is known (decision 0034).
std::optional<bool> volume_rotational(const VolumeInfo& volume, std::optional<bool> io_option,
    const std::map<std::string, bool>& overrides);

// Hashes every job. `on_result` runs on the calling thread once per finished
// file, a file whose reading failed included, whatever its error; returning
// false stops the pipeline (e.g. the device disappeared). Returns Cancelled
// when stopped by `cancel` or by `on_result`; otherwise every job's result was
// delivered (decision 0034).
//
// Progress is reported on the calling thread about five times a second, also
// while one large file is being read: bytes_done counts the bytes hashed so far,
// items_done the files finished. bytes_total is the sum of the jobs' sizes, so
// the share done can be shown by bytes; it follows the sizes actually read (a
// file that turned out shorter, or could not be read to its end, leaves only
// what was read), so it equals bytes_done when every file is done. After a
// cancel, a file stopped halfway and the files not started keep their
// expected sizes, as their bytes were not reached, so bytes_total stays above
// bytes_done unless every file was done. It is 0 (unknown) when no job
// carries a size.
std::expected<void, IoError> hash_files(
    IFileSystemAccess& files,
    std::span<const HashJob> jobs,
    const HashPipelineOptions& options,
    CancelToken& cancel,
    IProgressSink& progress,
    const std::function<bool(HashResult&&)>& on_result);

} // namespace va
