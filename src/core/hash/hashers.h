#pragma once
// Streaming hashers for file content (R-101). Feed bytes with update() in any
// number of pieces, then call finish() once; the result is the same as hashing
// all bytes at once. A BLAKE3 hasher can also tell the hash of the bytes so
// far and go on (current_hex()).
//  - BLAKE3: fast primary hash, also used for manifest bodies and history rows.
//  - SHA-256: second, widely available hash so archives can be checked with
//    standard tools (`sha256sum -c`, R-67).
#include <array>
#include <cstddef>
#include <memory>
#include <span>
#include <string>
#include <string_view>

namespace va {

// A BLAKE3 hash as its 32 bytes (manifests write it as 64 lowercase hex digits).
using Blake3Digest = std::array<std::byte, 32>;

class Blake3Hasher {
public:
    Blake3Hasher();
    ~Blake3Hasher();
    Blake3Hasher(const Blake3Hasher&) = delete;
    Blake3Hasher& operator=(const Blake3Hasher&) = delete;

    void update(std::span<const std::byte> data);
    // Lowercase hex of the 32-byte digest.
    std::string finish_hex();
    // The same for the bytes given so far; more may follow.
    std::string current_hex() const;

private:
    struct State;                  // hides the C library's struct from callers
    std::unique_ptr<State> state_;
};

class Sha256Hasher {
public:
    Sha256Hasher();
    ~Sha256Hasher();
    Sha256Hasher(const Sha256Hasher&) = delete;
    Sha256Hasher& operator=(const Sha256Hasher&) = delete;

    void update(std::span<const std::byte> data);
    std::string finish_hex();

private:
    struct State;
    std::unique_ptr<State> state_;
};

// One-shot helpers.
std::string blake3_hex(std::string_view data);
std::string sha256_hex(std::string_view data);
// The BLAKE3 of a few bytes, without a hasher object on the heap (Merkle
// tree nodes, §B6).
Blake3Digest blake3_digest(std::span<const std::byte> data);

} // namespace va
