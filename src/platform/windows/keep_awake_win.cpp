#include "platform/keep_awake.h"

#include "common/utf8.h"
#include "platform/windows/win_util.h"

#include <string>

namespace va::platform {

struct KeepAwake::Held {
    windows::UniqueHandle request;
};

KeepAwake::KeepAwake(const std::string& reason)
{
    // REASON_CONTEXT takes a writable string, which Windows copies when it
    // makes the request.
    const auto units = utf8_to_utf16(reason);
    std::wstring name(units.begin(), units.end());
    REASON_CONTEXT context{};
    context.Version = POWER_REQUEST_CONTEXT_VERSION;
    context.Flags = POWER_REQUEST_CONTEXT_SIMPLE_STRING;
    context.Reason.SimpleReasonString = name.data();
    windows::UniqueHandle request(PowerCreateRequest(&context));
    // Only the system is required: the display may turn off, and a sleep the
    // operator asks for (the Start menu, the power button, a laptop's lid)
    // still happens.
    if (request.valid() && PowerSetRequest(request.get(), PowerRequestSystemRequired)) {
        held_ = std::make_unique<Held>(std::move(request));
    }
}

KeepAwake::~KeepAwake()
{
    if (held_) {
        PowerClearRequest(held_->request.get(), PowerRequestSystemRequired);
    }
}

} // namespace va::platform
