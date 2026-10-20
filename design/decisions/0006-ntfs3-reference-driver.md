---
status: accepted
date: 2026-10-08
resolves: HANDOFF Part D D1
amends: HANDOFF §B1.2 (`filesystem_type` values), R-95
---

# `ntfs3` read-only is the reference Linux NTFS driver; driver names never reach identity or results

Linux can read Windows-written NTFS through the in-kernel `ntfs3` or the FUSE `ntfs-3g` (`fuseblk`). FUSE adds a kernel/user round trip per read, which threatens the M3 throughput criterion (≥ 80 % of `dd`), so `ntfs3` mounted read-only is the reference environment for acceptance tests and documentation. `ntfs-3g` is supported, not refused.

## Consequences

- `VolumeInfo.filesystem_type` is the on-disk format name (`ntfs`, `exfat`, `vfat`, `ext4`, …), taken from udev `ID_FS_TYPE` on Linux and the lower-cased `GetVolumeInformationW` name on Windows (`FAT32` → `vfat`). It is never the mount driver name (`ntfs3`, `fuseblk`, `exfat-fuse`); otherwise R-95 would add a `filesystem_type` warning to every cross-OS verify of the same healthy disk.
- When the verifying mount uses a non-reference driver, the report carries an informational environment note (`driver: ntfs-3g`). It does not change the run result: §A6 results describe the archive, and the same disk must not flip between `PASSED` and `PASSED_WITH_WARNINGS` depending on which driver happened to mount it.
- Mtime precision differences between drivers are already absorbed by the NTFS ±1 s tolerance (R-52).
