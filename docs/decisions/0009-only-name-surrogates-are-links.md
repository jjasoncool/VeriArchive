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
