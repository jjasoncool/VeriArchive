---
status: accepted
date: 2026-10-08
amends: HANDOFF §B4.3 (optional header key), §B16 (CLI)
---

# Each physical copy is its own archive; `copy_of` is display-only

Keeping the same content on several media (3-2-1) could be modelled as one archive with many copies, but that adds a copy dimension to §B10 matching, `verify_runs` and overdue policy across frozen Part B. For V1 each copy is registered as an independent archive. `register --copy-of <archive_uuid>` records an optional header key `copy_of` so the UI can group copies; it never takes part in matching, verification or results.

## Consequences

- `copy_of` lives in the manifest header, not only in SQLite, so it survives `repo rebuild-index` (R-80). It is omitted when absent, so existing golden manifests are byte-identical; a golden case with `copy_of` must be added.
- Relocation (decision 0004) carries `copy_of` forward to the new revision.
- "Only one healthy copy left" warnings need the one-archive-many-copies model and are deferred.
