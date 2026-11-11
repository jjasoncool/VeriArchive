# VeriArchive

English | [繁體中文](README.zh-TW.md)

**VeriArchive is a verification tool for archive disks.** It records integrity evidence when a
disk is registered, and later proves whether the archived files were modified, corrupted, deleted,
or added to. It detects; it never repairs, restores, or creates backups. Windows and Linux, one
codebase.

- **Register** a folder or volume: every file is read once and its BLAKE3 and SHA-256 are stored
  in a manifest (`.vam`), the baseline.
- **Verify** later: the media is identified (filesystem and device identity), every registered
  byte is re-read from the disk (bypassing the cache) and compared with the baseline.
- Results always carry their mode and date: "Last FULL verify: PASSED on …" — never a bare status.
- Verification history is hash-chained. Checking the chain (`repo verify-history`) detects a
  recorded run that was changed or removed, but not the removal of the newest runs, nor runs
  changed with the chain computed again; history signatures, which would detect those, are not
  part of this version.
- A **verification report** says what one run found as one self-contained HTML file (no script
  and nothing loaded from elsewhere; a light and a dark style, and A4 printing), with the archive's
  runs, the state of the history chain when it was made and the run's hash in it, saved from the
  run's result page or by `veriarchive-cli report` (decision 0048). It is not signed: the
  repository stays the record.

VeriArchive detects accidental corruption and unintended change. Tamper evidence (signed
manifests) is not part of this version.

## Why VeriArchive

VeriArchive is a verification tool, not a backup or sync tool: it answers one question, whether
an archive disk still holds exactly what was registered, and keeps the proof.

Archive disks often sit on a shelf for years between uses. Meanwhile their content can change
without anyone noticing: sectors decay, a USB enclosure fails, a file is deleted or overwritten by
mistake, a sync tool replaces it. Found only when the data is needed, such damage is often too old
to repair from a copy.

VeriArchive records each file's fingerprints when a disk is registered, reads the whole disk again
at each FULL verification and compares, and keeps a dated, hash-chained record of every run: which
disk, on which day, which files were still intact and which had changed. It finds and proves; it
does not repair, and it makes no backups.

A change made on purpose to the files on a disk is found like any other. But whoever can also
change the repository can change the records too: this version cannot prove that the records
themselves were not altered. So keep the repository away from the archive disks, where whoever
can reach those disks cannot change it, and back it up (see "Backing up the repository").

## A real verification

Not sample data: the FULL verification of a real archive, in the Chinese interface. Its 144,621
entries, 9.2 TiB, were all read and matched in 11 h 56 min, at 214 MiB/s on average; the chart is
the read speed along the archive's content. The device had been moved to another enclosure since
the archive was registered, and was confirmed after reading the device's own serial.

![The FULL verification of a real archive: passed](docs/real/full-verification.png)

## Screenshots

The pages of the graphical interface, showing sample data made by
[`tools/screenshots/`](tools/screenshots/README.md).

| Archives | Verifying an archive |
|---|---|
| ![Archives](docs/images/en/02-archive-list.png) | ![Verifying an archive](docs/images/en/04-verify.png) |
| **A verification's result** | **Register Archive** |
| ![A verification's result](docs/images/en/09-result.png) | ![Register Archive](docs/images/en/03-register.png) |
| **Devices** | **Jobs** |
| ![Devices](docs/images/en/10-devices.png) | ![Jobs](docs/images/en/08-jobs.png) |
| **History** | **Cross-check** |
| ![History](docs/images/en/05-history.png) | ![Cross-check](docs/images/en/06-compare.png) |
| **Relocate to a new carrier** | **Settings** |
| ![Relocate to a new carrier](docs/images/en/11-relocate.png) | ![Settings](docs/images/en/01-settings.png) |

## Documentation

- [`docs/user-guide.md`](docs/user-guide.md) / [`docs/user-guide.zh-TW.md`](docs/user-guide.zh-TW.md):
  how to use VeriArchive (English / 繁體中文): the graphical interface step by step, with
  screenshots, and the command line.
- [`design/HANDOFF.md`](design/HANDOFF.md): the specification (principles, contracts, milestones).
- [`design/decisions/`](design/decisions/): decisions that amend the specification.
- [`GLOSSARY.md`](GLOSSARY.md): the words used in code, documents and output.
- [`design/REVIEW_STATUS.md`](design/REVIEW_STATUS.md): what is implemented and what still needs
  verification on real hardware and CI.

## Build

Windows commands run in an **MSYS2 UCRT64** shell. Dependencies: GCC ≥ 14, CMake ≥ 3.28, Ninja,
SQLite ≥ 3.40, OpenSSL 3, GLib ≥ 2.72, GTK4 ≥ 4.14 and libadwaita ≥ 1.5 (for the GUI). BLAKE3 and nlohmann/json are
downloaded at configure time when not installed.

```sh
cmake --preset windows-ucrt64-debug        # or linux-debug
cmake --build --preset windows-ucrt64-debug
ctest --preset windows-ucrt64-debug
```

Release presets are `windows-ucrt64-release` and `linux-release`. CI runs on GitLab
(`.gitlab-ci.yml`): Windows UCRT64, Ubuntu 24.04 and Fedora.

### Windows package

A self-contained Windows build (programs, every DLL they need, GTK resources and the licenses of
every library they use, bundled or compiled in) runs on machines without MSYS2:

```sh
cmake --preset windows-ucrt64-release
cmake --build --preset windows-ucrt64-release
cd build/windows-ucrt64-release && cpack   # VeriArchive-<version>-windows-x86_64.zip
```

The zip holds `bin`, `lib` and `share` at its root, with no folder of its own, so unzipping a new
version into the program folder updates it whatever the version number (decision 0038).
`cmake --install build/windows-ucrt64-release --prefix <folder> --component VeriArchive` installs
the same tree into a folder. Installing into a folder that holds an earlier install updates it in
place; files the install does not write, such as `bin/config.json` and the repository in
`bin/data`, are left as they are. The `veriarchive_package` test checks that the installed tree
runs with only Windows on `PATH` and never imports `msys-2.0.dll` (R-121), also after a second
install over the first, and that the zip unzips into an installed folder as such an update. It
also checks that the device identity reader, the one program that runs with administrator
permission, is in `bin` and imports only Windows' own DLLs, none that `bin` holds (it is linked
statically, decision 0047).

### Editor (clangd)

`.clangd` reads the compile database in `build/dev-clangd`. Make it on each machine (it is not
committed) with the host's debug preset:

```sh
cmake --preset windows-ucrt64-debug -B build/dev-clangd -DCMAKE_EXPORT_COMPILE_COMMANDS=ON   # or linux-debug
```

The Windows presets name the compiler by its target (`x86_64-w64-mingw32-g++`, the same program as
`g++`), so clangd knows the MinGW headers without further settings.

## Command line

```sh
veriarchive-cli repo init                       # create the repository (first run)
veriarchive-cli repo info
veriarchive-cli media list                      # volumes and the disks behind them
veriarchive-cli register K:\Backups\2026        # baseline a folder or volume root
veriarchive-cli verify --full K:\Backups\2026   # re-read and compare everything
veriarchive-cli verify --full <archive_uuid>    # find the archive on attached disks
veriarchive-cli archive list                    # last FULL verify, last verify, OVERDUE
veriarchive-cli archive compare <uuid_a> <uuid_b>   # does copy B hold what archive A holds?
veriarchive-cli history <uuid>
veriarchive-cli repo verify-history             # check the history hash chain
veriarchive-cli report <run_id> --out run.html  # the verification report of one run, as HTML
veriarchive-cli export-sha256sums <uuid> --out sums.txt   # sha256sum -c, run in the archive's root
```

More: `register --new-revision | --force-new-archive | --copy-of UUID | --write-media-copy |
--resume`, `verify --full ... --resume RUN_ID`,
`archive show | retire | relocate [--resume] | abandon-registration | abandon-relocation`, `export`, `import`,
`repo rebuild-index`.
Every command accepts `--json`. Exit codes: 0 passed, 2 passed with warnings, 3 failed, 4
incomplete, 10 usage, 11 repository, 12 needs an operator decision.

`archive compare` compares the manifests of two registered archives, typically an archive and its
copy on other media, entry by entry: presence, type and content (BLAKE3 and SHA-256), not metadata.
It reads no media and records nothing; the result is IDENTICAL (exit 0), IDENTICAL_WITH_WARNINGS
(2) or DIFFERENT (3). It compares what each registration read; a FULL verify of each archive
proves that its disk still holds it (decision 0020).

The repository location is resolved explicitly (`--repository`, `VERIARCHIVE_REPOSITORY`), then
from the pinned config, and only on first initialisation falls back to `<program>/data` or
`~/VeriArchive`. A configured location that is unavailable is an error, never a fallback. A
repository inside the config file's folder is pinned relative to it, so a portable program
folder with its `data` can be moved as a whole (decision 0019). Back up the repository: losing it
loses the verification history (see "Backing up the repository").

## Backing up the repository

The repository holds the only copy of the verification history: losing it loses every record of
what was verified and when, and the manifests can then be recovered only from media copies, where
registration wrote them. Back it up after verifications, to another physical disk or a network
folder (NAS):

1. Close the VeriArchive window (stop its jobs or wait for them) and make sure that no
   `veriarchive-cli` command runs: a copy made while the database is being written may not be
   consistent.
2. Copy the whole repository folder, the RepositoryRoot that Settings and `repo info` show (in a
   portable install, `bin/data` beside the program), with everything in it: `veriarchive.db`
   (and `veriarchive.db-wal`, where there is one), `manifests`, `repository.json` and the other
   folders.
3. Never put the copy on a disk that holds a registered archive: if that disk fails, the archive
   and the evidence of it are lost together, and whoever can reach that disk could change the copy
   as well.

To restore, copy the folder back to the RepositoryRoot, or put it elsewhere and point the config
file's `repository` at it. Runs recorded after the backup are not in it: verify those archives
again. Never use two copies of one repository at once: their histories would part.

## Graphical interface

`veriarchive-gui` is built with GTK4 and libadwaita (decision 0046). Its pages are Archives (an
overview of how the archives stand, whose tiles filter the list, with Verify the attached archives
that need it; the archives with where each is attached now; and the details of the one chosen,
whose head stays in view as they scroll, from which it is verified, cross-checked, given a new
revision, exported, relocated or retired), Devices (the attached devices, their health, volumes and
archives), Jobs (the jobs of the window, and the unfinished operations wherever they were started),
History (the runs of every archive, each with its result page, from which the verification report
of a run that reached a result is saved) and Settings; Register Archive and Cross-check are dialogs
(decision 0049). A cross-check is the window's comparison: it takes neither archive for the
original, and its report gives each archive's last FULL verification.
Every registration, verification, cross-check, relocation and export runs as a job with its
progress, which Stop ends; interrupted registrations, verifications and relocations can be
resumed in Jobs, and unfinished ones abandoned there. A device moved to another USB enclosure is verified
in full once you confirm that it is the same device, by its own serial or on your word; the
confirmation is recorded in the hash-chained history, and only a pass records the new enclosure
(decision 0047). The GUI runs unprivileged: only to read a device's own serial behind its
enclosure, on your request, does it start the device identity reader
(`veriarchive-device-identity.exe`, Windows only), which Windows asks you to allow with
administrator permission each time. The command line never asks for it.

Only the command line shows every revision of an archive (`archive show`), imports manifests
(`import`), writes a list for `sha256sum -c` (`export-sha256sums`), rebuilds the index
(`repo rebuild-index`), registers over a lost manifest (`register --lost-manifest`), chooses the
reading strategy (`--io hdd|ssd`) and writes `--json`.

## Interface language

The CLI and the GUI speak English and Traditional Chinese (decision 0017): `--lang en|zh-TW`,
then `VERIARCHIVE_LANG`, then `"language"` in the config file (the GUI's Settings page sets it),
then the system's display language. `--json` output and exit codes are always English.
