---
status: accepted
date: 2026-10-08
amends: HANDOFF §B4.3 (`identity` semantics), §B10 (`MIGRATED`, matching), §B16 (CLI)
---

# Changing an archive's carrier writes a new revision

The manifest header's `identity` is the only trusted record of which media holds an archive, and a manifest is immutable (V1.1: signed). Recording a new carrier in SQLite alone would make the index an integrity source (violates R-14) and would be lost by `repo rebuild-index` (R-80); HANDOFF's `MIGRATED` row ("new device evidence is recorded") had this gap and would re-prompt forever. We decided that both relocation (archive copied to a new volume) and `MIGRATED` acceptance (same filesystem, new hardware) run one operation, `archive relocate <uuid> <root_path>`: a FULL verify of the new carrier against the latest revision rN that must yield `Verified` for every entry, after which the same read pass is written out as r(N+1) with identical entry hashes and the new carrier's `identity`.

## Consequences

- Any `Modified` / `Missing` / `New` / `TypeChanged` / `Unreadable` aborts the relocation; changing content is `register --new-revision`, not relocation. `MetadataMismatch` is allowed (copying commonly changes mtimes) and r(N+1) records the new metadata.
- Matching (§B10) uses the latest revision's `identity`. Media matching only an older revision's `identity` is reported as `PREVIOUS_CARRIER` (new `media_evidence.match_result` value) rather than `UNKNOWN`, and `register` on it is refused without `--force-new-archive`. It is not verified by default.
- No header key changes, so no format version bump; `identity` now means "the carrier this revision was read from" rather than "the carrier at first registration".
- Relocation is resumable like any FULL verify (R-104).
- Amended by decision 0010: entries already unreadable in rN do not block relocation and are carried forward unchanged.
- Amended by decision 0026: a relocation keeps a record of its own (`relocation:<archive_uuid>`) and checkpoints the entries that matched; `archive relocate --resume` continues it while the archive is at rN and the new root is on the volume and in the folder it started on; a carrier that differs ends it, and `archive abandon-relocation` gives it up.
