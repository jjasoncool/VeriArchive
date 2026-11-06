# VeriArchive HANDOFF — Revision 2

Date: 2026-09-15
Supersedes: Revision 1

## How to read this document

| Part | Content | Stability |
|---|---|---|
| A | Principles | Stable. Change only with a documented reason. |
| B | Contracts: data model, path rules, manifest format, repository layout, DB schema, core interfaces, identity matching | **Frozen for V1 / V1.1.** Changing these after M3 means rework. |
| C | Milestones and acceptance criteria | Updated per milestone |
| D | Open decisions | The agent MUST NOT resolve these by assumption |
| Appendix | Agent quick rules (copy into `CLAUDE.md`) | |

Keywords MUST / MUST NOT / SHOULD / MAY follow RFC 2119. Numbered requirements (`R-nn`) are referenced by tests.
Each rule has exactly one authoritative section; other sections reference it and do not restate it.

### Changes from Revision 1

- Archive unit defined: one logical file tree on one volume; chunks are per-file; whole-disk hash demoted to a media fingerprint (§B1)
- Manifest `.vam` v1 fully specified; validated manifest bytes are the source of truth, SQLite is a rebuildable index (§B4, §A5)
- Privilege model decided: V1 fully unprivileged; elevated helper deferred to V1.1 behind already-defined interfaces (§A4)
- RepositoryRoot resolution fixed: configured-but-unavailable is an error; fallback only at first initialisation; location pinned (§B7.4)
- `archive_uuid` is the key; `ARCH-NNNN` is a label; export/import and revision rollback protection defined (§B7.5, R-15)
- Media identity split into device / volume / archive; matching decision table added (§B1.2, §B10)
- Trust anchor, signature file format, key ids, hash-chained history defined (§A5, §B5, §B8)
- Filename encoding, entry types, exclusions, metadata tolerance defined (§B2, §B3)
- Result states expanded; progress / cancel / resume required in core from V1 (§A6, §B9, §B11.4)
- `IBlockReader` → `IByteSource` with error taxonomy; static `AppPaths` → injected `AppContext` (§B9, §B7.2)
- Toolchain minimums, dependency direction, CI checks, packaging checks (§A3, §B15)

---

# Part A — Principles

## A1. Purpose and scope

> Amended by `design/decisions/0017-interface-language.md`: reports and the GUI state R-02's guarantee without naming a version of the program, "VeriArchive detects accidental corruption and unintended change; tamper evidence (signed manifests) is not part of this version."; the milestones V1 and V1.1 are not the program's version numbers.

VeriArchive registers backup/archive media, records integrity evidence, and later verifies whether archived content has been modified, corrupted, deleted, or added to. Windows and Linux, one codebase.

- R-01 VeriArchive detects. It does not repair, restore, or create backups.
- R-02 V1 detects accidental corruption and unintended change. Tamper evidence (signed manifests, pinned keys, signed history checkpoints) begins in V1.1. Reports and the GUI MUST state which guarantee applies.
- R-03 The application MUST NOT write to archive media unless the operator explicitly enables the media-copy feature (§B7.6).

Priority (operator, 2026-10-08): verification is the core function. Relocation (decision 0004) exists only so an archive stays verifiable on a new carrier; the operator copies the data with their own tools and VeriArchive only verifies the copy (R-01). V1 targets data disks; system-volume specifics (cloud placeholders, deduplication, files in use) get safety guards, not dedicated features.

## A2. Cross-platform first

Target: 80–90 % shared code. Only OS / hardware / device / filesystem integration is platform-specific.

Bad:

```text
platform/windows/{hash,verify,manifest,disk}.cpp
platform/linux/{hash,verify,manifest,disk}.cpp
```

Correct:

```text
core/{hash,manifest,merkle,identity,scan,engine,repository,signing}/
platform/{windows,linux}/
```

## A3. Layering and dependency direction

```text
veriarchive_common    types, errors, UTF-8, ArchivePath, canonical JSON, config, RepositoryLayout
        ▲
veriarchive_core      hash, manifest, merkle, identity matching, scanner, register/verify engines,
                      repository (SQLite), progress/cancel/resume
        ▲
veriarchive_platform  implementations of the core interfaces (§B9): platform/windows, platform/linux
        ▲
veriarchive_cli / veriarchive_gui / veriarchive_helper   composition roots
```

- R-04 `common` and `core` MUST NOT include OS headers (`<windows.h>`, `<libudev.h>`, `<sys/ioctl.h>`, …) and MUST NOT link `veriarchive_platform`. The single permitted `#ifdef _WIN32` in `common/` is the `std::filesystem::path::value_type` encoding conversion in `archive_path.cpp` (a standard-library type difference, not an OS API).
- R-05 Platform objects are constructed only in a composition root (`cli/`, `gui/`) and injected into core through interfaces.
- R-06 Platform source selection happens in CMake (`if(WIN32)` / `elseif(UNIX AND NOT APPLE)`), never as `#ifdef` inside core logic.
- R-07 No global mutable state. Configuration and repository layout are resolved once at startup into value objects (`AppContext`, §B7.2) and injected.

## A4. Privilege model

> Amended by `design/decisions/0021-linux-without-udev.md`: R-09 on Linux also uses `statfs` on the mount point (the kernel call behind `statvfs`) for the kernel filesystem id.
> Amended by `design/decisions/0027-subst-drive-letters.md`: R-09 on Windows also uses `QueryDosDeviceW`, which resolves a drive letter made with subst to the folder it stands for before the volume is looked up.
> Amended by `design/decisions/0029-network-roots-are-refused.md`: R-09 on Windows also uses `GetDriveTypeW`, which tells a drive letter mapped to a network share, so that a root on one is refused before its volume is looked up.
> Amended by `design/decisions/0037-what-opening-the-repository-remembers-and-says.md`: R-12's warning comes first from every command that opens the repository, also where the repository cannot be opened, and from `media list`, which opens none; the GUI's Settings shows it also under the reason no repository is open.
> Amended by `design/decisions/0039-exclusions-helper-and-pinned-keys-are-not-built.md`: there is no `veriarchive-helper` and no IPC (R-11): the NTFS USN change journal is read in the main process without elevation (R-107); reads with `SeBackupPrivilege` / `CAP_DAC_READ_SEARCH` are put off until an archive needs them; raw device reads serve only V2's whole-disk fingerprint (§B14), which decides how they are made; a call of R-09 that needs elevation is left out, not worked around, until a decision record says how it is made.
> Amended by `design/decisions/0047-a-full-verification-may-accept-a-device-behind-a-changed-enclosure.md`: one program of VeriArchive runs with administrator permission, the device identity reader (`veriarchive-device-identity.exe`, Windows only, its manifest `requireAdministrator`, linked statically): the window alone starts it, through Windows' question for that permission (UAC, `ShellExecuteExW` "runas"), on the operator's request, to read a device's own model, serial and WWN behind its enclosure with ATA IDENTIFY DEVICE alone, nothing of the device's content; the window and the command line still run unprivileged and never elevate themselves (R-08, R-12), the command line never starts it, and no IPC, service or resident helper exists (R-11 as decision 0039 left it). Media identity stays unprivileged (R-09): the media enumerator tells, without elevation, that an enclosure hides the device's identity (Windows: the device identification page, VPD `0x83`, through `IOCTL_STORAGE_QUERY_PROPERTY` with `StorageDeviceIdProperty` on a handle opened without access rights; Linux: udev), and a read of the device's own identity serves a carrier confirmation, never identity matching. Linux has no reader.

- R-08 V1 runs entirely unprivileged. The GUI MUST never require Administrator/root (GTK under Wayland refuses to run as root; elevated GUIs are an anti-pattern).
- R-09 V1 media identity uses unprivileged APIs only.
  Windows: device/volume handles opened with `dwDesiredAccess = 0` for `IOCTL_STORAGE_QUERY_PROPERTY`, `IOCTL_STORAGE_GET_DEVICE_NUMBER`, `IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS`; `GetVolumeInformationW`, `GetVolumeNameForVolumeMountPointW`; SetupAPI enumeration.
  Linux: libudev property database (`ID_SERIAL_SHORT`, `ID_MODEL`, `ID_WWN`, `ID_FS_UUID`, `ID_FS_LABEL`, `ID_FS_TYPE`), `/sys/dev/block/<maj:min>/`, `/sys/block/*/`, `statx`/`statvfs` on the mount point, `/proc/self/mountinfo`. libblkid only on device nodes the process can already read.
  M2 MUST confirm each call works unprivileged on the target OS versions; any call that turns out to need elevation moves to helper scope (R-11), it is not worked around.
- R-10 Files the process cannot read are reported as `Unreadable(permission)`, never silently skipped.
- R-11 Operations that require elevation — raw device reads (`\\.\PhysicalDriveN`, `/dev/sdX`), NTFS USN journal, `SeBackupPrivilege` reads, `CAP_DAC_READ_SEARCH` reads — are V1.1 and are provided by a separate helper executable (`veriarchive-helper`) reached over local IPC (Windows named pipe, Linux Unix socket with polkit authorisation). Core MUST depend only on `IRawDeviceAccess` and `IChangeTracker` (§B9) so the helper is added without touching core.
- R-12 The main process MUST NOT self-elevate (no UAC relaunch of itself, no `sudo` re-exec). Launching an elevated *helper* is permitted. When the main process detects it is already elevated it prints a warning (home directory and repository pinning may differ).

## A5. Security model

Assume an attacker who may: modify/delete/add archive files; alter the SQLite database; copy `identity.json`; recreate checksum files; hold administrator/root on the verifying machine.

- R-13 Trust comes from a signature that validates against a pinned public key (§B5), never from where a file is stored. Once signing exists, a manifest copy on the media is exactly as trustworthy as the repository copy — if and only if its signature validates.
- R-14 SQLite is an index and history store, never an integrity source. Verification MUST compare against the parsed, validated manifest bytes (V1: manifest bytes whose header/body BLAKE3 match the repository record; V1.1: signature-validated).
- R-15 Rollback protection: every manifest carries `revision`; the repository records the highest known revision per archive; import and verify MUST reject a manifest with a lower revision than already known unless the operator passes `--force-rollback`.
- R-16 Sampling (QUICK/DEEP) MUST use a per-run CSPRNG seed, recorded for reproducibility. Fixed deterministic sampling is forbidden: an attacker who knows the sample can modify unsampled chunks indefinitely.
- R-17 Change trackers (USN) may only *escalate* the required verification level. They MUST NOT be used as evidence that content is unchanged.
- R-18 Verification history rows are hash-chained (`prev_hash`, `row_hash`, §B8) from V1; V1.1 adds signed checkpoints so deleted or edited FAIL records become detectable.
- R-19 The exclusion list is embedded in the manifest header and therefore signed (§B3.3). Changing exclusions creates a new revision.
- R-20 Private signing keys MUST NOT live inside RepositoryRoot. `ISigner` (§B9) is the only access path so hardware tokens / external signing services can replace the file signer.

## A6. Result semantics (authoritative)

### Run result

> Amended by `design/decisions/0032-registration-and-relocation-write-chunk-lines.md`: `PASSED_WITH_WARNINGS` also when a file's content matches both its file hashes while a chunk hash of it does not: the file is `Verified`, and the run warns that its chunk hashes cannot be relied on and that a new revision fixes them; and the other way round, when every chunk hash of a file with chunk lines matches while a file hash of it (BLAKE3, SHA-256 or both) does not: the file is `Verified`, and the run warns that the whole-file hash cannot be relied on and that a new revision fixes it. A file whose chunks do not all match is `Modified`.

> Amended by `design/decisions/0034-a-device-that-goes-away-interrupts-the-read.md`: a read error is the entry's only while the volume of the root is still attached, as a lookup of it at once tells (the same filesystem id and devices); a volume that is gone makes the verification `INCOMPLETE`, the registration `REGISTRATION_INCOMPLETE` and the relocation's attempt interrupted, the entry whose read failed unclassified; so do ten reads in a row that fail with an I/O error, with no read between them that succeeds, while the volume stays attached (a device that stopped answering, operator 2026-10-27), none of those entries classified, and the report says to attach the device again and resume.

> Amended by `design/decisions/0044-device-alerts-a-devices-health-and-a-slower-read.md`: a device alert (a device that reports its own health as failing, or a FULL verification that read markedly slower than the last usual one on that device) is told beside the run result and never changes it, nor the exit code; it is no warning of this table.

| Result | Meaning |
|---|---|
| `PASSED` | All checks of the mode passed, no warnings. |
| `PASSED_WITH_WARNINGS` | Content checks passed; non-content issues exist: metadata mismatch within policy, `MATCH_WEAK` identity (no hardware serial), portability warnings, media copy differs from repository copy. |
| `FAILED` | At least one content, identity, signature, revision, or continuity check failed. |
| `INCOMPLETE` | Cancelled, interrupted, or device disappeared. No PASS/FAIL claim. Resumable (§B11.4). |

### Mode label

Every result is prefixed by its mode. Only `FULL VERIFY PASSED` means "100 % of registered content was read and matched". `QUICK VERIFY PASSED` / `DEEP VERIFY PASSED` mean "identity, manifest, and the recorded sample passed"; the UI MUST show coverage (bytes and chunks) next to any sampled result.

### Item status

> Amended by `design/decisions/0010-unreadable-entries-at-registration.md`: adds `Unbaselined`; maps unreadable entries to run results (newly unreadable → `FAILED`; unreadable since registration → at most `PASSED_WITH_WARNINGS`); `Unreadable(removed)` is never reported: a device that disappears ends the run `INCOMPLETE`; a run with `Unbaselined` items whose content otherwise matched says that registering a new revision takes a baseline for them, unless the archive is retired.
> Amended by `design/decisions/0034-a-device-that-goes-away-interrupts-the-read.md`: `Unreadable(removed)` is never produced: a device that went away or stopped answering makes the run `INCOMPLETE` and no item. A `SkippedMountPoint` where the revision records nothing at or below the mount point is listed and changes no result (operator, 2026-10-27; decision 0009 describes both forms).

`Verified`, `Modified`, `Missing`, `New`, `Unreadable(permission|media|removed|other)`, `MetadataMismatch`, `Unrepresentable`, `TypeChanged`, `SkippedMountPoint`.

### Run-level failure classes

> Amended by `design/decisions/0028-a-manifest-that-cannot-be-used.md`: a manifest that cannot be used fails its verification with `ManifestHashMismatch` when it has changed (its line 1 or its body is not the one recorded) and with `RepositoryError` when it is missing or cannot be read, or the index says otherwise of a manifest whose line 1 is the one recorded (its copy of the header, or a record that names another body, archive or revision than that line), or has no record of the archive's revision (as where `repo rebuild-index` could not read its manifest); a verification of an archive named by its uuid or by the run it resumes fails so before it looks for the media, `FAILED` without reading it, and says so.

`MediaIdentityMismatch`, `MediaIdentityAmbiguous`, `ManifestSignatureFailure`, `ManifestHashMismatch`, `ManifestRevisionRollback`, `ChangeTrackerContinuityLost`, `FilesystemAccessError`, `RepositoryError`.

- R-21 The UI MUST NOT display a bare "integrity status". It displays "Last FULL verify: <result> on <date>" and "Last verify (<mode>): <result> on <date>" plus an `OVERDUE` flag from policy (§B11.5). A two-year-old PASS is not a current state.

## A7. Non-goals (V1 / V1.1)

Cloud sync, central server, Active Directory, users/RBAC, web UI, remote agents, automatic deletion, backup job creation, repair/restore, encryption key management (BitLocker/LUKS are unlocked by the OS; VeriArchive registers the unlocked filesystem).

## A8. Coding rules

Use: C++23, `std::filesystem::path` for native paths, `std::expected<T, IoError>` for fallible calls, RAII, strong types, UTF-8 internal strings, unit tests per target.

Windows specifics:
- R-22 Entry point is `wmain` linked with `-municode`, plus the UTF-8 `activeCodePage` application manifest. Never rely on ANSI `main(argv)`: CP950 conversion is lossy and Best-Fit mapping is a known injection vector.
- R-23 The application manifest sets `longPathAware=true`; the platform layer additionally applies the `\\?\` prefix to any path that may exceed 260 characters.
- R-24 Persist paths only via `path::u8string()` or explicit UTF-16→UTF-8 conversion. `path::string()` is forbidden.

Avoid: global mutable state, hard-coded drive letters or `/dev` names, manual slash concatenation, platform API calls in core, copy-pasted platform implementations, `std::filesystem::current_path()` for anything except diagnostics, exceptions crossing the platform boundary.

---

# Part B — Contracts

## B1. Archive unit and data model

### B1.1 Definition

> Amended by `design/decisions/0029-network-roots-are-refused.md`: a root on a network share (Windows: a network path, or a drive letter that Windows reports as a network drive; Linux: a network filesystem such as NFS, SMB or sshfs) is on no volume of this computer; `register`, `verify` by root and `archive relocate` refuse it before anything of it is read, and on Windows also a root written in the `\\?\` or `\\.\` form, except a volume's GUID path in the `\\?\` form (`\\?\Volume{...}\`), the only name of a volume that has no drive letter or folder path: it names the volume's root folder, and through it a folder is the same archive as through the volume's drive letter; written with `.`, `..` or separators in a row after the GUID, it names the folder they resolve to, which is the folder read, as Windows reads that form as given.
> Amended by `design/decisions/0029-network-roots-are-refused.md` (point 6): registration refuses a root that holds RepositoryRoot, or lies inside it, before anything of it is read (exit 10): the repository is no part of an archive, and it changes while the root is read. The two are compared by the paths the file system names them by, so that short names, links, drive letters made with subst and GUID paths are told too.
> Amended by `design/decisions/0009-only-name-surrogates-are-links.md`: R-30: on Windows a mount point is a mount-point reparse point whose target is a volume; every verification reports each mount point below the root as `SkippedMountPoint`, which changes no result where the revision records nothing at or below it, and fails the run through each recorded entry it covers.

- R-30 An **archive** is a logical file tree rooted at `archive_root`: a mounted volume root, or a directory on exactly one volume. Registration MUST refuse a root spanning multiple volumes; mount points below the root are not descended and are reported as `SkippedMountPoint`. Linux determines mount boundaries from `/proc/self/mountinfo`, not `st_dev` (btrfs subvolumes have distinct `st_dev` on the same volume).
- R-31 Chunks (V1.1) are per-file; `offset` is relative to the start of the file. A file with `size ≤ chunk_size` has exactly one chunk equal to the whole file, so its chunk hash equals its file BLAKE3 and no chunk record is written.
- R-32 The whole-disk hash (V2) is a **media fingerprint** stored in `media_evidence.disk_fingerprint`, never archive content and never part of the manifest.

### B1.2 Types (`common/types.h`)

> Amended by `design/decisions/0006-ntfs3-reference-driver.md`: `filesystem_type` is the on-disk format name, never the mount driver name; `VolumeInfo` also carries a runtime-only `driver`, the program that serves a FUSE mount (`ntfs-3g`), which a verification report notes and identity never holds.
> Amended by `design/decisions/0035-identity-strings-are-valid-utf-8.md`: every string of `VolumeInfo` and `PhysicalDeviceInfo` that the platform gives is valid UTF-8: each byte that belongs to no valid UTF-8 sequence becomes U+FFFD.
> Amended by `design/decisions/0036-the-repository-database-made-again.md`: R-33's automatic label skips every number that a label of the repository already has (typed, imported, or given back by `repo rebuild-index` into a new database); labels stay without a uniqueness constraint.
> Amended by `design/decisions/0044-device-alerts-a-devices-health-and-a-slower-read.md`: `HealthStatus` (`Unknown`, `Healthy`, `Warning`, `Unhealthy`) and `DeviceHealth` (its status and the conditions it reports); `PhysicalDeviceInfo::health`, what the device reported when an operation started to read its volume, runtime only and never identity.
> Amended by `design/decisions/0047-a-full-verification-may-accept-a-device-behind-a-changed-enclosure.md`: `PhysicalDeviceInfo::enclosure_hides_identity`, runtime only and never identity, kept in media evidence: the device is behind an enclosure that hides its own identity, so its `serial` and `wwn` are the enclosure's (never for removable media; false where nothing tells); `DeviceIdentity` (`model`, `serial`, `wwn`), a device's own identity as the device identity reader reads it, and `DeviceIdentityRead` (that identity, `how` it was read, `read_at`, `device_ref`).

```cpp
struct PhysicalDeviceInfo {
    std::string device_ref;          // opaque, platform-specific ("\\.\PhysicalDrive2", "/dev/sdb"); runtime only
    std::string model;
    std::string serial;              // may be empty (USB bridges)
    std::string wwn;                 // may be empty
    std::uint64_t capacity_bytes = 0;
    std::optional<bool> rotational;  // nullopt = unknown; operator override allowed
    std::string bus;                 // "usb" | "sata" | "nvme" | "scsi" | ""
};

struct VolumeInfo {
    std::filesystem::path mount_point;     // runtime only, never persisted as identity
    std::string filesystem_type;           // "ntfs" | "exfat" | "vfat" | "ext4" | "xfs" | "btrfs" | ...
    std::string filesystem_uuid;           // normalised (§B1.3)
    std::string filesystem_uuid_short;     // 32-bit serial as 8 hex for NTFS/exFAT/FAT; empty otherwise
    std::string volume_label;
    std::string volume_guid;               // Windows only, may be empty
    std::uint64_t capacity_bytes = 0;
    bool read_only = false;
    std::vector<PhysicalDeviceInfo> devices;   // usually 1; >1 for LVM / Storage Spaces / RAID
    std::string container;                 // "" | "bitlocker" | "luks" | "lvm" | "md" | "storage-spaces"
};

struct ArchiveIdentity {                 // VeriArchive-managed
    Uuid archive_uuid;                     // the key everywhere
    std::string label;                     // "ARCH-0007"; display only; not unique across repositories
    std::uint32_t revision = 0;
    std::string root_subpath;              // "" = volume root, else "Backups/2026" (ArchivePath form)
    Timestamp created;
};

struct MediaEvidence {                   // observed at register / verify time
    Timestamp seen_at;
    VolumeInfo volume;
    std::optional<Uuid> claimed_archive_uuid;   // from .veriarchive/identity.json — a claim, not a proof
};
```

- R-33 `archive_uuid` is the identity used for all keys, filenames and IPC. `label` is a human alias assigned by the registering repository (`settings.next_label_seq`, `ARCH-` + zero-padded ≥ 4 digits), stored in the manifest, and MUST NOT be unique-constrained in the database.

### B1.3 Filesystem UUID normalisation (known cross-platform pitfall)

> Amended by `design/decisions/0021-linux-without-udev.md`: on Linux, when neither udev nor `/dev/disk/by-uuid` gives the UUID of an ext2/3/4 or btrfs volume, `filesystem_uuid` holds the kernel filesystem id (`statfs` `f_fsid`, both 32-bit words, first word low; never zero) as `fsid-` + 16 lowercase hex, the form volumes without a block device already use; an `fsid-…` id is compared only with another `fsid-…` id (§B10).

- ext4/XFS/btrfs: lowercase RFC 4122 string, identical on both OSes.
- exFAT/FAT32: 32-bit volume serial; store as 8 lowercase hex (`a1b2c3d4`), no dash; identical on both OSes.
- NTFS: Linux (`blkid`/udev) reports the 64-bit boot-sector serial as 16 hex; Windows `GetVolumeInformationW` returns a 32-bit value. Windows MUST attempt `FSCTL_GET_NTFS_VOLUME_DATA` for the 64-bit serial and fall back to the 32-bit value. M2 MUST establish the exact relationship between the two on the same medium and encode it in a unit test; matching (§B10) uses `filesystem_uuid` when both sides have 64-bit values, otherwise `filesystem_uuid_short`.

## B2. Path and filename rules

> Amended by `design/decisions/0008-link-target-canonical-form.md`: R-44 holds only where another OS registered the revision (`created_by.os`); on the OS that registered it, a recorded entry not found now is `Missing`.
> Amended by `design/decisions/0042-what-a-revision-records-is-not-called-registered.md`: the OS that registered the revision, above, is the one that recorded it (`created_by.os`): that of its registration, or of the relocation or verification adding chunk hashes that recorded it.

Manifest paths (`ArchivePath`, `common/archive_path.*`) are relative to `archive_root`.

- R-40 Separator is `/`. On Windows both `\` and `/` in native names are separators. On Linux `\` is an ordinary character and MUST NOT be translated.
- R-41 Encoding: `p` holds the name as UTF-8. If the native name is not losslessly representable as valid UTF-8 (Linux: arbitrary bytes such as Big5 names; Windows: unpaired surrogates), the entry also carries `enc` and `raw`:
  - Linux: `enc:"bytes"`, `raw:` lowercase hex of the native bytes of the full relative path.
  - Windows: `enc:"utf16le"`, `raw:` lowercase hex of the native UTF-16LE code units of the full relative path.
  Then `p` is the lossy display form (invalid sequences → U+FFFD) and `raw` is the identity. Tests MUST cover both cases.
- R-42 No Unicode normalisation is applied; names are stored exactly as read. The scanner detects NFC/NFD collisions and case-insensitive collisions within one directory and records `warn:["nfc-collision"]` / `warn:["case-collision"]` (portability warnings, not failures).
- R-43 Canonical order is bytewise ascending on `p` (UTF-8), then on `raw`, then entry type (`f`, `d`, `l` before `s`), then `stream`. Never rely on `directory_iterator` order or locale collation; core sorts after the scan.
- R-44 A manifest name that cannot exist on the verifying platform (Windows: `< > : " | ? *`, control characters, trailing space or dot, reserved names `CON PRN AUX NUL COM1-9 LPT1-9`, component > 255 UTF-16 units) is reported as `Unrepresentable`, not `Missing`.
- R-45 Absolute paths, drive letters and mount points MUST NOT appear in the manifest body. The header MAY carry `root_hint` for display only.

## B3. Entry types, metadata, exclusions

### B3.1 Entry types

> Amended by `design/decisions/0008-link-target-canonical-form.md` (canonical link target, cross-OS comparison) and `design/decisions/0009-only-name-surrogates-are-links.md` (on Windows only name-surrogate reparse points are `l`; other reparse points are `f`; recall-on-access files are never opened).
> Amended by `design/decisions/0020-archive-comparison.md`: R-50 also where a directory has contents, which no entry records (R-49): told from its descendants, a file or link replaced by a folder with contents, or a folder with contents replaced by a file or link, is `TypeChanged` beside the folder's entries (`New` or `Missing`), in a comparison and in a verification.

| `t` | Entry | What is hashed |
|---|---|---|
| `f` | regular file | file bytes |
| `d` | directory (recorded when empty) | nothing |
| `l` | symbolic link (Linux); symlink, junction or other reparse point (Windows) | the link target string as UTF-8 |
| `s` | NTFS alternate data stream (V1.1, `FindFirstStreamW`) | stream bytes; `p` = owning file, `stream` = stream name |

- R-46 Symlinks are never followed. A link whose target is outside the root is still recorded as `l`.
- R-47 Hardlinks are independent entries, each hashed.
- R-48 Sparse files are hashed by logical content.
- R-49 Empty directories are recorded (`d`) so their disappearance is detectable.
- R-50 A change of entry type at the same path is reported as `TypeChanged`.

### B3.2 Metadata

- R-51 Content hash is the integrity criterion. Metadata differences are `MetadataMismatch` warnings, never failures, unless `--strict-metadata` is set.
- R-52 `mtime_ns` is int64 nanoseconds since the Unix epoch, UTC. Comparison tolerance is chosen by the filesystem type of the *verifying* mount:

| Filesystem | Tolerance |
|---|---|
| FAT32 | informational only (2 s resolution, local time, DST ambiguity) |
| exFAT | ±10 ms; if the UTC-offset field is absent → informational only |
| NTFS, ext4, XFS, btrfs | ±1 s (covers cross-driver truncation) |

- R-53 Stored fields: `size`, `mtime_ns`; optional `mode` (POSIX bits, decimal) and `attrs` (Windows attribute bitmask) when available, compared only under `--strict-metadata`.

### B3.3 Exclusions

> Amended by `design/decisions/0039-exclusions-helper-and-pinned-keys-are-not-built.md`: R-55's operator-defined additions are not part of V1; every manifest carries the default list (`exclusions_version` 1).

- R-54 The default exclusion list (version 1) is embedded in every manifest header (`exclusions`, `exclusions_version`) and thereby covered by the signature:

```text
.veriarchive/
System Volume Information/
$RECYCLE.BIN/
$Recycle.Bin/
.Trash-*/
.Trashes/
lost+found/
```

- R-55 These patterns match at the archive root only. Operator-defined additions are appended per archive and increment `exclusions_version`.
- R-56 Excluded entries are neither hashed nor reported as `New`.

## B4. Manifest format `.vam` version 1

### B4.1 Container

> Amended by `design/decisions/0023-manifest-named-in-the-record-transaction.md`: R-61's writer streams the body to a file of its own (`rNNNN.vam.body-<uuid>`) and writes header + body to a temporary file that it fsyncs without holding the database lock; the rename to `rNNNN.vam` happens inside the write transaction that records the revision, after it found the revision not recorded, so a recorded manifest is never replaced.
> Amended by `design/decisions/0028-a-manifest-that-cannot-be-used.md`: R-62's reader compares line 1 of a recorded revision's manifest with the BLAKE3 recorded for it before it parses it: a line 1 that differs, also one that no longer parses, ends without a newline or is empty, is a changed manifest (`ManifestHashMismatch`), never a malformed one. In the read of the entries of any manifest, a line that does not parse, comes out of canonical order or ends the file without its newline is a malformed manifest only when what was read, read on to the end of the body, is the body validated; otherwise the manifest changed while it was read (`BodyMismatch`).
> Amended by `design/decisions/0031-chunk-lines-and-the-merkle-root.md`: with a chunk size, the writer also streams the chunk lines into a side file of its own (`rNNNN.vam.chunks-<uuid>`, the uuid of its body file, with the body file's lifetime and leftover rules) and copies them after the entries; the body hash and length cover both.

One UTF-8 file, `\n` line endings, no BOM.

```text
line 1      header — one canonical JSON object
line 2..N   body   — one canonical JSON object per line: entries (sorted per R-43), then chunk lines (V1.1)
```

- R-60 The signature (§B5) covers exactly the bytes of line 1 **excluding** its trailing `\n`. The header contains `body.blake3` and `body.length` over all bytes after the first `\n`, so the body is transitively covered. This keeps signing independent of manifest size (multi-GB manifests are expected).
- R-61 Writer: stream the body to `rNNNN.vam.tmp` while computing BLAKE3 and length; then write header + body to the final file; fsync; rename.
- R-62 Reader: read line 1; validate signature if present; parse header; stream-hash the remainder and compare `body.blake3`/`body.length` **before** any body line is trusted.
- R-63 File names: `manifests/<archive_uuid>/r<revision, 4 digits>.vam` and `.vam.sig`.

### B4.2 Canonical JSON (`common/canonical_json.*`)

- R-64 Keys are emitted in the fixed order given in §B4.3 / §B4.4 (not alphabetical); no whitespace; integers as plain decimal; strings escape only `"`, `\`, and U+0000–U+001F (always as `\uXXXX`, including newline and tab); all other characters as raw UTF-8; hashes lowercase hex; booleans `true`/`false`; absent optional fields are omitted, never `null`.
- R-65 The writer is a small in-house emitter. Parsing MAY use nlohmann/json.
- R-66 Golden test: the same fixture tree registered on Windows and on Linux MUST produce byte-identical `.vam` files after a test helper blanks `created`, `created_by`, `identity`, `root_hint`. Golden fixtures use relative links only (decision 0008); the cross-OS comparison also blanks `mode` and `attrs`, and `archive_uuid` and the body's `blake3` and `length` (decision 0015).

### B4.3 Header (line 1; shown wrapped, real header is one line)

> Amended by `design/decisions/0027-subst-drive-letters.md`: `root_hint` is the archive root by its real path; a drive letter made with subst is replaced by the folder it stands for (`S:\Photos` is recorded as `D:\Data\Photos`).
> Amended by `design/decisions/0031-chunk-lines-and-the-merkle-root.md`: `hash.chunk_size` is `0` or a positive multiple of 65536 up to 1073741824 (real archives: 67108864, fixed per archive; D2 resolved); `counts.chunks` is the number of chunk lines; `merkle_root` is present exactly when `chunk_size` is not 0; the writer fills in all three. A manifest with a chunk size whose header breaks this is malformed. The format version stays 1.
> Amended by `design/decisions/0032-registration-and-relocation-write-chunk-lines.md`: registration fixes a revision's `chunk_size` when it makes its plan: 67108864 for a new archive; for a new revision the latest revision's, or 67108864 where that is 0 (an archive registered by V1); relocation keeps rN's, also 0. The engines write chunk lines and `merkle_root` from then on.
> Amended by `design/decisions/0033-a-full-verify-adds-chunk-hashes.md`: an archive registered by V1 also gets 64 MiB through a FULL verify that adds chunk hashes (`verify --full --add-chunk-hashes`), which records the next revision only when every entry matched: rN's entry lines byte for byte, their chunk lines and `merkle_root`.

```json
{"format":"vam","version":1,
 "archive_uuid":"28ff645e-a220-4aac-ae0b-6c328bf1a2b3",
 "label":"ARCH-0007",
 "revision":2,
 "supersedes":1,
 "created":"2026-09-15T05:00:00Z",
 "created_by":{"app":"veriarchive","app_version":"0.1.0","os":"windows","host":"ws-01"},
 "identity":{"filesystem_type":"ntfs","filesystem_uuid":"a1b2c3d4e5f60718","filesystem_uuid_short":"e5f60718",
             "volume_label":"ARCHIVE-07","capacity_bytes":8001563222016,"root_subpath":"",
             "devices":[{"model":"WDC WD80EFAX","serial":"WD-XXXX","wwn":"","capacity_bytes":8001563222016,"bus":"usb"}]},
 "root_hint":"E:\\",
 "hash":{"file":["blake3","sha256"],"chunk":"blake3","chunk_size":0},
 "exclusions_version":1,
 "exclusions":[".veriarchive/","System Volume Information/","$RECYCLE.BIN/","$Recycle.Bin/",".Trash-*/",".Trashes/","lost+found/"],
 "counts":{"files":123456,"dirs":789,"links":3,"streams":0,"chunks":0,"bytes":6543210987654},
 "merkle_root":"<64 hex, omitted when chunk_size is 0>",
 "body":{"blake3":"<64 hex>","length":98765432,"lines":124248}}
```

Field rules:
- `revision` starts at 1 and increments on re-registration after a legitimate change; `supersedes` is omitted for revision 1.
- `hash.chunk_size`: `0` = no chunk lines (V1). V1.1 uses 67108864 (64 MiB, see D2); fixed per archive at registration.
- `merkle_root` omitted when `chunk_size` is 0. A verifier that sees `chunk_size:0` skips chunk and Merkle checks.
- `identity` is the `VolumeInfo`/`PhysicalDeviceInfo` snapshot minus runtime fields (`mount_point`, `device_ref`, `read_only`, `rotational`). It describes the carrier this revision was read from (decision 0004).
- `copy_of` (optional, omitted when absent, directly after `supersedes`): `archive_uuid` of another archive holding the same content; display only (decisions 0005, 0015).

### B4.4 Body lines

> Amended by `design/decisions/0010-unreadable-entries-at-registration.md`: optional key `unreadable` replaces `b3`/`sha256` for entries unreadable at registration; key order `…,"sha256","unreadable","mode",…`. Link targets use the canonical form of decision 0008.
> Amended by `design/decisions/0031-chunk-lines-and-the-merkle-root.md`: only a file (`t:"f"`) with a `b3` and `size > chunk_size` has chunk lines; streams, links, directories and entries without a content hash have none. The chunk lines are the last `counts.chunks` lines of the body, and each such file owns the next ⌈size / chunk_size⌉ of them by position, as two files can share `p` (R-41): `idx` 0 to n − 1, the file's `p`, `offset` idx × chunk_size, `length` the chunk size or what is left of the file. A manifest with a chunk size whose chunk lines break this, are not canonical JSON, or carry a `b3` that is not 64 lowercase hex digits is malformed; a manifest with `chunk_size` 0 is read as before, its chunk lines skipped.

Entry line, keys in this order:

```json
{"t":"f","p":"Projects/2026/report.pdf","size":1048576,"mtime_ns":1757912400123456700,"b3":"<64 hex>","sha256":"<64 hex>","mode":420,"attrs":32,"enc":"bytes","raw":"<hex>","warn":["case-collision"],"stream":"<ADS name, s only>"}
```

- `t`, `p` required. `size`, `mtime_ns`, `b3`, `sha256` required for `f` and `s`; for `l`, `b3`/`sha256` hash the target string and `size` is its byte length; `d` carries only `t`, `p` (+ optional `mtime_ns`).
- Chunk line (V1.1), emitted after all entries, in entry order then `idx` ascending, only for files with `size > chunk_size`:

```json
{"t":"c","p":"Video/raw.mov","idx":3,"offset":201326592,"length":67108864,"b3":"<64 hex>"}
```

### B4.5 External format

- R-67 `export-sha256sums` writes GNU `sha256sum -c` compatible output (`<hex>  <path>`; names containing `\n` or `\` use the GNU escaped form with a leading `\`), so archives can be checked without VeriArchive (`sha256sum -c`, `Get-FileHash`).

## B5. Signatures, keys, trust anchor (format fixed now; implemented in V1.1)

- R-70 Algorithm: Ed25519 via libsodium `crypto_sign_detached` (pure Ed25519, **not** Ed25519ph). Input = header bytes per R-60.
- R-71 Signature file `rNNNN.vam.sig` (canonical JSON, one line):

```json
{"format":"vam-sig","version":1,"alg":"ed25519","key_id":"<first 16 hex of BLAKE3(pubkey)>","signed":"header","sig":"<base64>","signed_at":"2026-09-15T05:01:00Z"}
```

- R-72 Trust anchor: trusted public keys are listed in the **user config file** (§B7.3) as `trusted_keys:[{key_id,pubkey_hex,note}]`, i.e. outside RepositoryRoot. `RepositoryRoot/keys/` holds convenience copies only. A signature by an unlisted key is `ManifestSignatureFailure("untrusted key <key_id>")`.
- R-73 Builds MAY embed a compile-time pinned key list (`-DVERIARCHIVE_PINNED_KEYS=...`) for hardened deployments; config keys are additive.
- R-74 The file-based signer stores the private key at `<user_config_dir>/signing.key` (0600 / owner-only ACL), never in RepositoryRoot; `ISigner` is the only access path.
- R-75 Every verification of a signed manifest records and prints the `key_id` used.
- R-76 History checkpoints (V1.1): every N runs or on demand, sign `{last_row_hash,run_count,timestamp}` into `history_checkpoints`.

## B6. Merkle tree (definition fixed now; implemented in V1.1)

> Amended by `design/decisions/0031-chunk-lines-and-the-merkle-root.md`: only entries with a content hash give leaves: a file with chunk lines its chunk hashes, any other file, link or stream its `b3`; a directory or an entry without a content hash (decisions 0010, 0014) none. Content hashes that do not give `merkle_root` are a `ManifestHashMismatch`.

- Leaves: all chunk hashes in canonical order (entry order per R-43, `idx` ascending). Files with `size ≤ chunk_size` contribute their file `b3` as the single leaf; `l` and `s` entries contribute their `b3`; `d` entries contribute nothing.
- `leaf = BLAKE3(0x00 ‖ raw32(b3))`; `node = BLAKE3(0x01 ‖ left ‖ right)`.
- An unpaired node at any level is promoted unchanged to the next level (RFC 6962 §2.1 style).
- Zero leaves: `root = BLAKE3(0x00)`.
- Root stored in header `merkle_root` as 64 hex.

## B7. Repository

### B7.1 Layout

> Amended by `design/decisions/0026-relocation-resumes.md`: R-80's `repo rebuild-index` rebuilds only what the manifests define and keeps the saved progress of unfinished registrations: the rows of `files` under the revision a plan creates, while the plan exists and no manifest defines that revision; `verify_progress` (the saved progress of verifications and relocations) is not touched.
> Amended by `design/decisions/0028-a-manifest-that-cannot-be-used.md`: R-80's `repo rebuild-index` sets each archive to the latest revision that a manifest in its folder, read or not, or the record of a revision whose manifest it cannot read names, and never below the latest revision that a verification of it which read the media names, and keeps the record and the index of that revision where it cannot read its manifest (missing, or skipped): it never takes an earlier revision for an archive's current one, whose manifest then cannot be used. The record of an earlier revision whose manifest it cannot read goes with its index, as before. It names each revision whose manifest it cannot read and what became of its record, and exits 2 then too; it marks each archive whose revision's manifest it skipped, whose manifest the archive lists then read whole until a rebuild reads it.
> Amended by `design/decisions/0032-registration-and-relocation-write-chunk-lines.md`: R-80's index tables include `chunks` and `archive_revisions.merkle_root`, which `repo rebuild-index` rebuilds from the manifests; the saved progress it keeps keeps its files' chunk rows. RepositoryRoot also holds the copy of the database kept before each upgrade of its schema (`veriarchive-schema-N-before-upgrade-<date>.db`, beside `veriarchive.db`), which nothing reads.
> Amended by `design/decisions/0036-the-repository-database-made-again.md`: R-80's `repo rebuild-index` makes a database that is missing, empty, not a database or damaged (also beyond its first page, as it checks every page first, or as it meets damage while it rebuilds) again: it puts `veriarchive.db` (with its `-wal`, `-shm` and `-journal` files) aside as `veriarchive.db.damaged-<UTC date and time>`, creates a new database of the current schema and fills its index from `manifests/`, keeping the repository's uuid; what only the database held (the verification history, retired marks, unfinished operations) is lost, and it says so; a locked database, or one of another program or another schema, is never replaced. The new database keeps a mark until a rebuild has filled it: a rebuild that stops before (cancelled, failed) leaves it so, and every command names the rebuild until one finishes, naming the database put aside again.

```text
RepositoryRoot/
├── repository.json        {"format":"veriarchive-repo","version":1,"repository_uuid":"...","created":"..."}
├── veriarchive.db         SQLite (§B8)
├── manifests/<archive_uuid>/rNNNN.vam[.sig]
├── keys/                  convenience copies of trusted public keys (V1.1)
├── logs/
└── cache/                 disposable
```

- R-80 `manifests/` + the user config are sufficient to rebuild every index table (`repo rebuild-index`). History tables are not rebuildable; they are protected by the hash chain (R-18) and by the backup requirement (R-86).
- R-81 Internal paths are built from `RepositoryLayout` with `std::filesystem::path::operator/`; no string concatenation.

### B7.2 `RepositoryLayout` and `AppContext` (replaces the static `AppPaths`)

```cpp
struct RepositoryLayout {
    std::filesystem::path root;
    std::filesystem::path database()  const { return root / "veriarchive.db"; }
    std::filesystem::path manifests() const { return root / "manifests"; }
    std::filesystem::path keys()      const { return root / "keys"; }
    std::filesystem::path logs()      const { return root / "logs"; }
    std::filesystem::path cache()     const { return root / "cache"; }
};

struct AppContext {                 // built once in the composition root, passed by reference
    std::filesystem::path executable_dir;   // GetModuleFileNameW / readlink("/proc/self/exe"), from platform layer
    std::filesystem::path user_config_dir;
    Config config;
    RepositoryLayout repo;
    std::string repo_selected_by;           // "explicit" | "pinned" | "first-run:exe-dir" | "first-run:home"
};
```

### B7.3 Configuration file

- R-82 JSON. Lookup order for the config file itself: (1) `--config <path>`; (2) `<executable_dir>/config.json` (portable install); (3) user config dir — Windows `%APPDATA%\VeriArchive\config.json`, Linux `${XDG_CONFIG_HOME:-~/.config}/veriarchive/config.json`. First found wins; files are not merged.
- Keys: `repository` (path), `trusted_keys`, `policy` (`full_verify_max_age_days`, `any_verify_max_age_days`, `strict_metadata`), `io` (`rotational_override` by serial), `allow_network_repository` (bool).

> Amended by `design/decisions/0017-interface-language.md`: adds `language` (`"en"`, `"zh-TW"`; absent = follow the system).
> Amended by `design/decisions/0034-a-device-that-goes-away-interrupts-the-read.md`: `io.rotational_override` gives each device serial `"hdd"` or `"ssd"` (`true` or `false` read alike); any other value makes the config invalid.
> Amended by `design/decisions/0019-relative-repository-pin.md`: a relative `repository` is relative to the folder of the config file that holds it.

### B7.4 RepositoryRoot resolution (authoritative, R-83)

> Amended by `design/decisions/0019-relative-repository-pin.md`: step 2 resolves a relative pin against the folder of the config file that holds it; step 3 chooses the config file first and writes a RepositoryRoot inside that file's folder relative to it, with forward slashes (a portable first run pins `"data"`), any other RepositoryRoot absolute as before; pins already written are never rewritten. R-84 is unchanged for `--repository` and `VERIARCHIVE_REPOSITORY`.
> Amended by `design/decisions/0032-registration-and-relocation-write-chunk-lines.md`: R-85's `repo info` and GUI Settings also show where each copy of the database kept before an upgrade of its schema is, for as long as it is in RepositoryRoot, read from the files there by their names, and what it is for ("Copy of the database from before an upgrade: P (an earlier version of VeriArchive can open only this copy; you can delete it once you no longer need that version)"; `repo info --json`: `database_copies_before_upgrade`), also where the repository cannot be opened once RepositoryRoot is resolved (after the error).
> Amended by `design/decisions/0011-durable-writes-through-ifilewriter.md`: step 3 initialises in the order directories, schema in one transaction, `repository.json`, pin; `repo init` run again after a crash at any step finishes the job, the pin included: an init that finds a complete repository that no config file pins pins it, where the init pins.
> Amended by `design/decisions/0037-what-opening-the-repository-remembers-and-says.md`: step 5 applies to the pinned repository only: a repository chosen with `--repository` or `VERIARCHIVE_REPOSITORY` is neither compared with `last_repository_uuid` nor written into it, and `repo init --repository P` beside an existing pin creates P and leaves the config file as it is; a repository that cannot be opened (steps 1 and 2) is named with its path, and the pinned one with the config file that names it; R-85: `repo info` and GUI Settings also show the program's version.

```text
1. if --repository <path> or VERIARCHIVE_REPOSITORY is set:
       target = that path                  # explicit: must contain repository.json, else exit 11. Never fall back.
2. else if config.repository is set:
       target = config.repository          # pinned: must be usable, else exit 11. Never fall back.
3. else (first run, nothing pinned):
       if <executable_dir>/data can be created and a probe file written there:
           target = <executable_dir>/data
       else:
           target = <user_home>/VeriArchive      # %USERPROFILE% / $HOME
       initialise the repository at target (repository.json, db, dirs)
       pin: write config.repository = target into <executable_dir>/config.json if executable_dir is writable,
            otherwise into the user config file
4. if target is on SMB/CIFS/NFS/WebDAV and !allow_network_repository:
       exit 11 "SQLite on a network filesystem is unsafe (locking, WAL); set allow_network_repository or move the repository"
5. open; if repository.json.repository_uuid differs from config.last_repository_uuid → WARN and record the new uuid
```

- R-84 `<executable_dir>` is never `current_path()`. Launching `/opt/veriarchive/veriarchive` from `/home/user` MUST resolve `./data` to `/opt/veriarchive/data`.
- R-85 `repo info` and GUI Settings show the active RepositoryRoot, `repository_uuid`, and `repo_selected_by`.
- R-86 Documentation and GUI Settings MUST state that RepositoryRoot itself must be backed up; losing it without backup loses verification history (manifests can be re-imported from media copies only if those were written).

### B7.5 Export / import / labels

> Amended by `design/decisions/0030-import-names-its-manifest-in-the-record-transaction.md`: R-87's import copies the validated manifest under a temporary name and flushes it without the database lock, and renames it to `rNNNN.vam` in the write transaction that records its revision, after finding the revision not recorded, as registration and relocation do (decision 0023): an imported manifest never replaces a recorded one. An import that finds its revision recorded meanwhile, or an unfinished registration with saved files of it, records nothing (exit 12); one that records it removes the plan of a registration and the record of a relocation that create that revision. Import and export report their progress and stop on a cancel with nothing written (R-94, amended 2026-10-27).
> Amended by `design/decisions/0028-a-manifest-that-cannot-be-used.md`: R-87's import of a revision that is already known, whose manifest in the repository is missing, cannot be read or is not the one recorded, puts the file back when it is the one recorded (its line 1 has the BLAKE3 recorded for it, and its body is the one that line names), staged and named in a write transaction that finds the record still naming that line, as decision 0030 names a manifest; the record and its index stay as they are, and `--json` says `restored`. Any other file is `ManifestHashMismatch`, as before; a record that changed while the import ran refuses it (exit `12`).

- R-87 `export <uuid> --out DIR` copies `rNNNN.vam` (+ `.sig`) and writes `identity.json`. `import <file.vam>` validates (signature if present, body hash), enforces R-15, and inserts index rows. Importing an already-known uuid+revision byte-compares the files; a mismatch is `ManifestHashMismatch`.
- R-88 Label collisions after import are allowed; the UI shows `ARCH-0007 (28ff645e)` when two archives share a label.

### B7.6 Media copy (optional, off by default)

- R-89 With `--write-media-copy`, registration writes `<root>/.veriarchive/identity.json` **before** the scan (so it is excluded and never `New`) and `<root>/.veriarchive/rNNNN.vam[.sig]` after finalisation. This is a convenience copy; R-13/R-14 govern trust. When both copies exist at verify time they are byte-compared: a difference is at least `PASSED_WITH_WARNINGS`, and `FAILED` if the media copy validates with a *higher* revision than the repository knows (possible repository rollback).

## B8. Database schema v1 (all tables created by V1; V1.1 only fills columns and adds tables)

> Amended by `design/decisions/0022-unfinished-operations-in-archive-list.md`: the `settings` records of an unfinished registration (`registration:<archive_uuid>`) and verification (`run:<run_id>`) also hold `files_total`, `bytes_total` and `updated` (when it last worked, UTC, at least every 60 s while it works), a registration's also `stopped` (when an attempt ended without finishing), a verification's also its `root`; no schema change.
> Amended by `design/decisions/0024-attempts-record-their-process.md`: a registration's record (its plan) also holds `process`, the process that runs its attempt (`id`, `start`, `host`, `program`).
> Amended by `design/decisions/0025-abandoning-an-unfinished-registration.md`: a registration's plan also holds `media_copy` (`true` once an attempt writes a media copy); abandoning the registration removes its plan and its saved progress (its rows in `files`), and, when it was the first registration of an archive that never had a revision (its `archives` row says revision 0 and `REGISTRATION_INCOMPLETE` or `RETIRED`, and no `archive_revisions`, `verify_runs` or `media_evidence` row refers to it), that `archives` row; no schema change.
> Amended by `design/decisions/0026-relocation-resumes.md`: an unfinished relocation keeps a `settings` record of its own, `relocation:<archive_uuid>` (the revision N it verifies, its `run_id`, `started`, the new `root` and its `root_subpath`, the new carrier's media `evidence` at the start, `files_total`, `bytes_total`, `updated`, `stopped`, `process`, `resumes`, and `differs` once an attempt found that the new carrier does not hold the recorded content), and checkpoints the entries that matched into `verify_progress` under its `run_id`; recording r(N+1), a carrier that differs, N+1 recorded by another operation, and abandoning remove both; an interrupted attempt appends an `INCOMPLETE` row of its own to `verify_runs` (R-90), with its `verify_items` and `media_evidence`; no schema change.
> Amended by `design/decisions/0028-a-manifest-that-cannot-be-used.md`: `repo rebuild-index` writes a `settings` record `rebuild-skipped:<archive_uuid>` (`{"revision":N}`) for each archive whose revision's manifest it skipped, and removes those of the rebuild before; while it stays, the archive lists read that archive's manifest whole, as the rebuild reads one. The plan of a registration over a lost manifest (`register --lost-manifest`) also holds `lost_manifest`, the revision whose manifest is lost; the transaction that records its revision removes the lost revision's record and index where that manifest's line 1 still cannot be used, and the archive's `rebuild-skipped` record; no schema change.
> Amended by `design/decisions/0032-registration-and-relocation-write-chunk-lines.md`: the schema version is 2, with the tables of version 1 (R-92); this version raises a version 1 repository to 2 when it first opens it, in a transaction that first keeps a copy of the database as it is beside it, named for its version and the local date (`veriarchive-schema-1-before-upgrade-2026-10-17.db`, never replacing a file), and that upgrades nothing when the copy cannot be made; the open that upgraded says once where the copy is, and a V1 program refuses the repository from then on. `chunks` holds one row per chunk line of a recorded revision and of the files a registration has saved (`last_verified_at` NULL until QUICK and DEEP), written and removed with the file's row in `files`, also when a registration is abandoned; `archive_revisions.merkle_root` is the header's `merkle_root`, NULL for chunk size 0; a registration's plan also holds `chunk_size`, fixed when it is made. Only a database that can be used (decision 0036) is upgraded; `repo rebuild-index` upgrades one of an older version before it fills the index (amended 2026-10-27).
> Amended by `design/decisions/0033-a-full-verify-adds-chunk-hashes.md`: the schema version is 3, which adds `verify_progress_chunks (run_id, path, raw_hex, stream, idx, b3)`, the chunk hashes kept with the checkpoints of a FULL verify that adds chunk hashes and removed with them; a repository of version 1 or 2 is raised to 3 when this version opens it, after a copy of its database is kept as decision 0032 says. The record of such a verification (`run:<run_id>`) also holds `add_chunk_hashes` (its chunk size) and `process` (decision 0024), and its rows in `verify_runs` say `add_chunk_hashes` in `counts_json`; the one that records the next revision is a FULL verification of that revision, whose `counts_json` says `chunk_hashes_added_from_revision`.
> Amended by `design/decisions/0040-an-archive-is-created-when-its-first-revision-is.md`: `archives.created` is the time that the manifest of the archive's first revision gives (its header's `created`), as a rebuild of the index reads it; the upgrade to schema version 3 sets it so for every archive with a revision.
> Amended by `design/decisions/0044-device-alerts-a-devices-health-and-a-slower-read.md`: the `counts_json` of a run of a verification or a relocation also holds `device_health` (each device asked, with its model, serial, WWN and health); that of a FULL verification `read_speed` (the content's bytes, the device read, and the bytes and milliseconds of each hundredth of the content it read) and, once it reached a result, `read_compared` (the run compared with, both speeds, how much slower, the slower stretches, and whether it alerted); `media_evidence.volume_json` holds each asked device's `health`; no schema change.
> Amended by `design/decisions/0047-a-full-verification-may-accept-a-device-behind-a-changed-enclosure.md`: the `counts_json` of a FULL verification that went on past a changed enclosure holds `carrier_confirmed` (`how`: `device_serial` or `operator`; `device`: `model`, `serial`, `wwn`, only where the device's own identity was read; `enclosure_recorded`, `enclosure_now`; `from_revision`), whatever its result, `INCOMPLETE` and `FAILED` runs included, covered by `row_hash` (R-90), and its record `run:<run_id>` holds it for a resume; its `media_evidence.operator_note` says how it was confirmed, and the one that passed records revision N+1 with the new carrier in its transaction. `media_evidence.volume_json` marks a device behind an enclosure that hides its identity (`enclosure_hides_identity`), and a registration's holds the device's own identity read for it (`device_identity`: `model`, `serial`, `wwn`, `how`, `read_at`, `device_ref`), which the registration's plan keeps too for a resume. The setting `device-identity:<archive_uuid>` caches the archive's confirmed device (`model`, `serial`, `wwn`, `confirmed_at`, `run_id`), written by such a registration and by a pass that recorded a read, removed by a relocation; it is a cache only, as the confirmed device is read from the runs and the media evidence. No schema change: a version that knows none of these members opens the repository and ignores them.

```sql
CREATE TABLE schema_version (version INTEGER NOT NULL);

CREATE TABLE archives (
  archive_uuid TEXT PRIMARY KEY, label TEXT NOT NULL, created TEXT NOT NULL,
  current_revision INTEGER NOT NULL,
  status TEXT NOT NULL,                 -- REGISTERED | REGISTRATION_INCOMPLETE | RETIRED (decision 0003)
  root_subpath TEXT NOT NULL DEFAULT '', notes TEXT);
CREATE INDEX archives_label ON archives(label);          -- deliberately NOT unique (R-33)

CREATE TABLE archive_revisions (
  archive_uuid TEXT NOT NULL, revision INTEGER NOT NULL, manifest_relpath TEXT NOT NULL,
  header_json TEXT NOT NULL, header_blake3 TEXT NOT NULL, body_blake3 TEXT NOT NULL,
  merkle_root TEXT, signed INTEGER NOT NULL DEFAULT 0, key_id TEXT, imported_from TEXT,
  created TEXT NOT NULL,
  PRIMARY KEY (archive_uuid, revision));

CREATE TABLE media_evidence (
  id INTEGER PRIMARY KEY, archive_uuid TEXT, run_id TEXT, seen_at TEXT NOT NULL,
  volume_json TEXT NOT NULL,
  match_result TEXT NOT NULL,           -- MATCH | MATCH_WEAK | MIGRATED | AMBIGUOUS | MISMATCH | UNKNOWN | PREVIOUS_CARRIER
  operator_note TEXT, disk_fingerprint TEXT);            -- disk_fingerprint: V2

CREATE TABLE files (                    -- index; rebuildable from manifests
  archive_uuid TEXT NOT NULL, revision INTEGER NOT NULL,
  path TEXT NOT NULL, raw_hex TEXT NOT NULL DEFAULT '', stream TEXT NOT NULL DEFAULT '',
  type TEXT NOT NULL, size INTEGER, mtime_ns INTEGER, b3 TEXT, sha256 TEXT, flags TEXT,
  PRIMARY KEY (archive_uuid, revision, path, raw_hex, stream)) WITHOUT ROWID;   -- stream: decision 0002

CREATE TABLE chunks (                   -- index; rebuildable; populated from V1.1
  archive_uuid TEXT NOT NULL, revision INTEGER NOT NULL,
  path TEXT NOT NULL, raw_hex TEXT NOT NULL DEFAULT '', stream TEXT NOT NULL DEFAULT '',
  idx INTEGER NOT NULL,
  offset INTEGER NOT NULL, length INTEGER NOT NULL, b3 TEXT NOT NULL, last_verified_at TEXT,
  PRIMARY KEY (archive_uuid, revision, path, raw_hex, stream, idx)) WITHOUT ROWID;

CREATE TABLE verify_runs (
  run_id TEXT PRIMARY KEY, archive_uuid TEXT NOT NULL, revision INTEGER NOT NULL,
  mode TEXT NOT NULL,                   -- QUICK | DEEP | FULL
  started TEXT NOT NULL, finished TEXT,
  result TEXT NOT NULL,                 -- RUNNING | PASSED | PASSED_WITH_WARNINGS | FAILED | INCOMPLETE
  coverage_bytes INTEGER, coverage_chunks INTEGER, sample_seed TEXT,
  counts_json TEXT, failure_classes TEXT, key_id TEXT, host TEXT, app_version TEXT,
  prev_hash TEXT NOT NULL, row_hash TEXT NOT NULL);       -- hash chain (R-18)

CREATE TABLE verify_items (             -- only non-Verified items; totals live in counts_json
  run_id TEXT NOT NULL, path TEXT NOT NULL, raw_hex TEXT NOT NULL DEFAULT '',
  stream TEXT NOT NULL DEFAULT '', status TEXT NOT NULL, detail TEXT,
  PRIMARY KEY (run_id, path, raw_hex, stream)) WITHOUT ROWID;

CREATE TABLE verify_progress (          -- resume checkpoint; rows deleted when the run finishes
  run_id TEXT NOT NULL, path TEXT NOT NULL, raw_hex TEXT NOT NULL DEFAULT '',
  stream TEXT NOT NULL DEFAULT '',
  PRIMARY KEY (run_id, path, raw_hex, stream)) WITHOUT ROWID;

CREATE TABLE history_checkpoints (id INTEGER PRIMARY KEY, created TEXT NOT NULL,
  last_row_hash TEXT NOT NULL, run_count INTEGER NOT NULL, key_id TEXT, sig TEXT);
CREATE TABLE trusted_keys (key_id TEXT PRIMARY KEY, pubkey_hex TEXT NOT NULL, added TEXT, note TEXT);
CREATE TABLE settings (key TEXT PRIMARY KEY, value TEXT);
```

- R-90 `row_hash = BLAKE3(prev_hash ‖ canonical JSON of the run row without row_hash)`; the first row's `prev_hash` is `repository_uuid`. `repo verify-history` recomputes the chain.
- R-91 Writes use batched transactions (5 000 rows or 1 s, whichever first), WAL mode, `synchronous=NORMAL`; never one commit per file.
- R-92 Migrations are forward-only scripts keyed by `schema_version`. V1.1 MUST NOT alter or drop V1 columns.

## B9. Core interfaces (the platform boundary)

> Amended by `design/decisions/0011-durable-writes-through-ifilewriter.md`: adds `IOutputFile` / `IFileWriter` for durable atomic writes.
> Amended by `design/decisions/0014-special-files.md`: `EntryType::Special` for FIFOs, sockets and device nodes.
> `IFileWriter` also creates directories (`create_directories`), used for the optional media copy (R-89).
> Amended by `design/decisions/0023-manifest-named-in-the-record-transaction.md`: `IOutputFile::flush()` makes the bytes written so far durable without giving them the destination's name; a `commit()` with nothing written since only renames.
> Amended by `design/decisions/0024-attempts-record-their-process.md`: adds `IProcessTable` (`current()`, `find(id)`, both `std::expected`) and `ProcessIdentity` (`id`, `start`, `program`): which process runs an attempt of a registration, and whether it still runs.
> Amended by `design/decisions/0029-network-roots-are-refused.md`: on Windows `IMediaEnumerator::volumes()` also gives a volume that has no drive letter or folder path, with its GUID path (`\\?\Volume{...}\`) as its `mount_point`, so that verification by archive uuid finds an archive on it.
> Amended by `design/decisions/0022-unfinished-operations-in-archive-list.md` (2026-10-27): R-94's command line cancels an operation also when its console window is closed, the user signs out or the computer shuts down (Windows: `CTRL_CLOSE_EVENT`, `CTRL_LOGOFF_EVENT`, `CTRL_SHUTDOWN_EVENT`, and `WM_ENDSESSION` to a hidden top-level window, as the program loads user32.dll), holding the process until the operation has recorded its stop, and on SIGHUP (Linux), but for a signal the process was started to ignore; interrupts are handled only while the operation runs (`platform::CancelOnInterrupt`).
> Amended by `design/decisions/0039-exclusions-helper-and-pinned-keys-are-not-built.md`: there is no helper: the Windows `IChangeTracker`, `NtfsUsnChangeTracker`, runs in the main process, unprivileged; `IRawDeviceAccess` has no V1.1 implementation, as raw device reads serve only V2's whole-disk fingerprint.
> Amended by `design/decisions/0034-a-device-that-goes-away-interrupts-the-read.md`: `IoErrorKind::DeviceRemoved` also for Windows `ERROR_MEDIA_CHANGED`, `ERROR_NO_MEDIA_IN_DRIVE`, `ERROR_UNRECOGNIZED_VOLUME` and `ERROR_FILE_INVALID`, and for Linux `ENOLINK` and `ENOTCONN`; any other error of a read is taken for the entry's only once `IMediaEnumerator::volume_for_path` finds the volume of the root as the operation found it, and an I/O error (`MediaError`, `Timeout`) only once a later read succeeds: ten in a row are a device that stopped answering. A request the system aborted (Windows `ERROR_OPERATION_ABORTED`, Linux `ECANCELED`) is `Timeout`, and `Cancelled` is the program's own cancel alone (2026-10-30).
> Amended by `design/decisions/0035-identity-strings-are-valid-utf-8.md`: `IMediaEnumerator` gives every identity string of a volume and its devices as valid UTF-8 (`with_valid_identity_text`).
> Amended by `design/decisions/0043-the-window-keeps-the-computer-awake-and-says-when-an-operation-ended.md`: beside these interfaces, the platform offers `KeepAwake`, which the window holds while an operation runs so that the computer does not sleep on its own, without elevation (Windows: a power request that the system is required; Linux: a logind inhibitor lock of sleep in block mode on the system bus); the core never uses it.
> Amended by `design/decisions/0044-device-alerts-a-devices-health-and-a-slower-read.md`: `IMediaEnumerator::device_health(const PhysicalDeviceInfo&)` gives what a device reports of its own health, without elevation (Windows: `IOCTL_STORAGE_QUERY_PROPERTY` with `StorageDeviceManagementStatus` on a handle opened without access rights; Linux: udisks2's SMART properties on the system bus), `Unknown` where nothing tells; registration, verification and relocation ask it of every device of the volume before they read, and tell it at once to their caller.
> Amended by `design/decisions/0045-the-window-runs-its-operations-as-jobs.md`: `judge_attempt` asks whether this process runs the very operation that a plan or record is of (`runs_this_operation`, given the plan), where it asked whether it runs an operation: the window runs several jobs, and tells by their roots and archives which runs what.
> Amended by `design/decisions/0047-a-full-verification-may-accept-a-device-behind-a-changed-enclosure.md`: adds `IDeviceIdentityHelper` beside `IRawDeviceAccess`: `available()` (false where this platform has no device identity reader, or its program is missing) and `read_device_identity(device_ref, CancelToken&)` → `std::expected<DeviceIdentityRead, IoError>`, which reads a device's own identity behind its enclosure with administrator permission, waiting off the window's thread for the operator's answer and the reader, bound to the device described before and after; `NullDeviceIdentityHelper` where none exists (Linux, the tests); `IoErrorKind::CommandNotPassed`, the enclosure passes no ATA command on to the device; `PermissionDenied` from such a read only where the operator declined administrator permission (`device_identity_failure` tells NotAvailable, Declined, NotPassed, Unreadable and Cancelled apart). Only the window makes one (R-05).
> Amended by `design/decisions/0048-the-verification-report-of-a-run-is-one-html-file.md`: `IOutputFile::commit_new()` names the file as `commit()` does, durably and as a whole, but only where nothing of the destination's name is there when it is named (Windows: `MoveFileExW` with `MOVEFILE_WRITE_THROUGH`, without `MOVEFILE_REPLACE_EXISTING`; Linux: `renameat2` with `RENAME_NOREPLACE`, else a hard link and the temporary name removed, else, on a file system with neither, a look for the name and then `rename`, the one narrow window left); otherwise it fails with the new `IoErrorKind::AlreadyExists`, names nothing and removes its temporary file. The platform maps Windows `ERROR_FILE_EXISTS` and `ERROR_ALREADY_EXISTS` and Linux `EEXIST` to `AlreadyExists` wherever it meets them. `flush()` and `commit()` are unchanged (decisions 0011, 0023).

```cpp
namespace va {

enum class IoErrorKind { NotFound, PermissionDenied, DeviceRemoved, MediaError /* bad sector, CRC */,
                         Timeout, NotSupported, Cancelled, Other };
struct IoError { IoErrorKind kind; int native_code = 0; std::string message; };

struct ReadOptions { bool uncached = true; bool sequential = true; };   // uncached: bypass page cache (R-100)

class IByteSource {                        // files (V1) and raw devices (V1.1) look identical to the hasher
public:
    virtual ~IByteSource() = default;
    virtual std::expected<std::size_t, IoError> read(std::uint64_t offset, std::span<std::byte> out) = 0; // 0 only at EOF
    virtual std::uint64_t size() const = 0;
    virtual std::size_t alignment() const = 0;   // 1 for buffered; sector size for uncached/raw
};

enum class EntryType { File, Directory, Link, Stream };
struct DirEntry {
    std::filesystem::path native_name;     // exactly as the OS reports it (bytes on Linux, UTF-16 on Windows)
    EntryType type; std::uint64_t size; std::int64_t mtime_ns;
    std::optional<std::uint32_t> mode; std::optional<std::uint32_t> attrs;
    bool is_mount_point;
};

class IFileSystemAccess {                   // std::filesystem cannot express uncached reads, \\?\ prefix, backup semantics
public:
    virtual ~IFileSystemAccess() = default;
    virtual std::expected<std::vector<DirEntry>, IoError> list(const std::filesystem::path& dir) = 0;  // raw, unsorted
    virtual std::expected<std::unique_ptr<IByteSource>, IoError> open(const std::filesystem::path& file, ReadOptions) = 0;
    virtual std::expected<std::string, IoError> read_link(const std::filesystem::path& link) = 0;      // UTF-8 target
    virtual std::expected<void, IoError> drop_cache(const std::filesystem::path& file) = 0;            // posix_fadvise / no-op
};

class IMediaEnumerator {
public:
    virtual ~IMediaEnumerator() = default;
    virtual std::expected<std::vector<VolumeInfo>, IoError> volumes() = 0;
    virtual std::expected<VolumeInfo, IoError> volume_for_path(const std::filesystem::path&) = 0;  // mount → disk extents / slaves
};

class IRawDeviceAccess {                    // V1.1 via veriarchive-helper; V1 ships a stub returning NotSupported
public:
    virtual ~IRawDeviceAccess() = default;
    virtual std::expected<std::unique_ptr<IByteSource>, IoError> open_device(const std::string& device_ref) = 0;
};

struct ChangeTrackerState { std::string journal_id; std::string cursor; };   // opaque; persisted per archive
enum class ChangeTrackerVerdict { Unchanged /* advisory only, R-17 */, Changed, ContinuityLost, Unsupported };
class IChangeTracker {                      // V1: NullChangeTracker. V1.1 Windows: NtfsUsnChangeTracker via helper
public:
    virtual ~IChangeTracker() = default;
    virtual std::expected<ChangeTrackerState, IoError> snapshot(const VolumeInfo&) = 0;
    virtual std::expected<ChangeTrackerVerdict, IoError> compare(const VolumeInfo&, const ChangeTrackerState& previous,
                                                                 std::vector<ArchivePath>* changed_out) = 0;
};

class ISigner {                             // V1.1: FileSigner; later: token / service
public:
    virtual ~ISigner() = default;
    virtual std::string key_id() const = 0;
    virtual std::expected<std::vector<std::byte>, IoError> sign(std::span<const std::byte>) = 0;
};

struct Progress { std::uint64_t bytes_done, bytes_total, items_done, items_total; std::string current_item; };
class IProgressSink { public: virtual ~IProgressSink() = default; virtual void on_progress(const Progress&) = 0; };
class CancelToken  { public: void cancel(); bool cancelled() const; };   // thread-safe

class IEntropySource { public: virtual ~IEntropySource() = default;
    virtual std::expected<void, IoError> fill(std::span<std::byte>) = 0; };   // BCryptGenRandom / getrandom

} // namespace va
```

- R-93 Core engines (`Scanner`, `HashPipeline`, `RegisterEngine`, `VerifyEngine`, `IdentityMatcher`) depend only on these interfaces plus `common`. Test doubles (`FakeFileSystem`, `FaultInjectingByteSource`, `FakeMediaEnumerator`) live in `tests/` and are how `Unreadable`/`MediaError`/identity paths are tested on both OSes without special hardware.
- R-94 Every long-running core operation takes `IProgressSink&` and `CancelToken&` from V1 (CLI wires them to stderr and SIGINT/Ctrl-C; GUI to widgets). Retro-fitting this for the GUI is forbidden.

## B10. Identity matching (decision table)

> Amended by `design/decisions/0001-archive-matching-includes-root-subpath.md`: candidates are first narrowed by `root_subpath`; archives on the same volume with different, non-overlapping roots are not `AMBIGUOUS`.
> Amended by `design/decisions/0004-relocation-writes-a-new-revision.md`: `MIGRATED` acceptance and relocation are `archive relocate`, which writes a new revision; media matching only an older revision's identity is `PREVIOUS_CARRIER`.
> Amended by `design/decisions/0003-retired-archives.md`: retired archives remain match candidates, but one whose latest manifest is lost (decision 0028).
> Amended by `design/decisions/0018-unreadable-filesystem-id.md`: a `filesystem_uuid` unavailable on either side with a matching device is `MATCH_WEAK` when the volume capacity agrees within 1 %, otherwise `MISMATCH`.
> Amended by `design/decisions/0021-linux-without-udev.md`: a kernel filesystem id (`fsid-…`, §B1.3) compared with a UUID or volume serial counts as unavailable, so decision 0018 applies; two `fsid-…` ids compare like UUIDs.
> Amended by `design/decisions/0028-a-manifest-that-cannot-be-used.md`: R-96: a verification that failed on its manifest before it looked for the media records no media evidence, as it observed none. Matching by root needs every archive's latest manifest: while one cannot be used, the root may be that archive's, unless it is plainly another archive's, a `MATCH` of one at its folder (roots never overlap on one volume, decision 0001), which is verified as usual; for every other root, a verification by root stops (exit 11), naming the archive, what is wrong with its manifest, and to verify it by its uuid to record the failure, then the steps of the plain message: to put its manifest back with `import` from a backup of the repository, an export of that revision or its media copy, naming the last resort where none of these holds it and, after it, retiring the archive where its device is gone as well, or to rebuild the index where only the index is wrong. A registration of a new revision (`register --new-revision`) of a root that is plainly another archive's goes ahead by the same rule, as that archive's next revision; every other registration, of a new folder or volume too, and every relocation stop with the plain message (exit 11). A retired archive whose latest manifest is lost (missing, or its first line unreadable or changed: nothing that a rebuild of the index gives back; one changed only in its body stays a candidate, as matching reads its first line), as the operator retires one whose device is gone as well, is no candidate of matching: it stops nothing, and no root is matched as its carrier; where line 1 alone says that only the index is wrong of it, matching reads the manifest whole, as `repo rebuild-index` does, and one whose body has changed or whose body or entries cannot be read is lost all the same.
> Amended by `design/decisions/0047-a-full-verification-may-accept-a-device-behind-a-changed-enclosure.md`: the `MIGRATED` row is accepted in two ways: by `archive relocate` (decision 0004), for every `MIGRATED`; and, where the change is an enclosure's (the same filesystem on one device on each side, both on bus `usb`, or the device seen now behind an enclosure that hides its identity), by a FULL verification by the archive's uuid with a carrier confirmation, the device's own serial read and equal to the archive's confirmed device or the operator's word, after reading it or without: the confirmation is recorded in the run's counts whatever the result, and only a run that passed with every entry matched, as a relocation requires, records revision N+1 with the new carrier, so that it is not prompted again. A device taken out of its enclosure, two internal devices and a spanned volume are no enclosure's: relocation alone accepts them. `verify --full <archive_uuid>` of an archive attached only on another device stops on it `MIGRATED` (exit 12), where it found the archive on no attached volume (exit 10).

Evidence at verify time (`MediaEvidence`) is compared against every registered archive's latest `identity` snapshot. Primary evidence: `filesystem_uuid` (per §B1.3) and device `serial`/`wwn` (either matching counts as a device match).

| filesystem_uuid | device serial / wwn | Result | Action |
|---|---|---|---|
| match | match | `MATCH` | proceed |
| match | unavailable on either side | `MATCH_WEAK` | proceed; run result capped at `PASSED_WITH_WARNINGS` |
| match | differ | `MIGRATED` candidate (new enclosure, clone) | stop (exit 12); operator confirms; acceptance requires a FULL verify to pass, then the new device evidence is recorded for the archive |
| differ | match | `MISMATCH` ("reformatted or different filesystem on a known device") | FAIL; only `register` may create a new archive here |
| differ | differ | `UNKNOWN` | not a registered archive; offer `register` |
| evidence matches more than one archive | — | `AMBIGUOUS` (cloned media) | stop (exit 12); operator selects; both archives get a `media_evidence` row |
| `claimed_archive_uuid` present but inconsistent with the row selected above | — | `MISMATCH` + detail `identity-file-copied` | FAIL |

- R-95 `volume_label`, `capacity_bytes`, `model`, `filesystem_type` are secondary: a difference adds a warning detail but does not change the selected row, except that a `capacity_bytes` difference > 1 % with a matching serial adds detail `device-replaced` and downgrades to `MISMATCH`.
- R-96 Every register/verify records a `media_evidence` row with the raw `VolumeInfo` JSON so matching can be re-evaluated if the rules change.
- R-97 Registration MUST refuse to register a volume whose evidence already yields `MATCH`/`MATCH_WEAK` for an existing archive unless `--new-revision` (legitimate re-baseline, increments `revision`) or `--force-new-archive` is given.

## B11. Verification modes

> Amended by `design/decisions/0028-a-manifest-that-cannot-be-used.md`: an archive named by its uuid or by the run it resumes is known before its media is looked for: the manifest of its current revision is checked first (R-14, R-62), and one that cannot be used fails the verification at once, recorded as `FAILED` with nothing covered, without reading the media; a run being resumed stays resumable.
> Amended by `design/decisions/0032-registration-and-relocation-write-chunk-lines.md`: FULL compares a file with chunk lines by chunk as well, from the read that hashes it whole (R-101): one whose content differs names the chunks that differ and the byte where the first begins, a size change stays one, and one whose content matches while a chunk hash does not is `Verified` with a warning (§A6), as is one whose chunks all match while a file hash does not; a relocation compares by chunk too, and accepts such a file with the same warning, carrying rN's hashes (decision 0004).
> Amended by `design/decisions/0033-a-full-verify-adds-chunk-hashes.md`: a FULL verify of an archive whose current revision has chunk size 0 may add chunk hashes: its read gives them at 64 MiB, and when every entry matched (`Verified`, or unreadable since registration, decision 0010) the next revision is recorded with rN's entry lines byte for byte, their chunk lines and `merkle_root`, in the transaction of the run, as a relocation records its revision; any difference records the verification alone.

| Mode | Checks | Claim |
|---|---|---|
| FULL (V1) | identity match; manifest hash (V1.1: signature); revision; 100 % of entries read and hashed; all item statuses | "all registered content read and matched" |
| QUICK (V1.1) | identity; manifest hash/signature; revision; change-tracker verdict; 64 chunks (≈ 4 GiB at 64 MiB) chosen from the seed, or all chunks if fewer | "identity / manifest / sample passed" |
| DEEP (V1.1) | QUICK + 1–10 % of chunks (configurable), seed-driven, preferring the oldest `last_verified_at` | same, with coverage shown |

- R-100 Reads bypass the OS page cache so bytes come from the medium. Linux: `posix_fadvise(fd,0,0,POSIX_FADV_DONTNEED)` before and after each file (`O_DIRECT` for raw devices). Windows: `FILE_FLAG_NO_BUFFERING | FILE_FLAG_SEQUENTIAL_SCAN` with sector-aligned buffers and offsets (the last partial sector is read with an aligned over-read and truncated). Verifying immediately after registering MUST still hit the disk.
- R-101 One read pass computes BLAKE3 (file), SHA-256 (file) and, from V1.1, BLAKE3 per chunk.
- R-102 Sampling seed: 32 bytes from `IEntropySource`, stored in `verify_runs.sample_seed`; chunk selection is a pure function of (seed, chunk list). `--seed HEX` reproduces a run.
- R-103 Coverage policy: DEEP prefers chunks with the oldest `last_verified_at`, so "every chunk verified at least once per `policy.max_chunk_age_days`" is achievable and auditable.

### B11.4 Resume

> Amended by `design/decisions/0022-unfinished-operations-in-archive-list.md`: R-104's resume of a run by its archive's uuid (`verify --full <archive_uuid> --resume <run_id>`, and Resume in the Archive List) looks at every attached volume, and where more than one holds the archive (cloned media), it stops before it reads anything (exit 12, `AMBIGUOUS`), naming their roots and the root the run read, as identity cannot tell which one the run read; the run stays resumable by its root. A verification by the archive's uuid that is not a resume (`verify --full <archive_uuid>`, and Verify Archive, which verifies a chosen archive by its uuid) stops the same way, reading and recording nothing, and names their roots, to verify one of them by its root or to detach the other volumes; it never picks one by itself. A verification by a root that matches several archives (`AMBIGUOUS`, §B10) says to select one by its uuid and, where the volumes of those archives are attached together, to detach all but the volume of the one to verify. `register --resume` of a registration whose plan never says when it worked and names no process (an older version's, which may still run it and names its manifest before it records it) takes it over only once confirmed (`--confirm`, or Resume in the GUI's question); unconfirmed, it changes nothing (exit 12).
> Amended by `design/decisions/0023-manifest-named-in-the-record-transaction.md`: attempts of one registration may run at once (`register --resume` while an attempt still runs), and a relocation may record the revision that an unfinished registration creates; the first to record the revision wins, and another stops with exit 12 when it finds the revision recorded (at a checkpoint, at the record, whose transaction waits up to 2 minutes for the write lock, or at the start of a resume), changing nothing of it (manifest, index, archive record) nor the plan of the archive's next registration. A plan whose revision is recorded is removed, by whatever records the revision or by the attempt that finds it recorded. A relocation leaves the revision to a registration that has checkpointed files for it (exit 12). D10 and D11: decisions 0024 and 0025.
> Amended by `design/decisions/0024-attempts-record-their-process.md`: every attempt records in its registration's plan which process runs it (id and start time, on which computer); `register --resume` refuses (exit 12, naming the process) while that process still runs on this computer, and goes ahead as soon as that process has ended; a plan that names no process (an older version's) or a process on another computer keeps the rule above.
> Amended by `design/decisions/0025-abandoning-an-unfinished-registration.md`: the operator may abandon an unfinished registration (`archive abandon-registration <uuid> --confirm`, or the Archive List): its plan and checkpoints are removed in one transaction, never while its attempt runs; recorded revisions, manifests and history stay; the record of an archive that never had a revision goes with its abandoned first registration, judged from the archive's record and what refers to it, never from the index alone. The refusals of a relocation and of `register <root>` name the way out. An archive has one unfinished registration at a time: a registration that is not a resume, such as one through another path to the same volume, is refused (exit 12) while another registration of its archive is unfinished.
> Amended by `design/decisions/0026-relocation-resumes.md`: a relocation resumes (R-104). `archive relocate <uuid> <root_path> --resume` continues the archive's unfinished relocation without reading again the entries that matched, carrying them with rN's hashes, only while the archive is at the revision N it verifies, and the new root is on the volume it started on (`MATCH` or `MATCH_WEAK` against the evidence recorded then) and in the same folder, and never while its attempt's process runs on this computer (decision 0024). An archive has one unfinished relocation at a time: one that is not a resume is refused (exit 12) while another is unfinished. A carrier that differs ends the relocation, also when its attempt is interrupted after it found an entry that differs (an entry that could not be read differs only in a comparison that read everything: an attempt interrupted after one, as when a disk drops off, is interrupted); so does N+1 recorded by another operation, and whatever records N+1 removes the relocation's record and checkpoints; the record of a carrier that differs says so (`differs`) from the first such entry on, so that one that cannot be removed is never resumed; an interruption, a failure of its scan, manifest or record, or a registration with saved files of N+1 leaves them for `--resume`, which the message names. `archive abandon-relocation <uuid> --confirm` removes them on purpose, never while the attempt runs. An interrupted attempt is also recorded `INCOMPLETE` in the history (R-104), also one that ends the relocation, under a run of its own, which stays as every run does and which `verify --resume` does not continue (it names `archive relocate --resume` while the relocation is unfinished); the FULL verification that records r(N+1) spans those runs, from the relocation's start.
> Amended by `design/decisions/0030-import-names-its-manifest-in-the-record-transaction.md`: a registration or relocation made for an older revision never records: once the archive has a later revision than the one it was made for while the revision it creates is not recorded (an import of N+2), its record and its checkpoints stop it, and `register --resume`, `archive relocate --resume`, a registration of the archive and a relocation without `--resume` refuse it (exit 12), naming the abandon, and so does its attempt that is interrupted or that a failure stops after that, instead of naming the resume; `verify --resume` of the `INCOMPLETE` run of such a relocation's attempt names the abandon too; its plan or record and its saved progress stay until the operator abandons it. A registration of an archive and a relocation that are not a resume write their plan or record only while the archive is still at the revision they read when they started, and otherwise nothing (exit 12, to be run again).
> Amended by `design/decisions/0032-registration-and-relocation-write-chunk-lines.md`: R-104's registration checkpoints save each file's chunk hashes with it, at the chunk size its plan fixed, and a resume reuses a saved file only with all of them; a plan without `chunk_size` (an older version's) finishes at chunk size 0. A FULL verify, as a relocation, checkpoints an entry only once its content matched by file and, for a file with chunk lines, by every chunk: a resume reads again a file whose chunk hashes did not match, and compares by chunk what it reads.
> Amended by `design/decisions/0033-a-full-verify-adds-chunk-hashes.md`: a FULL verify that adds chunk hashes resumes as any FULL verify, its checkpoints keeping each file's chunk hashes, so that nothing is read twice; it is one of the operations that create an archive's next revision, of which one at a time is unfinished: while it is, a registration of that revision and a relocation (neither a resume) are refused at their start, and it is refused at its start while either, or another of its kind, is unfinished (exit 12), each refusal naming how to finish or abandon the other; its resume is refused while the process of the run before still runs on this computer; `archive abandon-verification <uuid> --confirm` abandons it, never while its attempt runs.
> Amended by `design/decisions/0022-unfinished-operations-in-archive-list.md` (2026-10-27): the way on of an interrupted or stopped registration or relocation, and each refusal that names such a resume, names its root as it can be typed (`register "K:\Photos 2026" --resume`; on Windows in single quotes, as PowerShell reads it, where the root holds `$`, a backtick or a typographic double quote), never `<root_path>`.

- R-104 FULL and DEEP checkpoint verified paths into `verify_progress` every 1 000 items or 60 s. `verify --resume <run_id>` continues an `INCOMPLETE` run without re-hashing verified items, provided identity still matches and the revision is unchanged. Registration checkpoints likewise (`REGISTRATION_INCOMPLETE`, `register --resume`).

### B11.5 Overdue policy

- R-105 `policy.full_verify_max_age_days` (default 365) and `policy.any_verify_max_age_days` (default 180, decision 0007). `archive list` and the GUI show `OVERDUE` when exceeded.

## B12. Change tracking

> Amended by `design/decisions/0039-exclusions-helper-and-pinned-keys-are-not-built.md`: R-107's `NtfsUsnChangeTracker` reads the journal in the main process, unprivileged (a handle to the volume's root folder with backup semantics, `FSCTL_READ_UNPRIVILEGED_USN_JOURNAL`), not through a helper.

- R-106 V1 ships `NullChangeTracker` only. Linux stays `Null` unless btrfs generation tracking is added later; ext4/XFS have no application-queryable change journal, and inotify/fanotify are meaningless for offline media. Do not attempt.
- R-107 Windows V1.1 `NtfsUsnChangeTracker` (through the helper) stores USN Journal ID + last USN per archive. Verdicts: journal ID changed or USN gap → `ContinuityLost` → QUICK is invalid, DEEP or FULL required; records exist → `Changed`, affected paths are forced into the sample; no records → `Unchanged` (advisory only, R-17). Raw-sector edits and edits made from another OS leave no USN trace; this is why R-17 exists.

## B13. I/O engine

> Amended by `design/decisions/0034-a-device-that-goes-away-interrupts-the-read.md`: R-111's override applies in registration, FULL verification and relocation, on the command line and in the GUI: `--io` wins, then `io.rotational_override` for the device's serial (compared as identity matching compares serials), then detection.

- R-110 Pipeline: scanner → ordered work queue → readers (1 for rotational; `min(8, cores)` for non-rotational) → hash workers → result sink, decoupled by bounded buffers (default 8 × 8 MiB per reader). Never one thread per file.
- R-111 Rotational detection: Windows `StorageDeviceSeekPenaltyProperty`; Linux `/sys/block/<dev>/queue/rotational`. USB bridges misreport; `io.rotational_override` (by serial) and `--io hdd|ssd` win.
- R-112 Target: saturate the medium (HDD ≈ 150–250 MB/s sequential; NVMe ≥ 2 GB/s) without seek amplification. Throughput is an M3 acceptance criterion.

## B14. Whole-disk hash (V2; preconditions)

Hashing `\\.\PhysicalDriveN` / `/dev/sdX` is reproducible only if the block device is read-only at the block layer **before** the OS sees it (`blockdev --setro`, `diskpart attributes disk set readonly`, hardware write blocker). `mount -o ro` is insufficient (ext4/XFS journal replay; NTFS metadata writes on Windows). Without that precondition the value is a one-shot fingerprint in `media_evidence.disk_fingerprint`, labelled as such. Requires `IRawDeviceAccess`.

## B15. Build, dependencies, CI, packaging

### Toolchain minimums

| Platform | Toolchain | Notes |
|---|---|---|
| Windows | MSYS2 UCRT64, GCC ≥ 14, CMake ≥ 3.28, Ninja | MINGW64 is not a baseline |
| Linux | GCC ≥ 13, or Clang ≥ 17 with libstdc++ ≥ 13; CMake ≥ 3.28 | Ubuntu 24.04+, Debian 13+, Fedora 40+. Debian 12 (GCC 12) is not a V1 target |

C++23 features used: `std::expected`, `std::format`, `std::span`, `std::ranges`. Avoid `std::print` (GCC 14 only).

### Dependencies (identical on both platforms)

> Amended by `design/decisions/0013-glib-for-unicode-comparison.md`: GLib (≥ 2.72) is required for Unicode comparison keys.
> Amended by `design/decisions/0046-the-window-is-built-with-libadwaita-around-the-archives.md`: the GUI needs GTK4 ≥ 4.14 and libadwaita ≥ 1.5, through their C API (no gtkmm, decision 0016), each API pinned to that version, so that a call of a later one fails the build (`mingw-w64-ucrt-x86_64-libadwaita`, `libadwaita-1-dev`, `libadwaita-devel`).

| Purpose | Library | How |
|---|---|---|
| BLAKE3 | official C implementation (SIMD dispatch) | `third_party/blake3` submodule or FetchContent, static |
| SHA-256 | OpenSSL libcrypto (SHA-NI accelerated) | system package |
| Ed25519 | libsodium | system package (V1.1) |
| SQLite | sqlite3 ≥ 3.40 | system package |
| JSON parsing | nlohmann/json | system/header-only; canonical writer is in-house |
| GUI | GTK4 ≥ 4.12, gtkmm-4.0 | system package (`mingw-w64-ucrt-x86_64-gtkmm-4.0`) |
| Linux platform | libudev, libblkid | system package |
| Tests | Catch2 v3 (or GoogleTest) | FetchContent |

libsodium's SHA-256 is not used (no SHA-NI; becomes the bottleneck on NVMe).

### Targets and presets

> Amended by `design/decisions/0012-gitlab-ci.md`: R-120's loop-device / `dm-flakey` test needs a privileged runner and is not part of the GitLab pipeline; the unit tests' injected media errors cover unreadable sectors on every OS.
> Amended by `design/decisions/0038-the-windows-package.md`: R-121's zip holds no folder of its own (`bin`, `lib` and `share` at its root, the version in its file name), so that unzipping a new version into the program folder updates it in place; the package holds a short README of its own in English and Chinese beside the guides, not the repository's README; its notices include the licenses of the code compiled into the programs (nlohmann/json, BLAKE3), on every system, and the license of every MSYS2 package that owns a bundled file, kept in `packaging/windows/licenses` for a package that MSYS2 ships without one (GStreamer's); the install fails on a bundled file whose package has no license, and when `glib-compile-schemas` reports a schema it left out, also with exit code 0, and the package test checks that every GTK schema is compiled.
> Amended by `design/decisions/0046-the-window-is-built-with-libadwaita-around-the-archives.md`: R-121's package also bundles libadwaita, with its Traditional Chinese texts (`share/locale/zh_TW/LC_MESSAGES/libadwaita.mo`) and its license (`share/licenses/libadwaita`), which the package test requires, so that the program folder still runs when it is moved to another computer.
> Amended by `design/decisions/0047-a-full-verification-may-accept-a-device-behind-a-changed-enclosure.md`: on Windows a target `veriarchive_device_identity` builds the device identity reader, `bin/veriarchive-device-identity.exe` (its own `wmain`, `-municode`, a manifest with `longPathAware`, UTF-8 `activeCodePage` and `requireAdministrator`, version information), from `src/platform/windows/helper/` and `device_identity_reader.cpp` alone, linked statically, so that it loads only Windows' own DLLs; `veriarchive_gui` depends on it, and it is installed into `bin`. R-121's package test requires it and checks that it imports Windows' DLLs alone and none that `bin` holds; the notice names the GCC runtime and winpthreads compiled into it, whose licenses the package holds. The test `veriarchive_device_identity_reader` starts it without administrator permission (`__COMPAT_LAYER=RunAsInvoker`).

```text
veriarchive_common  veriarchive_core  veriarchive_platform  veriarchive_cli  veriarchive_gui  veriarchive_tests
presets: linux-debug  linux-release  windows-ucrt64-debug  windows-ucrt64-release
```

- R-120 (CI runs on GitLab: `design/decisions/0012-gitlab-ci.md`) CI on every push: `windows-latest` (msys2/setup-msys2, UCRT64), `ubuntu-24.04`, and a Fedora container. Runs unit + integration tests (golden manifests, repository-resolution matrix, fault injection via `FaultInjectingByteSource`). Linux CI additionally runs the loop-device + `dm-flakey` unreadable-sector test when a privileged runner is available.
- R-121 Windows release check: `ntldd -R veriarchive-cli.exe` lists no `msys-2.0.dll`; the package includes GTK4/gtkmm DLLs, compiled `share/glib-2.0/schemas`, an icon theme, and LGPL notices.
- R-122 CI greps fail the build on `current_path()`, `path::string()`, `<windows.h>`, `<libudev.h>` under `src/core` and `src/common` (except the R-04 exemption).

### Repository structure

> Amended by `design/decisions/0020-archive-comparison.md`: `src/core/engine/compare_archives.*`; `tools/screenshots/` makes the user guide's pictures of the GUI.
> Amended by `design/decisions/0039-exclusions-helper-and-pinned-keys-are-not-built.md`: no `src/helper/` (no `veriarchive-helper`); `usn_tracker_win.*` runs in the main process; `raw_device_win.*` and `raw_device_linux.*` are not V1.1, as raw device reads serve only V2's whole-disk fingerprint.
> Amended by `design/decisions/0047-a-full-verification-may-accept-a-device-behind-a-changed-enclosure.md`: `src/core/engine/carrier_confirmation.*` (the carrier confirmation, its records and the confirmed device); `src/platform/device_identity_helper.h` (the factory of `IDeviceIdentityHelper`), `src/platform/windows/device_identity_helper_win.cpp`, `device_identity_reader.*`, `device_identity_start.h` and `device_id_descriptor.*`, `src/platform/linux/device_identity_helper_linux.cpp` (none); `src/platform/windows/helper/` holds the device identity reader's `main.cpp`, manifest and resources, a program of its own, not the `src/helper/` of R-11; `src/app/enclosure_words.*`; `src/gui/enclosure_flow.*`, `enclosure_dialog.cpp` and `device_reads.*`.
> Amended by `design/decisions/0048-the-verification-report-of-a-run-is-one-html-file.md`: `src/app/run_words.*`, `run_result.*` and `shown_identity.h` (the result page's model and words, moved from `src/gui/`, which the window's result page and the verification report both render); `src/app/run_report_html.*` (the verification report); `src/app/archive_roots.*` (the roots of the attached archives that hold a folder); `src/cli/report_commands.cpp`.

```text
VeriArchive/
├── CMakeLists.txt  CMakePresets.json  README.md  CLAUDE.md
├── design/HANDOFF.md            (this file; single source of truth)   design/decisions/NNNN-*.md
├── third_party/blake3/
├── src/
│   ├── common/      types.h  error.h  utf8.*  archive_path.*  canonical_json.*  config.*  repository_layout.*
│   ├── core/
│   │   ├── hash/        blake3_hasher.*  sha256_hasher.*  hash_pipeline.*
│   │   ├── manifest/    manifest_writer.*  manifest_reader.*  entry.*
│   │   ├── merkle/      (V1.1)
│   │   ├── identity/    matcher.*  (§B10)
│   │   ├── scan/        scanner.*  exclusions.*
│   │   ├── engine/      register_engine.*  verify_engine.*  compare_archives.*  sampling.* (V1.1)
│   │   ├── repository/  repository.*  schema.sql  history_chain.*
│   │   └── signing/     signature_file.*  file_signer.* (V1.1)
│   ├── platform/
│   │   ├── windows/     fs_access_win.*  media_enumerator_win.*  exe_path_win.*  entropy_win.*
│   │   │                raw_device_win.* (V1.1)  usn_tracker_win.* (V1.1)
│   │   └── linux/       fs_access_linux.*  media_enumerator_linux.*  exe_path_linux.*  entropy_linux.*
│   │                    raw_device_linux.* (V1.1)
│   ├── app/             app_context.*   (composition-root helpers shared by cli and gui)
│   ├── cli/
│   ├── gui/
│   └── helper/          (V1.1) veriarchive-helper
├── tests/
│   ├── unit/
│   ├── golden/          fixture trees + expected .vam
│   └── integration/
└── tools/
    └── screenshots/     make_screenshots.cmake: sample archives + GUI screenshot tour (decision 0020)
```

## B16. CLI

> Amended by `design/decisions/0017-interface-language.md`: global option `--lang en|zh-TW` (also `VERIARCHIVE_LANG`); text output in English or Traditional Chinese, `--json` always English.
> Amended by `design/decisions/0017-interface-language.md` (2026-10-27): with `--json`, a usage error found before a command runs (a parse error, an unknown command, no command) is printed as JSON (`error`, `exit_code`); `media list` refuses an unknown option.
> Amended by `design/decisions/0017-interface-language.md` (2026-10-27): text names no decision, requirement or failure class; it says a run's failure classes and an item's detail in plain words ("the manifest is not the one recorded", "folder -> file"), a command's failure without the failure class that leads its `error` in `--json`, and an error that Windows reports with what went wrong first ("access denied (CreateFileW, Windows error 5)"), and one of a thing in the singular ("1 empty directory"); `--json` keeps its codes and classes as they were.
> Amended by `design/decisions/0020-archive-comparison.md`: `archive compare <uuid> <uuid> [--json]` compares the manifests of two archives' current revisions, no media read; exit `0` IDENTICAL · `2` IDENTICAL_WITH_WARNINGS · `3` DIFFERENT or a manifest that cannot be used · `4` cancelled.
> Amended by `design/decisions/0022-unfinished-operations-in-archive-list.md`: `verify --full <archive_uuid>`, also with `--resume <run_id>`, exits `12` where more than one attached volume holds the archive (cloned media), naming their roots. `register <root_path> --resume [--confirm]` of a registration whose plan never says when it worked and names no process (an older version's) exits `12` without `--confirm`, changing nothing, naming the registration and saying what happens where the older version still runs it (`--json`: `registration`); `register` takes `--confirm` with `--resume` as well as with `--lost-manifest`.
> Amended by `design/decisions/0024-attempts-record-their-process.md`: `register --resume` exits `12` while the process of the attempt before still runs on this computer.
> Amended by `design/decisions/0025-abandoning-an-unfinished-registration.md`: `archive abandon-registration <uuid> [--confirm] [--json]` abandons the archive's unfinished registration; without `--confirm` it shows what would be lost and exits `12`; exit `0` abandoned · `12` refused (nothing to abandon, another registration by now, an attempt that runs) · `10` unknown or ambiguous uuid · `11` repository. `register` exits `12` while another registration of the archive is unfinished, also one through another root path.
> Amended by `design/decisions/0026-relocation-resumes.md`: `archive relocate <uuid> <root_path> --resume` (or `--resume <root_path>`) exits `12` with nothing to resume, while the attempt before runs, on another volume or folder than it started on, when the archive is at another revision, or when the relocation found that its new carrier differs; without `--resume`, `12` while a relocation of the archive is unfinished; `4` when cancelled or its device removed, and `11` when its scan, manifest or record failed, both of which `--resume` continues, as the message says; `--json` says `resumed`. `archive abandon-relocation <uuid> [--confirm] [--json]`, exit codes as `abandon-registration`'s. `archive list --json` and `archive show <uuid> --json` give an archive with an unfinished relocation `unfinished_relocation` (its root, revision, progress and state, `differs` once its new carrier was found to differ), and one with an unfinished registration `unfinished_registration` (its root, the revision it creates, progress and state); without `--json` they show one line for each under the archive. `history <uuid>` marks the runs of a relocation (`interrupted relocation attempt`, `relocation from rN`), and `history --json` gives each run `relocation` (null, or the relocation's `run_id`, `from_revision` and `interrupted_attempt`).
> Amended by `design/decisions/0004-relocation-writes-a-new-revision.md`: `archive relocate` refused for a new carrier that differs (exit `3`) says to copy the archive again from its carrier and relocate again, lists each entry that differs with its detail, and gives them in `--json` as `items` (`p`, `status`, `detail`).
> Amended by `design/decisions/0030-import-names-its-manifest-in-the-record-transaction.md`: `import` exits `12` when another operation recorded its revision while it ran, or an unfinished registration with saved files creates that revision; `register` and `archive relocate`, also with `--resume`, exit `12` when the archive's unfinished registration or relocation was made for an older revision than the archive has now, also when its attempt is interrupted or stopped by a failure then (not `4` or `11`), and when another operation recorded a later revision of the archive while they began; `archive list|show --json` give such a one the state `later_revision` once it has stopped.
> Amended by `design/decisions/0028-a-manifest-that-cannot-be-used.md`: `verify --full <archive_uuid>` and `verify --resume` exit `3` with a recorded `FAILED` run when the manifest of the archive's current revision cannot be used, saying that the media was not read and how to fix the manifest; every verification's `--json` gives `media_read` (`false` then, with `root` and `identity` null). `archive list` lists every archive, one whose latest manifest cannot be used with a line "problem: ..." (what is wrong, ending with how to fix it), and `--json` gives it `manifest_problem` (`kind`, `revision`, `manifest`, `message`); `archive list`, and `archive show` of such an archive, then exit `2`. `archive show` also marks an earlier revision whose manifest cannot be used (that revision's `manifest_problem` in `--json`), and exits `2` then too. `repo rebuild-index` names each revision whose manifest it could not read, kept as recorded, left out of the index or not in it, and exits `2` then too; its `--json` gives `revisions_kept`, `revisions_left_out` and `revisions_not_indexed`. `import` of a recorded revision whose manifest cannot be used puts back a file that is the one recorded, saying so (`restored` in `--json`), and exits `12` where the record of that revision changed while it ran. The message of a manifest that is missing, cannot be read or has changed says to put it back with `import <folder>/rNNNN.vam` from a backup of the repository, an export of that revision or its media copy, for an earlier revision then `repo rebuild-index`, which leaves that revision out of the index where none of these holds it, and for the archive's latest revision names the last resort and, after it, where the device that held the archive is gone as well, retiring the archive with `archive retire <archive_uuid> --confirm`. The last resort: `register <root_path> --lost-manifest <archive_uuid> [--confirm]` registers the archive's next revision from what the root holds now, a registration over the lost manifest, which compares nothing with the lost revision. Without `--confirm` it says so and what it would register, writing nothing (exit `12`; `--json`: `registration`); it exits `12` where the manifest can be used or only the index differs from it, for an archive without a revision, where the root contains or lies in the root of another archive on a volume that is plainly that archive's (a `MATCH` of it: the filesystem id and a device that its latest manifest records) or its identity file names another archive than one whose very root it is, and where the root's `.veriarchive` holds the manifest as a media copy that `import` puts back; that archive's very root is taken, as the same folder registered on purpose as two archives (`--force-new-archive`) gives each twin that root; before it is confirmed it names each archive whose very root it is, with the match of its volume, also on a volume that it matches only as `MIGRATED` or `MATCH_WEAK` (`same_root` in `--json`, with `match`), and, as possibly a clone, each archive whose root the root contains or lies in on a volume that it matches only so (`possible_clones`); `--label`, `--new-revision`, `--force-new-archive` and `--resume` beside it, and `--confirm` with neither it nor `--resume` (decision 0022), exit `10`; confirmed, it exits as any registration, and `--json` gives `lost_manifest` (`revision`, `left_out`); it refuses a retired archive (exit `12`). `archive retire <uuid> [--confirm]` of an archive whose latest manifest is lost so that identity matching cannot use it (not one changed only in its body, which is retired at once) says what retiring it means and exits `12` without `--confirm` (`retirement` in `--json`), and exits `12` while a registration of the archive is unfinished; `archive list` and `archive show` list a retired archive whose latest manifest is lost with a line "manifest lost: ..." in place of "problem: ...", `manifest_lost` in `--json`, and exit `0` for it; `archive list|show --json` give a registration over the lost manifest of an archive retired since, which an older version began, the state `archive_retired`.
> Amended by `design/decisions/0036-the-repository-database-made-again.md`: `repo rebuild-index` of a repository whose database is missing, empty, not a database or damaged puts it aside, makes it again and exits `2`, saying that the verification history is lost; its `--json` gives `replaced_database` (null, or `state`, `moved_to` and `set_aside`, every file put aside); every other command, `repo init` too, exits `11` naming `repo rebuild-index` for such a database, for one made again whose rebuild did not finish, and wherever SQLite finds the database damaged or not a database while it runs. `repo rebuild-index`, `import` and `export` show their progress on stderr and exit `4` when cancelled, changing nothing (R-94; for import and export decision 0030); a rebuild that stops after it made the database again names the database put aside, the history lost and the rebuild to run again (exit `4`, or `11` where it failed; `replaced_database` in its `--json`).
> Amended by `design/decisions/0029-network-roots-are-refused.md`: `register`, `verify --full <root_path>` and `archive relocate` exit `10` for a root on a network share, or on Windows one written in the `\\?\` or `\\.\` form, before anything of it is read; they take a volume's GUID path in the `\\?\` form (`\\?\Volume{...}\`), also for the volume's root folder, and `media list` shows a volume that has no drive letter or folder path under that path.
> Amended by `design/decisions/0032-registration-and-relocation-write-chunk-lines.md`: `archive show` gives each revision's chunk size, number of chunks and Merkle root from its manifest (`--json`: `chunk_size`, `chunks`, `merkle_root` in each member of `revisions`); `register --json` gives `counts.chunks`; `verify --json` gives the item of a file whose content differs, compared by chunk, `differing_chunks`, `chunks` and `first_differing_offset`; `archive relocate` prints the warnings of its FULL verification, as `verify` does, `--json` gives them as `warnings`, and its refusal of a new carrier that differs gives each entry's detail. The command whose open upgrades the repository's database says on stderr, once, where the copy of the database from before the upgrade is kept, also with `--json`, and before the error when the open then fails; one whose copy cannot be made exits `11`, the repository not upgraded. `repo info` says where each such copy is for as long as it is in RepositoryRoot, also after the open that made it, and `--json` gives their paths as `database_copies_before_upgrade`; where the repository cannot be opened once RepositoryRoot is resolved, it says so after its error, on stderr, and its `--json` error object gives `database_copies_before_upgrade` too.
> Amended by `design/decisions/0033-a-full-verify-adds-chunk-hashes.md`: `verify --full <root_path|archive_uuid> --add-chunk-hashes` adds chunk hashes; it exits `12` for an archive that has them or is retired, and while another operation that creates the archive's next revision is unfinished, and `verify --json` gives a run that adds them `chunk_hashes` (`chunk_size`, `added_revision`, `chunks`, `merkle_root`, `not_added`). `register` and `archive relocate` without `--resume` exit `12` while a verification that adds chunk hashes to the archive is unfinished. `archive abandon-verification <uuid> [--confirm] [--json]`, exit codes as `abandon-registration`'s. `archive list` and `archive show` show such a verification under the archive ("Unfinished verification of revision N adding chunk hashes, from "R": P; S"), and their `--json` gives it as `unfinished_verification`, except where the archive is retired with its latest manifest lost (decision 0022, point 5).
> Amended by `design/decisions/0029-network-roots-are-refused.md` (point 7): `register`, `verify --full` and `archive relocate` exit `10` for a root that is not there ("the folder or volume P was not found", and on Windows, where no volume is attached as its drive letter, asking to check that the device is attached); `verify --full` takes an archive's uuid, a folder or volume that is there, or the first 8 or more characters of an archive's uuid, as the other archive commands do; it exits `10` too where it does not find what it was given (an archive's uuid that names no registered archive, an archive whose volume is not attached or whose folder is not on its attached volume, which it names), and for an empty folder that is no registered archive, asking to check that the device of the volume mounted there is attached.
> Amended by `design/decisions/0005-copies-are-separate-archives.md`: `register --copy-of` takes the original's uuid or the first 8 or more characters of the uuid of one archive of this repository, and exits `10` naming where the whole uuid is shown where they start no archive's uuid, or several.
> Amended by `design/decisions/0037-what-opening-the-repository-remembers-and-says.md`: `veriarchive-cli --version` prints the program's version (exit `0`; `--json`: `app_version`), and `repo info` shows it too (`app_version` in its `--json`).
> Amended by `design/decisions/0041-a-comparison-says-when-each-revision-was-recorded.md`: `archive compare` says of each archive which revision is compared and when it was recorded (`A: <label>, revision N recorded <time>, <n> entries`), where it said `registered <time>`; `--json` names that time `created` in `archive_a` and `archive_b`, where it was `registered`.
> Amended by `design/decisions/0044-device-alerts-a-devices-health-and-a-slower-read.md`: `media list` shows each device's health (`health` in `--json`); `register`, `verify` and `archive relocate` print a device alert on stderr as soon as the devices were asked, and go on; their reports name each alert below the result and each device's health (`device_health` in `--json`); `verify` says its read speed and how it compares (`read_speed`, `read_compared`); `archive list` and `archive show` mark the archive's last verification with its device alerts, and `history` each run (`device_alerts` in `--json`); no exit code changes.
> Amended by `design/decisions/0046-the-window-is-built-with-libadwaita-around-the-archives.md`: `archive retire` exits `12` for every archive while a registration (its first one included), a relocation or a verification that adds chunk hashes of it is unfinished, in the words in which such an operation meets a registration; `register <root> --resume` of a registration of an archive retired since exits `12`, naming `archive abandon-registration`, and `archive list|show --json` give it, and a stopped relocation of such an archive, the state `archive_retired`; a registration or relocation whose archive was retired while it ran records nothing; `verify` says when the run that its read speed is compared with finished ("..., which finished <time>, read N MiB/s").
> Amended by `design/decisions/0047-a-full-verification-may-accept-a-device-behind-a-changed-enclosure.md`: no new option, and the command line never asks for administrator permission: `verify --full <archive_uuid>` of an archive behind a changed enclosure stops `MIGRATED`, naming `archive relocate`, as before (exit `12`); `verify --full <archive_uuid>` of an archive attached only on another device (`MIGRATED`) stops so where its folder is there on that volume, where it said "archive U is not on any attached volume" (exit `10`); `verify --full ... --resume <run_id>` of a run that went on past a changed enclosure in the window goes on with its confirmation, and says what it recorded of the new enclosure ("new enclosure recorded: revision N records it (...)", "new enclosure not recorded: ..."); `history` notes a run's confirmation ("enclosure changed, same device by its serial", "... confirmed by the operator after reading the device's serial", "... without the serial"); `history --json` gives each run `carrier_confirmed` (null, or `how`, `device`, `enclosure_recorded`, `enclosure_now`, `from_revision`), and `verify --json` gives `carrier_confirmed` (null, or the same with `revision` and `not_recorded`).
> Amended by `design/decisions/0048-the-verification-report-of-a-run-is-one-html-file.md`: `report <run_id> --out FILE.html [--json]` writes the verification report of one recorded run that reached a result (named by its whole id, as `history` lists it), one self-contained HTML file in the language of `--lang`, the file that the window's Save report... makes; it records nothing in the repository (opened as every command opens it, decision 0032's upgrade included) and writes FILE through the atomic writer. It exits `10` for a usage error, a FILE that is there already (`report` never replaces a file: it looks before it opens the repository, and its writer's commit names FILE only where nothing of that name is there then, `IOutputFile::commit_new()`, but for the one narrow window that §B9's note names; `export-sha256sums`, by contrast, replaces its FILE), a run that is not recorded, an `INCOMPLETE` run, and a FILE in the root of an attached archive or below it, also written through a symbolic link, a junction, a drive letter made with subst or a short name (its next FULL verification would report the file as added); `11` where the repository cannot be opened or read, or FILE cannot be written; `0` once written. `--json` gives `run_id`, `archive_uuid`, `result` and `out`.

```text
veriarchive-cli repo init [--repository P] | info | rebuild-index | verify-history
veriarchive-cli media list [--json]
veriarchive-cli register <root_path> [--label ARCH-NNNN] [--write-media-copy] [--io hdd|ssd] [--new-revision] [--copy-of UUID] [--resume]
veriarchive-cli verify --full <root_path|archive_uuid> [--resume RUN_ID] [--strict-metadata] [--io hdd|ssd]
veriarchive-cli verify --quick|--deep <...> [--seed HEX]            (V1.1)
veriarchive-cli archive list | show <uuid> | retire <uuid> | relocate <uuid> <root_path> [--resume]
veriarchive-cli history <uuid> [--json]
veriarchive-cli export <uuid> --out DIR | import <file.vam> [--force-rollback]
veriarchive-cli export-sha256sums <uuid> --out FILE
veriarchive-cli key generate | list | trust <pubkey_hex>           (V1.1)
```

Exit codes: `0` PASSED · `2` PASSED_WITH_WARNINGS · `3` FAILED · `4` INCOMPLETE · `10` usage error · `11` configuration/repository error · `12` identity needs operator decision (`MIGRATED` / `AMBIGUOUS`). Every command supports `--json`.

## B17. GUI (M5)

> Amended by `design/decisions/0016-gtk4-c-api-for-the-gui.md`: the GUI uses the GTK4 C API (no gtkmm).
> Amended by `design/decisions/0017-interface-language.md`: Settings has a language drop-down (English, 繁體中文).
> Amended by `design/decisions/0020-archive-comparison.md`: a sixth view, Compare Archives, after Verify Archive; it reads no media, shows progress in entries and allows cancel, and has no resume (it writes nothing). A screenshot mode (`VERIARCHIVE_GUI_SCREENSHOTS`) makes the user guide's pictures.
> Amended by `design/decisions/0022-unfinished-operations-in-archive-list.md`: above the archives, the Archive List shows the registrations and verifications that have not finished, wherever they were started, with their progress (counted only in WAL mode) and a state that claims no more than the repository shows (`running`, `... it may have stopped`, `stopped; it can be resumed`); it reads the repository read only, every 5 s while shown, reads the archives again when such a read finds them changed, and leaves out stopped verifications that cannot be resumed (R-104) or that a later verification of the same archive supersedes, and every unfinished verification of a retired archive whose latest manifest is lost (decision 0028), which can never be resumed, leaving their records as they are; another retired archive's are listed and resumed as any archive's. A stopped verification that can be resumed, and a stopped registration, have **Resume**, and so has a registration whose plan never says when it worked, an older version's, once it reads `... it may have stopped`, unless it can no longer finish; there Resume asks first where the plan names no process, or an older version's resume took it up, with Cancel as the default, as the older version may still run it and would then replace the manifest that the resume recorded as it finishes (decision 0023), and resumes only once the operator chose Resume, as does a resume of such a registration that Register Archive starts; Resume continues it exactly as `verify --full <archive_uuid> --resume <run_id>` or `register <root> --resume` does, given no option: no check box of Verify Archive or Register Archive takes part, so it writes no media copy (R-03) and takes strict metadata from the policy alone; its progress and report show in Verify Archive or Register Archive; a relocation has none. Verify Archive, verifying a chosen archive by its uuid, stops where more than one attached volume holds it, naming their roots and saying to type or choose the folder of the one to verify, with "The folder or volume above" chosen, or to detach the other volumes.
> Amended by `design/decisions/0024-attempts-record-their-process.md`: a registration whose attempt's process has ended reads `stopped; it can be resumed` at once; one whose process still runs, but whose plan has said nothing for minutes, reads `no progress reported for N minutes; its process is still running`.
> Amended by `design/decisions/0025-abandoning-an-unfinished-registration.md`: the row of a stopped registration, and of an older version's registration that reads `... it may have stopped`, has **Abandon...**, which reads what abandoning removes with the CLI's check, then asks first, saying that the saved progress will be lost.
> Amended by `design/decisions/0026-relocation-resumes.md`: the unfinished operations include relocations ("Relocation"), wherever they were started, with their progress and a state judged as a registration's (decision 0024), and `new carrier differs; abandon it` for a stopped one whose new carrier was found to differ; a stopped one has **Abandon...**, as a registration's row. The Verification History marks the runs of a relocation in a column Note, shown only for an archive that has such runs.
> Amended by `design/decisions/0028-a-manifest-that-cannot-be-used.md`: Verify Archive verifies a registered archive chosen by its uuid, so when its manifest cannot be used the report says `FULL VERIFY FAILED`, that the media was not read, and how to fix the manifest. The Archive List lists every archive: one whose latest manifest cannot be used shows what is wrong and how to fix it under its row, in red, in either language, the command line's registration over a lost manifest named as the last resort, and retiring the archive where its device is gone as well, and stays one to choose in Verify Archive; a retired archive whose latest manifest is lost shows "Manifest lost: ..." under its row, not in red; a stopped registration over the lost manifest of an archive retired since reads `archive is retired; abandon it`, with **Abandon...** and without Resume. A verification that read no media supersedes no unfinished verification (decision 0022), so the run that a failed resume was to resume stays listed, resumable, unless its archive is retired with its latest manifest lost (decision 0022, point 5).
> Amended by `design/decisions/0030-import-names-its-manifest-in-the-record-transaction.md`: a stopped registration or relocation made for an older revision than its archive has now reads `archive has a later revision; abandon it`, with the stopped operations, and has **Abandon...**.
> Amended by `design/decisions/0029-network-roots-are-refused.md`: Register Archive and Verify Archive refuse a root on a network share, or on Windows one written in the `\\?\` or `\\.\` form, before anything of it is read; they take a volume's GUID path in the `\\?\` form (`\\?\Volume{...}\`).
> Amended by `design/decisions/0032-registration-and-relocation-write-chunk-lines.md`: before an open that will upgrade the repository's database, the window shows at once, with the notice "Upgrading the repository database..." and a moving progress bar in place of the views, also while the open waits for another process that upgrades it; once the copy and the upgrade are done, the window shows its views, Settings first, which says where the copy of the database from before the upgrade is kept, as Settings does in every window for as long as the copy is in RepositoryRoot, also after why no repository is open; an open that fails says why no repository is open in that window, in place of the notice, followed by where each such copy is.
> Amended by `design/decisions/0033-a-full-verify-adds-chunk-hashes.md`: Verify Archive has the checkbox "Full verify, and add chunk hashes", offered only for a registered archive chosen in its list whose current revision has chunk size 0 and that is not retired; the Archive List shows an unfinished verification that adds chunk hashes ("Verification adding chunk hashes"), which no later verification supersedes, its state judged by the process its record names, as a registration's (decision 0024); a stopped one has **Abandon...**, and Verify Archive's Resume continues it once its archive is chosen in the view's list, also after the window that ran it was closed; Verify Archive and Register Archive, refused while another operation that creates the archive's next revision is unfinished (a registration, a relocation, or a verification that adds chunk hashes), name the windows' own ways on where the command line names its commands (first Resume and Abandon... on the operation's row in the Archive List, then Resume interrupted run or Register Archive's "Resume an interrupted registration of this folder or volume"; a relocation, which has no window and no Resume in the list, keeps its command line for continuing), and say instead that it still runs, and to wait for it, only while one read finds that operation running or held up; a record that says nothing of its state, as an older version's, counts as one that cannot be judged, and the ways on are named.
> Amended by `design/decisions/0037-what-opening-the-repository-remembers-and-says.md`: Settings shows the program's version first, also while no repository is open, and R-12's warning also under the reason no repository is open.
> Amended by `design/decisions/0041-a-comparison-says-when-each-revision-was-recorded.md`: Compare Archives says of each archive when its compared revision was recorded (`revision N, recorded <time>`), where it said `registered <time>`.
> Amended by `design/decisions/0043-the-window-keeps-the-computer-awake-and-says-when-an-operation-ended.md`: while an operation of the window runs, the computer does not sleep on its own; one that ends while no window of the program is active is told in a notification of the system, its body the first two lines of the report that hold text; the command line does neither.
> Amended by `design/decisions/0044-device-alerts-a-devices-health-and-a-slower-read.md`: Register Archive and Verify Archive show a device alert in red above the progress as soon as the devices were asked, and the operation goes on; their reports name it below the result, with each device's health and, for a FULL verification, its read speed; the notification that an operation ended adds every alert line; the Archive List marks the last verification of an archive in red with its device alerts until a later one gives none, and the Verification History marks each run in its column Note.
> Amended by `design/decisions/0045-the-window-runs-its-operations-as-jobs.md`: the window runs every registration, verification and comparison as a job of its own, several at once; jobs that read the same device run one after another, in the order started, others at once, a comparison at once; a page Jobs lists them with their state, progress and device alert, Stop and Details (a window with the job's progress and report); the view that started a job shows it; closing the window while jobs run asks first, and stops them, each resumable; the Archive List judges each row by the job that works on its root or archive, and abandoning or resuming an operation that a job works on is refused.
> Amended by `design/decisions/0046-the-window-is-built-with-libadwaita-around-the-archives.md`: the window is built with libadwaita: a sidebar of the pages Archives, Devices, Jobs, History and Settings beside the page shown, and in each page's header the jobs button with its popover. Archives holds the unfinished operations, the list of archives (each with where it is attached now, its last FULL verify with its age, its result and its marks) and the details of the one chosen, from which it is verified (FULL verify, its menu: adding chunk hashes, strict metadata, a folder or volume), compared, given a new revision, exported, relocated, a stopped relocation resumed, and retired; the Archive List, Verify Archive and Verification History are gone, each of their items kept there or in History (the decision lists where each lives). Register Archive (three pages: the source among the attached volumes or a folder, the options that apply there, among them the archive it is a copy of, and a summary), Compare Archives, the relocation and a job's details are dialogs; each recorded run has a result page. Devices lists the attached devices with their health, their volumes and the archives on them, read off the window's thread every 30 s; History lists the runs of every archive, newest first, with a filter and the notes in each row. Settings shows the program's version in its last group. A relocation and an export run as jobs. Retiring asks first, and for a lost manifest a confirmation. The screenshot mode lists sample devices and volumes, and shows the volume of the sample folders only through the sample identity, never another device or volume of this computer.
> Amended by `design/decisions/0047-a-full-verification-may-accept-a-device-behind-a-changed-enclosure.md`: an archive whose carrier is found behind a changed enclosure (`MIGRATED`, not retired) shows "The enclosure differs from the one recorded" in Archives, and in its details a notice with Confirm and FULL verify... in place of Relocate to this carrier..., and the enclosure's serial as recorded and seen now; its FULL verify asks first, in a dialog, whether it is the same device: Read the device's own serial, which starts the device identity reader through Windows' question for administrator permission, a read of the archive's confirmed device then verifying at once, another device's offering Register as another archive..., one with nothing to compare shown for the operator to compare with the device's label; or Confirm without reading the serial..., which asks once more, saying that a sector-by-sector clone passes too and that the history records it. The question reads the device again each time. While it runs, at its end and on its result page the window says whether the new enclosure was recorded, and History notes the confirmation. Devices reads a device's own serial behind an enclosure that hides it on request, recording nothing itself, and names an archive found under a volume behind a changed enclosure so; Register Archive records the device's own serial with "Record the device's own serial", off by default, or, in that switch's place, a read of that device that the window keeps. The window itself stays unprivileged (R-08); on Linux, or where the reader is missing, it offers no read. The screenshot mode uses a sample reader that asks Windows nothing.
> Amended by `design/decisions/0048-the-verification-report-of-a-run-is-one-html-file.md`: the result page of a run that reached a result offers Save report... in its banner, which saves the run's verification report, one self-contained HTML file in the window's language that says what the page says, with every item, the archive's runs, the history chain's state when it was made and the run's `row_hash`: through the system's save dialog, which proposes the archive's label, "verification report", the date the run ended and its result as the file's name, in the folder last saved into in this window, else Documents, and asks before it replaces a file; made and written off the window's thread, with no progress or cancel, as it is one read and one file; then a toast with Open. An `INCOMPLETE` run's page says that no report can be saved for it. A folder in the root of an attached archive, or below it, is refused for a report, and for Export manifest... on its job's thread, also where it is written through a symbolic link, a junction, a drive letter made with subst or a short name (`app::roots_holding`). The screenshot tour saves the sample run's report as well, which is no picture of the guides and is moved out of `docs/images`.
> Amended by `design/decisions/0049-archives-has-an-overview-and-jobs-lists-the-unfinished-operations.md`: Jobs lists the unfinished operations below the jobs, with Resume and Abandon..., those a job of the window works on once only, and the jobs button and the Jobs row count those that stopped; Stop of a registration, verification or relocation says that it keeps what it did. Archives shows an overview above the archives (how many need attention, how many are attached, the latest FULL verification, and a tile for each standing that filters the list), and Verify the attached archives that need it, which FULL verifies those OVERDUE, never FULL verified or last FAILED that are attached as recorded. The head of an archive's details stays in view while they scroll, with FULL verify and Cross-check.... The window calls a comparison a cross-check (核對完整性), takes neither archive for the original (Archive A, Archive B; only in A, only in B, differs) and lists each archive's last FULL verification in its report.

Views: Register Archive, Verify Archive, Archive List, Verification History, Settings.
Archive List columns: label, short uuid, volume label, model, capacity, last FULL (result/date), last verify (mode/result/date), OVERDUE, revision.
Settings shows: active RepositoryRoot and `repo_selected_by`, `repository_uuid`, config file path, trusted keys (V1.1), backup reminder (R-86).
Every long operation shows progress, allows cancel, and offers resume for INCOMPLETE runs. The GUI runs unprivileged (R-08).

---

# Part C — Milestones and acceptance criteria

V1 = M1–M5. A milestone is done only when its acceptance tests pass on **both** OSes in CI.

## M1 — Skeleton, config, repository

Build: all targets compile under both presets; `repo init` / `repo info` work.
Acceptance:
- Repository-resolution matrix (R-83) as parameterised tests: explicit path; pinned path; pinned-but-unavailable → exit 11, no fallback; first run with writable exe dir → `<exe>/data`; unwritable exe dir → `<home>/VeriArchive`; launched from a different cwd; network-filesystem refusal (filesystem-type probe injected); relative pins (decision 0019): a portable first run pins `"data"`, a copied program folder opens its own repository, a relative pin in the user config file follows that file, an absolute pin still works and is not rewritten.
- `repository.json` created; `schema_version` = 1 (2 since decision 0032, 3 since decision 0033); every §B8 table exists.
- Canonical JSON writer golden tests (escaping, key order, integers, omitted optionals).
- `ArchivePath` round-trip tests for R-40/R-41 on both platforms.

## M2 — Media enumeration and identity

`media list` on both OSes for: internal SATA/NVMe, USB HDD through a bridge with and without an exposed serial, exFAT stick, NTFS external disk.
Acceptance: `VolumeInfo` JSON has fs type/uuid/label, capacity, ≥ 1 device with model; missing serial does not error; `volume_for_path` resolves through the mount to the physical device(s) (Windows disk extents; Linux `/sys/dev/block/…/slaves`); NTFS serial relationship established (§B1.3); every call in R-09 confirmed unprivileged; `IdentityMatcher` unit tests cover every §B10 row.

## M3 — Register + Full Verify (the core proof)

Fixture tree: 100 000 files, mixed sizes, ≥ 20 GB total; names incl. 中文, 日本語, NFD, emoji, a non-UTF-8 name on Linux, an unpaired surrogate on Windows; empty dirs; symlink; hardlink; sparse file; paths > 260 chars.
Acceptance:
- Register on Windows → verify on Linux (same medium; NTFS via `ntfs3` read-only, and exFAT) → `FULL VERIFY PASSED` or `PASSED_WITH_WARNINGS` with only `Unrepresentable`/portability warnings; and the reverse direction.
- Golden: `.vam` byte-identical across OSes for the golden tree (R-66).
- Fault matrix: 1-byte change → `Modified`; delete → `Missing`; add → `New`; `chmod 000` / deny ACL → `Unreadable(permission)`; injected `MediaError` → `Unreadable(media)`; file→dir → `TypeChanged`; OS-created `System Volume Information` → not reported.
- Uncached reads: Linux `Cached` in `/proc/meminfo` does not grow by the archive size; Windows handle audit shows `FILE_FLAG_NO_BUFFERING`.
- Throughput ≥ 80 % of `dd` / raw sequential read on the same HDD; hashing is not the bottleneck on NVMe with ≥ 4 cores.
- `export-sha256sums` output passes `sha256sum -c` on Linux.
- `register` on a volume already registered is refused without `--new-revision` (R-97).

## M4 — Cancel, resume, history chain, export/import, relocate

> Amended by `design/decisions/0047-a-full-verification-may-accept-a-device-behind-a-changed-enclosure.md`: an enclosure swap (`MIGRATED`) is accepted by a relocation, or by a FULL verification with a carrier confirmation in the window, which records the new carrier as the next revision only when it passes, and is not prompted again afterwards.

Kill `verify --full` at ~50 % → `INCOMPLETE`; `--resume` finishes without re-hashing verified items (measured by bytes read). `repo verify-history` detects a manually edited or deleted `verify_runs` row. `export` → `import` round-trip into a second repository; import of a lower revision is rejected; label collision displays per R-88. `archive relocate` (decision 0004): a copy on a new volume is accepted only after a FULL verify of every entry; an enclosure swap (`MIGRATED`) is accepted the same way and not re-prompted afterwards; known-unreadable entries do not block it (decision 0010); the old carrier then matches as `PREVIOUS_CARRIER`.

## M5 — GTK4 GUI

> Amended by `design/decisions/0046-the-window-is-built-with-libadwaita-around-the-archives.md`: the views of §B17 are the pages Archives, Devices, Jobs, History and Settings, the dialogs Register Archive, Compare Archives and the relocation, and the result page of a run.

All views of §B17 (six, with decision 0020's Compare Archives); results per §A6 / R-21; progress + cancel + resume; Settings per §B17; Windows package passes R-121; runs unprivileged under Wayland.

## V1.1 (after V1 acceptance)

> Amended by `design/decisions/0039-exclusions-helper-and-pinned-keys-are-not-built.md`: no `veriarchive-helper` + IPC; the Windows USN tracker (R-107) runs in the main process, unprivileged; reads with `SeBackupPrivilege` / `CAP_DAC_READ_SEARCH` are put off until an archive needs them.

Chunk lines + Merkle (§B4.4, §B6) · Ed25519 signing, `.sig`, trusted keys, `key_id` in results (§B5) · QUICK / DEEP with seed and coverage (§B11) · history checkpoints (R-76) · `veriarchive-helper` + IPC · Windows USN tracker (R-107) · NTFS ADS entries (`s`) · `SeBackupPrivilege` / `CAP_DAC_READ_SEARCH` reads.
Constraint: no change to §B4 header/entry keys (only filling `chunk_size`, `merkle_root`, chunk lines, `s` entries); no `ALTER TABLE` on V1 columns (R-92).

## V2

Whole-disk fingerprint (§B14) · btrfs change tracking · hardware-token / service `ISigner`.

---

# Part D — Open decisions (do not assume; ask the operator)

| # | Question | Default if unanswered before M3 |
|---|---|---|
| D1 | ~~Linux driver for verifying Windows-written NTFS media: kernel `ntfs3` or `ntfs-3g`? (affects mtime precision and ADS visibility)~~ | **Resolved** by `design/decisions/0006-ntfs3-reference-driver.md`: `ntfs3` read-only is the reference; `ntfs-3g` supported with an informational note; `filesystem_type` is the on-disk format name |
| D2 | ~~V1.1 chunk size: 64 MiB vs 16 MiB (smaller = finer corruption localisation, larger index)~~ | **Resolved** by `design/decisions/0031-chunk-lines-and-the-merkle-root.md`: 64 MiB (67108864), fixed per archive; a manifest's chunk size is 0 or a multiple of 64 KiB up to 1 GiB, sizes below 64 MiB for tests only |
| D3 | ~~Overdue defaults (365 / 90 days) and whether OVERDUE blocks QUICK results~~ | **Resolved** by `design/decisions/0007-overdue-defaults.md`: 365 / 180; OVERDUE flags only |
| D4 | ~~Compile-time pinned keys for production builds (R-73)?~~ | **Resolved** by `design/decisions/0039-exclusions-helper-and-pinned-keys-are-not-built.md`: off; no build embeds a key list until signing is built, when the question is asked again |
| D5 | ~~Is writing `.veriarchive/` to sealed media ever acceptable in this organisation?~~ | **Resolved** (2026-10-08): off by default (R-89); `register` without `--write-media-copy` prints a one-line notice that the repository then holds the only copy of the baseline and must be backed up (R-86) |
| D6 | ~~Windows helper authorisation: Windows service, or UAC-elevated helper launched on demand?~~ | **Resolved** by `design/decisions/0039-exclusions-helper-and-pinned-keys-are-not-built.md`: there is no helper; the USN change journal is read without elevation; amended by `design/decisions/0047-a-full-verification-may-accept-a-device-behind-a-changed-enclosure.md`: one program is launched on demand through UAC, the device identity reader, from the window for one read of a device's own identity; no service |
| D7 | ~~Retention of `verify_items` detail (storage growth on large archives)~~ | **Resolved** (2026-10-08): non-Verified items only, kept forever |
| D8 | ~~Registering a *directory* (not a volume root) as an archive: allowed in V1, or volume roots only?~~ | **Resolved** by `design/decisions/0001-archive-matching-includes-root-subpath.md`: allowed; overlapping roots refused; matching keyed by volume evidence + `root_subpath` |
| D9 | ~~Linux without the udev database (containers, minimal systems; seen on the GitLab runners, 2026-10-14): neither the filesystem UUID nor the SATA/SCSI drive serial can be read, so every volume is `UNKNOWN` — registration is never refused (R-97) and verification never finds the archive. Proposed (a decision record to be written): (a) fall back to the kernel filesystem id (`statvfs` `f_fsid`, recorded as `fsid-…`) for ext2/3/4 and btrfs, where the kernel derives it from the filesystem UUID (not xfs, where it follows the device number); an `fsid-…` id compared with a real UUID counts as unavailable, so decision 0018 applies; (b) read the drive serial from SCSI VPD page 0x80 (`/sys/block/<disk>/device/vpd_pg80`) when udev is missing.~~ | **Resolved** by `design/decisions/0021-linux-without-udev.md`: the kernel filesystem id (`fsid-…`) for ext2/3/4 and btrfs, compared only with another `fsid-…` id (against a UUID, decision 0018 applies); for a SATA device behind libata that udev does not know, the device serial from SCSI VPD page 0x80 |
| D10 | ~~`register --resume` while another attempt of the same registration may still run (decision 0023): refuse it? An attempt whose process was killed cannot be told from one that works (decision 0022 sees only that its plan went quiet), so a refusal needs a record of the running process, or an operator override.~~ | **Resolved** by `design/decisions/0024-attempts-record-their-process.md` (option c): every attempt records in its plan which process runs it (id and start time, on which computer); `register --resume` refuses while that process still runs, naming it, and goes ahead as soon as that process has ended; a plan without the record keeps first-to-record-wins; the Archive List reads a registration whose process has ended as stopped at once |
| D11 | ~~`archive relocate` while a `register --new-revision` of the same archive is unfinished: both create the archive's next revision. Which should win? (a) The relocation refuses while the registration is unfinished; but nothing removes an unfinished registration whose root is gone, so the relocation, which exists for carriers that go away, could be blocked for good. (b) The relocation discards the registration's plan and checkpoints, which can hold hours of hashing.~~ | **Resolved** by `design/decisions/0025-abandoning-an-unfinished-registration.md`: first to record wins, and the relocation refuses once the registration has checkpointed files for the revision (decision 0023, unchanged); the operator may abandon an unfinished registration on purpose (`archive abandon-registration <uuid> --confirm`, or Abandon in the Archive List), which removes its plan and checkpoints in one transaction, never while its attempt runs, and with its first registration the record of an archive that never had a revision |

---

# Appendix — Agent quick rules (copy into CLAUDE.md)

> Amended by `design/decisions/0039-exclusions-helper-and-pinned-keys-are-not-built.md`: rule 9 has no V1.1 helper to send an API to; an API that needs admin/root is left out until a decision record says how it is made.
> Amended by `design/decisions/0047-a-full-verification-may-accept-a-device-behind-a-changed-enclosure.md`: rule 9 reads: "V1's programs run unprivileged; the one elevated program is the device identity reader of decision 0047, started through UAC from the window only; further elevated operations belong behind helper interfaces."

1. Read `design/HANDOFF.md` §A3, §A6, §B4, §B7.4, §B9 before writing code. Part B is frozen: propose changes as `design/decisions/NNNN-*.md`; do not just implement them. Accepted records override the HANDOFF text they amend; each amended section names its records.
2. `core` / `common` never include OS headers and never link `veriarchive_platform`. OS selection lives in CMake only.
3. Validated manifest bytes are the truth; SQLite is an index. Never verify against DB rows.
4. Paths: `std::filesystem::path` and `operator/`; persist with `u8string()`. `path::string()` and `current_path()` are banned in `src/`.
5. RepositoryRoot: explicit → pinned → first-run fallback. Configured-but-unavailable is an error, never a fallback.
6. Fallible calls return `std::expected<T, IoError>`. No exceptions across the platform boundary.
7. Every long operation takes `IProgressSink&` and `CancelToken&`.
8. Windows entry point is `wmain` with `-municode`; embed `longPathAware` + UTF-8 `activeCodePage` manifest.
9. Unprivileged only. If an API needs admin/root it belongs to the V1.1 helper behind `IRawDeviceAccess` / `IChangeTracker`.
10. Golden test first: anything that changes `.vam` bytes for the golden tree needs a format version bump and a decision record.
11. Build/test: `cmake --preset linux-release && cmake --build --preset linux-release && ctest --preset linux-release` (Windows: `windows-ucrt64-release` inside an MSYS2 UCRT64 shell).
12. When a rule here is unclear, stop and ask; do not pick the easiest interpretation.
13. Name things with `GLOSSARY.md` terms in identifiers, docs, and CLI/GUI text; its _Avoid_ words are rejected synonyms.
