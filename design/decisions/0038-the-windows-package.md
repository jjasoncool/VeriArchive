---
status: accepted
date: 2026-10-27
amends: HANDOFF R-121 (§B15)
---

# The Windows package updates its program folder in place

R-121 says what the Windows package holds, never how its zip is laid out. The zip held one folder named after the package, `VeriArchive-<version>-windows-x86_64/`, with `bin`, `lib` and `share` inside. The user guides tell the operator to update the program by unzipping the new version's zip over the program folder, and that worked only while the version number stayed the same: with another number the new programs land in a second folder beside the first, which has no `bin/config.json` and no `bin/data`, so on first use they start a new, empty repository, and the operator's records seem gone.

## Decision

1. The zip holds no folder of its own: `bin`, `lib` and `share` sit at its root. Unzipped into the program folder (`D:\VeriArchive`), a zip of any version replaces the programs, libraries and documents there and leaves `bin/config.json` and `bin/data` alone, which no zip holds. Its file name keeps the version: `VeriArchive-<version>-windows-x86_64.zip`.

## Why

The program folder is the unit the operator keeps: program, config file and repository (decision 0019). An update has to land in that folder whatever its version is called, and the version belongs in the name of the file downloaded, where it tells two zips apart, not in a folder that every update would add.

Rejected alternatives:

- A top-level folder without the version (`VeriArchive/`): the update then works only when the operator unzips into the folder above the program folder, which the guides would have to explain, and a zip unzipped into the program folder itself would nest a second program inside the first.
- Keeping the versioned folder and telling the operator to copy its contents over the old folder, or to move `bin/config.json` and `bin/data` into the new one: two steps where one did, and a missed step looks like lost records.

## Consequences

- The guides' installing and updating steps say to unzip into the program folder. Windows Explorer's "Extract All..." proposes a new folder named after the zip, so the guides say to type the program folder as the destination and to replace the files there.
- Files that an older version shipped and a newer one does not stay in the folder. They do no harm: the programs load the DLLs that `bin` holds by name, and every one they import is replaced, and GTK loads the image loaders that `loaders.cache` lists, which is replaced too.
- The package test (`veriarchive_package`) makes the zip as cpack makes it, checks that its name carries the version and that it holds nothing outside `bin`, `lib` and `share`, and unzips it into an installed folder that holds an older program, a config file and a repository (sample data): the program is replaced, the config file and the repository are kept, and no other folder appears.
