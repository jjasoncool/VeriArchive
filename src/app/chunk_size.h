#pragma once
// A chunk size as people read it (decisions 0032, 0033): a multiple of 64 KiB
// (decision 0031), in the largest unit that keeps it whole, "64 MiB" or
// "64 KiB". The units are the same in every language.
#include <cstdint>
#include <format>
#include <string>

namespace va::app {

inline std::string chunk_size_text(std::uint64_t chunk_size)
{
    constexpr std::uint64_t mebibyte = 1024 * 1024;
    return chunk_size % mebibyte == 0 ? std::format("{} MiB", chunk_size / mebibyte)
                                      : std::format("{} KiB", chunk_size / 1024);
}

} // namespace va::app
