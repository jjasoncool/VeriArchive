# Implementation status

Status of branch `v1-implementation` against HANDOFF Part C. "Done" means implemented and covered
by automated tests on Windows (MSYS2 UCRT64, GCC 16). A milestone is accepted only when its
acceptance tests pass on both operating systems in CI (HANDOFF Part C), which needs the branch
pushed to GitLab.

## Done

| Milestone | Implemented |
|---|---|
| M1 | Repository resolution matrix (explicit, pinned, first run, unavailable, other working directory, network refusal), atomic and durable config and repository files, crash-safe schema creation, repository UUID validation and change warning, lossless non-Unicode names, canonical JSON, `repo init/info`, `--json` everywhere |
| M2 | Unprivileged volume and device enumeration on Windows (64-bit NTFS serial, disk extents, model, serial, bus, HDD/SSD, NVMe EUI) and Linux (mountinfo, udev database, sysfs through partitions, dm and md), filesystem id normalisation, identity matcher for every §B10 row and decisions 0001, 0003, 0004, `media list` |
| M3 | Scanner (exclusions, mount points, collisions, unreadable entries, links), uncached threaded hash pipeline (BLAKE3 + SHA-256), `.vam` writer/reader with golden test, registration, FULL verification with every §A6 item status, `export-sha256sums` |
| M4 | Ctrl-C cancellation, resumable verification and registration with checkpoints, hash-chained history and `repo verify-history`, `repo rebuild-index`, export/import with rollback protection, `archive relocate`, optional media copy |
| M5 | GTK4 GUI with Archive List, Register, Verify, History and Settings views, progress, cancel and resume |
| Language | CLI and GUI in English and Traditional Chinese with a language setting; `--json` stays English (decision 0017) |

Automated tests: unit tests for every module (fake file system and media enumerator per R-93)
and the translation catalog, a CLI end-to-end test (CJK and emoji names, damage detection,
export/import, interface language), a windowless GUI smoke test, and a check that the Windows
package runs without MSYS2 in both languages.

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

- **Linux compilation and tests.** The Linux platform files have not been compiled yet (no Linux
  environment was available); the GitLab pipeline is the first place they build.
- **Hardware acceptance (M2, M3).** Done on Windows for an internal SATA HDD (model, serial,
  NTFS serial, identity MATCH, throughput above). Still open: USB bridges with and without
  serials, exFAT and NTFS external disks; the NTFS serial relationship on one medium seen from both
  OSes; the 100 000-file / 20 GB fixture; register on Windows and verify on Linux and the reverse;
  uncached-read audits.
- **Packaging (R-121).** The Windows package is built and checked locally (`veriarchive_package`:
  the installed tree runs in both languages with only Windows on `PATH` and imports no
  `msys-2.0.dll`); it still has to pass in the GitLab Windows job and on a machine without MSYS2.
- **CMake 4 deprecation.** CMake ≥ 4 warns that `SQLite::SQLite3` is deprecated in favour of
  `SQLite3::SQLite3`; switching needs a fallback because CMake 3.28's FindSQLite3 only defines
  the old name.

## V1.1 (not started, by design)

Chunk lines and Merkle trees, Ed25519 signatures and trusted keys, QUICK/DEEP sampling, signed
history checkpoints, the elevated helper, the NTFS USN change tracker and alternate data streams.
The V1 schema and manifest format already reserve their fields (decision 0002).
