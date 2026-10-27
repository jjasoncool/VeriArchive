---
status: accepted
date: 2026-10-08
resolves: HANDOFF Part D D1
amends: HANDOFF §B1.2 (`filesystem_type` values; the runtime-only `driver`, 2026-10-27), R-95
---

# `ntfs3` read-only is the reference Linux NTFS driver; driver names never reach identity or results

Linux can read Windows-written NTFS through the in-kernel `ntfs3` or the FUSE `ntfs-3g` (`fuseblk`). FUSE adds a kernel/user round trip per read, which threatens the M3 throughput criterion (≥ 80 % of `dd`), so `ntfs3` mounted read-only is the reference environment for acceptance tests and documentation. `ntfs-3g` is supported, not refused.

## Consequences

- `VolumeInfo.filesystem_type` is the on-disk format name (`ntfs`, `exfat`, `vfat`, `ext4`, …), taken from udev `ID_FS_TYPE` on Linux and the lower-cased `GetVolumeInformationW` name on Windows (`FAT32` → `vfat`). It is never the mount driver name (`ntfs3`, `fuseblk`, `exfat-fuse`); otherwise R-95 would add a `filesystem_type` warning to every cross-OS verify of the same healthy disk.
- When the verifying mount uses a non-reference driver, the report carries an informational environment note (`driver: ntfs-3g`). It does not change the run result: §A6 results describe the archive, and the same disk must not flip between `PASSED` and `PASSED_WITH_WARNINGS` depending on which driver happened to mount it.
- Mtime precision differences between drivers are already absorbed by the NTFS ±1 s tolerance (R-52).
- The note (2026-10-27). On Linux a volume mounted through FUSE (mount type `fuseblk` or `fuse.<name>`) is read by a program rather than by the kernel's driver of its format. `VolumeInfo` carries that program's name in a runtime-only field `driver` (`ntfs-3g` for NTFS, `exfat-fuse` for exFAT, otherwise the name after `fuse.` or `fuseblk.`, or the mount type), and a FULL verification's report notes it: "note: driver: ntfs-3g" on the command line, "Note: ..." in the GUI's Verify Archive, `notes` in `--json`. It is no warning: the result, the manifest identity and the media evidence never carry it. A mount through the kernel's driver (`ntfs3`, `exfat`) has none.
- Without the udev database (decision 0021) the mount's type stands in for the format, but a FUSE type names the program, never the format: the format is then read from the device's first sector (the name at byte 3 of NTFS and exFAT, the FAT type field), as blkid reads it for udev, where the process may read the device node (R-09); where it may not, the format is left unknown (empty), never `fuseblk`.
