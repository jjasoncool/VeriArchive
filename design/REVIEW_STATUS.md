# Implementation status

Status of branch `v1-implementation`, version 1.0.0, against HANDOFF Part C. "Done" means implemented and covered
by automated tests on Windows (MSYS2 UCRT64, GCC 16). A milestone is accepted only when its
acceptance tests pass on both operating systems in CI (HANDOFF Part C), which needs the branch
pushed to GitLab.

## Done

| Milestone | Implemented |
|---|---|
| M1 | Repository resolution matrix (explicit, pinned, first run, unavailable, other working directory, network refusal), relative pin for a repository inside the config file's folder so a portable program folder can be moved or copied (decision 0019), atomic and durable config and repository files, crash-safe schema creation, repository UUID validation and a change warning for the pinned repository (decision 0037), lossless non-Unicode names, canonical JSON, `repo init/info`, `--json` everywhere |
| M2 | Unprivileged volume and device enumeration on Windows (64-bit NTFS serial, disk extents, model, serial, bus, HDD/SSD, NVMe EUI) and Linux (mountinfo, udev database, sysfs through partitions, dm and md; without udev the kernel filesystem id and the SATA serial page, decision 0021), filesystem id normalisation, identity matcher for every §B10 row and decisions 0001, 0003, 0004, 0018, 0021, `media list` |
| M3 | Scanner (exclusions, mount points, collisions, unreadable entries, links), uncached threaded hash pipeline (BLAKE3 + SHA-256), `.vam` writer/reader with golden test, registration, FULL verification with every §A6 item status, `export-sha256sums` |
| M4 | Ctrl-C cancellation; resumable verification, registration and relocation with checkpoints (`archive relocate --resume`, decision 0026); attempts that record their process, so that a resume never runs beside the attempt before it (decision 0024), and a resume of a registration that an older version left, which that version may still run, only once confirmed (`register --resume --confirm`, decision 0022); abandoning an unfinished registration or relocation (decisions 0025, 0026); the manifest named in the transaction that records its revision, by registration, relocation and import (decisions 0023, 0030), and a registration or relocation made for an older revision that never records (decision 0030); a device that goes away, or stops answering (ten I/O errors in a row while its volume stays listed), interrupts the operation, resumable once it is attached again, instead of making its entries unreadable (decision 0034); a closed console window, a sign-out or a shutdown interrupts a command-line operation as Ctrl+Break does, and every way on names the root to type (decision 0022); hash-chained history and `repo verify-history`; `repo rebuild-index`, which keeps the saved progress of unfinished registrations and the record of a revision whose manifest it cannot read (decisions 0026, 0028); export/import with rollback protection; `archive relocate`; optional media copy |
| Damaged repository | A manifest that is missing, cannot be read, has changed or differs from the index (decision 0028): its message says how to put it back (from a backup of the repository, an export or the media copy, with `import`); the archive list keeps listing every archive, with the problem; a verification of that archive by its uuid records FAILED without reading the media; a verification or a new revision by root goes ahead only where the root is plainly another, intact archive's. A latest manifest lost for good: `register <root> --lost-manifest <uuid> --confirm` registers the next revision from what the root holds now, and `archive retire <uuid> --confirm` retires an archive whose device is gone as well (decisions 0003, 0028). A database that is missing, empty, not a database or damaged: every command names `repo rebuild-index`, which puts it aside and makes it again from the manifests, saying plainly that the verification history is lost; a rebuild that stops leaves the new database marked unfilled until one finishes (decision 0036) |
| Roots | Network shares refused as archive roots on both systems, roots written as `\\?\` drive-letter paths refused with the drive-letter advice, volume GUID paths accepted, `subst` drive letters resolved to the folder they stand for (decisions 0027, 0029); a root that holds the repository, or lies inside it, refused however either is spelled (decision 0029); a verification by archive uuid stops where several attached volumes hold the archive (decision 0022) |
| M5 | GTK4 GUI with Archive List, Register, Verify, Compare, History and Settings views, progress, cancel and resume; the Archive List lists the unfinished registrations, verifications and relocations with progress and state, and offers Resume and Abandon... (decisions 0022, 0025, 0026), asking first before it resumes a registration that an older version left; a screenshot mode and `tools/screenshots/` for the user guide's pictures, from sample data |
| Comparison | `archive compare` and the GUI's Compare Archives view: two archives' current manifests compared by path, type and content (BLAKE3 + SHA-256) without reading media, results IDENTICAL / IDENTICAL_WITH_WARNINGS / DIFFERENT, typically an archive against its copy on other media (decision 0020) |
| Language | CLI and GUI in English and Traditional Chinese with a language setting, messages in plain words without decision numbers or failure class names; `--json` stays English (decision 0017) |
| Version | 1.0.0, shown by `--version`, `repo info` and Settings |

Automated tests: unit tests for every module (fake file system and media enumerator per R-93)
and the translation catalog, golden manifests of a fixture tree registered on each system
(R-66), a CLI end-to-end test (CJK and emoji names, damage detection, comparison of an archive
with a copy, export/import, damaged and lost manifests, a database made again, interface
language), a windowless GUI smoke test, and a check that the Windows package runs without MSYS2
in both languages. Every rule added since decision 0022 was removed once to see a test fail
(mutation checks).

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

R-112's HDD throughput criterion (≥ 80 % of `dd`) is met. `sha256sum -c` of the exported list
agreed with every file of the 4.3 GB archive, and an archive given by uuid was found on the disk.

In use on the operator's own data: a 22 TB internal SATA disk and a 24 TB disk on USB, both NTFS,
registered read-only into a repository on another disk as two archives of 10.1 TB in 144 270 files
each, the second a copy of the first; `archive compare` of the two gives IDENTICAL. Their first
FULL verify is still to run.

## Still required before acceptance

- **CI for the latest commits.** The last GitLab pipeline (commit 06cce92, 2026-10-22) passed on
  Ubuntu 24.04, Fedora 41 and Windows UCRT64. The commits since are checked on Windows only; they
  still have to pass in the three CI jobs, which needs the branch pushed.
- **Linux without udev (HANDOFF D9, decision 0021).** The Linux build compiles and passes the
  unit and end-to-end tests in GitLab CI, but so far only on a runner whose device reports a
  serial. Decision 0021 takes the kernel filesystem id (`fsid-…`, read with `statfs`, never zero)
  of ext2/3/4 and btrfs volumes and, for SATA devices behind libata, the SCSI serial page (VPD
  0x80) when udev is missing, with unit tests for both; it still has to pass in CI on a runner
  with a virtual SCSI device, where the kernel id alone gives `MATCH_WEAK` and the second
  registration of the same root must be refused (exit 12).
- **Hardware acceptance (M2, M3).** Done on Windows for an internal SATA HDD (model, serial,
  NTFS serial, identity MATCH, throughput above), and for registration through a USB bridge that
  reports the disk's serial (the operator's 24 TB disk). Still open: USB bridges without serials,
  a FULL verify through a USB bridge, a USB disk unplugged during a FULL verify and resumed once
  attached again (decision 0034), exFAT external disks; the NTFS serial relationship on one medium seen from both
  OSes; the 100 000-file / 20 GB fixture; register on Windows and verify on Linux and the reverse;
  uncached-read audits.
- **Packaging (R-121).** The Windows package is checked locally (`veriarchive_package`: the
  installed tree runs in both languages with only Windows on `PATH` and imports no
  `msys-2.0.dll`) and passed the GitLab Windows job at 06cce92. Its zip now holds no folder of its
  own, so that a new version unzips over the old one, and the licenses of what it bundles
  (decision 0038). It still has to run on a machine without MSYS2.

## V1.1

On branch `v1.1-implementation`, built on this branch. Stage 1, chunk hashes and the Merkle root
(decisions 0031 to 0033), is implemented there: registration, relocation, import and the index
keep 64 MiB chunk hashes, a FULL verify names the chunks that differ, and a FULL verify adds chunk
hashes to an archive registered without them. Still to come: QUICK and DEEP verification, Ed25519
signatures and trusted keys, signed history checkpoints, NTFS alternate data streams and the USN
change tracker. The elevated helper is not planned: the change journal can be read without
elevation.
