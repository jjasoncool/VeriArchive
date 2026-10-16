# VeriArchive

VeriArchive records integrity evidence for archived file trees and later proves whether they were modified, corrupted, deleted, or added to. Contracts live in `docs/HANDOFF.md`; this file only fixes the words.

## Language

### What is being protected

**Archive**:
One registered file tree, rooted at a volume root or at a directory on exactly one volume, identified by its archive UUID.
_Avoid_: backup, disk, media (when the file tree is meant)

**Archive root**:
The directory where an archive's file tree starts; recorded relative to its volume as the root subpath. Archive roots on one volume never overlap.
_Avoid_: mount point (a mount point is runtime-only)

**Retired archive**:
An archive the operator no longer expects to verify; it takes no new revision and keeps its history. It is still recognised when its media reappears, unless its latest manifest is lost: then it is no longer taken for a possible owner of a folder or volume.
_Avoid_: deleted, removed archive

**Carrier**:
The media an archive currently lives on. An archive has exactly one carrier at a time; earlier carriers are its previous carriers.
_Avoid_: location, disk

**Relocation**:
Moving an archive to a new carrier while keeping its identity and history; accepted only when every entry on the new carrier matches the latest revision, and it produces the next revision.
_Avoid_: migration (reserved for the same filesystem seen through different hardware), re-registration

**Copy**:
Another archive registered from the same content on different media. Copies are separate archives, linked only for display.
_Avoid_: replica, mirror

**Label**:
A human-readable alias for an archive (`ARCH-0007`), assigned by the registering repository and not guaranteed unique.
_Avoid_: archive ID, name

**Revision**:
One immutable baseline of an archive, produced by a full scan and stored as exactly one manifest. A new revision always means a new full scan.
_Avoid_: version, snapshot

**Manifest**:
The `.vam` file holding one revision's header and entries; its validated bytes are the integrity truth.
_Avoid_: checksum file, index

**Lost manifest**:
The manifest of an archive's latest revision that is missing or cannot be used, and that nothing can put back: no backup of the repository, export of that revision or media copy holds it. The archive then takes its next revision from what its root holds now, in a registration over its lost manifest (`register --lost-manifest`), which compares nothing with the lost revision; where the device that held the archive is gone as well, the operator retires the archive (`archive retire --confirm`).
_Avoid_: missing manifest (a missing one may still be put back), deleted manifest

**Entry**:
One item recorded in a manifest: a file, directory, link, or alternate data stream.
_Avoid_: file (when directories, links, or streams are included)

**Link**:
An entry that only points to another name (symlink or directory junction) and has no content of its own.
_Avoid_: reparse point (many reparse points are files with real content)

**Known-unreadable entry**:
An entry recorded in a revision without content hashes because it could not be read at registration; it is never treated as verified.
_Avoid_: skipped file

**Unbaselined entry**:
An entry that is readable now but has no content hash in the revision being verified, so there is nothing to compare it against.
_Avoid_: new file (new means added since the revision)

### Where it lives

**Device**:
A physical storage device (disk, SSD, USB bridge), known by model, serial, and WWN.
_Avoid_: drive, disk (ambiguous with volume)

**Volume**:
A filesystem instance on one or more devices, known by its filesystem UUID.
_Avoid_: partition, drive letter

**Media**:
A volume together with its devices, as presented to a machine when attached. Used in prose and UI only; identity is always stated as device, volume, or archive.
_Avoid_: using it as a synonym for archive

**Media evidence**:
One observation of media at register or verify time, compared against registered archives to decide which archive is present.

### Bookkeeping

**Repository**:
The operator's store of manifests, index, and verification history (RepositoryRoot); not part of the archive.
_Avoid_: database (the database is only its index)

**Registration**:
Creating an archive's first or a new revision by scanning its root.
_Avoid_: backup, import

**Abandoned registration**:
An unfinished registration that the operator gave up on purpose, as when its media failed: its plan and saved progress are removed, so it can never be resumed, while recorded revisions, manifests and history stay. An archive that never had a revision goes with its abandoned first registration.
_Avoid_: cancelled registration (a cancelled one can be resumed), deleted registration

**Abandoned relocation**:
An unfinished relocation that the operator gave up on purpose, as when the new carrier failed: its record and saved progress are removed, so it can never be resumed, while the archive, its revisions and history stay.
_Avoid_: cancelled relocation (a cancelled one can be resumed)

**Verification**:
Re-reading an archive and comparing it against a revision's manifest; its outcome is a run result qualified by its mode.
_Avoid_: check, integrity status

**Comparison**:
Confirming from their manifests alone that two archives' current revisions hold the same entries with the same content, such as a copy against its original; no media is read, so it says nothing about what the media hold now.
_Avoid_: diff, verification
