# Implementation status

Status of branch `v1-implementation` against HANDOFF Part C. "Done" means implemented and covered
by automated tests on Windows (MSYS2 UCRT64, GCC 16). A milestone is accepted only when its
acceptance tests pass on both operating systems in CI (HANDOFF Part C), which needs the branch
pushed to GitLab.

## Done

| Milestone | Implemented |
|---|---|
| M1 | Repository resolution matrix (explicit, pinned, first run, unavailable, other working directory, network refusal), relative pin for a repository inside the config file's folder so a portable program folder can be moved or copied (decision 0019), atomic and durable config and repository files, crash-safe schema creation, repository UUID validation and change warning, lossless non-Unicode names, canonical JSON, `repo init/info`, `--json` everywhere |
| M2 | Unprivileged volume and device enumeration on Windows (64-bit NTFS serial, disk extents, model, serial, bus, HDD/SSD, NVMe EUI) and Linux (mountinfo, udev database, sysfs through partitions, dm and md; without udev the kernel filesystem id and the SATA serial page, decision 0021), filesystem id normalisation, identity matcher for every §B10 row and decisions 0001, 0003, 0004, 0018, 0021, `media list` |
| M3 | Scanner (exclusions, mount points, collisions, unreadable entries, links), uncached threaded hash pipeline (BLAKE3 + SHA-256), `.vam` writer/reader with golden test, registration, FULL verification with every §A6 item status, `export-sha256sums` |
| M4 | Ctrl-C cancellation, resumable verification, registration and relocation with checkpoints (a relocation kept nothing and started over until decision 0026: now `archive relocate --resume`, and `archive abandon-relocation`), hash-chained history and `repo verify-history`, `repo rebuild-index` (which keeps the saved progress of unfinished registrations, decision 0026), export/import with rollback protection, `archive relocate`, optional media copy |
| M5 | GTK4 GUI with Archive List, Register, Verify, Compare, History and Settings views, progress, cancel and resume; a screenshot mode and `tools/screenshots/` for the user guide's pictures |
| Comparison | `archive compare` and the GUI's Compare Archives view: two archives' current manifests compared by path, type and content (BLAKE3 + SHA-256) without reading media, results IDENTICAL / IDENTICAL_WITH_WARNINGS / DIFFERENT, typically an archive against its copy on other media (decision 0020) |
| Language | CLI and GUI in English and Traditional Chinese with a language setting; `--json` stays English (decision 0017) |

Automated tests: unit tests for every module (fake file system and media enumerator per R-93)
and the translation catalog, a CLI end-to-end test (CJK and emoji names, damage detection,
comparison of an archive with a copy, export/import, interface language), a windowless GUI smoke
test, and a check that the Windows package runs without MSYS2 in both languages.

Measured locally (Release build, Windows 11, WD SN850X NVMe, uncached reads, four 512 MiB
files): FULL verify at about 4.5 GB/s with eight readers and about 1.47 GB/s with one reader
(the HDD setting, limited by SHA-256). Hashing therefore stays well above HDD speeds.

Measured on a real HDD (Release build, internal SATA WD WD221KFGX 20 TB, NTFS, data registered
read-only into a repository on another disk), against an uncached sequential read of the same
files as the `dd` baseline:

| Data | Baseline | Registration | FULL verify |
|---|---|---|---|
| 154 GB, 2 166 files | 150.0 MB/s | 154.6 MB/s (103 %) | 153.8 MB/s (103 %), PASSED, identity MATCH |
| 4.3 GB, 1 607 files | 101 MB/s | 105 MB/s | 138 MB/s, PASSED, identity MATCH |

R-120's HDD throughput criterion (≥ 80 % of `dd`) is met. `sha256sum -c` of the exported list
agreed with every file of the 4.3 GB archive, and an archive given by uuid was found on the disk.

## Still required before acceptance

- **Linux without udev (HANDOFF D9, decision 0021).** The Linux build compiles and passes the
  unit and end-to-end tests in GitLab CI (Ubuntu 24.04, Fedora 41, 2026-10-15), but so far only on
  a runner whose device reports a serial. Decision 0021 now takes the kernel filesystem id
  (`fsid-…`, read with `statfs`, never zero) of ext2/3/4 and btrfs volumes and, for SATA devices
  behind libata, the SCSI serial page (VPD 0x80) when udev is missing, with unit tests for both;
  it still has to pass in CI on a runner with a virtual SCSI device, where the kernel id alone
  gives `MATCH_WEAK` and the second registration of the same root must be refused (exit 12).
- **Hardware acceptance (M2, M3).** Done on Windows for an internal SATA HDD (model, serial,
  NTFS serial, identity MATCH, throughput above). Still open: USB bridges with and without
  serials, exFAT and NTFS external disks; the NTFS serial relationship on one medium seen from both
  OSes; the 100 000-file / 20 GB fixture; register on Windows and verify on Linux and the reverse;
  uncached-read audits.
- **Packaging (R-121).** The Windows package is built and checked locally (`veriarchive_package`:
  the installed tree runs in both languages with only Windows on `PATH` and imports no
  `msys-2.0.dll`); it still has to pass in the GitLab Windows job and on a machine without MSYS2.

## V1.1 (not started, by design)

Chunk lines and Merkle trees, Ed25519 signatures and trusted keys, QUICK/DEEP sampling, signed
history checkpoints, the elevated helper, the NTFS USN change tracker and alternate data streams.
The V1 schema and manifest format already reserve their fields (decision 0002).
