---
status: accepted
date: 2026-10-27
resolves: HANDOFF Part D D4, D6
amends: HANDOFF R-55 (§B3.3), R-09 and R-11 (§A4), §B9 (the comments of `IRawDeviceAccess` and `IChangeTracker`), R-107 (§B12), §B15 (repository structure), Part C (V1.1), Appendix (quick rule 9)
---

# Operator-defined exclusions, the elevated helper and compile-time pinned keys are not built

Three parts of the specification were decided against or put off without a record, and the specification still describes them as coming: the operator's own exclusions (R-55), the elevated helper (R-11, D6) and compile-time pinned keys (R-73, D4).

## Decision

1. **Operator-defined exclusions (R-55) are not part of V1** (operator, 2026-10-27). Every manifest carries the default exclusion list of R-54 (`exclusions_version` 1), matched at the archive root only; no command, view or config key adds to it. A folder that holds something that should not be registered is registered from a folder that holds only what should be. The header keeps `exclusions` and `exclusions_version`, so a later version can add exclusions without a format change, each change of them a new revision (R-19).

2. **There is no elevated helper.** R-11 put the operations that need elevation in a separate executable, `veriarchive-helper`, reached over local IPC, and named the NTFS USN change journal among them; D6 asked how Windows would authorise it. The test of plan item 6-1 (2026-10-16, Windows 11 build 26100, an administrator's account with its UAC-filtered token, at medium integrity, not elevated) showed that the journal is read without elevation:
   - A handle to the volume's root folder (`C:\`), opened with no access rights or with `FILE_READ_ATTRIBUTES`, and `FILE_FLAG_BACKUP_SEMANTICS`, takes `FSCTL_QUERY_USN_JOURNAL`, `FSCTL_READ_UNPRIVILEGED_USN_JOURNAL` and `FSCTL_READ_FILE_USN_DATA`. A handle to the volume itself (`\\.\C:`) cannot read the journal: it fails with `ERROR_INVALID_FUNCTION`, or with access denied where read access is asked. The privileged `FSCTL_READ_USN_JOURNAL` and `FSCTL_ENUM_USN_DATA` are denied, and not needed.
   - The records of the unprivileged read carry no file names, only file reference numbers, which the tracker matches to entries itself; they include changes to files the account cannot read.
   - A lost continuity shows: a read from a cursor older than the journal's oldest record fails with `ERROR_JOURNAL_ENTRY_DELETED`, and a read that names the id of a journal since replaced fails with `ERROR_INVALID_PARAMETER`, the two cases of R-107's `ContinuityLost`.

   So the Windows USN change tracker (R-107) runs in the main process, unprivileged (R-08), and by plan item 7-1, which made the helper only for a journal that needed elevation, nothing else asks for one: no `veriarchive-helper`, no IPC, no `src/helper/`, no polkit, and D6 has nothing to authorise.
   - Reads of files that the operator's own account cannot read (`SeBackupPrivilege`, `CAP_DAC_READ_SEARCH`) are put off until an archive needs them (plan item 7-4). Until then such a file is recorded as unreadable, never skipped (R-10, decision 0010).
   - Raw device reads (`\\.\PhysicalDriveN`, `/dev/sdX`) serve only the whole-disk fingerprint of V2 (§B14), which decides how they are made when it is designed.
   - A call that turns out to need elevation (R-09) is left out, not worked around, until a decision record says how it is made.
   - `IRawDeviceAccess` and `IChangeTracker` stay as §B9 defines them, so core still depends on interfaces only; the V1 stubs say only that they are not available. R-12 stands: the main process never elevates itself, and there is no helper to launch elevated.

3. **No compile-time pinned keys until signing is built** (R-73; D4 answered with its default, off). No build embeds a key list, and no option such as `VERIARCHIVE_PINNED_KEYS` exists. Signing (V1.1, R-70 to R-75) brings the trusted keys of the config file (R-72); whether hardened builds should also embed keys is asked again when signing is designed.

## Why

- Exclusions: the operator decided that V1 does without them. What should stay out of an archive stays out by registering a folder that does not hold it, and a list of exceptions inside each manifest would be one more thing to set, show and get wrong.
- The helper existed for the operations that need elevation, and the one that a planned feature needs, reading the change journal, does not. A second executable, local IPC and an authorisation prompt would add what R-12 warns of, a program that runs elevated, for no feature that is planned; the one that would need it, reading files the account cannot read, waits until an archive needs it.
- Pinned keys harden a build against someone who edits the trusted keys in the config file. With no signatures yet there is nothing for them to protect, and signing is planned for personal use (plan item 3-4), with the trust list in the program folder's config file (plan item 3-7).

## Consequences

- HANDOFF carries a note at R-55, at §A4, §B9, R-107, §B15's repository structure, Part C's V1.1 and the quick rules, and Part D marks D4 and D6 as resolved.
- CLAUDE.md, which copies the quick rules, still says that elevated operations belong behind V1.1 helper interfaces (rule 9); read it with this record.
- V1.1's plan loses stage 7 except item 7-4, which waits for an archive that needs it.
