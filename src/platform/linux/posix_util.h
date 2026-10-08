#pragma once
// Linux helpers shared by the platform implementation files.
#include "common/error.h"

#include <string>
#include <unistd.h>

namespace va::platform::posix {

// Maps an errno value to the platform-neutral error taxonomy (§B9).
IoError error_from_errno(int code, std::string operation);

// Owns a file descriptor and closes it when it goes out of scope (RAII).
class UniqueFd {
public:
    UniqueFd() = default;
    explicit UniqueFd(int descriptor) noexcept : descriptor_(descriptor) {}
    ~UniqueFd() { reset(); }

    UniqueFd(const UniqueFd&) = delete;
    UniqueFd& operator=(const UniqueFd&) = delete;
    UniqueFd(UniqueFd&& other) noexcept : descriptor_(other.release()) {}
    UniqueFd& operator=(UniqueFd&& other) noexcept
    {
        if (this != &other) {
            reset(other.release());
        }
        return *this;
    }

    int get() const noexcept { return descriptor_; }
    bool valid() const noexcept { return descriptor_ >= 0; }
    int release() noexcept
    {
        const int descriptor = descriptor_;
        descriptor_ = -1;
        return descriptor;
    }
    void reset(int descriptor = -1) noexcept
    {
        if (descriptor_ >= 0) {
            ::close(descriptor_);
        }
        descriptor_ = descriptor;
    }

private:
    int descriptor_ = -1;
};

} // namespace va::platform::posix
