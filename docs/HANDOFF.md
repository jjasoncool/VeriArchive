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

> Amended by `docs/decisions/0021-linux-without-udev.md`: R-09 on Linux also uses `statfs` on the mount point (the kernel call behind `statvfs`) for the kernel filesystem id.
> Amended by `docs/decisions/0027-subst-drive-letters.md`: R-09 on Windows also uses `QueryDosDeviceW`, which resolves a drive letter made with subst to the folder it stands for before the volume is looked up.
> Amended by `docs/decisions/0029-network-roots-are-refused.md`: R-09 on Windows also uses `GetDriveTypeW`, which tells a drive letter mapped to a network share, so that a root on one is refused before its volume is looked up.

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

| Result | Meaning |
|---|---|
| `PASSED` | All checks of the mode passed, no warnings. |
| `PASSED_WITH_WARNINGS` | Content checks passed; non-content issues exist: metadata mismatch within policy, `MATCH_WEAK` identity (no hardware serial), portability warnings, media copy differs from repository copy. |
| `FAILED` | At least one content, identity, signature, revision, or continuity check failed. |
| `INCOMPLETE` | Cancelled, interrupted, or device disappeared. No PASS/FAIL claim. Resumable (§B11.4). |

### Mode label

Every result is prefixed by its mode. Only `FULL VERIFY PASSED` means "100 % of registered content was read and matched". `QUICK VERIFY PASSED` / `DEEP VERIFY PASSED` mean "identity, manifest, and the recorded sample passed"; the UI MUST show coverage (bytes and chunks) next to any sampled result.

### Item status

> Amended by `docs/decisions/0010-unreadable-entries-at-registration.md`: adds `Unbaselined`; maps unreadable entries to run results (newly unreadable → `FAILED`; unreadable since registration → at most `PASSED_WITH_WARNINGS`).

`Verified`, `Modified`, `Missing`, `New`, `Unreadable(permission|media|removed|other)`, `MetadataMismatch`, `Unrepresentable`, `TypeChanged`, `SkippedMountPoint`.

### Run-level failure classes

> Amended by `docs/decisions/0028-a-manifest-that-cannot-be-used.md`: a manifest that cannot be used fails its verification with `ManifestHashMismatch` when it has changed (its line 1 or its body is not the one recorded) and with `RepositoryError` when it is missing or cannot be read, or the index says otherwise of a manifest whose line 1 is the one recorded (its copy of the header, or a record that names another body, archive or revision than that line), or has no record of the archive's revision (as where `repo rebuild-index` could not read its manifest); a verification of an archive named by its uuid or by the run it resumes fails so before it looks for the media, `FAILED` without reading it, and says so.

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

> Amended by `docs/decisions/0029-network-roots-are-refused.md`: a root on a network share (Windows: a network path, or a drive letter that Windows reports as a network drive; Linux: a network filesystem such as NFS, SMB or sshfs) is on no volume of this computer; `register`, `verify` by root and `archive relocate` refuse it before anything of it is read, and on Windows also a root written in the `\\?\` or `\\.\` form, except a volume's GUID path in the `\\?\` form (`\\?\Volume{...}\`), the only name of a volume that has no drive letter or folder path: it names the volume's root folder, and through it a folder is the same archive as through the volume's drive letter; written with `.`, `..` or separators in a row after the GUID, it names the folder they resolve to, which is the folder read, as Windows reads that form as given.

- R-30 An **archive** is a logical file tree rooted at `archive_root`: a mounted volume root, or a directory on exactly one volume. Registration MUST refuse a root spanning multiple volumes; mount points below the root are not descended and are reported as `SkippedMountPoint`. Linux determines mount boundaries from `/proc/self/mountinfo`, not `st_dev` (btrfs subvolumes have distinct `st_dev` on the same volume).
- R-31 Chunks (V1.1) are per-file; `offset` is relative to the start of the file. A file with `size ≤ chunk_size` has exactly one chunk equal to the whole file, so its chunk hash equals its file BLAKE3 and no chunk record is written.
- R-32 The whole-disk hash (V2) is a **media fingerprint** stored in `media_evidence.disk_fingerprint`, never archive content and never part of the manifest.

### B1.2 Types (`common/types.h`)

> Amended by `docs/decisions/0006-ntfs3-reference-driver.md`: `filesystem_type` is the on-disk format name, never the mount driver name.

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

> Amended by `docs/decisions/0021-linux-without-udev.md`: on Linux, when neither udev nor `/dev/disk/by-uuid` gives the UUID of an ext2/3/4 or btrfs volume, `filesystem_uuid` holds the kernel filesystem id (`statfs` `f_fsid`, both 32-bit words, first word low; never zero) as `fsid-` + 16 lowercase hex, the form volumes without a block device already use; an `fsid-…` id is compared only with another `fsid-…` id (§B10).

- ext4/XFS/btrfs: lowercase RFC 4122 string, identical on both OSes.
- exFAT/FAT32: 32-bit volume serial; store as 8 lowercase hex (`a1b2c3d4`), no dash; identical on both OSes.
- NTFS: Linux (`blkid`/udev) reports the 64-bit boot-sector serial as 16 hex; Windows `GetVolumeInformationW` returns a 32-bit value. Windows MUST attempt `FSCTL_GET_NTFS_VOLUME_DATA` for the 64-bit serial and fall back to the 32-bit value. M2 MUST establish the exact relationship between the two on the same medium and encode it in a unit test; matching (§B10) uses `filesystem_uuid` when both sides have 64-bit values, otherwise `filesystem_uuid_short`.

## B2. Path and filename rules

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

> Amended by `docs/decisions/0008-link-target-canonical-form.md` (canonical link target, cross-OS comparison) and `docs/decisions/0009-only-name-surrogates-are-links.md` (on Windows only name-surrogate reparse points are `l`; other reparse points are `f`; recall-on-access files are never opened).

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

> Amended by `docs/decisions/0023-manifest-named-in-the-record-transaction.md`: R-61's writer streams the body to a file of its own (`rNNNN.vam.body-<uuid>`) and writes header + body to a temporary file that it fsyncs without holding the database lock; the rename to `rNNNN.vam` happens inside the write transaction that records the revision, after it found the revision not recorded, so a recorded manifest is never replaced.
> Amended by `docs/decisions/0028-a-manifest-that-cannot-be-used.md`: R-62's reader compares line 1 of a recorded revision's manifest with the BLAKE3 recorded for it before it parses it: a line 1 that differs, also one that no longer parses, ends without a newline or is empty, is a changed manifest (`ManifestHashMismatch`), never a malformed one. In the read of the entries of any manifest, a line that does not parse, comes out of canonical order or ends the file without its newline is a malformed manifest only when what was read, read on to the end of the body, is the body validated; otherwise the manifest changed while it was read (`BodyMismatch`).

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
- R-66 Golden test: the same fixture tree registered on Windows and on Linux MUST produce byte-identical `.vam` files after a test helper blanks `created`, `created_by`, `identity`, `root_hint`. Golden fixtures use relative links only (decision 0008); the cross-OS comparison also blanks `mode` and `attrs` (decision 0015).

### B4.3 Header (line 1; shown wrapped, real header is one line)

> Amended by `docs/decisions/0027-subst-drive-letters.md`: `root_hint` is the archive root by its real path; a drive letter made with subst is replaced by the folder it stands for (`S:\Photos` is recorded as `D:\Data\Photos`).

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

> Amended by `docs/decisions/0010-unreadable-entries-at-registration.md`: optional key `unreadable` replaces `b3`/`sha256` for entries unreadable at registration; key order `…,"sha256","unreadable","mode",…`. Link targets use the canonical form of decision 0008.

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

- Leaves: all chunk hashes in canonical order (entry order per R-43, `idx` ascending). Files with `size ≤ chunk_size` contribute their file `b3` as the single leaf; `l` and `s` entries contribute their `b3`; `d` entries contribute nothing.
- `leaf = BLAKE3(0x00 ‖ raw32(b3))`; `node = BLAKE3(0x01 ‖ left ‖ right)`.
- An unpaired node at any level is promoted unchanged to the next level (RFC 6962 §2.1 style).
- Zero leaves: `root = BLAKE3(0x00)`.
- Root stored in header `merkle_root` as 64 hex.

## B7. Repository

### B7.1 Layout

> Amended by `docs/decisions/0026-relocation-resumes.md`: R-80's `repo rebuild-index` rebuilds only what the manifests define and keeps the saved progress of unfinished registrations: the rows of `files` under the revision a plan creates, while the plan exists and no manifest defines that revision; `verify_progress` (the saved progress of verifications and relocations) is not touched.
> Amended by `docs/decisions/0028-a-manifest-that-cannot-be-used.md`: R-80's `repo rebuild-index` sets each archive to the latest revision that a manifest in its folder, read or not, or the record of a revision whose manifest it cannot read names, and never below the latest revision that a verification of it which read the media names, and keeps the record and the index of that revision where it cannot read its manifest (missing, or skipped): it never takes an earlier revision for an archive's current one, whose manifest then cannot be used. The record of an earlier revision whose manifest it cannot read goes with its index, as before. It names each revision whose manifest it cannot read and what became of its record, and exits 2 then too; it marks each archive whose revision's manifest it skipped, whose manifest the archive lists then read whole until a rebuild reads it.

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

> Amended by `docs/decisions/0017-interface-language.md`: adds `language` (`"en"`, `"zh-TW"`; absent = follow the system).
> Amended by `docs/decisions/0019-relative-repository-pin.md`: a relative `repository` is relative to the folder of the config file that holds it.

### B7.4 RepositoryRoot resolution (authoritative, R-83)

> Amended by `docs/decisions/0019-relative-repository-pin.md`: step 2 resolves a relative pin against the folder of the config file that holds it; step 3 chooses the config file first and writes a RepositoryRoot inside that file's folder relative to it, with forward slashes (a portable first run pins `"data"`), any other RepositoryRoot absolute as before; pins already written are never rewritten. R-84 is unchanged for `--repository` and `VERIARCHIVE_REPOSITORY`.

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

> Amended by `docs/decisions/0030-import-names-its-manifest-in-the-record-transaction.md`: R-87's import copies the validated manifest under a temporary name and flushes it without the database lock, and renames it to `rNNNN.vam` in the write transaction that records its revision, after finding the revision not recorded, as registration and relocation do (decision 0023): an imported manifest never replaces a recorded one. An import that finds its revision recorded meanwhile, or an unfinished registration with saved files of it, records nothing (exit 12); one that records it removes the plan of a registration and the record of a relocation that create that revision.
> Amended by `docs/decisions/0028-a-manifest-that-cannot-be-used.md`: R-87's import of a revision that is already known, whose manifest in the repository is missing, cannot be read or is not the one recorded, puts the file back when it is the one recorded (its line 1 has the BLAKE3 recorded for it, and its body is the one that line names), staged and named in a write transaction that finds the record still naming that line, as decision 0030 names a manifest; the record and its index stay as they are, and `--json` says `restored`. Any other file is `ManifestHashMismatch`, as before; a record that changed while the import ran refuses it (exit `12`).

- R-87 `export <uuid> --out DIR` copies `rNNNN.vam` (+ `.sig`) and writes `identity.json`. `import <file.vam>` validates (signature if present, body hash), enforces R-15, and inserts index rows. Importing an already-known uuid+revision byte-compares the files; a mismatch is `ManifestHashMismatch`.
- R-88 Label collisions after import are allowed; the UI shows `ARCH-0007 (28ff645e)` when two archives share a label.

### B7.6 Media copy (optional, off by default)

- R-89 With `--write-media-copy`, registration writes `<root>/.veriarchive/identity.json` **before** the scan (so it is excluded and never `New`) and `<root>/.veriarchive/rNNNN.vam[.sig]` after finalisation. This is a convenience copy; R-13/R-14 govern trust. When both copies exist at verify time they are byte-compared: a difference is at least `PASSED_WITH_WARNINGS`, and `FAILED` if the media copy validates with a *higher* revision than the repository knows (possible repository rollback).

## B8. Database schema v1 (all tables created by V1; V1.1 only fills columns and adds tables)

> Amended by `docs/decisions/0022-unfinished-operations-in-archive-list.md`: the `settings` records of an unfinished registration (`registration:<archive_uuid>`) and verification (`run:<run_id>`) also hold `files_total`, `bytes_total` and `updated` (when it last worked, UTC, at least every 60 s while it works), a registration's also `stopped` (when an attempt ended without finishing), a verification's also its `root`; no schema change.
> Amended by `docs/decisions/0024-attempts-record-their-process.md`: a registration's record (its plan) also holds `process`, the process that runs its attempt (`id`, `start`, `host`, `program`).
> Amended by `docs/decisions/0025-abandoning-an-unfinished-registration.md`: a registration's plan also holds `media_copy` (`true` once an attempt writes a media copy); abandoning the registration removes its plan and its saved progress (its rows in `files`), and, when it was the first registration of an archive that never had a revision (its `archives` row says revision 0 and `REGISTRATION_INCOMPLETE` or `RETIRED`, and no `archive_revisions`, `verify_runs` or `media_evidence` row refers to it), that `archives` row; no schema change.
> Amended by `docs/decisions/0026-relocation-resumes.md`: an unfinished relocation keeps a `settings` record of its own, `relocation:<archive_uuid>` (the revision N it verifies, its `run_id`, `started`, the new `root` and its `root_subpath`, the new carrier's media `evidence` at the start, `files_total`, `bytes_total`, `updated`, `stopped`, `process`, `resumes`, and `differs` once an attempt found that the new carrier does not hold the recorded content), and checkpoints the entries that matched into `verify_progress` under its `run_id`; recording r(N+1), a carrier that differs, N+1 recorded by another operation, and abandoning remove both; an interrupted attempt appends an `INCOMPLETE` row of its own to `verify_runs` (R-90), with its `verify_items` and `media_evidence`; no schema change.
> Amended by `docs/decisions/0028-a-manifest-that-cannot-be-used.md`: `repo rebuild-index` writes a `settings` record `rebuild-skipped:<archive_uuid>` (`{"revision":N}`) for each archive whose revision's manifest it skipped, and removes those of the rebuild before; while it stays, the archive lists read that archive's manifest whole, as the rebuild reads one. The plan of a registration over a lost manifest (`register --lost-manifest`) also holds `lost_manifest`, the revision whose manifest is lost; the transaction that records its revision removes the lost revision's record and index where that manifest's line 1 still cannot be used, and the archive's `rebuild-skipped` record; no schema change.

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

> Amended by `docs/decisions/0011-durable-writes-through-ifilewriter.md`: adds `IOutputFile` / `IFileWriter` for durable atomic writes.
> Amended by `docs/decisions/0014-special-files.md`: `EntryType::Special` for FIFOs, sockets and device nodes.
> `IFileWriter` also creates directories (`create_directories`), used for the optional media copy (R-89).
> Amended by `docs/decisions/0023-manifest-named-in-the-record-transaction.md`: `IOutputFile::flush()` makes the bytes written so far durable without giving them the destination's name; a `commit()` with nothing written since only renames.
> Amended by `docs/decisions/0024-attempts-record-their-process.md`: adds `IProcessTable` (`current()`, `find(id)`, both `std::expected`) and `ProcessIdentity` (`id`, `start`, `program`): which process runs an attempt of a registration, and whether it still runs.
> Amended by `docs/decisions/0029-network-roots-are-refused.md`: on Windows `IMediaEnumerator::volumes()` also gives a volume that has no drive letter or folder path, with its GUID path (`\\?\Volume{...}\`) as its `mount_point`, so that verification by archive uuid finds an archive on it.

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

> Amended by `docs/decisions/0001-archive-matching-includes-root-subpath.md`: candidates are first narrowed by `root_subpath`; archives on the same volume with different, non-overlapping roots are not `AMBIGUOUS`.
> Amended by `docs/decisions/0004-relocation-writes-a-new-revision.md`: `MIGRATED` acceptance and relocation are `archive relocate`, which writes a new revision; media matching only an older revision's identity is `PREVIOUS_CARRIER`.
> Amended by `docs/decisions/0003-retired-archives.md`: retired archives remain match candidates, but one whose latest manifest is lost (decision 0028).
> Amended by `docs/decisions/0018-unreadable-filesystem-id.md`: a `filesystem_uuid` unavailable on either side with a matching device is `MATCH_WEAK` when the volume capacity agrees within 1 %, otherwise `MISMATCH`.
> Amended by `docs/decisions/0021-linux-without-udev.md`: a kernel filesystem id (`fsid-…`, §B1.3) compared with a UUID or volume serial counts as unavailable, so decision 0018 applies; two `fsid-…` ids compare like UUIDs.
> Amended by `docs/decisions/0028-a-manifest-that-cannot-be-used.md`: R-96: a verification that failed on its manifest before it looked for the media records no media evidence, as it observed none. Matching by root needs every archive's latest manifest: while one cannot be used, the root may be that archive's, unless it is plainly another archive's, a `MATCH` of one at its folder (roots never overlap on one volume, decision 0001), which is verified as usual; for every other root, a verification by root stops (exit 11), naming the archive, what is wrong with its manifest, and to verify it by its uuid to record the failure, then the steps of the plain message: to put its manifest back with `import` from a backup of the repository, an export of that revision or its media copy, naming the last resort where none of these holds it and, after it, retiring the archive where its device is gone as well, or to rebuild the index where only the index is wrong. A registration of a new revision (`register --new-revision`) of a root that is plainly another archive's goes ahead by the same rule, as that archive's next revision; every other registration, of a new folder or volume too, and every relocation stop with the plain message (exit 11). A retired archive whose latest manifest is lost (missing, or its first line unreadable or changed: nothing that a rebuild of the index gives back; one changed only in its body stays a candidate, as matching reads its first line), as the operator retires one whose device is gone as well, is no candidate of matching: it stops nothing, and no root is matched as its carrier; where line 1 alone says that only the index is wrong of it, matching reads the manifest whole, as `repo rebuild-index` does, and one whose body has changed or whose body or entries cannot be read is lost all the same.

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

> Amended by `docs/decisions/0028-a-manifest-that-cannot-be-used.md`: an archive named by its uuid or by the run it resumes is known before its media is looked for: the manifest of its current revision is checked first (R-14, R-62), and one that cannot be used fails the verification at once, recorded as `FAILED` with nothing covered, without reading the media; a run being resumed stays resumable.

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

> Amended by `docs/decisions/0022-unfinished-operations-in-archive-list.md`: R-104's resume of a run by its archive's uuid (`verify --full <archive_uuid> --resume <run_id>`, and Resume in the Archive List) looks at every attached volume, and where more than one holds the archive (cloned media), it stops before it reads anything (exit 12, `AMBIGUOUS`), naming their roots and the root the run read, as identity cannot tell which one the run read; the run stays resumable by its root. A verification by the archive's uuid that is not a resume (`verify --full <archive_uuid>`, and Verify Archive, which verifies a chosen archive by its uuid) stops the same way, reading and recording nothing, and names their roots, to verify one of them by its root or to detach the other volumes; it never picks one by itself. A verification by a root that matches several archives (`AMBIGUOUS`, §B10) says to select one by its uuid and, where the volumes of those archives are attached together, to detach all but the volume of the one to verify.
> Amended by `docs/decisions/0023-manifest-named-in-the-record-transaction.md`: attempts of one registration may run at once (`register --resume` while an attempt still runs), and a relocation may record the revision that an unfinished registration creates; the first to record the revision wins, and another stops with exit 12 when it finds the revision recorded (at a checkpoint, at the record, whose transaction waits up to 2 minutes for the write lock, or at the start of a resume), changing nothing of it (manifest, index, archive record) nor the plan of the archive's next registration. A plan whose revision is recorded is removed, by whatever records the revision or by the attempt that finds it recorded. A relocation leaves the revision to a registration that has checkpointed files for it (exit 12). D10 and D11: decisions 0024 and 0025.
> Amended by `docs/decisions/0024-attempts-record-their-process.md`: every attempt records in its registration's plan which process runs it (id and start time, on which computer); `register --resume` refuses (exit 12, naming the process) while that process still runs on this computer, and goes ahead as soon as that process has ended; a plan that names no process (an older version's) or a process on another computer keeps the rule above.
> Amended by `docs/decisions/0025-abandoning-an-unfinished-registration.md`: the operator may abandon an unfinished registration (`archive abandon-registration <uuid> --confirm`, or the Archive List): its plan and checkpoints are removed in one transaction, never while its attempt runs; recorded revisions, manifests and history stay; the record of an archive that never had a revision goes with its abandoned first registration, judged from the archive's record and what refers to it, never from the index alone. The refusals of a relocation and of `register <root>` name the way out. An archive has one unfinished registration at a time: a registration that is not a resume, such as one through another path to the same volume, is refused (exit 12) while another registration of its archive is unfinished.
> Amended by `docs/decisions/0026-relocation-resumes.md`: a relocation resumes (R-104). `archive relocate <uuid> <root_path> --resume` continues the archive's unfinished relocation without reading again the entries that matched, carrying them with rN's hashes, only while the archive is at the revision N it verifies, and the new root is on the volume it started on (`MATCH` or `MATCH_WEAK` against the evidence recorded then) and in the same folder, and never while its attempt's process runs on this computer (decision 0024). An archive has one unfinished relocation at a time: one that is not a resume is refused (exit 12) while another is unfinished. A carrier that differs ends the relocation, also when its attempt is interrupted after it found an entry that differs (an entry that could not be read differs only in a comparison that read everything: an attempt interrupted after one, as when a disk drops off, is interrupted); so does N+1 recorded by another operation, and whatever records N+1 removes the relocation's record and checkpoints; the record of a carrier that differs says so (`differs`) from the first such entry on, so that one that cannot be removed is never resumed; an interruption, a failure of its scan, manifest or record, or a registration with saved files of N+1 leaves them for `--resume`, which the message names. `archive abandon-relocation <uuid> --confirm` removes them on purpose, never while the attempt runs. An interrupted attempt is also recorded `INCOMPLETE` in the history (R-104), also one that ends the relocation, under a run of its own, which stays as every run does and which `verify --resume` does not continue (it names `archive relocate --resume` while the relocation is unfinished); the FULL verification that records r(N+1) spans those runs, from the relocation's start.
> Amended by `docs/decisions/0030-import-names-its-manifest-in-the-record-transaction.md`: a registration or relocation made for an older revision never records: once the archive has a later revision than the one it was made for while the revision it creates is not recorded (an import of N+2), its record and its checkpoints stop it, and `register --resume`, `archive relocate --resume`, a registration of the archive and a relocation without `--resume` refuse it (exit 12), naming the abandon, and so does its attempt that is interrupted or that a failure stops after that, instead of naming the resume; `verify --resume` of the `INCOMPLETE` run of such a relocation's attempt names the abandon too; its plan or record and its saved progress stay until the operator abandons it. A registration of an archive and a relocation that are not a resume write their plan or record only while the archive is still at the revision they read when they started, and otherwise nothing (exit 12, to be run again).

- R-104 FULL and DEEP checkpoint verified paths into `verify_progress` every 1 000 items or 60 s. `verify --resume <run_id>` continues an `INCOMPLETE` run without re-hashing verified items, provided identity still matches and the revision is unchanged. Registration checkpoints likewise (`REGISTRATION_INCOMPLETE`, `register --resume`).

### B11.5 Overdue policy

- R-105 `policy.full_verify_max_age_days` (default 365) and `policy.any_verify_max_age_days` (default 180, decision 0007). `archive list` and the GUI show `OVERDUE` when exceeded.

## B12. Change tracking

- R-106 V1 ships `NullChangeTracker` only. Linux stays `Null` unless btrfs generation tracking is added later; ext4/XFS have no application-queryable change journal, and inotify/fanotify are meaningless for offline media. Do not attempt.
- R-107 Windows V1.1 `NtfsUsnChangeTracker` (through the helper) stores USN Journal ID + last USN per archive. Verdicts: journal ID changed or USN gap → `ContinuityLost` → QUICK is invalid, DEEP or FULL required; records exist → `Changed`, affected paths are forced into the sample; no records → `Unchanged` (advisory only, R-17). Raw-sector edits and edits made from another OS leave no USN trace; this is why R-17 exists.

## B13. I/O engine

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

> Amended by `docs/decisions/0013-glib-for-unicode-comparison.md`: GLib (≥ 2.72) is required for Unicode comparison keys.

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

```text
veriarchive_common  veriarchive_core  veriarchive_platform  veriarchive_cli  veriarchive_gui  veriarchive_tests
presets: linux-debug  linux-release  windows-ucrt64-debug  windows-ucrt64-release
```

- R-120 (CI runs on GitLab: `docs/decisions/0012-gitlab-ci.md`) CI on every push: `windows-latest` (msys2/setup-msys2, UCRT64), `ubuntu-24.04`, and a Fedora container. Runs unit + integration tests (golden manifests, repository-resolution matrix, fault injection via `FaultInjectingByteSource`). Linux CI additionally runs the loop-device + `dm-flakey` unreadable-sector test when a privileged runner is available.
- R-121 Windows release check: `ntldd -R veriarchive-cli.exe` lists no `msys-2.0.dll`; the package includes GTK4/gtkmm DLLs, compiled `share/glib-2.0/schemas`, an icon theme, and LGPL notices.
- R-122 CI greps fail the build on `current_path()`, `path::string()`, `<windows.h>`, `<libudev.h>` under `src/core` and `src/common` (except the R-04 exemption).

### Repository structure

> Amended by `docs/decisions/0020-archive-comparison.md`: `src/core/engine/compare_archives.*`; `tools/screenshots/` makes the user guide's pictures of the GUI.

```text
VeriArchive/
├── CMakeLists.txt  CMakePresets.json  README.md  CLAUDE.md
├── docs/HANDOFF.md            (this file; single source of truth)   docs/decisions/NNNN-*.md
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

> Amended by `docs/decisions/0017-interface-language.md`: global option `--lang en|zh-TW` (also `VERIARCHIVE_LANG`); text output in English or Traditional Chinese, `--json` always English.
> Amended by `docs/decisions/0020-archive-comparison.md`: `archive compare <uuid> <uuid> [--json]` compares the manifests of two archives' current revisions, no media read; exit `0` IDENTICAL · `2` IDENTICAL_WITH_WARNINGS · `3` DIFFERENT or a manifest that cannot be used · `4` cancelled.
> Amended by `docs/decisions/0022-unfinished-operations-in-archive-list.md`: `verify --full <archive_uuid>`, also with `--resume <run_id>`, exits `12` where more than one attached volume holds the archive (cloned media), naming their roots.
> Amended by `docs/decisions/0024-attempts-record-their-process.md`: `register --resume` exits `12` while the process of the attempt before still runs on this computer.
> Amended by `docs/decisions/0025-abandoning-an-unfinished-registration.md`: `archive abandon-registration <uuid> [--confirm] [--json]` abandons the archive's unfinished registration; without `--confirm` it shows what would be lost and exits `12`; exit `0` abandoned · `12` refused (nothing to abandon, another registration by now, an attempt that runs) · `10` unknown or ambiguous uuid · `11` repository. `register` exits `12` while another registration of the archive is unfinished, also one through another root path.
> Amended by `docs/decisions/0026-relocation-resumes.md`: `archive relocate <uuid> <root_path> --resume` (or `--resume <root_path>`) exits `12` with nothing to resume, while the attempt before runs, on another volume or folder than it started on, when the archive is at another revision, or when the relocation found that its new carrier differs; without `--resume`, `12` while a relocation of the archive is unfinished; `4` when cancelled or its device removed, and `11` when its scan, manifest or record failed, both of which `--resume` continues, as the message says; `--json` says `resumed`. `archive abandon-relocation <uuid> [--confirm] [--json]`, exit codes as `abandon-registration`'s. `archive list --json` and `archive show <uuid> --json` give an archive with an unfinished relocation `unfinished_relocation` (its root, revision, progress and state, `differs` once its new carrier was found to differ), and one with an unfinished registration `unfinished_registration` (its root, the revision it creates, progress and state); without `--json` they show one line for each under the archive. `history <uuid>` marks the runs of a relocation (`interrupted relocation attempt`, `relocation from rN`), and `history --json` gives each run `relocation` (null, or the relocation's `run_id`, `from_revision` and `interrupted_attempt`).
> Amended by `docs/decisions/0030-import-names-its-manifest-in-the-record-transaction.md`: `import` exits `12` when another operation recorded its revision while it ran, or an unfinished registration with saved files creates that revision; `register` and `archive relocate`, also with `--resume`, exit `12` when the archive's unfinished registration or relocation was made for an older revision than the archive has now, also when its attempt is interrupted or stopped by a failure then (not `4` or `11`), and when another operation recorded a later revision of the archive while they began; `archive list|show --json` give such a one the state `later_revision` once it has stopped.
> Amended by `docs/decisions/0028-a-manifest-that-cannot-be-used.md`: `verify --full <archive_uuid>` and `verify --resume` exit `3` with a recorded `FAILED` run when the manifest of the archive's current revision cannot be used, saying that the media was not read and how to fix the manifest; every verification's `--json` gives `media_read` (`false` then, with `root` and `identity` null). `archive list` lists every archive, one whose latest manifest cannot be used with a line "problem: ..." (what is wrong, ending with how to fix it), and `--json` gives it `manifest_problem` (`kind`, `revision`, `manifest`, `message`); `archive list`, and `archive show` of such an archive, then exit `2`. `archive show` also marks an earlier revision whose manifest cannot be used (that revision's `manifest_problem` in `--json`), and exits `2` then too. `repo rebuild-index` names each revision whose manifest it could not read, kept as recorded, left out of the index or not in it, and exits `2` then too; its `--json` gives `revisions_kept`, `revisions_left_out` and `revisions_not_indexed`. `import` of a recorded revision whose manifest cannot be used puts back a file that is the one recorded, saying so (`restored` in `--json`), and exits `12` where the record of that revision changed while it ran. The message of a manifest that is missing, cannot be read or has changed says to put it back with `import <folder>/rNNNN.vam` from a backup of the repository, an export of that revision or its media copy, for an earlier revision then `repo rebuild-index`, which leaves that revision out of the index where none of these holds it, and for the archive's latest revision names the last resort and, after it, where the device that held the archive is gone as well, retiring the archive with `archive retire <archive_uuid> --confirm`. The last resort: `register <root_path> --lost-manifest <archive_uuid> [--confirm]` registers the archive's next revision from what the root holds now, a registration over the lost manifest, which compares nothing with the lost revision. Without `--confirm` it says so and what it would register, writing nothing (exit `12`; `--json`: `registration`); it exits `12` where the manifest can be used or only the index differs from it, for an archive without a revision, where the root contains or lies in the root of another archive on a volume that is plainly that archive's (a `MATCH` of it: the filesystem id and a device that its latest manifest records) or its identity file names another archive than one whose very root it is, and where the root's `.veriarchive` holds the manifest as a media copy that `import` puts back; that archive's very root is taken, as the same folder registered on purpose as two archives (`--force-new-archive`) gives each twin that root; before it is confirmed it names each archive whose very root it is, with the match of its volume, also on a volume that it matches only as `MIGRATED` or `MATCH_WEAK` (`same_root` in `--json`, with `match`), and, as possibly a clone, each archive whose root the root contains or lies in on a volume that it matches only so (`possible_clones`); `--label`, `--new-revision`, `--force-new-archive` and `--resume` beside it, and `--confirm` without it, exit `10`; confirmed, it exits as any registration, and `--json` gives `lost_manifest` (`revision`, `left_out`); it refuses a retired archive (exit `12`). `archive retire <uuid> [--confirm]` of an archive whose latest manifest is lost so that identity matching cannot use it (not one changed only in its body, which is retired at once) says what retiring it means and exits `12` without `--confirm` (`retirement` in `--json`), and exits `12` while a registration of the archive is unfinished; `archive list` and `archive show` list a retired archive whose latest manifest is lost with a line "manifest lost: ..." in place of "problem: ...", `manifest_lost` in `--json`, and exit `0` for it; `archive list|show --json` give a registration over the lost manifest of an archive retired since, which an older version began, the state `archive_retired`.
> Amended by `docs/decisions/0029-network-roots-are-refused.md`: `register`, `verify --full <root_path>` and `archive relocate` exit `10` for a root on a network share, or on Windows one written in the `\\?\` or `\\.\` form, before anything of it is read; they take a volume's GUID path in the `\\?\` form (`\\?\Volume{...}\`), also for the volume's root folder, and `media list` shows a volume that has no drive letter or folder path under that path.

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

> Amended by `docs/decisions/0016-gtk4-c-api-for-the-gui.md`: the GUI uses the GTK4 C API (no gtkmm).
> Amended by `docs/decisions/0017-interface-language.md`: Settings has a language drop-down (English, 繁體中文).
> Amended by `docs/decisions/0020-archive-comparison.md`: a sixth view, Compare Archives, after Verify Archive; it reads no media, shows progress in entries and allows cancel, and has no resume (it writes nothing). A screenshot mode (`VERIARCHIVE_GUI_SCREENSHOTS`) makes the user guide's pictures.
> Amended by `docs/decisions/0022-unfinished-operations-in-archive-list.md`: above the archives, the Archive List shows the registrations and verifications that have not finished, wherever they were started, with their progress (counted only in WAL mode) and a state that claims no more than the repository shows (`running`, `... it may have stopped`, `stopped; it can be resumed`); it reads the repository read only, every 5 s while shown, reads the archives again when such a read finds them changed, and leaves out stopped verifications that cannot be resumed (R-104) or that a later verification of the same archive supersedes, and every unfinished verification of a retired archive, whose records stay as they are. A stopped verification that can be resumed, and a stopped registration, have **Resume**, and so has an older version's registration, whose plan never says when it worked, once it reads `... it may have stopped`, unless it can no longer finish; Resume continues it exactly as `verify --full <archive_uuid> --resume <run_id>` or `register <root> --resume` does, given no option: no check box of Verify Archive or Register Archive takes part, so it writes no media copy (R-03) and takes strict metadata from the policy alone; its progress and report show in Verify Archive or Register Archive; a relocation has none. Verify Archive, verifying a chosen archive by its uuid, stops where more than one attached volume holds it, naming their roots and saying to type or choose the folder of the one to verify, with "The folder or volume above" chosen, or to detach the other volumes.
> Amended by `docs/decisions/0024-attempts-record-their-process.md`: a registration whose attempt's process has ended reads `stopped; it can be resumed` at once; one whose process still runs, but whose plan has said nothing for minutes, reads `no progress reported for N minutes; its process is still running`.
> Amended by `docs/decisions/0025-abandoning-an-unfinished-registration.md`: the row of a stopped registration, and of an older version's registration that reads `... it may have stopped`, has **Abandon...**, which reads what abandoning removes with the CLI's check, then asks first, saying that the saved progress will be lost.
> Amended by `docs/decisions/0026-relocation-resumes.md`: the unfinished operations include relocations ("Relocation"), wherever they were started, with their progress and a state judged as a registration's (decision 0024), and `new carrier differs; abandon it` for a stopped one whose new carrier was found to differ; a stopped one has **Abandon...**, as a registration's row. The Verification History marks the runs of a relocation in a column Note, shown only for an archive that has such runs.
> Amended by `docs/decisions/0028-a-manifest-that-cannot-be-used.md`: Verify Archive verifies a registered archive chosen by its uuid, so when its manifest cannot be used the report says `FULL VERIFY FAILED`, that the media was not read, and how to fix the manifest. The Archive List lists every archive: one whose latest manifest cannot be used shows what is wrong and how to fix it under its row, in red, in either language, the command line's registration over a lost manifest named as the last resort, and retiring the archive where its device is gone as well, and stays one to choose in Verify Archive; a retired archive whose latest manifest is lost shows "Manifest lost: ..." under its row, not in red; a stopped registration over the lost manifest of an archive retired since reads `archive is retired; abandon it`, with **Abandon...** and without Resume. A verification that read no media supersedes no unfinished verification (decision 0022), so the run that a failed resume was to resume stays listed, resumable.
> Amended by `docs/decisions/0030-import-names-its-manifest-in-the-record-transaction.md`: a stopped registration or relocation made for an older revision than its archive has now reads `archive has a later revision; abandon it`, with the stopped operations, and has **Abandon...**.
> Amended by `docs/decisions/0029-network-roots-are-refused.md`: Register Archive and Verify Archive refuse a root on a network share, or on Windows one written in the `\\?\` or `\\.\` form, before anything of it is read; they take a volume's GUID path in the `\\?\` form (`\\?\Volume{...}\`).

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
- `repository.json` created; `schema_version` = 1; every §B8 table exists.
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

Kill `verify --full` at ~50 % → `INCOMPLETE`; `--resume` finishes without re-hashing verified items (measured by bytes read). `repo verify-history` detects a manually edited or deleted `verify_runs` row. `export` → `import` round-trip into a second repository; import of a lower revision is rejected; label collision displays per R-88. `archive relocate` (decision 0004): a copy on a new volume is accepted only after a FULL verify of every entry; an enclosure swap (`MIGRATED`) is accepted the same way and not re-prompted afterwards; known-unreadable entries do not block it (decision 0010); the old carrier then matches as `PREVIOUS_CARRIER`.

## M5 — GTK4 GUI

All views of §B17 (six, with decision 0020's Compare Archives); results per §A6 / R-21; progress + cancel + resume; Settings per §B17; Windows package passes R-121; runs unprivileged under Wayland.

## V1.1 (after V1 acceptance)

Chunk lines + Merkle (§B4.4, §B6) · Ed25519 signing, `.sig`, trusted keys, `key_id` in results (§B5) · QUICK / DEEP with seed and coverage (§B11) · history checkpoints (R-76) · `veriarchive-helper` + IPC · Windows USN tracker (R-107) · NTFS ADS entries (`s`) · `SeBackupPrivilege` / `CAP_DAC_READ_SEARCH` reads.
Constraint: no change to §B4 header/entry keys (only filling `chunk_size`, `merkle_root`, chunk lines, `s` entries); no `ALTER TABLE` on V1 columns (R-92).

## V2

Whole-disk fingerprint (§B14) · btrfs change tracking · hardware-token / service `ISigner`.

---

# Part D — Open decisions (do not assume; ask the operator)

| # | Question | Default if unanswered before M3 |
|---|---|---|
| D1 | ~~Linux driver for verifying Windows-written NTFS media: kernel `ntfs3` or `ntfs-3g`? (affects mtime precision and ADS visibility)~~ | **Resolved** by `docs/decisions/0006-ntfs3-reference-driver.md`: `ntfs3` read-only is the reference; `ntfs-3g` supported with an informational note; `filesystem_type` is the on-disk format name |
| D2 | V1.1 chunk size: 64 MiB vs 16 MiB (smaller = finer corruption localisation, larger index) | 64 MiB |
| D3 | ~~Overdue defaults (365 / 90 days) and whether OVERDUE blocks QUICK results~~ | **Resolved** by `docs/decisions/0007-overdue-defaults.md`: 365 / 180; OVERDUE flags only |
| D4 | Compile-time pinned keys for production builds (R-73)? | off |
| D5 | ~~Is writing `.veriarchive/` to sealed media ever acceptable in this organisation?~~ | **Resolved** (2026-10-08): off by default (R-89); `register` without `--write-media-copy` prints a one-line notice that the repository then holds the only copy of the baseline and must be backed up (R-86) |
| D6 | Windows helper authorisation: Windows service, or UAC-elevated helper launched on demand? | on-demand elevated helper, exits when idle |
| D7 | ~~Retention of `verify_items` detail (storage growth on large archives)~~ | **Resolved** (2026-10-08): non-Verified items only, kept forever |
| D8 | ~~Registering a *directory* (not a volume root) as an archive: allowed in V1, or volume roots only?~~ | **Resolved** by `docs/decisions/0001-archive-matching-includes-root-subpath.md`: allowed; overlapping roots refused; matching keyed by volume evidence + `root_subpath` |
| D9 | ~~Linux without the udev database (containers, minimal systems; seen on the GitLab runners, 2026-10-14): neither the filesystem UUID nor the SATA/SCSI drive serial can be read, so every volume is `UNKNOWN` — registration is never refused (R-97) and verification never finds the archive. Proposed (a decision record to be written): (a) fall back to the kernel filesystem id (`statvfs` `f_fsid`, recorded as `fsid-…`) for ext2/3/4 and btrfs, where the kernel derives it from the filesystem UUID (not xfs, where it follows the device number); an `fsid-…` id compared with a real UUID counts as unavailable, so decision 0018 applies; (b) read the drive serial from SCSI VPD page 0x80 (`/sys/block/<disk>/device/vpd_pg80`) when udev is missing.~~ | **Resolved** by `docs/decisions/0021-linux-without-udev.md`: the kernel filesystem id (`fsid-…`) for ext2/3/4 and btrfs, compared only with another `fsid-…` id (against a UUID, decision 0018 applies); for a SATA device behind libata that udev does not know, the device serial from SCSI VPD page 0x80 |
| D10 | ~~`register --resume` while another attempt of the same registration may still run (decision 0023): refuse it? An attempt whose process was killed cannot be told from one that works (decision 0022 sees only that its plan went quiet), so a refusal needs a record of the running process, or an operator override.~~ | **Resolved** by `docs/decisions/0024-attempts-record-their-process.md` (option c): every attempt records in its plan which process runs it (id and start time, on which computer); `register --resume` refuses while that process still runs, naming it, and goes ahead as soon as that process has ended; a plan without the record keeps first-to-record-wins; the Archive List reads a registration whose process has ended as stopped at once |
| D11 | ~~`archive relocate` while a `register --new-revision` of the same archive is unfinished: both create the archive's next revision. Which should win? (a) The relocation refuses while the registration is unfinished; but nothing removes an unfinished registration whose root is gone, so the relocation, which exists for carriers that go away, could be blocked for good. (b) The relocation discards the registration's plan and checkpoints, which can hold hours of hashing.~~ | **Resolved** by `docs/decisions/0025-abandoning-an-unfinished-registration.md`: first to record wins, and the relocation refuses once the registration has checkpointed files for the revision (decision 0023, unchanged); the operator may abandon an unfinished registration on purpose (`archive abandon-registration <uuid> --confirm`, or Abandon in the Archive List), which removes its plan and checkpoints in one transaction, never while its attempt runs, and with its first registration the record of an archive that never had a revision |

---

# Appendix — Agent quick rules (copy into CLAUDE.md)

1. Read `docs/HANDOFF.md` §A3, §A6, §B4, §B7.4, §B9 before writing code. Part B is frozen: propose changes as `docs/decisions/NNNN-*.md`; do not just implement them. Accepted records override the HANDOFF text they amend; each amended section names its records.
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
