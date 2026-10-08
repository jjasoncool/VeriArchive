#include "test_support.h"

#include "core/hash/hashers.h"

#include <string>

void hash_tests()
{
    // Published test vectors.
    CHECK(va::blake3_hex("") ==
          "af1349b9f5f9a1a6a0404dea36dcc9499bcb25c9adc112b7cc9a93cae41f3262");
    CHECK(va::blake3_hex("abc") ==
          "6437b3ac38465133ffb63b75273a8db548c558465d79db03fd359c6cd5bd9d85");
    CHECK(va::sha256_hex("") ==
          "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    CHECK(va::sha256_hex("abc") ==
          "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");

    // Feeding the data in pieces gives the same digest as one call.
    std::string data(1024 * 1024 + 7, '\0');
    for (std::size_t index = 0; index < data.size(); ++index) {
        data[index] = static_cast<char>(index * 31 + 7);
    }
    va::Blake3Hasher blake3;
    va::Sha256Hasher sha256;
    const auto bytes = std::as_bytes(std::span(data.data(), data.size()));
    for (std::size_t offset = 0; offset < bytes.size(); offset += 65537) {
        const auto piece = bytes.subspan(offset, std::min<std::size_t>(65537, bytes.size() - offset));
        blake3.update(piece);
        sha256.update(piece);
    }
    CHECK(blake3.finish_hex() == va::blake3_hex(data));
    CHECK(sha256.finish_hex() == va::sha256_hex(data));
}
