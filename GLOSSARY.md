# VeriArchive

VeriArchive records integrity evidence for archived file trees and later proves whether they were modified, corrupted, deleted, or added to. Contracts live in `design/HANDOFF.md`; this file only fixes the words.

## Language

### What is being protected

**Archive**:
One registered file tree, rooted at a volume root or at a directory on exactly one volume, identified by its archive UUID.
_Avoid_: backup, disk, media (when the file tree is meant)

**Archive root**:
The directory where an archive's file tree starts; recorded relative to its volume as the root subpath. Archive roots on one volume never overlap.
_Avoid_: mount point (a mount point is runtime-only)

**Retired archive**:
An archive the operator no longer expects to verify; it takes no new revision and keeps its history. It is still recognised when its media reappears, unless its latest manifest is lost so that identity matching cannot use it (not only in its body): then it is no longer taken for a possible owner of a folder or volume. In the interface: 停用 (to retire), 已停用 (retired).
_Avoid_: deleted, removed, archived (every archive is one), disabled archive

**Carrier**:
The media an archive currently lives on. An archive has exactly one carrier at a time; earlier carriers are its previous carriers.
_Avoid_: location, disk

**Relocation**:
Moving an archive to a new carrier while keeping its identity and history; accepted only when every entry on the new carrier matches the latest revision, and it produces the next revision. VeriArchive copies nothing: the operator copies the files, and the relocation reads the new carrier in full. In the interface: 搬移.
_Avoid_: migration (reserved for the same filesystem seen through different hardware), re-registration, move as its name (VeriArchive moves no file)

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

**Chunk**:
A part of a file larger than its archive's chunk size (64 MiB): its bytes from a multiple of the chunk size, hashed on its own in the read that hashes the whole file, so that verification can name where a file differs. A file no larger than the chunk size is one chunk, the whole file, and has no chunk lines. In the interface: 區塊.
_Avoid_: block (a unit of the filesystem or the device), segment

**Merkle root**:
The one hash over a revision's content hashes in canonical order (the chunks of its larger files, its other files, links and streams), recorded in the manifest header as `merkle_root`; it ties every chunk hash to its place in the revision. In the interface: 總碼; in technical output (manifest messages, the header field): Merkle 根雜湊.
_Avoid_: root hash (the archive root is something else), tree hash, top hash

### Where it lives

**Device**:
A physical storage device (disk, SSD), known by model, serial, and WWN.
_Avoid_: drive, disk (ambiguous with volume)

**Enclosure**:
The USB bridge or dock between a device and the computer. The system may report the enclosure's serial in place of the device's own, so the same device in another enclosure is seen with another serial while its filesystem stays the same. In the interface: 外接盒; the serial it reports, 外接盒的序號 (enclosure serial).
_Avoid_: case, adapter, drive (an enclosure is no device)

**Device's own serial**:
The serial that a device itself holds, as its label shows it, where its enclosure reports another in its place. Through such an enclosure it is read only with administrator permission, by the device identity reader. Once recorded, with a registration or with the FULL verification that passes after a change of enclosure, it is the archive's confirmed device (已確認的裝置), which tells that device from a sector-by-sector clone when the enclosure changes again. In the interface: 裝置本身的序號.
_Avoid_: drive serial, disk serial, hardware serial, real serial (the enclosure's serial is a real one too)

**Device identity reader**:
The small program of VeriArchive that reads a device's own model, serial and WWN behind its enclosure, with the administrator permission that the operator gives it each time it is asked; only the window starts it, on the operator's request. It reads nothing of the device's content and writes nothing to the device. Only on Windows. In the interface: 裝置身分讀取程式.
_Avoid_: driver (it installs nothing), service (it runs for one read and ends), administrator tool

**Operator confirmation**:
The operator's word that a carrier seen behind another enclosure holds the archive's device, given without the device's own serial matching the one confirmed before: without reading it, or after reading it where the archive has no confirmed device to compare it with. It is recorded in the history with the FULL verification it starts, whatever that verification's result; a sector-by-sector clone passes that verification too, so it cannot catch a mistaken confirmation. In the interface: 由您確認 (the answer that gives it without a read: 不讀序號，自行確認).
_Avoid_: override, acknowledgement, manual verification (it verifies nothing; the FULL verification it starts does)

**Volume**:
A filesystem instance on one or more devices, known by its filesystem UUID.
_Avoid_: partition, drive letter

**Media**:
A volume together with its devices, as presented to a machine when attached. Used in prose and UI only; identity is always stated as device, volume, or archive.
_Avoid_: using it as a synonym for archive

**Media evidence**:
One observation of media at register or verify time, compared against registered archives to decide which archive is present.

**Attached**:
Of a device or volume: present on this computer now, as the system lists it. Of an archive: on an attached volume whose identity matches the archive's carrier, at its root there. A runtime state, read again whenever it is shown, and never identity: nothing records it, and an archive that is not attached keeps its identity and history. In the interface: 已連接 (not attached: 未連接).
_Avoid_: online, mounted (a volume may be attached without a mount point), present (matching's results say which archive is present), connected archive

**Device alert**:
A sign that a device may be failing, told beside a run result and never changing it: the device reports its own health as failing, or a FULL verification read the archive markedly slower than the last usual one on that device. It stays marked beside the archive's last verification until a later one gives none. In the interface: 裝置警訊.
_Avoid_: warning (a warning caps the run result at PASSED_WITH_WARNINGS), device error

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

**Adding chunk hashes**:
A FULL verification of an archive whose current revision has no chunk size, as V1 registered it, that also hashes the chunks of its files in the same read and, when every entry matched, records the next revision: the same entries, with their chunk hashes and Merkle root. Otherwise it records the verification alone. In the interface: 加入區塊雜湊 (`verify --full <uuid> --add-chunk-hashes`).
_Avoid_: rechunking, upgrading (a repository's schema is upgraded), re-registration (a registration takes a new baseline from what it reads)

**Abandoned verification**:
An unfinished verification that adds chunk hashes, given up on purpose: its record and saved progress, with the chunk hashes kept with it, are removed, so it can never be resumed, while the archive, its revisions and history stay. A stopped verification that adds none is not abandoned: a later verification supersedes it.
_Avoid_: cancelled verification (a cancelled one can be resumed)

**Comparison**:
Confirming from their manifests alone that two archives' current revisions hold the same entries with the same content, such as a copy against its original; no media is read, so it says nothing about what the media hold now. In the window: Cross-check, 核對完整性 (decision 0049), whose report lists each archive's last FULL verification beside it; the command line's is `archive compare`.
_Avoid_: diff, verification

**Export**:
Writing the manifest of an archive's latest revision, with its identity file, into a folder outside the repository, from which an import takes it into this repository or another. It copies a baseline, never the archive's files. In the interface: 匯出.
_Avoid_: backup (VeriArchive makes none), save, report (a report says what a run found)

**Verification report**:
A file that says what one verification run found, as its result page says it, with the archive's runs and the state of the history chain when it was made. Only a run that reached a result has one. It says what the repository recorded when it was made and is not signed: the repository stays the record. In the interface: 驗證報告.
_Avoid_: export (an export copies a baseline), certificate (it certifies nothing and is not signed)

**Unfinished operation**:
A registration, relocation or verification recorded in the repository that has not finished, wherever it was started (the window or the command line): still running, stopped and resumable, or one that can no longer finish and waits to be abandoned. It lives in the repository, not in a program. In the interface: 未完成的作業.
_Avoid_: pending job (a job is the window's), interrupted run (a run is one verification), incomplete operation (INCOMPLETE is a run result)

**Job**:
An operation that the window runs in the background: a registration, verification, comparison, relocation or export started there, shown with its state, progress and result until it is cleared. Jobs that read the same device wait for one another. Stopping a job ends its operation; a stopped registration, verification or relocation stays an unfinished operation, which can be resumed. The job itself is gone when the window closes. In the interface: 工作.
_Avoid_: task, activity, process (a process runs the program), unfinished operation (it stays in the repository after the job)

### In the window

**Archives**:
The window's page of the archives: their unfinished operations, the list of archives and the details of the one chosen, from which an archive is verified, relocated, exported and retired. In the interface: 封存.
_Avoid_: Archive List (封存列表), the page's name before

**History**:
The window's page of the recorded verifications of every archive, newest first, or of the archive chosen, with the check of the history chain. In the interface: 歷程.
_Avoid_: Verification History (驗證歷程), the page's name before; log
