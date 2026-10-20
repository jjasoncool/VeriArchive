---
status: accepted
date: 2026-10-15
amends: HANDOFF §B7.3 (the `repository` key), §B7.4 (R-83 steps 2 and 3, R-84)
---

# A pin inside the config file's folder is stored relative to that folder

VeriArchive is portable: one folder holds the program, `config.json` and the repository (`data`). §B7.4 step 3 pins the repository by writing `config.repository = target`, and `target` was always an absolute path. When the operator moves or copies the program folder (another folder, another computer, a USB device attached under another drive letter), the pin still names the old place:

- If the old place is gone, every start fails with exit 11. R-83 is right to refuse a fallback, but the operator did nothing wrong.
- If the old place still exists, the second program folder silently uses the first one's repository. Both installations then add to one verification history, and the second one stops working as soon as the first is deleted.

§B7.4 also never said what a relative `config.repository` is relative to. The code took the executable's folder, as R-84 does for `--repository`; for the user config file (`%APPDATA%\VeriArchive`, `~/.config/veriarchive`) that made the result depend on which program read the file.

## Decision

1. A relative `config.repository` is resolved against the folder of the config file that holds it, never against the current directory (R-84). For the portable `<executable_dir>/config.json` that folder is `<executable_dir>`, as before.
2. When a repository is pinned (§B7.4 step 3, or `repo init --repository P` with nothing pinned yet), the config file that receives the pin is chosen first. If RepositoryRoot lies inside that file's folder, the pin is written relative to it, with forward slashes: the first run of a portable install writes `"repository": "data"`. Otherwise the pin is absolute, as before. The folder itself does not count as inside.
3. "Inside" is decided on the path text, after `lexically_normal()`. A RepositoryRoot spelled differently from the folder (another letter case on Windows, a path through a symbolic link) counts as outside and gets the absolute pin, which is the old behaviour.
4. Pins already written are never rewritten. An absolute pin keeps working unchanged, and later writes of the config file (the `last_repository_uuid` of R-83 step 5, the interface language) keep the stored value as it is.
5. `repo info` and GUI Settings (R-85) show the resolved, absolute RepositoryRoot, never the stored relative value.
6. A relative `--repository` or `VERIARCHIVE_REPOSITORY` is unchanged: it is taken from `<executable_dir>` (R-84).

## Why

The pin is a value in the config file, and the config file is what travels with the repository. Resolving it against that file's folder keeps the folder self-contained, and gives one answer whichever program reads the file.

Only a repository inside the folder is pinned relative. A path that leaves the folder (`../VeriArchive`) or lies on another volume would follow the folder to places that may not exist on the next computer; an absolute pin there is what the operator chose.

Rejected alternatives:

- Rewriting absolute pins to relative ones at start-up: it changes an operator's file without being asked, and an absolute pin to a place outside the folder is deliberate.
- A placeholder such as `"<exe>/data"` or a `"portable": true` key: a new syntax for what a relative path already says.
- Falling back to `<executable_dir>/data` when the pinned repository is missing: R-83 forbids it, because a silent fallback hides a lost repository.

## Consequences

- A portable folder can be moved, copied, or reached under another drive letter, and it keeps its repository. Backing up the program folder also backs up RepositoryRoot (R-86).
- Copying the program folder copies the repository: both folders then carry the same `repository_uuid` and record separate verification histories from that point on. This was already true when RepositoryRoot itself was copied. Keep one of them as the working installation.
- Installations pinned before this decision keep their absolute pin. To make one portable, replace the value in `<executable_dir>/config.json` with `"data"`, or with the repository's path relative to that folder.
- A relative pin written by hand into the user config file is now taken from the user config folder instead of the executable's folder. VeriArchive itself never wrote one there before.
- The M1 repository-resolution matrix gains: a portable first run pins `"data"`; a program folder copied elsewhere (and the original removed) opens its own repository; a relative pin in the user config file follows that file and ignores the current directory; an absolute pin works and is not rewritten.
