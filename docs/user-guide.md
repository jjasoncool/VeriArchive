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
3. Press "Register". The progress bar shows the files hashed so far; "Cancel" stops at any time.

CLI:

```text
veriarchive-cli register E:\ --label ARCH-0012
```

Options:

| Option | Purpose |
|---|---|
| `--write-media-copy` | Also write the manifest onto the disk (`.veriarchive` folder) as a media copy. By default nothing is written to the archive. |
| `--new-revision` | The folder is already registered and its content changed on purpose: create a new revision (a new baseline). |
| `--force-new-archive` | The folder is already registered, but register it as a separate archive. |
| `--resume` | Continue an interrupted registration without re-reading hashed files. |
| `--io hdd` or `--io ssd` | Reading strategy: `hdd` reads sequentially, `ssd` in parallel. Usually detected automatically. |

Registering an already registered folder again stops with exit code 12 and asks you to choose `--new-revision` or `--force-new-archive`.

Files that cannot be read at registration (permission, media errors) are recorded without hashes and are never reported as verified.

## Verifying an archive

GUI, Verify Archive view: choose the folder, or pick a registered archive in the "Verify:" drop-down, then press "Verify (FULL)".

CLI, by path or by archive uuid:

```text
veriarchive-cli verify --full E:\
veriarchive-cli verify --full 46d4c696-4999-4c7d-80eb-b626da614e77
```

With a uuid, VeriArchive finds the archive on the attached disks.

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
| FAILED | files were modified, missing or unreadable, or the manifest or media identity is wrong | 3 |
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

An interrupted verification ends INCOMPLETE and tells you how to continue:

- GUI: press "Resume interrupted run".
- CLI: `veriarchive-cli verify --full <archive_uuid> --resume <RUN_ID>`

Files already verified are not read again.

### When VeriArchive needs your decision (exit code 12)

| Message | Situation | What to do |
|---|---|---|
| UNKNOWN | not a registered archive | check that the right disk is attached, or register it |
| MIGRATED | the filesystem matches but the device changed (e.g. cloned onto a new disk) | if you moved it: `archive relocate <uuid> <new root>` |
| AMBIGUOUS | several archives match (cloned disks) | choose one with `verify --full <archive_uuid>` |
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
- A sector-by-sector clone of F has F's filesystem id, so registering it stops with exit code 12 (MIGRATED). Add `--force-new-archive` (GUI: "Register as a separate archive even though this folder is registered") to register it as an archive of its own.

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

Times are shown in this computer's local time with the offset from UTC, for example `2026-10-14 08:13:53 (UTC+8)`. `--json` output and the history database always use UTC, for example `2026-10-14T00:13:53Z`.

More archive commands:

```text
veriarchive-cli archive show <uuid>               # details and every revision
veriarchive-cli archive retire <uuid>             # no longer expected to be verified; history is kept
veriarchive-cli archive relocate <uuid> <root>    # after moving an archive to a new disk: confirm and verify it
veriarchive-cli archive compare <uuid> <uuid>     # do two archives hold the same content? (see above)
```

`archive show`, `retire`, `relocate`, `compare`, `history`, `export` and `export-sha256sums` accept the first 8 or more characters of a uuid, as long as no other archive starts with them.

## Verification history

The Verification History view, or `veriarchive-cli history <uuid>`, lists every verification of an archive.

The history is a hash chain, so edited or deleted rows are detected:

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
