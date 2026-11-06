# Implementation status

Status of `master`, version 1.2.0: version 1.0.0 (tag `v1.0.0`, V1) with V1.1 stage 1 on top
(last section), against HANDOFF Part C. "Done" means implemented and covered by automated tests,
on Windows (MSYS2 UCRT64, GCC 16) and in GitLab CI. A milestone is accepted only when its
acceptance tests pass on both operating systems in CI (HANDOFF Part C); the hardware tests that CI
cannot run are listed under "Still required before acceptance".

## Done

| Milestone | Implemented |
|---|---|
| M1 | Repository resolution matrix (explicit, pinned, first run, unavailable, other working directory, network refusal), relative pin for a repository inside the config file's folder so a portable program folder can be moved or copied (decision 0019), atomic and durable config and repository files, crash-safe schema creation, repository UUID validation and a change warning for the pinned repository (decision 0037), lossless non-Unicode names, canonical JSON, `repo init/info`, `--json` everywhere |
| M2 | Unprivileged volume and device enumeration on Windows (64-bit NTFS serial, disk extents, model, serial, bus, HDD/SSD, NVMe EUI) and Linux (mountinfo, udev database, sysfs through partitions, dm and md; without udev the kernel filesystem id and the SATA serial page, decision 0021), filesystem id normalisation, identity matcher for every §B10 row and decisions 0001, 0003, 0004, 0018, 0021, `media list` |
| M3 | Scanner (exclusions, mount points, collisions, unreadable entries, links), uncached threaded hash pipeline (BLAKE3 + SHA-256), `.vam` writer/reader with golden test, registration, FULL verification with every §A6 item status, `export-sha256sums` |
| M4 | Ctrl-C cancellation; resumable verification, registration and relocation with checkpoints (`archive relocate --resume`, decision 0026); attempts that record their process, so that a resume never runs beside the attempt before it (decision 0024), and a resume of a registration that an older version left, which that version may still run, only once confirmed (`register --resume --confirm`, decision 0022); abandoning an unfinished registration or relocation (decisions 0025, 0026); the manifest named in the transaction that records its revision, by registration, relocation and import (decisions 0023, 0030), and a registration or relocation made for an older revision that never records (decision 0030); a device that goes away, or stops answering (ten I/O errors in a row while its volume stays listed), interrupts the operation, resumable once it is attached again, instead of making its entries unreadable (decision 0034); a closed console window, a sign-out or a shutdown interrupts a command-line operation as Ctrl+Break does, and every way on names the root to type (decision 0022); hash-chained history and `repo verify-history`; `repo rebuild-index`, which keeps the saved progress of unfinished registrations and the record of a revision whose manifest it cannot read (decisions 0026, 0028); export/import with rollback protection; `archive relocate`; optional media copy |
| Damaged repository | A manifest that is missing, cannot be read, has changed or differs from the index (decision 0028): its message says how to put it back (from a backup of the repository, an export or the media copy, with `import`); the archive list keeps listing every archive, with the problem; a verification of that archive by its uuid records FAILED without reading the media; a verification or a new revision by root goes ahead only where the root is plainly another, intact archive's. A latest manifest lost for good: `register <root> --lost-manifest <uuid> --confirm` registers the next revision from what the root holds now, and `archive retire <uuid> --confirm` retires an archive whose device is gone as well (decisions 0003, 0028). A database that is missing, empty, not a database or damaged: every command names `repo rebuild-index`, which puts it aside and makes it again from the manifests, saying plainly that the verification history is lost; a rebuild that stops leaves the new database marked unfilled until one finishes (decision 0036) |
| Roots | Network shares refused as archive roots on both systems, roots written as `\\?\` drive-letter paths refused with the drive-letter advice, volume GUID paths accepted, `subst` drive letters resolved to the folder they stand for (decisions 0027, 0029); a root that holds the repository, or lies inside it, refused however either is spelled (decision 0029); a verification by archive uuid stops where several attached volumes hold the archive (decision 0022) |
| M5 | GTK4 GUI, built with libadwaita since decision 0046 (see the V1.1 table): the pages Archives, Devices, Jobs, History and Settings, the dialogs Register Archive and Cross-check (Compare Archives before decision 0049), a result page per run; progress, cancel and resume; Archives lists the unfinished registrations, verifications and relocations with progress and state, and offers Resume and Abandon... (decisions 0022, 0025, 0026, 0046), asking first before it resumes a registration that an older version left; a screenshot mode and `tools/screenshots/` for the user guide's pictures, from sample data |
| Comparison | `archive compare` and the window's Cross-check dialog (decision 0049): two archives' current manifests compared by path, type and content (BLAKE3 + SHA-256) without reading media, results IDENTICAL / IDENTICAL_WITH_WARNINGS / DIFFERENT, typically an archive against its copy on other media (decision 0020) |
| Language | CLI and GUI in English and Traditional Chinese with a language setting, messages in plain words without decision numbers or failure class names; `--json` stays English (decision 0017) |
| Version | 1.0.0 (tag `v1.0.0`), shown by `--version`, `repo info` and Settings |

Automated tests: unit tests for every module (fake file system and media enumerator per R-93)
and the translation catalog, golden manifests of a fixture tree registered on each system
(R-66), a CLI end-to-end test (CJK and emoji names, damage detection, comparison of an archive
with a copy, export/import, damaged and lost manifests, a database made again, interface
language), a windowless GUI smoke test that builds every page in both languages, unit tests of
the window's logic kept in files without GTK (decision 0046), and a check that the Windows
package runs without MSYS2 in both languages. Every rule added from decision 0022 to decision
0045 was removed once to see a test fail (mutation checks); no mutation check of decision 0046's
rules has been run yet. The last GitLab pipeline, at commit 31e1abb (2026-11-02), passed on Ubuntu
24.04, Fedora 41 and Windows UCRT64, as did the pipeline of the tag `v1.0.0`.

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

In use on the operator's own data: a 22 TB internal SATA disk and a 24 TB disk on a USB 3 bridge
(UAS) that reports the disk's serial, both NTFS, registered read-only into a repository on another
disk as two archives of 10.1 TB in 144 270 files each, the second a copy of the first. A build of
version 1.0.0 from before the tag verified both FULL, the two at once (2026-10-30 to 31): PASSED,
all 10 135 965 225 485 bytes of each read and matched, at about 180 MB/s on the internal disk and
237 MB/s on the USB disk.
`archive compare` of the two gives IDENTICAL.

## Still required before acceptance

- **Linux without udev (HANDOFF D9, decision 0021).** The Linux build compiles and passes the
  unit and end-to-end tests in GitLab CI, but so far only on a runner whose device reports a
  serial. Decision 0021 takes the kernel filesystem id (`fsid-…`, read with `statfs`, never zero)
  of ext2/3/4 and btrfs volumes and, for SATA devices behind libata, the SCSI serial page (VPD
  0x80) when udev is missing, with unit tests for both; it still has to pass in CI on a runner
  with a virtual SCSI device, where the kernel id alone gives `MATCH_WEAK` and the second
  registration of the same root must be refused (exit 12).
- **Hardware acceptance (M2, M3).** Done on Windows for an internal SATA HDD (model, serial,
  NTFS serial, identity MATCH, throughput above), and for registration and FULL verification
  through a USB bridge that reports the disk's serial (the operator's 24 TB disk, two FULL verifies
  of 10.1 TB, PASSED). Still open: USB bridges without serials, a USB disk unplugged during a FULL
  verify and resumed once attached again (decision 0034), exFAT external disks; the NTFS serial
  relationship on one medium seen from both OSes; the 100 000-file / 20 GB fixture; register on
  Windows and verify on Linux and the reverse; uncached-read audits.
- **Packaging (R-121).** The Windows package is checked locally (`veriarchive_package`: the
  installed tree runs in both languages with only Windows on `PATH` and imports no
  `msys-2.0.dll`) and in the GitLab Windows job, last at 31e1abb. Its zip now holds no folder of its
  own, so that a new version unzips over the old one, and the licenses of what it bundles
  (decision 0038). It still has to run on a machine without MSYS2.

## V1.1

Stage 1, chunk hashes and the Merkle root (decisions 0031 to 0033), is done, and numbered the
program 1.1.0 (`--version`, `repo info` and Settings show it, and every manifest and verification
run records it); the window's jobs, its redesign, a device behind a changed enclosure and the
verification report (decisions 0045 to 0048) number it 1.2.0:

| Part | Implemented |
|---|---|
| Chunk hashes | Registration and relocation write 64 MiB chunk lines and the Merkle root (總碼 in the interface) in every manifest, hashed in the same read pass as the file hashes; the index and export/import keep them; `archive show` gives each revision's chunk size, chunks and Merkle root. A manifest without a chunk size, as V1 wrote it, stays valid (decisions 0031, 0032) |
| Verification by chunk | A FULL verify compares a file whose content differs chunk by chunk and names the chunks that differ and the first byte that differs, also after a resume, and warns where the chunk hashes and the whole-file hashes disagree (decision 0032) |
| Adding chunk hashes | `verify --full <root_path\|archive_uuid> --add-chunk-hashes`, and in the window "FULL verify, and add chunk hashes" in the menu beside an archive's FULL verify, give an archive registered by V1 a next revision with chunk hashes, its entry lines byte for byte the verified revision's, where every entry still matches it. One operation at a time creates an archive's next revision; an unfinished one is listed until it finishes or is abandoned (`archive abandon-verification`) (decision 0033) |
| Repository schema version 3 | The first open by this version keeps a copy of the database beside it, then upgrades it, the window shown at once with a notice while it does; Settings and `repo info` name each copy and what it is for, for as long as it is there (decision 0032). Only a database that can be used is upgraded: one that is missing, empty, not a database, damaged or made again and not filled is named for `repo rebuild-index` as V1 names it, and the rebuild upgrades an older database, keeping its copy, before it fills its chunk rows and Merkle roots from the manifests; one it makes again has version 3 (decisions 0032, 0036) |
| Times and words | An archive is created when its first revision is, at the time its manifest gives, and the upgrade sets the archives of an older repository so (decision 0040); `archive compare` and Compare Archives say when each compared revision was recorded (decision 0041); what a revision records is called recorded, not registered (decision 0042) |
| Long operations of the window | While an operation of the window runs, the computer does not sleep on its own (Windows: a power request; Linux: a logind inhibitor lock), and one that ends while no window of the program is active is told in a notification of the system; the command line does neither, its power settings deciding (decision 0043). That each system shows the notification, and that logind takes the lock, is reviewed, not tested |
| Device alerts | Registration, verification and relocation ask each device of the volume for its own health before they read (Windows: the storage stack's health of the disk; Linux: udisks2's SMART), and `media list` shows it; a FULL verification times each hundredth of the content it reads and compares its read with the last usual FULL verification of the archive on the same device. A device that reports failing, or a read 30 % slower over all or 50 % slower in a stretch, is a device alert: told at once, in the report and the notification, and marked in Archives and History until a later verification gives none, never changing the result (decision 0044). That udisks2 answers on a Linux desktop, that a failing device reads as failing, and how the window shows the alert, is reviewed, not tested |
| Jobs of the window | Every registration, verification, comparison, relocation and export of the window runs as a job of its own, several at once; jobs of one device take turns in the order started, others run at once; a page Jobs lists them with Stop and Details, and closing the window with jobs running asks first. The judgement of an attempt asks whether this process runs that very operation, by the job's root and archive (decisions 0045, 0046). Two jobs of one device waiting in the window is reviewed, not tested: a sample archive is read before a second job could wait |
| The window (decision 0046) | Built with libadwaita ≥ 1.5 on GTK ≥ 4.14, both APIs pinned, libadwaita bundled in the Windows package with its Traditional Chinese texts and license. Archives: the unfinished operations, the list of archives with where each is attached now, its last FULL verify, result and marks, and the details of the one chosen, from which it is verified (FULL verify and its menu), compared, given a new revision, exported, relocated (also resumed) and retired; Devices: the attached devices with their health, volumes and archives, read every 30 s; History: the runs of every archive with a filter; a result page per run; Register Archive in three pages with the archive it is a copy of; Settings as a preferences page. Retiring waits for every operation that would give the archive a new revision, on the command line too, and a registration or relocation of an archive retired meanwhile records nothing. The window's logic has unit tests in files without GTK, and the smoke test builds every page in both languages; the widgets, dialogs and polls are reviewed, not tested, and seen in the screenshot tour on sample data, where every run passes and every archive is attached, so a FAILED or INCOMPLETE result page and the not-attached and MIGRATED notices appear in no picture. Still required: the GitLab pipeline on this work, whose command-line end-to-end test checks `media list`, which now asks devices' health through the function Devices uses, and `archive retire`'s wider refusal; the local build links no command line (an antivirus deletes it, decision 0043) |
| A device behind a changed enclosure (decision 0047) | A FULL verification by an archive's uuid whose carrier is seen behind another enclosure (`MIGRATED`, both behind a USB bridge or the one seen now behind an enclosure that hides its identity) goes on with a carrier confirmation: the device's own serial read and equal to the archive's confirmed device, or the operator's confirmation; the confirmation is recorded in the run's hash-chained counts whatever the result, and only a pass records the new carrier as the next revision, so that later verifications match; every other `MIGRATED` stays a relocation's. The device identity reader (`veriarchive-device-identity.exe`, Windows only, statically linked, its manifest `requireAdministrator`) reads the device's own model, serial and WWN with ATA IDENTIFY DEVICE alone, started through UAC from the window on the operator's request; the window asks first in a dialog, Devices reads on request, and Register Archive records the serial with a switch that is off by default. The command line gains no option and never elevates; `verify --full <uuid>` of an archive attached only on another device now exits 12 (MIGRATED), where it exited 10. The engine, the reader's pure code and its argument checks (the reader started without administrator permission), and the window's flow have unit tests on sample data. Still required: the owner's tries by hand, which no test can make (Windows' question in front of the window and the name it shows, a real SAT bridge, a bridge that passes no ATA command, a sleeping hard disk, the identification page on real enclosures, the whole flow); the GitLab pipeline, for the Linux code, the package test with the reader and the command line's end-to-end test; no mutation check of this decision's rules has been run yet |
| The verification report (decision 0048) | One self-contained HTML file of one recorded run that reached a result: no script, nothing loaded from elsewhere, every value escaped (invalid UTF-8 and control characters as U+FFFD), a light and a dark style and A4 printing, the read speed chart as inline SVG, the same bytes for the same records and time of making. It says what the run's result page says, from the same model, moved into `src/app` so that the window and the command line render it alike, with every item, the device's own identity that a carrier confirmation read (decision 0047), the archive's runs newest first with this run marked, the history chain's state when it was made (the check of `repo verify-history`) and this run's `row_hash`. Made by Save report... on the result page (the system's save dialog, written off the window's thread, a toast with Open) and by `veriarchive-cli report <run_id> --out FILE.html [--json]`, which records nothing and never replaces a file: it refuses a FILE that is there, also one made while the report is made, as its writer's commit names a file only where nothing of that name is there (`IOutputFile::commit_new()`, `IoErrorKind::AlreadyExists`; on Linux but for a file system with neither `RENAME_NOREPLACE` nor hard links), and an unknown or `INCOMPLETE` run (exit 10). Neither writes a report into an attached archive's root, also through a symbolic link, a junction, a drive letter made with subst or a short name, and the window's export now checks so too. The writer, the root guard (with a real sample junction on Windows), the platform writer's no-replace commit (the real writer in a temporary sample folder) and the command line's sentences have unit tests on sample data, and the smoke tests check a sample report in both languages. Still required: the GitLab pipeline, for the command line's end-to-end test of `report` (the file, a second report refused, zh-TW, an unknown run, the archive's root and a link to it) and the Linux branches of the unit tests (the root guard through a symbolic link, the no-replace commit); by hand, the save dialog (the name and folder it proposes, its question before it replaces a file), Open, the refusal message and the failure toast, and the file in browsers other than Edge; reviewed only, the Linux writer's fallbacks where a file system refuses `RENAME_NOREPLACE` (a hard link, also one that NFS says `EEXIST` to after making it) or has no hard links either (a look and then the rename); one mutation check (a no-replace commit that replaced failed the writer's test), none of the other rules |
| Archives' overview, Jobs' unfinished operations (decision 0049, after 1.2.0) | Archives shows an overview above the archives: how many need attention (OVERDUE, never FULL verified, last FAILED, a device alert, a manifest that cannot be used; none retired), how many are attached, the latest FULL verification, and a tile per standing that filters the list; Verify the attached archives that need it FULL verifies those OVERDUE, never FULL verified or last FAILED that are attached as recorded, behind no other enclosure, with no job and no unfinished operation. The head of an archive's details (label, result, menu, volume and root, FULL verify, Cross-check...) stays in view while they scroll; the banner says how long ago before when. Jobs lists the unfinished operations below the jobs, leaving out those a job of the window works on unless stopped, with Resume, Abandon... and the bar of abandoning; the jobs button and the Jobs row count those that stopped; Stop's tooltip says what it keeps; the window's refusals name Jobs. The window calls a comparison a cross-check (核對完整性), side by side (Archive A, Archive B; only in A, only in B, differs), its report giving each archive's last FULL verification and what it shows of the media. No change to the command line, the engine or the database. Unit tests: the overview's filters, tiles, heading and line, needs_full_verify, the cross-check's words and report, the banner's line, the catalog. Reviewed and seen in the screenshot tour on sample data, not tested: the Jobs group and its filtering, the tiles filtering the list, the frozen head, the one-click verification. Not yet run in CI (the GitLab compute quota was used up on 2026-11-06). |

On the operator's repository (the two archives above), a build of 1.1.0 from before decisions
0040 to 0042 upgraded the database from schema version 1, and its FULL verifies that add chunk
hashes, the two disks at once (2026-11-01), PASSED and recorded revision 2 of both archives:
152 408 chunk lines for the 11 929 files larger than 64 MiB, the same Merkle root on both disks,
and `archive compare` IDENTICAL on revision 2, at about 220 MB/s on the internal disk and 280 MB/s
on the USB disk. Decision 0040's update of the archives' times was then applied to that database
with the upgrade's own statement, after a copy was kept, and the final 1.1.0 opens it with its
history intact.

Still required before stage 1 is accepted: the HDD throughput measurement against `dd` (R-120)
again, now that the read pass also hashes chunks. The FULL verifies above read faster with chunk
hashes than version 1.0.0 did without them on the same disks, but were not compared with `dd`.

Still to come: QUICK and DEEP verification, Ed25519 signatures and trusted keys, signed history
checkpoints, NTFS alternate data streams and the USN change tracker. The elevated helper is not
planned: the change journal can be read without elevation. The one elevated program is the device
identity reader of decision 0047, started through UAC from the window for one read at a time.
