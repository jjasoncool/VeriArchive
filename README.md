# VeriArchive

VeriArchive records integrity evidence for archive disks and later proves whether the archived
files were modified, corrupted, deleted, or added to. It detects; it never repairs, restores, or
creates backups. Windows and Linux, one codebase.

- **Register** a folder or volume: every file is read once and its BLAKE3 and SHA-256 are stored
  in a manifest (`.vam`), the baseline.
- **Verify** later: the media is identified (filesystem and device identity), every registered
  byte is re-read from the disk (bypassing the cache) and compared with the baseline.
- Results always carry their mode and date: "Last FULL verify: PASSED on …" — never a bare status.
- Verification history is hash-chained, so edited or deleted history rows are detected.

V1 detects accidental corruption and unintended change. Tamper evidence (signed manifests) begins
in V1.1.

## Documentation

- [`docs/HANDOFF.md`](docs/HANDOFF.md): the specification (principles, contracts, milestones).
- [`docs/decisions/`](docs/decisions/): decisions that amend the specification.
- [`GLOSSARY.md`](GLOSSARY.md): the words used in code, documents and output.
- [`docs/REVIEW_STATUS.md`](docs/REVIEW_STATUS.md): what is implemented and what still needs
  verification on real hardware and CI.

## Build

Windows commands run in an **MSYS2 UCRT64** shell. Dependencies: GCC ≥ 14, CMake ≥ 3.28, Ninja,
SQLite ≥ 3.40, OpenSSL 3, GLib ≥ 2.72, GTK4 ≥ 4.10 (for the GUI). BLAKE3 and nlohmann/json are
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
all bundled libraries) runs on machines without MSYS2:

```sh
cmake --preset windows-ucrt64-release
cmake --build --preset windows-ucrt64-release
cd build/windows-ucrt64-release && cpack   # VeriArchive-<version>-windows-x86_64.zip
```

`cmake --install build/windows-ucrt64-release --prefix <folder> --component VeriArchive` installs
the same tree into a folder. The `veriarchive_package` test checks that the installed tree runs
with only Windows on `PATH` and never imports `msys-2.0.dll` (R-121).

## Command line

```sh
veriarchive-cli repo init                       # create the repository (first run)
veriarchive-cli repo info
veriarchive-cli media list                      # volumes and the disks behind them
veriarchive-cli register E:\Backups\2026        # baseline a folder or volume root
veriarchive-cli verify --full E:\Backups\2026   # re-read and compare everything
veriarchive-cli verify --full <archive_uuid>    # find the archive on attached disks
veriarchive-cli archive list                    # last FULL verify, last verify, OVERDUE
veriarchive-cli history <uuid>
veriarchive-cli repo verify-history             # check the history hash chain
veriarchive-cli export-sha256sums <uuid> --out sums.txt   # check later with sha256sum -c
```

More: `register --new-revision | --force-new-archive | --write-media-copy | --resume`,
`verify --full ... --resume RUN_ID`, `archive show | retire | relocate`, `export`, `import`,
`repo rebuild-index`. Every command accepts `--json`. Exit codes: 0 passed, 2 passed with
warnings, 3 failed, 4 incomplete, 10 usage, 11 repository, 12 needs an operator decision.

The repository location is resolved explicitly (`--repository`, `VERIARCHIVE_REPOSITORY`), then
from the pinned config, and only on first initialisation falls back to `<program>/data` or
`~/VeriArchive`. A configured location that is unavailable is an error, never a fallback. Back up
the repository: losing it loses the verification history.

## Graphical interface

`veriarchive-gui` offers the same operations in five views: Archive List, Register Archive,
Verify Archive, Verification History and Settings. Long operations show progress and can be
cancelled and resumed. The GUI runs unprivileged.

## Interface language

The CLI and the GUI speak English and Traditional Chinese (decision 0017): `--lang en|zh-TW`,
then `VERIARCHIVE_LANG`, then `"language"` in the config file (the GUI's Settings view sets it),
then the system's display language. `--json` output and exit codes are always English.
