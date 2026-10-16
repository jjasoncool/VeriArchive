---
status: accepted
date: 2026-10-05
defines: `archives.status = RETIRED` (HANDOFF §B8), `archive retire` (§B16)
---

# A retired archive is no longer expected, but is still recognised

HANDOFF named `RETIRED` and `archive retire` without defining them. A retired archive is excluded from `OVERDUE` (R-105) and cannot receive new revisions, but it stays in §B10 matching: if its media is attached again, verification identifies it and reports it as retired instead of falling through to `UNKNOWN`, where the operator would be offered `register` and the same media would gain a second archive identity. Its history is never deleted (R-18).

## Consequences

- Amended by decision 0028 (point 8): a retired archive whose latest manifest is lost takes its next revision in a registration over the lost manifest, and stays retired. As a candidate of matching, its lost manifest would otherwise stop every registration of a new folder and every relocation of the repository for good.
