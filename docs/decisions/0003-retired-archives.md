---
status: accepted
date: 2026-10-05
defines: `archives.status = RETIRED` (HANDOFF §B8), `archive retire` (§B16)
---

# A retired archive is no longer expected, but is still recognised

HANDOFF named `RETIRED` and `archive retire` without defining them. A retired archive is excluded from `OVERDUE` (R-105) and cannot receive new revisions, but it stays in §B10 matching: if its media is attached again, verification identifies it and reports it as retired instead of falling through to `UNKNOWN`, where the operator would be offered `register` and the same media would gain a second archive identity. Its history is never deleted (R-18).

## Consequences

- Amended by decision 0028 (point 9): a retired archive whose latest manifest is lost (missing, or its first line unreadable or changed, not only an index that a rebuild repairs) is no candidate of matching; where line 1 alone says that only the index is wrong of it, matching reads that manifest whole, as one whose body has changed or whose body or entries cannot be read stands behind an index that no rebuild repairs. Without that manifest matching cannot recognise its media anyway, and as a candidate the archive stopped every registration of a new folder or volume and every relocation of the repository for good. It takes no new revision, not even in a registration over its lost manifest, which point 8 of that decision first let it take. Where an archive's latest manifest is lost and the device that held it is gone as well, the operator retires it: `archive retire` of an archive whose latest manifest is lost so that matching cannot use it says what that means and asks for `--confirm`, and waits until no registration of the archive is unfinished; a manifest changed only in its body keeps its archive a candidate of matching, which reads its first line, so its archive is retired at once, as any, and is still recognised. Retirement was reused for it rather than a state of its own: it already says what the operator declares, that the archive is no longer expected to be verified, takes no new revision and keeps its history; what it adds where the latest manifest is lost (no candidate of matching) follows from that manifest, which the file at its place shows, holds for an archive retired before its manifest was lost as well, and ends where the manifest is put back. A state of its own would be another value of `archives.status`, which every reader of the status (the lists, OVERDUE, matching, registration, relocation, abandoning, `repo rebuild-index`) would learn for what retirement already says.
