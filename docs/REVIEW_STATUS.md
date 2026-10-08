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

Automated tests: unit tests for every module (fake file system and media enumerator per R-93),
a CLI end-to-end test (CJK and emoji names, damage detection, export/import) and a windowless GUI
smoke test.

Measured locally (Release build, Windows 11, WD SN850X NVMe, uncached reads, four 512 MiB
files): FULL verify at about 4.5 GB/s with eight readers and about 1.47 GB/s with one reader
(the HDD setting, limited by SHA-256). Hashing therefore stays well above HDD speeds; the HDD
acceptance against `dd` still has to be measured on a real HDD.

## Still required before acceptance

- **Linux compilation and tests.** The Linux platform files have not been compiled yet (no Linux
  environment was available); the GitLab pipeline is the first place they build.
- **Hardware acceptance (M2, M3).** `media list` on internal SATA/NVMe, USB bridges with and
  without serials, exFAT and NTFS external disks; the NTFS serial relationship on one medium seen
  from both OSes; the 100 000-file / 20 GB fixture; register on Windows and verify on Linux and
  the reverse; uncached-read audits; throughput ≥ 80 % of `dd` on an HDD.
- **Packaging (R-121).** Windows package with GTK4 DLLs, schemas, icon theme and LGPL notices;
  `ntldd` check (in CI).
- **CMake 4 deprecation.** CMake ≥ 4 warns that `SQLite::SQLite3` is deprecated in favour of
  `SQLite3::SQLite3`; switching needs a fallback because CMake 3.28's FindSQLite3 only defines
  the old name.

## V1.1 (not started, by design)

Chunk lines and Merkle trees, Ed25519 signatures and trusted keys, QUICK/DEEP sampling, signed
history checkpoints, the elevated helper, the NTFS USN change tracker and alternate data streams.
The V1 schema and manifest format already reserve their fields (decision 0002).
