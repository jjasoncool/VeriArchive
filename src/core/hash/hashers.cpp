#include "core/hash/hashers.h"

#include "common/hex.h"

#include <blake3.h>
#include <cstdint>
#include <openssl/evp.h>
#include <stdexcept>

namespace va {

struct Blake3Hasher::State {
    blake3_hasher hasher;
};

Blake3Hasher::Blake3Hasher() : state_(std::make_unique<State>())
{
    blake3_hasher_init(&state_->hasher);
}

Blake3Hasher::~Blake3Hasher() = default;

void Blake3Hasher::update(std::span<const std::byte> data)
{
    blake3_hasher_update(&state_->hasher, data.data(), data.size());
}

std::string Blake3Hasher::finish_hex()
{
    std::array<std::byte, BLAKE3_OUT_LEN> digest{};
    blake3_hasher_finalize(&state_->hasher, reinterpret_cast<std::uint8_t*>(digest.data()),
        digest.size());
    return to_hex(digest);
}

// OpenSSL's EVP interface uses the SHA-NI CPU instructions when available, which
// keeps SHA-256 from becoming the bottleneck on fast NVMe drives (§B15).
struct Sha256Hasher::State {
    EVP_MD_CTX* context = nullptr;
    ~State() { EVP_MD_CTX_free(context); }
};

Sha256Hasher::Sha256Hasher() : state_(std::make_unique<State>())
{
    state_->context = EVP_MD_CTX_new();
    if (!state_->context || EVP_DigestInit_ex(state_->context, EVP_sha256(), nullptr) != 1) {
        throw std::runtime_error("cannot initialise SHA-256");
    }
}

Sha256Hasher::~Sha256Hasher() = default;

void Sha256Hasher::update(std::span<const std::byte> data)
{
    EVP_DigestUpdate(state_->context, data.data(), data.size());
}

std::string Sha256Hasher::finish_hex()
{
    std::array<std::byte, 32> digest{};
    unsigned int length = 0;
    EVP_DigestFinal_ex(state_->context, reinterpret_cast<unsigned char*>(digest.data()), &length);
    return to_hex(std::span(digest.data(), length));
}

std::string blake3_hex(std::string_view data)
{
    Blake3Hasher hasher;
    hasher.update(std::as_bytes(std::span(data.data(), data.size())));
    return hasher.finish_hex();
}

std::string sha256_hex(std::string_view data)
{
    Sha256Hasher hasher;
    hasher.update(std::as_bytes(std::span(data.data(), data.size())));
    return hasher.finish_hex();
}

} // namespace va
