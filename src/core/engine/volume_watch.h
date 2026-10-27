#pragma once
// Whether the volume that an operation reads is still attached (decision
// 0034). A device that goes away while a FULL verification, a registration or
// a relocation reads it makes every read fail, and what each read reports
// depends on the bus, the driver and the moment: an error of a removed device,
// but as often an I/O error (Linux EIO, the read in flight on Windows), "not
// found" for the files opened after it, or a timeout. An error is therefore
// classified only once the volume has been looked up again: gone, the
// operation is interrupted (INCOMPLETE, resumable); still attached, the error
// is the entry's (Unreadable, decision 0010).
#include "core/interfaces.h"

#include <filesystem>

namespace va {

// Whether `now` is the volume that `before` was: the same filesystem id (its
// full and short forms, and on Windows the volume GUID) and the same devices
// (each one's model, serial and WWN, in the order given). A device that went
// away takes these with it: the platform then finds no volume at the root,
// another one, or this one without its devices or their names. What may read
// otherwise while the volume stays attached (its label, the capacities, the
// rotational flag) is left out.
bool same_volume(const VolumeInfo& before, const VolumeInfo& now);

class VolumeWatch {
public:
    // `root`: where the operation reads; `volume`: the volume of the root as
    // the operation found it when it began.
    VolumeWatch(IMediaEnumerator& media, std::filesystem::path root, VolumeInfo volume);

    // Looks up the volume of the root again: true while it is the same volume
    // (same_volume), false once it is gone (the lookup fails, or gives another
    // volume) and every time after that. Asked on the operation's own thread,
    // only when a read failed.
    bool still_attached();

private:
    IMediaEnumerator& media_;
    std::filesystem::path root_;
    VolumeInfo volume_;
    bool gone_ = false;
};

} // namespace va
