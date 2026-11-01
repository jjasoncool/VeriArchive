---
status: accepted
date: 2026-11-01
amends: HANDOFF §B8 (`archives.created`: the time that the manifest of the archive's first revision gives, which a rebuild of the index reads again; the upgrade to schema version 3 sets it so)
---

# An archive is created when its first revision is

Validated manifest bytes are the source of truth, and SQLite is a rebuildable index (HANDOFF §B4, R-14): `repo rebuild-index` makes the index again from the manifests alone (decision 0036). A registration recorded a new archive as created when its plan was made (`plan.created`, as the registration began), while the manifest of its first revision says when that revision was made (`created` in its header, as the registration ended), and a rebuild of the index, like an import, takes the archive's creation from that manifest. The index therefore held a time that no manifest gives, and that a rebuild changed: an archive of the operator's whose registration took 13 hours was shown in its details at the time the registration began, while Compare Archives showed the time it ended, and a rebuild would have moved the first to the second. A unit test of the rebuild failed whenever a registration crossed into the next second.

## Decision (operator, 2026-11-01)

1. **An archive is created when its first revision is**, at the time the manifest of that revision gives. A registration that records a new archive's first revision records that time, and `ArchiveStore::update_archive` writes `created` with the rest of the record. The registration's plan keeps the time it was made, by which an attempt knows its plan (decision 0022).
2. **The upgrade makes the archives of an older repository so.** The step from schema version 2 to 3 (decision 0033), through which a V1 database of version 1 passes as well, sets each archive that has a revision to the time of its lowest revision in the index (`archive_revisions.created`, which is that revision's manifest's). An archive without a revision, whose first registration is unfinished, keeps its time. The copy kept before the upgrade keeps the times as they were (decision 0032).
3. No change to the manifests or the golden test.

## Why

The operator chose the time of the manifest: it is the one time that every place can show alike, and a rebuild of the index leaves it as it is. The time a registration began is in no manifest.

Rejected alternatives:

- Keeping the time the registration began: no rebuild can give it, so the index keeps a time the manifests do not hold, and the two views go on disagreeing.
- Writing the time the registration began into the manifest: a new header member, which changes the manifests' bytes for a time no verification needs.
- Leaving the archives of existing repositories as they are, to change only at a rebuild: the archives the operator registered would keep their two times until a rebuild, which discards the verification history (decision 0036).

## Consequences

- Amended by decision 0041: `archive show` gives an archive's registration, when its first revision's manifest was made, at the end of its registration, and no GUI view shows it. Compare Archives gives the time each compared revision was recorded, under that name, which is the archive's registration only while its first revision is the one compared. This bullet said that the archive's details (`archive show`, and the GUI) and Compare Archives show the same time for an archive.
- Upgrading a V1 repository moves the time of each registered archive to the end of its first registration, later by as long as that registration took.
- Tests: a registration that takes more than a second (its file read slowly, sample data) records its archive at its manifest's time; the upgrade of a version 1 and of a version 2 database sets an archive with revisions to its first revision's time, keeps the time of one without a revision, and leaves the copy as it was. Removing the recording, the writing of `created` or the upgrade's update makes a test fail.
