# VeriArchive user guide

VeriArchive records integrity evidence for archive disks and later proves whether the archived files were modified, corrupted, deleted, or added to. It only **detects**: it never repairs, restores, or makes backups.

The whole workflow is two steps:

1. **Register**: read every file of a folder or volume once and store its fingerprints (BLAKE3 and SHA-256) as the **baseline**.
2. **Verify**: later, re-read every registered byte from the disk, compare it with the baseline, and report what changed.

[繁體中文版](user-guide.zh-TW.md)

## Installing (Windows)

Unzip `VeriArchive-<version>-windows-x86_64.zip` into any folder; nothing else (such as MSYS2) is needed. It contains:

- `bin/veriarchive-gui.exe`: the graphical interface
- `bin/veriarchive-cli.exe`: the command line
- `share/`: icons, settings and translations GTK needs; `share/licenses/`: licenses of the bundled libraries

## Interface language

The interface is available in English and Traditional Chinese. The first setting found wins:

1. `--lang en` or `--lang zh-TW` on the command line
2. the `VERIARCHIVE_LANG` environment variable
3. `"language"` in the config file
4. the Windows display language
5. English

In the GUI, the Settings view has a language drop-down (English / 繁體中文). The choice is saved in the config file and the window is rebuilt in the new language at once; typed folders, chosen options, the window size and the reports shown are kept, and the reports are shown in the new language. It cannot be changed while a registration, verification or comparison runs.

`--json` output, exit codes and the codes inside JSON (`PASSED`, `Modified`, ...) are **always English**, so scripts behave the same in every language.

## First use: create the repository

The **repository** holds every manifest (baseline) and the verification history.

- GUI: it opens on Settings; press "Initialise repository".
- CLI: `veriarchive-cli repo init`

By default the repository is created in `data` next to the program, or in `VeriArchive` in your home folder when the program folder is not writable. Its location is remembered in the config file and used from then on. To choose it yourself: `veriarchive-cli --repository D:\VeriArchiveRepo repo init`.

> **Back up the repository.** Losing it loses all verification history; manifests can be recovered only from media copies, if they were written.

If a configured repository is unavailable later (for example an external disk is not attached), VeriArchive stops with an error; it never falls back to another location.

### Moving the program folder

On first use, the repository (`data`) and the config file that remembers it (`config.json`) are created in the program's folder (`bin` in the zip), and the config file records the repository relative to that folder: `"repository": "data"`. Program, configuration and repository are therefore one folder. You can move or copy the whole unzipped folder, to another drive, to another computer, or onto a USB disk that gets another drive letter, and it keeps using its own repository. Backing up that folder also backs up the repository.

- Copying the folder copies the repository; from then on each copy records its own verification history. Keep one of them as the folder you work with.
- A repository outside the program folder (`VeriArchive` in your home folder, or a place chosen with `--repository`) is remembered by its full path and does not move with the folder.
- A folder set up with an earlier version remembers the full path. To make it movable, open `bin\config.json` in a text editor and change the value of `"repository"` to `"data"`.

## Registering an archive

GUI, Register Archive view:

1. Choose a folder with "Choose...", or type a volume root such as `E:\`.
2. Optionally give a label (for example `ARCH-0012`).
3. Press "Register". The progress bar shows the share done, the speed and the time left (see "Reading the progress" below); "Cancel" stops at any time.

CLI:

```text
veriarchive-cli register E:\ --label ARCH-0012
```

Options:

| Option | Purpose |
|---|---|
| `--write-media-copy` | Also write the manifest onto the disk (`.veriarchive` folder) as a media copy. By default nothing is written to the archive. |
| `--new-revision` | The folder or volume is already registered and its content changed on purpose: create a new revision (a new baseline). |
| `--force-new-archive` | The folder or volume is already registered, but register it as a separate archive. |
| `--resume` | Continue an interrupted registration without re-reading hashed files. While the program that ran it still runs, it stops and says which one. In the GUI: "Resume" on its row in the Archive List, or the resume box of the Register Archive view. |
| `--io hdd` or `--io ssd` | Reading strategy: `hdd` reads sequentially, `ssd` in parallel. Usually detected automatically. |
| `--lost-manifest <uuid>` | The last resort for an archive whose latest manifest is lost: register its next revision from what the folder or volume holds now (see "Archive list and OVERDUE" below). It first only says what it would do; add `--confirm` to do it. |

Registering an already registered folder or volume again stops with exit code 12 and asks you to choose `--new-revision` or `--force-new-archive`.

A drive letter made with `subst` (such as `S:` for the folder `D:\Data`) is not a volume. A folder reached through one is registered by its real path: registering `S:\Photos` records `D:\Data\Photos` on the volume that holds it, and the reports show that path. It can be verified through the letter or by its real path.

VeriArchive registers and verifies folders on volumes attached to this computer. A folder on a network share is refused: a network path (`\\server\share\Photos`), a folder on a drive letter mapped to a share, and on Linux a folder on a network filesystem (NFS, SMB, sshfs) or on a share that GNOME or KDE opened (choosing such a share in their file dialogs, "Choose..." included, gives a folder below `/run/user/<uid>/gvfs` or `/run/user/<uid>/kio-fuse-...`). A phone or camera that GNOME shows there is refused as well. On Windows, a root written with `\\?\` in front (`\\?\D:\Photos`) is refused too: write it as `D:\Photos`. A volume that has no drive letter is the exception: Windows names it only by its GUID path, which `mountvol` lists, such as `\\?\Volume{...}\`. Type that path exactly as `mountvol` lists it, for the whole volume (`\\?\Volume{...}\`) or a folder on it (`\\?\Volume{...}\Photos`): it is registered, verified and relocated to like any other root, and is the same archive as through a drive letter the volume gets later. `verify --full <uuid>` finds an archive on such a volume too. For each refused root, `register`, `verify --full <root>` and `archive relocate` stop with exit code 10 before anything is read.

Files that cannot be read at registration (permission, media errors) are recorded without hashes and are never reported as verified.

### Reading the progress

While files are hashed (registering, verifying, relocating), the GUI's progress bar and the CLI show one line, for example:

```text
hashing: 2.16 TiB of 9.44 TiB (23%) · 240 MiB/s · about 8-9 hours left
```

The CLI writes it to stderr, and not at all with `--json`.

- **The share done is exact**: it counts bytes, so a folder of large videos and a folder of small photos move it alike. It shows 100% only once the last file is done. Scanning comes first and shows only the entries found, as the total is not known yet.
- **The speed** is the average of the last three minutes.
- **The time left is an estimate**, shown as a range. The low end of the range picks the unit: minutes while it is under an hour, hours while it is under two days, then days ("about 55-175 minutes left", "about 47-96 hours left"). It counts each file's fixed cost (opening it; on an HDD, moving the head) as well as its bytes, so a stretch of small files does not make it jump. It reads "estimating time left" during the first minute and after minutes without progress, and the range widens when the speed varies or keeps dropping (an HDD reads its inner tracks more slowly). A pause, such as the computer sleeping, does not count as slow reading. Once every file is hashed, the line shows no time left while the results are written.

## Verifying an archive

GUI, Verify Archive view: choose the folder or volume, or pick a registered archive in the "Verify:" drop-down, then press "Verify (FULL)".

CLI, by path or by archive uuid:

```text
veriarchive-cli verify --full E:\
veriarchive-cli verify --full 46d4c696-4999-4c7d-80eb-b626da614e77
```

With a uuid, VeriArchive finds the archive on the attached disks. If more than one attached volume holds it, as a device and a sector-by-sector clone of it do, it does not choose one: it stops (exit code 12) and names their folders. Verify one of them by its folder, or detach the other volumes and verify it again. In the Verify Archive view, type or choose the folder of the one to verify, choose "The folder or volume above" in "Verify:", and press "Verify (FULL)".

### Reading the result

```text
FULL VERIFY FAILED  ARCH-0001 (46d4c696-...) revision 1
  root: E:\
  identity: MATCH
  coverage: 7 of 13 registered bytes read and matched
  items:  Modified 1  New 1  Verified 1
  New  new.txt
  Modified  readme.txt  (content differs)
```

A result always carries its mode and date: only FULL VERIFY PASSED means 100% of the registered content was read and matched.

| Result | Meaning | Exit code |
|---|---|---|
| PASSED | every registered byte was read and matched | 0 |
| PASSED_WITH_WARNINGS | content matched, but something needs attention (no hardware serial to compare, an unreadable filesystem id, metadata differences) | 2 |
| FAILED | files were modified, missing or unreadable, or the media identity is wrong, or the archive's manifest in the repository cannot be used (missing, unreadable or changed: then the media is not read at all, and the result says how to fix the manifest) | 3 |
| INCOMPLETE | the verification was interrupted (cancelled, disk removed); it can be resumed | 4 |

Item statuses:

| Status | Meaning |
|---|---|
| Verified | content matches the baseline |
| Modified | content differs from the baseline |
| Missing | in the baseline, not found now |
| New | a file the baseline does not have |
| Unreadable | could not be read (permission, media error, ...) |
| MetadataMismatch | same content, different modification time (a warning by default); with "Strict metadata" or `--strict-metadata` mode and attributes are compared too, and a difference fails the run |
| TypeChanged | for example a file became a folder |
| Unbaselined | unreadable at registration, so there is no hash to compare |

### Resuming an interrupted run

An interrupted verification ends INCOMPLETE and can be continued; files already verified are not read again:

- GUI: in the Archive List view, press "Resume" on the verification's row among the unfinished operations. This works for any interrupted verification, also one started on the command line or interrupted before the program was closed. It continues the verification exactly as the command below does: the archive is looked for on the attached disks by its uuid, so a disk that got another drive letter is found too, and the Verify Archive view shows the progress and the result. In the window where the verification was interrupted, "Resume interrupted run" in the Verify Archive view continues it as well.
- CLI: `veriarchive-cli verify --full <archive_uuid> --resume <RUN_ID>`, for any interrupted verification, also one started in the GUI. Among the unfinished operations of the Archive List, pointing at the label of its row shows the archive's uuid, and pointing at its state shows the run's id.

If more than one attached volume holds the archive, as a device and a sector-by-sector clone of it do, a resume by the archive's uuid cannot tell which one the run read: it stops (exit code 12) and names them, with the root the run read. Resume it at the root on the volume it read with `veriarchive-cli verify --full <root> --resume <RUN_ID>`, or detach the other volumes and resume it again.

### When VeriArchive needs your decision (exit code 12)

| Message | Situation | What to do |
|---|---|---|
| UNKNOWN | not a registered archive | check that the right disk is attached, or register it |
| MIGRATED | the filesystem matches but the device changed (e.g. cloned onto a new disk) | if you moved it: `archive relocate <uuid> <new root>` |
| AMBIGUOUS | several archives match (cloned disks) | choose one with `verify --full <archive_uuid>`; where the volumes of those archives are attached together, detach all but the volume of the one to verify, then verify it |
| AMBIGUOUS: archive ... is on more than one attached volume | a verification by the archive's uuid finds it on several attached volumes (a device and a clone of it) | verify one of them by its folder: `verify --full <root>`, or detach the other volumes and verify again |
| AMBIGUOUS: archive ... is on more than one attached volume ...; run ... | a resume by the archive's uuid finds it on several attached volumes | resume at the root on the volume the run read: `verify --full <root> --resume <RUN_ID>`, or detach the other volumes and resume again |
| PREVIOUS_CARRIER | this is the old disk of a relocated archive | the old disk is no longer verified; verify the new carrier |

## Comparing an archive with its copy

Say you copied disk F to disk G with your own tools (VeriArchive never makes backups) and want proof that G holds exactly what F holds. Verifying G alone does not show that: it compares G with G's own baseline. Instead, register each disk as an archive of its own and **compare** the two archives. The comparison takes every entry (file, folder, link) of both manifests and confirms that it is in both, with the same type and the same content (BLAKE3 and SHA-256). It reads no disk, only the manifests in the repository, so it takes seconds. Modification times and attributes are not compared, because copy tools do not always keep them.

### 1. Register both disks

Register them once the copy is complete; each registration reads its whole disk.

```text
veriarchive-cli register F:\
veriarchive-cli register G:\ --copy-of <uuid of F's archive>
```

- `--copy-of` is optional: it records that G is a copy of F, for display only. It needs the full uuid, which `archive list` shows.
- Register the folder that corresponds to F's root, so the paths line up: if the copy of F sits in `G:\Backup\F`, register `G:\Backup\F`. Anything else in the folder registered on G shows up as New.
- A sector-by-sector clone of F has F's filesystem id, so registering it stops with exit code 12 (MIGRATED). Add `--force-new-archive` (GUI: "Register as a separate archive even though this folder or volume is registered") to register it as an archive of its own.

### 2. Compare

GUI, Compare Archives view:

1. In "Reference archive (A):" choose F's archive, the original.
2. In "Copy to check (B):" choose G's archive.
3. Press "Compare". The progress bar counts the entries compared; "Cancel" stops at any time.

The report begins with "ARCHIVES IDENTICAL" or "ARCHIVES DIFFERENT" and lists every difference. Choosing the same archive twice is refused.

CLI, archive A (the reference) first, then archive B (the copy). The first 8 or more characters of each uuid are enough:

```text
veriarchive-cli archive compare 0d0ce48d a82ebd4e
```

```text
COMPARE DIFFERENT  ARCH-0001 (0d0ce48d-9445-4e7a-ae49-8fb42ef05201) r1  vs  ARCH-0002 (a82ebd4e-7766-4291-9795-d1a8d29efadd) r1
  A: ARCH-0001, registered 2026-10-15 10:18:09 (UTC+8), 6 entries
  B: ARCH-0002, registered 2026-10-15 10:18:09 (UTC+8), 6 entries
  same: 4  Missing 1  Modified 1  New 1
  Modified  專案/budget 預算.csv  (size 26 -> 41)
  New  新增的檔案.txt
  Missing  會議紀錄.txt
  This compares what each registration read. A FULL verify of each archive proves that its carrier still holds that content.
```

### Reading the comparison

| Result | Meaning | Exit code |
|---|---|---|
| IDENTICAL | every entry of A is in B with the same type and content, and B has no other entries | 0 |
| IDENTICAL_WITH_WARNINGS | nothing differs, but the entries listed could not be compared (see Unbaselined and MetadataMismatch below) | 2 |
| DIFFERENT | at least one entry is missing, new, modified or of another type | 3 |

Each listed entry is B seen from A:

| Status | Meaning |
|---|---|
| Missing | in A, not in B: the copy lacks it |
| New | in B, not in A: the copy has it in addition |
| Modified | different content; `size 26 -> 41` gives the size in A and in B (bytes) |
| TypeChanged | for example a file in A is a folder in B |
| Unbaselined | no hash to compare: it was unreadable when A or B was registered (the detail says which and why), or it lies in a folder one of them could not read |
| MetadataMismatch | a link whose absolute target differs between an archive registered on Windows and one registered on Linux; a warning only |

If the comparison cannot run, for example because a manifest in the repository was changed or damaged or an archive's first registration never finished, it reports the reason and exits 3; a cancelled comparison exits 4. `--json` gives the result and every difference, always in English.

### What the comparison proves

It compares what each registration read, not what the disks hold today. For proof that G holds F's content now:

1. The comparison says IDENTICAL: the two baselines hold the same entries with the same content.
2. A FULL verify of each archive (`verify --full F:\`, `verify --full G:\`) says PASSED: each disk still holds its baseline.

If F changed between its registration and the copy, the comparison shows the change. Register F again with `--new-revision` and compare again. A comparison writes nothing: it is not part of the verification history and does not change "Last FULL verify" or OVERDUE, so you can run it as often as you like.

## Archive list and OVERDUE

The Archive List view, or `veriarchive-cli archive list`, shows for each archive:

- **Last FULL verify**: result and date of the last complete read
- **Last verify**: result and date of the last verification of any mode
- **OVERDUE**: no FULL verify for 365 days or no verification for 180 days (adjust under `policy` in the config file)

An archive whose manifest in the repository cannot be used (missing, unreadable or changed) is listed too, with a line that says what is wrong and how to fix it (in the Archive List view, in red under its row), and `archive list` then exits with code 2 (not for a retired archive whose manifest is lost, see below). Verifying that archive (by its uuid, or chosen in the Verify Archive view) records the failure in its history. A manifest that is missing, unreadable or changed is put back with `veriarchive-cli import <folder>/r0002.vam` (the file of that revision), from a backup of the repository, from an export of that revision, or from its media copy (in the `.veriarchive` folder on the archive's media, where registration wrote one): the import checks that the file is the one the repository recorded, and puts it back.

If none of these holds the manifest of the archive's latest revision, the manifest is lost. Where its first line cannot be used (the manifest is missing, its first line cannot be read, or its first line has changed), registering a new folder or volume and relocating stop on it until it is back, or until the archive is retired (below), as finding which archive a folder or volume holds reads the first line of every archive's latest manifest; a new revision of another archive's folder (`register --new-revision`, or the new revision box in the Register Archive view) is still registered where the folder is plainly that archive's, on the volume and the device its manifest records. A manifest changed only in its body stops neither, though verifying its archive fails on it. The last resort is to register the archive's next revision from what its folder or volume holds now: `veriarchive-cli register <root> --lost-manifest <uuid>`. Without `--confirm` it only says what it would do (exit code 12); run it again with `--confirm` to do it. What the folder or volume holds then becomes the new baseline without being compared with the lost revision, so damage since that revision was registered would not be found. The archive keeps its verification history and never goes back to an earlier revision. Where the lost manifest's first line cannot be used, the lost revision leaves its list of revisions; a manifest changed only in its body stays listed, as an earlier revision. It refuses a manifest that is not lost (one that can be used, or one where only the index differs: rebuild the index), a root that belongs to another archive (a folder containing another archive's folder or inside it, on the volume and the device that archive's manifest records), and a root whose `.veriarchive` folder still holds the lost manifest as a media copy (import that file instead). Another archive's very folder, on its volume and device, is taken for an archive registered at that same folder on purpose (`--force-new-archive`), as its twin: before you confirm, it names the archive whose folder it is, so confirm only where the folder was registered as both. Where the volume of the folder may be a sector-by-sector clone of another archive's volume, or that volume itself (the same filesystem on another device, or no device serial to compare), it names that archive as possibly a clone before you confirm: confirm only where the folder is not that archive's. The Archive List view names this command in the problem line; it runs on the command line only.

If the device that held the archive is gone as well (broken, lost or wiped), no folder or volume is left to register its next revision from. Retire the archive then: `veriarchive-cli archive retire <uuid>`. As its manifest is lost, the command first only says what retiring means (exit code 12); run it again with `--confirm` to do it. The archive keeps its verification history and its hash chain, but can no longer be verified or take a new revision, and it no longer stops registering a new folder or volume or relocating: a retired archive whose latest manifest is lost is no longer taken as a possible owner of a folder or volume. Should its device turn up again, nothing recognises it, and registering it makes a new archive. `archive list`, `archive show` and the Archive List view show the archive as retired, with a line "manifest lost: ..." in place of the problem (in the view, not in red), and `archive list` no longer exits with code 2 for it. While a registration of the archive is unfinished, retiring it is refused: abandon that registration first, or finish it. An archive retired earlier whose manifest is lost later is treated the same way, and a retired archive takes no registration over its lost manifest. Should the manifest turn up after all, `import` puts it back, and the archive is recognised again as a retired archive.

The manifest of an earlier revision can be lost too: `archive show` marks that revision, and registering a new folder or volume, or verifying a folder, may stop on it where VeriArchive looks among the archive's previous carriers. Put it back with `import` as above; if nothing holds it, `veriarchive-cli repo rebuild-index` leaves that revision out of the index, after which a volume that held only that revision is no longer recognised as a previous carrier of the archive. Its verifications stay in the history.

Above the archives, the Archive List view also shows the registrations, verifications and relocations that have not finished, with their progress, also those started from the command line. A stopped verification or registration that can be resumed has "Resume" there: it continues it exactly as `verify --full <uuid> --resume <RUN_ID>` or `register <root> --resume` does, wherever it was started, and the Verify Archive or Register Archive view shows its progress and result (what is ticked in those views takes no part: like the command, Resume writes no media copy, and uses strict metadata only where `strict_metadata` is set under `policy` in the config file). A stopped relocation is resumed on the command line, with `archive relocate <uuid> <root> --resume`. A stopped registration that can never be finished, for example because its media failed, can be given up there with "Abandon..." (CLI: `veriarchive-cli archive abandon-registration <uuid> --confirm`): its saved progress is lost, while recorded revisions and history stay. A stopped relocation can be given up there the same way, for example when its new carrier failed (CLI: `veriarchive-cli archive abandon-relocation <uuid> --confirm`): its saved progress is lost, while the archive, its revisions and history stay.

`archive list` and `archive show` print an archive's unfinished registration and its unfinished relocation as one more line each under the archive: the revision, the folder or volume it reads, what it has done so far and its state, in the words of the Archive List view (for example "stopped; it can be resumed" or "new carrier differs; abandon it").

Times are shown in this computer's local time with the offset from UTC, for example `2026-10-14 08:13:53 (UTC+8)`. `--json` output and the history database always use UTC, for example `2026-10-14T00:13:53Z`.

More archive commands:

```text
veriarchive-cli archive show <uuid>               # details and every revision
veriarchive-cli archive retire <uuid>             # no longer expected to be verified; history is kept
                                                  # (--confirm where its manifest is lost, see above)
veriarchive-cli archive relocate <uuid> <root>    # after moving an archive to a new disk: confirm and verify it
veriarchive-cli archive compare <uuid> <uuid>     # do two archives hold the same content? (see above)
```

An interrupted `archive relocate` keeps what it has matched and is listed with the unfinished operations: continue it with `archive relocate <uuid> <root> --resume`, or give it up with `archive abandon-relocation <uuid> --confirm`. A relocation that finds an entry on the new carrier that differs from the archive, such as a modified or missing file, ends instead, also when it is interrupted after that, and its saved progress is removed: copy again, and the next relocation reads the whole new carrier. An entry that cannot be read ends it only once everything else has been read; if the new carrier is unplugged before that, the relocation is interrupted, and its resume reads that entry again. If the saved progress of a relocation that ended cannot be removed, the relocation stays listed as "new carrier differs; abandon it": abandon it, then relocate again.

`archive show`, `retire`, `relocate`, `compare`, `history`, `export` and `export-sha256sums` accept the first 8 or more characters of a uuid, as long as no other archive starts with them.

## Verification history

The Verification History view, or `veriarchive-cli history <uuid>`, lists every verification of an archive.

An interrupted `archive relocate` is listed there too, as an INCOMPLETE FULL verification of the revision it compared the new carrier with, marked "interrupted relocation attempt": it read the new carrier, not the archive's carrier. While the relocation is unfinished, continue it with `archive relocate <uuid> <root> --resume`; `verify --resume` does not continue it, and says so. The FULL verification with which a relocation recorded the archive's next revision is marked "relocation from r1", r1 being the revision it compared the new carrier with. The GUI shows these marks in a column "Note", which appears only for an archive that has such runs; `history --json` gives each run `relocation`: null, or the relocation's `run_id`, `from_revision` and `interrupted_attempt`.

The history is a hash chain. In V1, checking it detects a recorded run that was changed or removed, but not the removal of the newest runs, nor runs changed with the chain computed again; V1.1's signed checkpoints are planned for that. To check it:

- GUI: press "Check history chain"
- CLI: `veriarchive-cli repo verify-history`

## Export and import

```text
veriarchive-cli export-sha256sums <uuid> --out sums.txt   # sha256sum-format list
veriarchive-cli export <uuid> --out D:\export             # export the manifests
veriarchive-cli import D:\export\r0001.vam                # import into another repository
```

`sums.txt` can be checked without VeriArchive: `sha256sum -c sums.txt`.

## More

- If the database is damaged, rebuild the index from the manifests: `veriarchive-cli repo rebuild-index`. Manifests are the truth; the database is an index.
- Every command accepts `--json` for machine-readable output.
- Exit codes: 0 passed, 2 passed with warnings, 3 failed, 4 incomplete, 10 usage, 11 repository, 12 needs your decision.
- V1 detects accidental corruption and unintended change; tamper evidence (signed manifests) begins in V1.1.
- VeriArchive needs no administrator rights and should not be run elevated.
- In PowerShell, run `[Console]::OutputEncoding = [Text.UTF8Encoding]::new()` before capturing, redirecting (`>`) or piping CLI output; otherwise Chinese text is garbled. Output shown directly in the window is not affected.
- If the GUI closes unexpectedly, the reason is written to `veriarchive-gui.log` in the user config folder (`%APPDATA%\VeriArchive` on Windows, `~/.config/veriarchive` on Linux). Include it when reporting a problem.
