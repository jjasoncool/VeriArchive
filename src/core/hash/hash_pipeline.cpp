#include "core/hash/hash_pipeline.h"

#include "core/hash/hashers.h"
#include "core/identity/matcher.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <new>
#include <thread>
#include <vector>

namespace va {
namespace {

// Buffers satisfy any sector size up to 64 KiB for uncached reads (R-100).
constexpr std::size_t buffer_alignment = 64 * 1024;

// A queue with a fixed capacity: push() waits while it is full, pop() waits
// while it is empty. close() wakes everyone; pop() then drains what is left.
template <typename T>
class BoundedQueue {
public:
    explicit BoundedQueue(std::size_t capacity) : capacity_(capacity) {}

    void push(T value)
    {
        std::unique_lock lock(mutex_);
        not_full_.wait(lock, [&] { return items_.size() < capacity_ || closed_; });
        items_.push_back(std::move(value));
        not_empty_.notify_one();
    }

    std::optional<T> pop()
    {
        bool timed_out = false;
        return pop_for(std::chrono::milliseconds::max(), timed_out);
    }

    // Like pop(), but waits at most `timeout`: when nothing arrived by then,
    // `timed_out` is set and nothing is returned. (Without it, nothing returned
    // means the queue is closed and empty.)
    std::optional<T> pop_for(std::chrono::milliseconds timeout, bool& timed_out)
    {
        std::unique_lock lock(mutex_);
        const auto ready = [&] { return !items_.empty() || closed_; };
        if (timeout == std::chrono::milliseconds::max()) {
            not_empty_.wait(lock, ready);
            timed_out = false;
        } else {
            timed_out = !not_empty_.wait_for(lock, timeout, ready);
        }
        if (items_.empty()) {
            return std::nullopt;
        }
        T value = std::move(items_.front());
        items_.pop_front();
        not_full_.notify_one();
        return value;
    }

    void close()
    {
        std::lock_guard lock(mutex_);
        closed_ = true;
        not_empty_.notify_all();
        not_full_.notify_all();
    }

private:
    std::size_t capacity_;
    std::deque<T> items_;
    bool closed_ = false;
    std::mutex mutex_;
    std::condition_variable not_full_;
    std::condition_variable not_empty_;
};

// One message from a reader to its hash thread: a file's bytes (Data), its end
// (End), the error that ended its reading (Failed, whatever the error's kind),
// a file whose reading the pipeline's stop interrupted (Interrupted: it has no
// result, and a resume reads it again), or the reader's own end (Stop).
struct Message {
    enum class Kind { Data, End, Failed, Interrupted, Stop };

    explicit Message(Kind kind_, std::size_t job_ = 0, std::byte* data_ = nullptr,
        std::size_t size_ = 0, std::optional<IoError> error_ = std::nullopt)
        : kind(kind_), job(job_), data(data_), size(size_), error(std::move(error_))
    {
    }

    Kind kind;
    std::size_t job;
    std::byte* data;
    std::size_t size;
    std::optional<IoError> error;
};

// The hashes of one file, fed with its bytes in whatever pieces the reads
// gave: BLAKE3 and SHA-256 of the whole file and, with a chunk size, the
// BLAKE3 of each chunk (R-101). A chunk ends at every multiple of the chunk
// size from the start of the file (R-31), wherever a buffer ends.
class FileHashes {
public:
    explicit FileHashes(std::uint64_t chunk_size) : chunk_size_(chunk_size) {}

    void update(std::span<const std::byte> bytes)
    {
        while (!bytes.empty()) {
            auto piece = bytes;
            if (chunk_size_ > 0) {
                const auto to_boundary = chunk_size_ - size_ % chunk_size_;
                piece = bytes.first(static_cast<std::size_t>(std::min<std::uint64_t>(to_boundary, bytes.size())));
            }
            blake3_.update(piece);
            sha256_.update(piece);
            // The first chunk is the file's first chunk_size bytes: its hash
            // is the file's BLAKE3 at that point, so only later chunks need a
            // hasher of their own.
            if (chunk_size_ > 0 && size_ >= chunk_size_) {
                if (!chunk_) {
                    chunk_.emplace();
                }
                chunk_->update(piece);
            }
            size_ += piece.size();
            bytes = bytes.subspan(piece.size());
            if (chunk_size_ > 0 && size_ % chunk_size_ == 0) {
                chunks_.push_back(size_ == chunk_size_ ? blake3_.current_hex() : chunk_->finish_hex());
                chunk_.reset();
            }
        }
    }

    std::uint64_t size() const { return size_; }

    // The hashes of a file read to its end.
    void finish(HashResult& result)
    {
        result.b3 = blake3_.finish_hex();
        result.sha256 = sha256_.finish_hex();
        if (chunk_) {
            chunks_.push_back(chunk_->finish_hex()); // the last chunk, shorter than the others
        }
        // A file no larger than the chunk size is one chunk, the whole file,
        // and gets no chunk hashes of its own (R-31).
        if (chunk_size_ > 0 && size_ > chunk_size_) {
            result.chunks = std::move(chunks_);
        }
    }

private:
    std::uint64_t chunk_size_;
    std::uint64_t size_ = 0;
    Blake3Hasher blake3_;
    Sha256Hasher sha256_;
    std::optional<Blake3Hasher> chunk_; // the chunk being read, after the first
    std::vector<std::string> chunks_;
};

class AlignedBuffer {
public:
    explicit AlignedBuffer(std::size_t size)
        : data_(static_cast<std::byte*>(::operator new(size, std::align_val_t(buffer_alignment))))
    {
    }
    ~AlignedBuffer() { ::operator delete(data_, std::align_val_t(buffer_alignment)); }
    AlignedBuffer(const AlignedBuffer&) = delete;
    AlignedBuffer& operator=(const AlignedBuffer&) = delete;
    std::byte* data() const { return data_; }

private:
    std::byte* data_;
};

class Pipeline {
public:
    Pipeline(IFileSystemAccess& files, std::span<const HashJob> jobs,
        const HashPipelineOptions& options, CancelToken& cancel)
        : files_(files), jobs_(jobs), options_(options), cancel_(cancel),
          results_(std::max<std::size_t>(64, options.readers * 4))
    {
    }

    std::uint64_t bytes_hashed() const { return bytes_hashed_.load(); }
    // Bytes actually read minus the sizes expected, over the files finished.
    std::int64_t size_difference() const { return size_difference_.load(); }
    void stop() { stopping_.store(true); }
    bool stopping() const { return stopping_.load() || cancel_.cancelled(); }
    BoundedQueue<HashResult>& results() { return results_; }

    void start()
    {
        const auto readers = std::max<std::size_t>(1, std::min(options_.readers, jobs_.size()));
        active_hashers_.store(readers);
        for (std::size_t slot = 0; slot < readers; ++slot) {
            auto lane = std::make_unique<Lane>(options_);
            auto* raw = lane.get();
            lanes_.push_back(std::move(lane));
            threads_.emplace_back([this, raw] { read_loop(*raw); });
            threads_.emplace_back([this, raw] { hash_loop(*raw); });
        }
        if (readers == 0 || jobs_.empty()) {
            results_.close();
        }
    }

    void join()
    {
        for (auto& thread : threads_) {
            thread.join();
        }
    }

private:
    // A reader thread, its hash thread, and the buffers they pass back and forth.
    struct Lane {
        explicit Lane(const HashPipelineOptions& options)
            : free(options.buffers_per_reader), filled(options.buffers_per_reader + 2)
        {
            for (std::size_t index = 0; index < options.buffers_per_reader; ++index) {
                buffers.push_back(std::make_unique<AlignedBuffer>(options.buffer_size));
                free.push(buffers.back()->data());
            }
        }
        std::vector<std::unique_ptr<AlignedBuffer>> buffers;
        BoundedQueue<std::byte*> free;
        BoundedQueue<Message> filled;
    };

    void read_loop(Lane& lane)
    {
        for (;;) {
            const auto index = next_job_.fetch_add(1);
            if (index >= jobs_.size() || stopping()) {
                break;
            }
            read_file(lane, index);
        }
        lane.filled.push(Message{Message::Kind::Stop});
    }

    void read_file(Lane& lane, std::size_t index)
    {
        const auto& job = jobs_[index];
        auto source = files_.open(job.native, ReadOptions{options_.uncached, true});
        if (!source) {
            lane.filled.push(Message{Message::Kind::Failed, index, nullptr, 0, source.error()});
            return;
        }
        const bool unbuffered = (*source)->alignment() > 1;
        std::uint64_t offset = 0;
        for (;;) {
            if (stopping()) {
                lane.filled.push(Message{Message::Kind::Interrupted, index});
                return;
            }
            auto buffer = lane.free.pop();
            if (!buffer) {
                return;
            }
            const auto count = (*source)->read(offset, std::span(*buffer, options_.buffer_size));
            if (!count) {
                lane.free.push(*buffer);
                lane.filled.push(Message{Message::Kind::Failed, index, nullptr, 0, count.error()});
                return;
            }
            if (*count == 0) {
                lane.free.push(*buffer);
                break;
            }
            lane.filled.push(Message{Message::Kind::Data, index, *buffer, *count, std::nullopt});
            offset += *count;
            // An unbuffered short read can only be the end of the file, and the
            // next offset would not be sector-aligned anyway.
            if (unbuffered && *count < options_.buffer_size) {
                break;
            }
        }
        lane.filled.push(Message{Message::Kind::End, index, nullptr, 0, std::nullopt});
    }

    void hash_loop(Lane& lane)
    {
        std::optional<FileHashes> file;
        while (auto message = lane.filled.pop()) {
            if (message->kind == Message::Kind::Stop) {
                break;
            }
            if (message->kind == Message::Kind::Interrupted) {
                // No result: the file keeps its expected size in the total to
                // hash, as the bytes it did not reach are not done.
                file.reset();
                continue;
            }
            if (!file) {
                file.emplace(options_.chunk_size);
            }
            if (message->kind == Message::Kind::Data) {
                file->update(std::span<const std::byte>(message->data, message->size));
                bytes_hashed_.fetch_add(message->size);
                lane.free.push(message->data);
                continue;
            }
            HashResult result;
            result.id = jobs_[message->job].id;
            result.size = file->size();
            if (message->kind == Message::Kind::End) {
                file->finish(result);
            } else {
                result.error = message->error; // no hashes, not even of the chunks read
            }
            // The file is done (read to its end, or as far as it could be read):
            // the total to hash now counts what was read instead of what the
            // scan expected.
            size_difference_.fetch_add(static_cast<std::int64_t>(result.size) -
                                       static_cast<std::int64_t>(jobs_[message->job].size));
            file.reset();
            results_.push(std::move(result));
        }
        if (active_hashers_.fetch_sub(1) == 1) {
            results_.close();
        }
    }

    IFileSystemAccess& files_;
    std::span<const HashJob> jobs_;
    HashPipelineOptions options_;
    CancelToken& cancel_;
    BoundedQueue<HashResult> results_;
    std::vector<std::unique_ptr<Lane>> lanes_;
    std::vector<std::thread> threads_;
    std::atomic<std::size_t> next_job_{0};
    std::atomic<std::size_t> active_hashers_{0};
    std::atomic<std::uint64_t> bytes_hashed_{0};
    std::atomic<std::int64_t> size_difference_{0};
    std::atomic<bool> stopping_{false};
};

} // namespace

std::size_t default_reader_count(std::optional<bool> rotational)
{
    if (rotational.value_or(true)) {
        return 1;
    }
    const auto cores = std::max(1u, std::thread::hardware_concurrency());
    return std::min<std::size_t>(8, cores);
}

std::optional<bool> volume_rotational(const VolumeInfo& volume, std::optional<bool> io_option,
    const std::map<std::string, bool>& overrides)
{
    if (io_option) {
        return io_option;
    }
    std::optional<bool> rotational;
    for (const auto& device : volume.devices) {
        auto known = device.rotational;
        if (!device.serial.empty()) {
            const auto chosen = std::ranges::find_if(overrides,
                [&](const auto& entry) { return serials_equal(entry.first, device.serial); });
            if (chosen != overrides.end()) {
                known = chosen->second;
            }
        }
        if (known) {
            rotational = rotational.value_or(false) || *known;
        }
    }
    return rotational;
}

std::expected<void, IoError> hash_files(
    IFileSystemAccess& files,
    std::span<const HashJob> jobs,
    const HashPipelineOptions& options,
    CancelToken& cancel,
    IProgressSink& progress,
    const std::function<bool(HashResult&&)>& on_result)
{
    // The total to hash, by bytes; 0 (unknown) when the caller gave no sizes.
    std::uint64_t expected_bytes = 0;
    for (const auto& job : jobs) {
        expected_bytes += job.size;
    }

    Pipeline pipeline(files, jobs, options, cancel);
    pipeline.start();

    std::uint64_t items_done = 0;
    const auto report = [&] {
        const auto done = pipeline.bytes_hashed();
        std::uint64_t total = 0;
        if (expected_bytes > 0) {
            // The expected sizes corrected by the files already finished; never
            // below what was hashed (a file may have grown since the scan).
            const auto corrected = static_cast<std::int64_t>(expected_bytes) + pipeline.size_difference();
            total = std::max<std::uint64_t>(done, static_cast<std::uint64_t>(std::max<std::int64_t>(0, corrected)));
        }
        progress.on_progress({done, total, items_done, jobs.size(), ""});
    };

    constexpr auto report_interval = std::chrono::milliseconds(200);
    auto last_report = std::chrono::steady_clock::now();
    // Once on_result has said stop, it gets no further results: files already
    // hashed and waiting in the queue are drained, not delivered. (After a
    // cancel they are still delivered: they are valid and resume can use them.)
    bool consumer_stopped = false;
    for (;;) {
        // Waits for the next finished file, but not longer than a report
        // interval: a large file takes minutes, and its bytes should show.
        bool timed_out = false;
        auto result = pipeline.results().pop_for(report_interval, timed_out);
        if (!result && !timed_out) {
            break; // every hash thread has finished and the queue is empty
        }
        if (result) {
            // Every result is a file's own, failed files included, whatever
            // their error: a file whose reading a stop interrupted makes none
            // (Message::Kind::Interrupted).
            if (!consumer_stopped) {
                ++items_done;
                if (!on_result(std::move(*result))) {
                    consumer_stopped = true;
                    pipeline.stop();
                }
            }
        }
        const auto now = std::chrono::steady_clock::now();
        if (now - last_report >= report_interval) {
            report();
            last_report = now;
        }
    }
    pipeline.join();
    report();

    if (pipeline.stopping()) {
        return std::unexpected(IoError{IoErrorKind::Cancelled, 0, "hashing stopped"});
    }
    return {};
}

} // namespace va
