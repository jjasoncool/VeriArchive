---
status: accepted
date: 2026-10-08
amends: HANDOFF §B4.3 (optional header key), §B16 (CLI)
---

# Each physical copy is its own archive; `copy_of` is display-only

Keeping the same content on several media (3-2-1) could be modelled as one archive with many copies, but that adds a copy dimension to §B10 matching, `verify_runs` and overdue policy across frozen Part B. For V1 each copy is registered as an independent archive. `register --copy-of <archive_uuid>` records an optional header key `copy_of` so the UI can group copies; it never takes part in matching, verification or results.

## Consequences

- `copy_of` lives in the manifest header, not only in SQLite, so it survives `repo rebuild-index` (R-80). It is omitted when absent, so existing golden manifests are byte-identical; a golden case with `copy_of` must be added. (2026-10-27) It is `tests/golden/golden_copy_of_r0002.vam`, the second revision of a copy of the golden manifest's archive, which pins `copy_of` directly after `supersedes` (decision 0015).
- Relocation (decision 0004) carries `copy_of` forward to the new revision; a unit test relocates a copy and reads `copy_of` in its next revision.
- Amended by decision 0028 (point 8): a registration over a lost manifest names the original as `--copy-of` names it, otherwise as the newest manifest of the archive that can be used names it, as the lost manifest's own `copy_of` is no longer known.
- "Only one healthy copy left" warnings need the one-archive-many-copies model and are deferred.
