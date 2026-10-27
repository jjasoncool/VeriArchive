---
status: accepted
date: 2026-10-08
amends: HANDOFF §B3.1 (`l` on Windows), R-30
---

# Only name-surrogate reparse points are links; other reparse points are files

§B3.1 recorded "symlink, junction or other reparse point" as `l`, which hashes only a target string. Many reparse points carry real file content (WOF-compressed files from `compact /exe`, Windows Server deduplicated files, cloud placeholders), so their content would never be read and corruption would go unseen. On Windows an entry is `l` only when its reparse tag is a name surrogate (`IsReparseTagNameSurrogate`: symlinks, directory junctions, WSL symlinks). A mount-point reparse point whose target is a volume (`\??\Volume{…}`) is `SkippedMountPoint` (R-30). Every other reparse point is a regular `f` entry, opened through the normal path so the filesystem returns its logical content.

## Consequences

- Files marked for recall (`FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS`, `FILE_ATTRIBUTE_RECALL_ON_OPEN`, `FILE_ATTRIBUTE_OFFLINE`) are never opened: reading them triggers a download that writes to the volume (R-03). They are `Unreadable(other)` with detail `offline`, and are recorded at registration per decision 0010.
- Priority (operator, 2026-10-08): V1 targets data disks. Cloud placeholders and deduplication occur almost only on system volumes and Windows Server, so V1 implements the attribute guard but builds no dedicated fixtures for them. Symlink, junction and volume-mount-point classification are tested.
- How Linux drivers present non-surrogate reparse points is established in M2/M3, not assumed.
- A mount point below the root (R-30, on Windows a volume mount point as above, on Linux a mount in `/proc/self/mountinfo`) is reported at every verification as well (2026-10-27), as R-30 and §A6 say, where only registration listed it: never descended, it is an item `SkippedMountPoint`. Where the revision records nothing at or below it (it was there at registration, which skipped it, or it was mounted where nothing was recorded), it holds no part of the archive: the item says "another volume is mounted here; it is not part of the archive" and changes no result, as every registered byte was still read. Where recorded entries lie below it, or at its own path (a recorded empty folder with a volume mounted on it), each of them is `SkippedMountPoint`, "a mount point now covers this path", and the run fails, as it did for entries below it. Decision 0026's `SkippedMountPoint` that ends a relocation is this failing form. The operator kept the first form as built (2026-10-27, decision 0034): such a verification stays `PASSED` and lists the folder.
