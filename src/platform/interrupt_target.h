#pragma once
// The operation that an interrupt cancels (platform/interrupt.h), and the
// hold of an interrupt after which the system ends the process. Windows calls
// the handler of a console control event on a thread of its own, and the
// window procedure that hears of the session's end on the thread of that
// window: each cancels the operation that runs and, where Windows ends the
// process as soon as it returns, waits until the operation has ended.
// Plain C++, so it is tested on every OS although only Windows holds an
// interrupt: a Linux signal handler may not wait for a lock, and Linux does
// not end the process when its handler returns.
#include "core/interfaces.h"

#include <condition_variable>
#include <cstdint>
#include <mutex>

namespace va::platform {

class InterruptTarget {
public:
    // The operation begins, cancelled by `token`; one at a time.
    void begin(CancelToken& token)
    {
        const std::lock_guard lock(mutex_);
        token_ = &token;
        ++begun_;
    }

    // The operation has ended: an interrupt cancels nothing any more, and a
    // hold waiting for it returns.
    void end()
    {
        {
            const std::lock_guard lock(mutex_);
            token_ = nullptr;
        }
        ended_.notify_all();
    }

    // Cancels the operation that runs now, and with `hold` waits until it has
    // ended. False, at once, when none runs.
    bool interrupt(bool hold)
    {
        std::unique_lock lock(mutex_);
        if (token_ == nullptr) {
            return false;
        }
        token_->cancel();
        if (hold) {
            const auto operation = begun_;
            ended_.wait(lock, [&] { return token_ == nullptr || begun_ != operation; });
        }
        return true;
    }

private:
    std::mutex mutex_;
    std::condition_variable ended_;
    CancelToken* token_ = nullptr;
    std::uint64_t begun_ = 0; // operations begun, so that a hold ends with the operation it held for
};

} // namespace va::platform
