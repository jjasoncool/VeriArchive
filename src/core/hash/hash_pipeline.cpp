#include "core/hash/hash_pipeline.h"

#include "core/hash/hashers.h"

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
        std::unique_lock lock(mutex_);
        not_empty_.wait(lock, [&] { return !items_.empty() || closed_; });
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

// One message from a reader to its hash thread.
struct Chunk {
    enum class Kind { Data, End, Failed, Stop };

    explicit Chunk(Kind kind_, std::size_t job_ = 0, std::byte* data_ = nullptr,
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
        BoundedQueue<Chunk> filled;
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
        lane.filled.push(Chunk{Chunk::Kind::Stop});
    }

    void read_file(Lane& lane, std::size_t index)
    {
        const auto& job = jobs_[index];
        auto source = files_.open(job.native, ReadOptions{options_.uncached, true});
        if (!source) {
            lane.filled.push(Chunk{Chunk::Kind::Failed, index, nullptr, 0, source.error()});
            return;
        }
        const bool unbuffered = (*source)->alignment() > 1;
        std::uint64_t offset = 0;
        for (;;) {
            if (stopping()) {
                lane.filled.push(Chunk{Chunk::Kind::Failed, index, nullptr, 0,
                    IoError{IoErrorKind::Cancelled, 0, "cancelled"}});
                return;
            }
            auto buffer = lane.free.pop();
            if (!buffer) {
                return;
            }
            const auto count = (*source)->read(offset, std::span(*buffer, options_.buffer_size));
            if (!count) {
                lane.free.push(*buffer);
                lane.filled.push(Chunk{Chunk::Kind::Failed, index, nullptr, 0, count.error()});
                return;
            }
            if (*count == 0) {
                lane.free.push(*buffer);
                break;
            }
            lane.filled.push(Chunk{Chunk::Kind::Data, index, *buffer, *count, std::nullopt});
            offset += *count;
            // An unbuffered short read can only be the end of the file, and the
            // next offset would not be sector-aligned anyway.
            if (unbuffered && *count < options_.buffer_size) {
                break;
            }
        }
        lane.filled.push(Chunk{Chunk::Kind::End, index, nullptr, 0, std::nullopt});
    }

    void hash_loop(Lane& lane)
    {
        std::optional<Blake3Hasher> blake3;
        std::optional<Sha256Hasher> sha256;
        std::uint64_t size = 0;
        while (auto chunk = lane.filled.pop()) {
            if (chunk->kind == Chunk::Kind::Stop) {
                break;
            }
            if (!blake3) {
                blake3.emplace();
                sha256.emplace();
                size = 0;
            }
            if (chunk->kind == Chunk::Kind::Data) {
                const auto bytes = std::span<const std::byte>(chunk->data, chunk->size);
                blake3->update(bytes);
                sha256->update(bytes);
                size += chunk->size;
                bytes_hashed_.fetch_add(chunk->size);
                lane.free.push(chunk->data);
                continue;
            }
            HashResult result;
            result.id = jobs_[chunk->job].id;
            result.size = size;
            if (chunk->kind == Chunk::Kind::End) {
                result.b3 = blake3->finish_hex();
                result.sha256 = sha256->finish_hex();
            } else {
                result.error = chunk->error;
            }
            blake3.reset();
            sha256.reset();
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

std::expected<void, IoError> hash_files(
    IFileSystemAccess& files,
    std::span<const HashJob> jobs,
    const HashPipelineOptions& options,
    CancelToken& cancel,
    IProgressSink& progress,
    const std::function<bool(HashResult&&)>& on_result)
{
    Pipeline pipeline(files, jobs, options, cancel);
    pipeline.start();

    std::uint64_t items_done = 0;
    auto last_report = std::chrono::steady_clock::now();
    while (auto result = pipeline.results().pop()) {
        const bool cancelled_file =
            result->error && result->error->kind == IoErrorKind::Cancelled;
        if (!cancelled_file) {
            ++items_done;
            if (!on_result(std::move(*result))) {
                pipeline.stop();
            }
        }
        const auto now = std::chrono::steady_clock::now();
        if (now - last_report > std::chrono::milliseconds(200)) {
            progress.on_progress({pipeline.bytes_hashed(), 0, items_done, jobs.size(), ""});
            last_report = now;
        }
    }
    pipeline.join();
    progress.on_progress({pipeline.bytes_hashed(), 0, items_done, jobs.size(), ""});

    if (pipeline.stopping()) {
        return std::unexpected(IoError{IoErrorKind::Cancelled, 0, "hashing stopped"});
    }
    return {};
}

} // namespace va
