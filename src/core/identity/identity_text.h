#pragma once
// The identity strings of a volume and its devices as the platform reads them,
// made fit for the manifest header and the media evidence (decision 0035).
#include "common/types.h"

namespace va {

// `volume` with each of its identity strings made valid UTF-8: the filesystem
// type and id (full and short form), the volume label and GUID, the container,
// and of each device its reference, model, serial, WWN and bus. Every byte
// that belongs to no valid UTF-8 sequence becomes U+FFFD; valid text stays as
// it is. The platform gives these strings as the hardware or the filesystem
// holds them: a USB bridge may report bytes that are no text in its model or
// serial, and a volume label may be in a legacy code page, which Linux's udev
// passes on as raw bytes. The manifest header is UTF-8 (§B4.1, R-64) and the
// media evidence JSON: neither can hold such bytes and be read back. Both
// platforms apply it to every volume they describe (IMediaEnumerator), so
// the same bytes read on Windows and on Linux give the same text.
VolumeInfo with_valid_identity_text(VolumeInfo volume);

} // namespace va
