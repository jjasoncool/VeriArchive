---
status: accepted
date: 2026-10-27
amends: HANDOFF R-121 (§B15)
---

# The Windows package updates its program folder in place and carries its own README and notices

R-121 says what the Windows package holds for the programs to run, not how its zip is laid out or what it holds for the person who unzips it.

The zip held one folder named after the package, `VeriArchive-<version>-windows-x86_64/`, with `bin`, `lib` and `share` inside. The user guides tell the operator to update the program by unzipping the new version's zip over the program folder, and that worked only while the version number stayed the same: with another number the new programs land in a second folder beside the first, which has no `bin/config.json` and no `bin/data`, so on first use they start a new, empty repository, and the operator's records seem gone.

Beside the guides, the package held the repository's `README.md`, which is written for developers: it explains the build, and its links point to files the package does not hold (`docs/`, `design/`, `GLOSSARY.md`).

R-121's notices are the licenses of the bundled DLLs, which the install takes from MSYS2 with the files. Two libraries are compiled into the programs instead and come with no DLL: nlohmann/json, whose MIT license asks for its notice in every copy, and BLAKE3. The package held no license of either.

R-121's compiled settings schemas were checked only by the exit code of `glib-compile-schemas`. That program names a schema file it cannot open or parse, leaves it out of `gschemas.compiled`, and still exits 0; it cannot open a path of 260 characters or more, and cpack stages the package four folders below the build folder, so a package made in a deep build folder lacked two of GTK's five schemas, and nothing said so. GLib ends the program when a GTK widget asks for a schema that is missing.

## Decision

1. The zip holds no folder of its own: `bin`, `lib` and `share` sit at its root. Unzipped into the program folder (`D:\VeriArchive`), a zip of any version replaces the programs, libraries and documents there and leaves `bin/config.json` and `bin/data` alone, which no zip holds. Its file name keeps the version: `VeriArchive-<version>-windows-x86_64.zip`.
2. The package holds a short README of its own beside the guides, in English (`share/doc/VeriArchive/README.md`) and Traditional Chinese (`README.zh-TW.md`), made from `packaging/windows/package-readme.md` and `package-readme.zh-TW.md`: what the program is, how to start it, the guides beside it, where the records are and how to update the program. The repository's `README.md` is not installed. `NOTICE.txt` keeps the notices and points to the README.
3. The licenses of the code compiled into the programs are installed beside those of the DLLs: nlohmann/json's MIT license in `share/licenses/nlohmann-json/`, and BLAKE3's in `share/licenses/blake3/` (CC0 1.0, or at the user's choice the Apache License 2.0 with or without the LLVM exceptions). `NOTICE.txt` names both. The texts are those of the versions the build fetches, nlohmann/json 3.12.0 and BLAKE3 1.8.2, kept in `packaging/licenses/`; a new version of either brings its own text there. Every install holds them, on Linux too, where the same code is compiled in.
4. The install compiles a copy of the schema files in a short folder in `TEMP` and copies `gschemas.compiled` back, so that no path it gives `glib-compile-schemas` reaches 260 characters, and it fails when `glib-compile-schemas` reports a problem, also when it exits 0: that program prints nothing when it compiled every schema file, so any message counts as a failure, and the failure repeats it (`packaging/windows/compile_schemas.cmake`).

## Why

The program folder is the unit the operator keeps: program, config file and repository (decision 0019). An update has to land in that folder whatever its version is called, and the version belongs in the name of the file downloaded, where it tells two zips apart, not in a folder that every update would add.

Someone who unzips the package and looks for a README needs to know how to start the program and where to read on, in the guides beside it; a build description and links to files that are not there help nobody.

The license texts are kept in the source tree rather than taken from the fetched sources at install time: a build that finds either library installed on its system has no fetched source, and a copy of fetched sources that lost files, as the developers' cache of them did, would package no license without anyone noticing.

A package that lacks a schema still starts, and fails only on the operator's computer, when the GUI first asks for that schema; the build is the one place where the failure can be seen before that, so the build stops there. Of all the tools of the install, only `glib-compile-schemas` minds a deep folder, as CMake reaches long paths itself; compiling a copy elsewhere lets a package be made in any build folder.

Rejected alternatives:

- A top-level folder without the version (`VeriArchive/`): the update then works only when the operator unzips into the folder above the program folder, which the guides would have to explain, and a zip unzipped into the program folder itself would nest a second program inside the first.
- Keeping the versioned folder and telling the operator to copy its contents over the old folder, or to move `bin/config.json` and `bin/data` into the new one: two steps where one did, and a missed step looks like lost records.
- No README in the package: the guides are long, and the first file a person opens should say in a few lines what to start and where to read on.
- Running `glib-compile-schemas --strict`: it stops on a schema it cannot parse, but not on one it cannot open, the case of the deep build folder.
- Failing in a deep build folder and asking for a shorter one: whether a package can be made would then depend on where it is built, in build folders where every other step of the build works.

## Consequences

- The guides' installing and updating steps say to unzip into the program folder. Windows Explorer's "Extract All..." proposes a new folder named after the zip, so the guides say to type the program folder as the destination and to replace the files there.
- Files that an older version shipped and a newer one does not stay in the folder. They do no harm: the programs load the DLLs that `bin` holds by name, and every one they import is replaced, and GTK loads the image loaders that `loaders.cache` lists, which is replaced too.
- Only the Windows package holds the README: its paths and steps are those of Windows. An install on Linux, built from source, has the guides and the repository at hand.
- The package test (`veriarchive_package`) makes the zip as cpack makes it, checks that its name carries the version and that it holds nothing outside `bin`, `lib` and `share`, and unzips it into an installed folder that holds an older program, a config file and a repository (sample data): the program is replaced, the config file and the repository are kept, and no other folder appears. It checks that each installed README points to both guides and that everything it links to is in the package, and that the licenses of nlohmann/json and BLAKE3 are installed and named in `NOTICE.txt`. It compiles GTK's schema files in a folder deep enough that each of their paths has 260 characters or more, which must succeed, and then with a broken schema beside them (sample data), which must fail; and with `gsettings`, kept from every other schema folder, it lists the schemas compiled in the package (those of the zip, unzipped over the install), which must include every schema that GTK's schema files define.
