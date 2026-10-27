#pragma once
// Whether the device that an operation reads still holds its volume and still
// answers (decision 0034). A device that goes away while a FULL verification,
// a registration or a relocation reads it makes every read fail, and what each
// read reports depends on the bus, the driver and the moment: an error of a
// removed device, but as often an I/O error (Linux EIO, the read in flight on
// Windows), "not found" for the files opened after it, or a timeout. An error
// is therefore classified only once the volume has been looked up again:
// gone, the operation is interrupted (INCOMPLETE, resumable); still attached,
// the error is the entry's (Unreadable, decision 0010). A device that stops
// answering while the computer still lists its volume (a USB enclosure that
// hangs) fails every read with an I/O error, as a damaged area does: a run of
// such errors with no read between them that succeeds interrupts the
// operation as well.
#include "core/interfaces.h"
#include "core/scan/scanner.h"

#include <cstddef>
#include <filesystem>

namespace va {

// Decision 0034 (operator, 2026-10-27): this many reads in a row that fail
// with an I/O error (IoErrorKind::MediaError or Timeout), with no read
// between them that succeeds, while the volume stays attached, are taken for
// a device that stopped answering.
inline constexpr std::size_t io_errors_that_stop = 10;

// Why a read made an operation stop (decision 0034).
enum class DeviceStop {
    None,
    WentAway,     // the volume at the root is gone, or a read named a removed device
    NotAnswering, // io_errors_that_stop I/O errors in a row while the volume stayed attached
};

// What a read error is, as VolumeWatch::failed() tells it.
enum class ReadFailure {
    Entry, // the entry's own: Unreadable (decision 0010)
    // An I/O error while the volume stays attached: the entry's own once a
    // later read succeeds (VolumeWatch::answered), or once the operation has
    // read everything; more such errors in a row would make it the device's.
    // An operation that keeps what it classified before it stopped (the items
    // of an interrupted verification) holds it until then; one that keeps
    // nothing then (the scan, a registration) may mark it at once.
    Held,
    Stop,  // the device went away or stopped answering (VolumeWatch::stop): not classified
};

// Whether `now` is the volume that `before` was: the same filesystem id (its
// full and short forms, and on Windows the volume GUID) and the same devices
// (each one's model, serial and WWN, in the order given). A device that went
// away takes these with it: the platform then finds no volume at the root,
// another one, or this one without its devices or their names. What may read
// otherwise while the volume stays attached (its label, the capacities, the
// rotational flag) is left out.
bool same_volume(const VolumeInfo& before, const VolumeInfo& now);

// Told of the reads of one operation, on the operation's own thread, in the
// order they finish.
class VolumeWatch {
public:
    // `root`: where the operation reads; `volume`: the volume of the root as
    // the operation found it when it began.
    VolumeWatch(IMediaEnumerator& media, std::filesystem::path root, VolumeInfo volume);

    // A read succeeded (a file read to its end, a folder listed, a link's
    // target read): the device answers, and the I/O errors in a row count
    // from none again.
    void answered();

    // A read failed with `error`. Stop when the error names a removed device,
    // when the volume of the root, looked up again, is no longer the same
    // volume (same_volume), and when the error is the io_errors_that_stop-th
    // I/O error in a row; then and every time after, stop() says why. Held
    // for any other I/O error, Entry for every other error (permission, not
    // found, ...), which neither counts nor ends a run of I/O errors.
    ReadFailure failed(const IoError& error);

    // Why the operation must stop, once failed() has said Stop; None before.
    DeviceStop stop() const { return stop_; }

    // Has the scan of the operation tell this watch of its reads: each folder
    // listed and each link target read answers, and a folder or a link that
    // cannot be read ends the scan when failed() says Stop.
    void watch_scan(ScanOptions& options);

private:
    IMediaEnumerator& media_;
    std::filesystem::path root_;
    VolumeInfo volume_;
    std::size_t io_errors_in_a_row_ = 0;
    DeviceStop stop_ = DeviceStop::None;
};

} // namespace va
