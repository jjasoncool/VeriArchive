---
status: accepted
date: 2026-10-15
resolves: HANDOFF Part D D9
amends: HANDOFF §A4 R-09 (`statfs` on the mount point), §B1.3 (`filesystem_uuid` on Linux when the UUID cannot be read), §B10 (comparing a kernel filesystem id)
---

# Without udev, Linux uses the kernel filesystem id and the SATA serial page

On Linux a volume's filesystem UUID comes from the udev database (`ID_FS_UUID`) or from the `/dev/disk/by-uuid` links, and a SATA or SCSI device's serial from udev (`ID_SERIAL_SHORT`); sysfs has a `device/serial` file only for some devices, such as NVMe. Containers and minimal systems have neither the udev database nor the `/dev/disk` links. A volume on a SATA or SCSI device then has no filesystem id and no device serial, so it is `UNKNOWN` to every archive: registration never refuses the same root twice (R-97), and verification by archive UUID never finds the archive.

The GitLab Linux runners are such containers (first seen on 2026-10-14, HANDOFF D9), and the device a job gets decides the result. On 2026-10-15 one run got an NVMe device: its serial came from sysfs, decision 0018 recognised the volume as `MATCH_WEAK`, and the end-to-end test passed. The next run got an ext4 volume on a virtual SCSI device (model `PersistentDisk`) that showed neither a UUID nor a serial: the second registration of the same root was accepted instead of being refused with exit 12, and the test failed.

## Decision

1. **Kernel filesystem id.** When neither udev nor `/dev/disk/by-uuid` gives the UUID of a volume on a block device, and its filesystem is ext2, ext3, ext4 or btrfs, `filesystem_uuid` holds the kernel's filesystem id: `f_fsid` from `statfs` on the mount point, its two 32-bit words combined with the first as the low half, written as `fsid-` and 16 lowercase hex digits (`fsid-1a2b3c4d5e6f7081`). A zero id is not recorded. Volumes without a block device (ZFS datasets, overlayfs, tmpfs) already used this form, for any filesystem and also when the id is zero; that is unchanged (see Consequences). `filesystem_uuid_short` stays empty.
2. **Comparing it.** An `fsid-…` id is compared only with another `fsid-…` id: equal is the same filesystem, anything else a different one, and the §B10 rows apply as they are. Against a UUID or a volume serial it counts as unavailable, so decision 0018 applies:

   | filesystem_uuid, seen and recorded | Comparison |
   |---|---|
   | both `fsid-…` | equal or different, like UUIDs |
   | one `fsid-…`, the other a UUID or serial | unavailable: device and volume capacity decide (decision 0018), detail `filesystem-id-unavailable` |
   | either empty | unavailable, as before |

3. **SATA device serial from VPD page 0x80.** When the udev database has no entry for a device, sysfs has no `device/serial`, and the device is a SATA device behind the Linux ATA layer (libata: its SCSI vendor in `/sys/block/<device>/device/vendor` is `ATA`), the serial is read from `/sys/block/<device>/device/vpd_pg80`, the SCSI Unit Serial Number page as the device returned it: byte 1 is the page code `0x80`, bytes 2–3 the length of the rest (big-endian), then the serial in ASCII. The spaces and NULs that pad it are removed. A page with another code, a length beyond its data, or a serial that is not printable ASCII gives no serial. Other devices get no serial this way.

Both fallbacks are used only where the usual source gives nothing, so nothing changes where udev is present: udev reports a UUID for every ext2/3/4 and btrfs volume except one whose UUID is all zeros, which has no kernel id either, and the serial page is read only for a device that the udev database does not know. Both are unprivileged reads: `/sys/block/*/` is in R-09's list, and `statfs` is the kernel call behind the `statvfs` listed there.

## Why

**The kernel id is the filesystem's own.** For ext2, ext3 and ext4 the kernel computes `f_fsid` from the UUID in the superblock (the two halves of the UUID combined). The kernel gives the same id on every machine and every mount, and it changes only when the UUID changes. For btrfs it is computed from the filesystem UUID and the id of the subvolume mounted there.

**Zero is no id.** ext2/3/4 report zero for a filesystem whose UUID is all zeros (`tune2fs -U clear`, some image builders). udev reports no UUID for such a filesystem even where it runs, so every one of them would read `fsid-0000000000000000`: an unrelated volume would be refused at registration as the same filesystem, and verification would ask for a relocation. Without an id such a volume is recognised only by its device, as before (decision 0018).

**Read with `statfs`, not `statvfs`.** The kernel reports `f_fsid` as two 32-bit words, and `statvfs` has the C library narrow them to one `unsigned long`: 64-bit glibc keeps both words (the first as the low half), while musl (Alpine containers) and 32-bit glibc keep only the first. The same volume would then read differently in an Alpine container and an Ubuntu one, which with a matching device is `MISMATCH`. `statfs` returns both words under every C library, and VeriArchive combines them as 64-bit glibc does, so ids recorded through `statvfs` on 64-bit glibc keep their value.

**Not xfs, and no other filesystem.** xfs, like many filesystems, derives `f_fsid` from the device number (major:minor), which depends on the order in which devices were found. The same volume would read differently on another machine or after a reboot, and two different volumes could read the same. Recording it would turn a healthy volume into `MISMATCH` and could make two volumes look like one. Other filesystems keep an empty id, and decision 0018 applies to them; one whose kernel id is shown to come from its UUID can be added later.

**A separate form, compared only with itself.** The kernel id is computed from the UUID but is not equal to it, so the `fsid-` prefix keeps the two apart in the matcher and for anyone reading media evidence or `media list`. When one side has a UUID and the other the kernel id (registered with udev, verified without it, or the reverse), the comparison says nothing either way. That is decision 0018's case of an id unavailable on one side.

**VPD page 0x80 only for SATA devices.** For a SATA device the Linux ATA layer answers page 0x80 with the ATA serial number, and udev reports the same serial as `ID_SERIAL_SHORT` (it runs `ata_id` for devices whose vendor is `ATA`). For other devices udev takes `ID_SERIAL_SHORT` from elsewhere: `scsi_id` prefers page 0x83 (a world-wide name) for SAS, virtio-scsi and other SCSI devices, and USB devices give their USB serial. A page 0x80 serial for those would differ from the serial recorded where udev runs. In a container given `/dev` but not the udev database, where the UUID is still readable from `/dev/disk/by-uuid`, the device would then read as different and the archive as `MIGRATED`: verification would stop and ask for a relocation, and registration would refuse. Without a serial the device is unavailable and the volume `MATCH_WEAK`, as before. sysfs shows the page and the vendor to every user, so no device node is opened and no privilege is needed (R-09).

Rejected alternatives:

- Computing the kernel id from a recorded UUID, to compare the two exactly. For btrfs the recorded evidence lacks the subvolume that is part of the id, and the rule would tie identity matching to one kernel's formula. Decision 0018's rule already covers the mixed case.
- Taking `f_fsid` for every filesystem. For xfs and many others it follows the device number, as above.
- Reading page 0x80 for every SCSI device. For most of them its serial is not the one udev reports, as above.
- Requiring udev on Linux. VeriArchive would then not work in containers, including the CI that tests it.

## Consequences

- The Linux CI end-to-end test no longer depends on the device a runner gets. On an ext4 volume on a virtual SCSI device without udev (vendor `Google`, model `PersistentDisk`), registration records `fsid-…` and no serial. Registering the same root again is then `MATCH_WEAK` and is refused with exit 12. Verification finds the archive by its root and by its UUID.
- Two ext2/3/4 or btrfs volumes without udev are told apart by their kernel ids even on one device. Decision 0018 could tell them apart only by size.
- A volume registered with udev and verified without it, or the reverse, is `MATCH_WEAK` with `filesystem-id-unavailable` when the device and the volume capacity match (decision 0018). For SATA devices the page 0x80 serial is the one udev reports, so the device matches. Other devices have no serial without udev, so the volume stays `UNKNOWN`, as before. Registering and verifying in the same environment is unaffected.
- A volume whose UUID is all zeros has no kernel id, just as it has no UUID with udev: on a device whose serial is readable it is recognised by its device and size (decision 0018), otherwise it stays `UNKNOWN`, as before.
- Volumes without a block device keep the kernel id they had before, of any filesystem and also when it is zero. FUSE and NFS report zero, so all of them read as one volume, `fsid-0000000000000000`, and `media list` shows only one of them. Leaving their id empty instead would turn archives registered on them from `MATCH_WEAK` into `UNKNOWN`, so that needs its own decision.
- A container that does not show a volume's device node (an unprivileged container given the volume by a bind mount) describes it as a volume without a block device: whatever its filesystem, including xfs, it is identified by its kernel id, as before this decision. For xfs and the other filesystems whose id follows the device number, such a volume can read differently after a reboot or on another machine (`UNKNOWN`), and two volumes can read the same. Excluding those filesystems there would also turn existing archives from `MATCH_WEAK` into `UNKNOWN`, so that also needs its own decision.
- On btrfs the kernel id also names the subvolume mounted at the mount that holds the archive. Mounting another subvolume there, such as a snapshot after a rollback, gives another id: the archive is then `MISMATCH` with a matching device, otherwise `UNKNOWN`. With udev the UUID does not change.
- Archives registered without udev before this decision have an empty `filesystem_uuid`. Against the new `fsid-…` reading it counts as unavailable, so decision 0018 applies as before.
- Kernel ids recorded before this decision on 64-bit glibc keep their value. Under musl or 32-bit glibc the ids recorded before (only for volumes without a block device) hold the first word alone; such a volume now reads the full id, and where the second word is not zero its archive is `UNKNOWN` until it is registered again.
- No format change: `identity.filesystem_uuid` in the manifest header (§B4.3) and in media evidence (R-96) is a string, and the golden manifest is unchanged.
