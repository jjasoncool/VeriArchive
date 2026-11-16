---
status: accepted
date: 2026-11-16
amends: HANDOFF §B9 (beside the interfaces, the platform offers `DeviceWatch`), HANDOFF §B15 (`device_watch.h`, `device_watch_win.cpp`, `device_watch_linux.cpp`, `gui/device_changes.h`), HANDOFF §B17 (Archives and Devices read the attached devices at once when the system tells of a change), decision 0046 (point 4: the timed reads are no longer the only way to see a device attached or removed)
---

# The window reads the devices when the system tells of a change

Archives and Devices show where each archive is attached and which devices are there from a read of the attached devices, made when either page comes on screen, with Refresh, and every 30 seconds (decision 0046). A device attached or removed was therefore shown up to 30 seconds late. The owner asked whether the window can show it at once (2026-11-16).

## Decision (operator, 2026-11-16)

1. **The platform tells that the attached devices or volumes may have changed** (`platform::DeviceWatch`, beside the interfaces of §B9, as `KeepAwake` is; the core never uses it). It says only that something changed, never what: the window reads the devices as before (`IMediaEnumerator`). Without elevation (R-08, R-09); no device is opened.
   - **Windows**: a hidden top-level window on a thread of its own gets `WM_DEVICECHANGE`; it tells for `DBT_DEVICEARRIVAL`, `DBT_DEVICEREMOVECOMPLETE` and `DBT_DEVNODES_CHANGED`. Windows broadcasts a volume that got or lost its drive letter and a change of the device tree to top-level windows; for volumes without a drive letter the window asks for volume interfaces (`RegisterDeviceNotificationW`, `GUID_DEVINTERFACE_VOLUME`).
   - **Linux**: a thread waits, with `poll`, on `/proc/self/mountinfo`, which the kernel marks (`POLLPRI`, `POLLERR`) when the mounted filesystems change: the list the volumes are read from. A device attached and not mounted is not listed, and not told.
   - Where the system cannot be listened to, nothing is told, and the timed reads serve as before.

2. **The window reads once the changes are quiet.** One device attached is told several times within a moment (the device, its volume, the device tree), and its volume is mounted a little after the device arrives. The window reads once the changes have been quiet for 0.5 s, and at the latest 2 s after the first of them (`DeviceChanges`); a change told later asks for another read. The read is the one Refresh makes; one in flight is followed by another.

3. **Only while Archives or Devices is shown.** While neither is, a change reads nothing: either page reads when it comes on screen, as before.

4. **The timed reads stay**, every 30 seconds, for a change that the system does not tell of. Decision 0047's rule for the reads of devices' own serials that the window keeps is unchanged: they are kept only while the devices are watched, which the reads at a change make closer, not looser.

5. **Not in the screenshot mode or the smoke test**, whose devices are sample data. The command line has no use for it.

6. **The version is 1.3.1.** The repository's schema stays at version 4: 1.3.0 and 1.3.1 open the same repository.

## Why

The systems tell of these changes to any program, without elevation, so waiting for a timer was a choice of the first build, not a limit. Saying only "something changed" keeps the watch small and the read in one place: what Windows tells of a device differs between enclosures and drivers, and the media enumerator already reads the truth. Waiting for the changes to be quiet reads once for the several messages of one device; the limit of 2 s keeps a stream of changes from putting the read off.

Rejected alternatives:

- Reading only the device named in the message: what is told differs by system and enclosure, and the list must be matched against the archives as a whole anyway.
- Reading at every message: one device attached would be read several times in a second.
- Dropping the timed reads: a missed message would leave the pages wrong until Refresh.
- A libudev monitor on Linux: the volumes are read from the mount list (decision 0021), which the kernel already tells of.

## Consequences

- HANDOFF §B9, §B15 and §B17 carry a note naming this record; decision 0046 does too.
- Devices' explanation and both user guides say that the page reads again when a device or volume is attached or removed.
- Tests: `device_watch_test` (when the read is due: quiet for 0.5 s, at most 2 s after the first change, again for a later change; the watch starts, tells nothing once gone, two at once); on Windows `device_watch_win_test` (the window is hidden and top-level; it tells for the three messages, sent as Windows sends them, and for no other; and a real change of the volumes that needs no device: a drive letter made for a sample folder with `DefineDosDeviceW` and removed again, both told). Linux's watch is built and started in CI; no test there mounts a filesystem.
- Tried by hand on the owner's computer (2026-11-16, real devices, the window recorded every 0.2 s): a USB device (`G:`) unplugged while Devices was shown was gone from the page within 1 s of Windows no longer listing its volume; attached again, the device and its archive were shown at most 6.3 s after Windows listed the volume (the page was being scrolled, so the moment is an upper bound; a read was under way 1.6 s after). With Settings shown while it was unplugged and attached again, Archives showed the archive attached when it came on screen. A listener of `WM_DEVICECHANGE` beside the idle window got no message in 80 s: the window's own reads cause none.
- Not tried: a volume without a drive letter, a SATA device hot-plugged, Linux on a desktop.
