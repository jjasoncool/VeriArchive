// The device watch on Linux (decision 0053): the kernel tells a reader of
// /proc/self/mountinfo, through poll, that the mounted filesystems changed
// (POLLERR and POLLPRI). The volumes are read from that list
// (mountinfo.cpp), so a volume that comes or goes changes it; a device that
// is attached and not mounted is not listed, and not told. Nothing needs
// elevation.
#include "platform/device_watch.h"

#include <fcntl.h>
#include <poll.h>
#include <unistd.h>

#include <cerrno>
#include <thread>
#include <utility>

namespace va::platform {

struct DeviceWatch::Watching {
    std::function<void()> changed;
    int mounts = -1;
    int stop[2] = {-1, -1};  // a pipe: closing its write end ends the thread
    std::thread thread;

    void run() const
    {
        for (;;) {
            pollfd waited[2] = {{mounts, POLLPRI, 0}, {stop[0], POLLIN, 0}};
            if (::poll(waited, 2, -1) < 0) {
                if (errno == EINTR) {
                    continue;
                }
                return;
            }
            if (waited[1].revents != 0) {
                return;
            }
            if ((waited[0].revents & (POLLPRI | POLLERR)) != 0) {
                try {
                    changed();
                } catch (...) {
                }
            } else if (waited[0].revents != 0) {
                return;  // the list can no longer be read
            }
        }
    }
};

DeviceWatch::DeviceWatch(std::function<void()> changed)
{
    auto watching = std::make_unique<Watching>();
    watching->changed = std::move(changed);
    watching->mounts = ::open("/proc/self/mountinfo", O_RDONLY | O_CLOEXEC);
    if (watching->mounts < 0) {
        return;
    }
    if (::pipe2(watching->stop, O_CLOEXEC) != 0) {
        ::close(watching->mounts);
        return;
    }
    try {
        watching->thread = std::thread([state = watching.get()] { state->run(); });
    } catch (...) {
        ::close(watching->mounts);
        ::close(watching->stop[0]);
        ::close(watching->stop[1]);
        return;
    }
    watching_ = std::move(watching);
}

DeviceWatch::~DeviceWatch()
{
    if (!watching_) {
        return;
    }
    ::close(watching_->stop[1]);
    if (watching_->thread.joinable()) {
        watching_->thread.join();
    }
    ::close(watching_->stop[0]);
    ::close(watching_->mounts);
}

bool DeviceWatch::watching() const
{
    return watching_ != nullptr;
}

void* DeviceWatch::native_window() const
{
    return nullptr;
}

} // namespace va::platform
