// A sparse sample file on Windows: NTFS keeps a range unallocated only in a
// file marked sparse (FSCTL_SET_SPARSE, which any user may set on a file of
// their own), so the file is marked, its end set past the hole, and the tail
// written after the hole.
#include "sparse_file.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winioctl.h>

namespace test {

SparseFile make_sparse_file(const std::filesystem::path& path, std::uint64_t hole, std::string_view tail)
{
    SparseFile result;
    const HANDLE file = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return result;
    }
    DWORD returned = 0;
    const bool marked = DeviceIoControl(file, FSCTL_SET_SPARSE, nullptr, 0, nullptr, 0, &returned, nullptr) != 0;
    LARGE_INTEGER end{};
    end.QuadPart = static_cast<LONGLONG>(hole + tail.size());
    LARGE_INTEGER start_of_tail{};
    start_of_tail.QuadPart = static_cast<LONGLONG>(hole);
    DWORD written = 0;
    const bool made = SetFilePointerEx(file, end, nullptr, FILE_BEGIN) && SetEndOfFile(file) &&
                      SetFilePointerEx(file, start_of_tail, nullptr, FILE_BEGIN) &&
                      WriteFile(file, tail.data(), static_cast<DWORD>(tail.size()), &written, nullptr) &&
                      written == tail.size();
    CloseHandle(file);
    result.created = made;
    if (made && marked) {
        DWORD high = 0;
        const DWORD low = GetCompressedFileSizeW(path.c_str(), &high);
        const bool known = low != INVALID_FILE_SIZE || GetLastError() == NO_ERROR;
        const auto allocated = (static_cast<std::uint64_t>(high) << 32) | low;
        result.sparse = known && allocated < hole + tail.size();
    }
    return result;
}

} // namespace test
