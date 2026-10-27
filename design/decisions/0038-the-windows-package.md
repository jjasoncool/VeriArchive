---
status: accepted
date: 2026-10-27
amends: HANDOFF R-121 (§B15)
---

# The Windows package updates its program folder in place and has its own README

R-121 says what the Windows package holds for the programs to run, not how its zip is laid out or what it holds for the person who unzips it.

The zip held one folder named after the package, `VeriArchive-<version>-windows-x86_64/`, with `bin`, `lib` and `share` inside. The user guides tell the operator to update the program by unzipping the new version's zip over the program folder, and that worked only while the version number stayed the same: with another number the new programs land in a second folder beside the first, which has no `bin/config.json` and no `bin/data`, so on first use they start a new, empty repository, and the operator's records seem gone.

Beside the guides, the package held the repository's `README.md`, which is written for developers: it explains the build, and its links point to files the package does not hold (`docs/`, `design/`, `GLOSSARY.md`).

## Decision

1. The zip holds no folder of its own: `bin`, `lib` and `share` sit at its root. Unzipped into the program folder (`D:\VeriArchive`), a zip of any version replaces the programs, libraries and documents there and leaves `bin/config.json` and `bin/data` alone, which no zip holds. Its file name keeps the version: `VeriArchive-<version>-windows-x86_64.zip`.
2. The package holds a short README of its own beside the guides, in English (`share/doc/VeriArchive/README.md`) and Traditional Chinese (`README.zh-TW.md`), made from `packaging/windows/package-readme.md` and `package-readme.zh-TW.md`: what the program is, how to start it, the guides beside it, where the records are and how to update the program. The repository's `README.md` is not installed. `NOTICE.txt` keeps the notices and points to the README.

## Why

The program folder is the unit the operator keeps: program, config file and repository (decision 0019). An update has to land in that folder whatever its version is called, and the version belongs in the name of the file downloaded, where it tells two zips apart, not in a folder that every update would add.

Someone who unzips the package and looks for a README needs to know how to start the program and where to read on, in the guides beside it; a build description and links to files that are not there help nobody.

Rejected alternatives:

- A top-level folder without the version (`VeriArchive/`): the update then works only when the operator unzips into the folder above the program folder, which the guides would have to explain, and a zip unzipped into the program folder itself would nest a second program inside the first.
- Keeping the versioned folder and telling the operator to copy its contents over the old folder, or to move `bin/config.json` and `bin/data` into the new one: two steps where one did, and a missed step looks like lost records.
- No README in the package: the guides are long, and the first file a person opens should say in a few lines what to start and where to read on.

## Consequences

- The guides' installing and updating steps say to unzip into the program folder. Windows Explorer's "Extract All..." proposes a new folder named after the zip, so the guides say to type the program folder as the destination and to replace the files there.
- Files that an older version shipped and a newer one does not stay in the folder. They do no harm: the programs load the DLLs that `bin` holds by name, and every one they import is replaced, and GTK loads the image loaders that `loaders.cache` lists, which is replaced too.
- Only the Windows package holds the README: its paths and steps are those of Windows. An install on Linux, built from source, has the guides and the repository at hand.
- The package test (`veriarchive_package`) makes the zip as cpack makes it, checks that its name carries the version and that it holds nothing outside `bin`, `lib` and `share`, and unzips it into an installed folder that holds an older program, a config file and a repository (sample data): the program is replaced, the config file and the repository are kept, and no other folder appears. It checks that each installed README points to both guides and that everything it links to is in the package.
