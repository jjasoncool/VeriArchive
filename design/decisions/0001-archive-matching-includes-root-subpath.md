---
status: accepted
date: 2026-10-05
amends: HANDOFF §B10, resolves Part D D8
---

# Archive matching includes `root_subpath`; overlapping archive roots are refused

R-30 and `ArchiveIdentity.root_subpath` already allow an archive rooted at a directory, but §B10 matched only on volume evidence (`filesystem_uuid`, device serial/wwn). Two archives on one volume (`Backups/2025`, `Backups/2026`) would therefore make every verify `AMBIGUOUS` (exit 12). We decided that directory archives are allowed, that the verify target is resolved to (volume evidence, `root_subpath`) and candidates are narrowed by `root_subpath` before the §B10 table is applied, and that registration refuses a root that equals, contains, or is contained by an existing archive's root on the same volume.

## Consequences

- A volume-root archive (`root_subpath = ""`) excludes every other archive on that volume, and vice versa.
- `AMBIGUOUS` now means what it was meant to mean: cloned media (same volume evidence and same `root_subpath` registered twice).
- Overlap is compared per `ArchivePath` component after case folding, on every filesystem type. A false overlap only blocks an unusual registration; a missed overlap (`Backups` vs `backups` on NTFS/exFAT/FAT) silently registers one tree twice. Case folding applies only to this check; manifest paths stay exactly as read (R-42).
