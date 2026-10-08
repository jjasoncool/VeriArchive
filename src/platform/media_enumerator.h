#pragma once
// Factory for the platform's media enumerator (IMediaEnumerator in core/interfaces.h).
#include "core/interfaces.h"

#include <memory>

namespace va::platform {

std::unique_ptr<IMediaEnumerator> make_media_enumerator();

} // namespace va::platform
