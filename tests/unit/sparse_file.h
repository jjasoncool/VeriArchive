#pragma once
// A sparse sample file for the tests of R-48, made the way each operating
// system makes one: sparse_file_win.cpp or sparse_file_linux.cpp, which CMake
// chooses.
#include <cstdint>
#include <filesystem>
#include <string_view>

namespace test {

struct SparseFile {
    bool created = false;  // the file holds `hole` zero bytes, then `tail`
    bool sparse = false;   // and its filesystem keeps the hole unallocated
};

// Creates `path` (replacing a file there) with `hole` bytes never written,
// which read as zeros, followed by `tail`.
SparseFile make_sparse_file(const std::filesystem::path& path, std::uint64_t hole, std::string_view tail);

} // namespace test
