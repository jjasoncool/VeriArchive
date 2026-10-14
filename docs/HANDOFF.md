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

### B7.4 RepositoryRoot resolution (authoritative, R-83)

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

- R-87 `export <uuid> --out DIR` copies `rNNNN.vam` (+ `.sig`) and writes `identity.json`. `import <file.vam>` validates (signature if present, body hash), enforces R-15, and inserts index rows. Importing an already-known uuid+revision byte-compares the files; a mismatch is `ManifestHashMismatch`.
- R-88 Label collisions after import are allowed; the UI shows `ARCH-0007 (28ff645e)` when two archives share a label.

### B7.6 Media copy (optional, off by default)

- R-89 With `--write-media-copy`, registration writes `<root>/.veriarchive/identity.json` **before** the scan (so it is excluded and never `New`) and `<root>/.veriarchive/rNNNN.vam[.sig]` after finalisation. This is a convenience copy; R-13/R-14 govern trust. When both copies exist at verify time they are byte-compared: a difference is at least `PASSED_WITH_WARNINGS`, and `FAILED` if the media copy validates with a *higher* revision than the repository knows (possible repository rollback).

## B8. Database schema v1 (all tables created by V1; V1.1 only fills columns and adds tables)

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
> Amended by `docs/decisions/0003-retired-archives.md`: retired archives remain match candidates.
> Amended by `docs/decisions/0018-unreadable-filesystem-id.md`: a `filesystem_uuid` unavailable on either side with a matching device is `MATCH_WEAK` when the volume capacity agrees within 1 %, otherwise `MISMATCH`.

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
│   │   ├── engine/      register_engine.*  verify_engine.*  sampling.* (V1.1)
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
└── tests/
    ├── unit/
    ├── golden/          fixture trees + expected .vam
    └── integration/
```

## B16. CLI

> Amended by `docs/decisions/0017-interface-language.md`: global option `--lang en|zh-TW` (also `VERIARCHIVE_LANG`); text output in English or Traditional Chinese, `--json` always English.

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
- Repository-resolution matrix (R-83) as parameterised tests: explicit path; pinned path; pinned-but-unavailable → exit 11, no fallback; first run with writable exe dir → `<exe>/data`; unwritable exe dir → `<home>/VeriArchive`; launched from a different cwd; network-filesystem refusal (filesystem-type probe injected).
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

All five views; results per §A6 / R-21; progress + cancel + resume; Settings per §B17; Windows package passes R-121; runs unprivileged under Wayland.

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
