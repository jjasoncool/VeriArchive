# VeriArchive

Cross-platform archive media integrity verification tool (work in progress).

## Current architecture baseline

- C++23 and modern target-based CMake
- Frozen `common` contracts and injected core/platform interfaces
- Windows and Linux platform backends selected by CMake
- Repository root resolution based on the executable directory, never the process working directory
- SQLite schema v1 and `repo init` / `repo info`

Media enumeration, scanning, BLAKE3/SHA-256 manifests, registration, and full verification are the next milestones.

## Build

Windows commands must run in an **MSYS2 UCRT64** shell (or an IDE terminal using that toolchain):

```sh
cmake --preset windows-ucrt64-debug
cmake --build --preset windows-ucrt64-debug
ctest --preset windows-ucrt64-debug
```

Release presets are `windows-ucrt64-release` and `linux-release`.

## CLI

```sh
veriarchive-cli repo init
veriarchive-cli repo info
veriarchive-cli --repository /trusted/existing/repository repo info
```

Fallback is used only on first initialisation and the selected location is pinned in config. An explicit or pinned location that is unavailable is an error and never falls back.