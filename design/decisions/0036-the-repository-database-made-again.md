---
status: accepted
date: 2026-10-27
amends: HANDOFF §B7.1 (R-80: `repo rebuild-index` makes a database that is missing, empty, not a database or damaged again, into a new one), §B16 (`repo rebuild-index` puts such a database aside and exits `2`, `replaced_database` in its `--json`; every command, `repo init` too, names the rebuild for it)
---

# The repository database is made again from the manifests

R-80 promises that `manifests/` and the user config are enough to rebuild every index table with `repo rebuild-index`, and both user guides gave it as the fix for a damaged database. It could not do that. The rebuild opened the database first, so where `veriarchive.db` was missing it stopped with "cannot open repository database: unable to open database file", where it held other bytes with "cannot prepare SQL: file is not a database", and where it was a file of 0 bytes with "unsupported repository schema", each exit 11; `repo init` and every other command stopped the same way. The only way out, which no document described, was to move `repository.json` aside and make a new repository, which gives the repository another uuid, and then to import each manifest.

## Decision

1. **What the database is.** Before it rebuilds, `repo rebuild-index` reads what is at `veriarchive.db` (`database_state`, read only):
   - usable: a database of this version's schema;
   - missing: no file;
   - empty: a file of 0 bytes, or a database without any table;
   - not a database: SQLite does not read the file as one ("file is not a database");
   - damaged: SQLite reads it as a damaged one ("database disk image is malformed").

   Anything else is an error that changes nothing, exit 11 as before: a database that cannot be read now (locked by another program, access denied, a disk error), and one that holds another program's tables or another version's schema ("unsupported repository schema"), such as a later version's: it may hold what this version cannot read, so nothing ever replaces it.
2. **Made again.** A database that is missing, empty, not a database or damaged is put aside and made again, in the repository's own folder: `veriarchive.db` is renamed `veriarchive.db.damaged-<UTC date and time>` (`veriarchive.db.damaged-2026-10-27T10-15-30Z`, with `-2`, `-3`, ... where that name is taken), together with the files SQLite keeps beside it (`-wal`, `-shm`, `-journal`, also where the database itself is missing), so that the new database never takes up the journal of the old one. Then a new database of this version's schema is created in its place, as `repo init` creates one, and the rebuild fills it from `manifests/` (R-80). Nothing is deleted. The repository keeps its uuid (`repository.json` is not touched), so the pin's record (R-83 step 5) and the hash chain's first link (R-90) stay right.
3. **What the manifests give back.** Every archive that `manifests/` holds a readable manifest of is listed again: its uuid; its label and root as its latest manifest records them; when its first revision was registered; its latest revision, every revision and the index. What only the database held is not given back, as R-80 says of the history tables: the verification history (each archive is listed as never verified until it is verified again, and a new hash chain starts), which archives were retired (each is registered), the plans and saved progress of unfinished registrations and relocations, and the label sequence. An archive whose manifests are all gone is not listed.
4. **Said plainly.** The rebuild says what was wrong and where the old file went, "The repository's database was damaged; it was put aside as P and a new one was made from the manifests." (or was missing, was empty, was not a database), then its usual lines, then: "The verification history was kept only in the repository's database, so the manifests cannot give it back: every archive is listed as never verified until it is verified again, an archive that was retired is listed as registered, and unfinished registrations and relocations are gone. If a backup of the repository holds veriarchive.db, put that file in place of the new one and run `repo rebuild-index` again." It exits 2, as a rebuild that skipped a manifest does: it worked, and the operator has to know. Its `--json` gives `replaced_database`: null, or `state` (`missing`, `empty`, `not_a_database`, `damaged`) and `moved_to` (null where the database was missing).
5. **Named everywhere else.** Every other command, `repo init` too, and the GUI, which open such a repository, stop as before (exit 11), now saying what is wrong and what to run: "the repository's database P is missing; make it again from the manifests with `repo rebuild-index`" (or is empty, is not a database, is damaged). `repo init` does not make the database again itself: the repository exists, and only the rebuild says what is lost.

## Why

The manifests are the truth (R-14) and the database an index: a lost index must not make a repository unusable, and the operator must not be asked to make a new repository, with a new uuid, to get the manifests listed again. The rebuild is the command that already rebuilds the index, and the guides already sent the operator there.

The old file is put aside, never deleted: a damaged database may still hold most of the history, which someone with SQLite's own tools may read, and a backup restored later replaces the new database as the message says. Its name keeps its own and says when it was put aside, so several such files can lie side by side and none is mistaken for the database.

Only the states that cannot be read as a database of this version are replaced. A locked database belongs to a running program; a database of another version's schema belongs to that version (a later version may upgrade the schema, and this one must leave such a database alone); another program's tables mean that the file is not the one this repository made, which the operator should look at. A rebuild never turns those into an empty database.

Exit 2, and the history named in so many words: a rebuild that made the database again worked, but every archive now reads as never verified, and the operator who has a backup should use it instead.

Rejected alternatives:

- Making the database again in `repo init`: init finishes a repository that a crash left unfinished (decision 0011); here the repository is complete and only its index is gone, which is the rebuild's work, and init would say nothing of the history lost.
- Asking for `--confirm` first: nothing is deleted, the database cannot be used as it is, and the rebuild's own output says what is lost and how a backup brings it back.
- Repairing a damaged database in place (SQLite's recovery): the result is a database whose history cannot be trusted, which R-18's hash chain would then contradict or hide; the old file stays for such work outside VeriArchive.
- A new repository uuid: the pin's record would warn (R-83 step 5), and nothing about the repository changed but its index.

## Consequences

- `repo rebuild-index` recovers a repository whose database is missing, empty, not a database or damaged, keeping its uuid, its manifests and everything they record; the history and what else only the database held are lost, as R-80 and R-86 say, and the rebuild says so. Backing up RepositoryRoot (R-86) stays the only way to keep the history.
- The user guides describe the rebuild as making the database again, the file put aside, and what is lost.
- Tests: the state of the database (usable; missing; 0 bytes and a database without tables, empty; sample bytes, not a database; its first page marked with a b-tree type that does not exist, damaged; another program's tables, a later schema and a locked database, errors), and what every command says of each state; the database opened for the rebuild where it cannot be used: put aside with its journal beside it, under a name that says when, made again where it was missing with a journal left beside it put aside, the uuid kept, a usable database and another program's left as they are; the rebuild into an empty database giving each archive its uuid, label, root, registration time, revisions and index from the manifests alone, registered, and the archive verifying again with a history of its own; the CLI end to end: a missing database named by `archive list` and `repo init` in either language, made again with the message and exit 2, the archive listed with its label and never verified, the repository's uuid kept, and verifying again; a database that is not one, put aside with its bytes kept, `replaced_database` in `--json`; an empty one in Chinese; a usable one not replaced. A mutation check removed each rule in turn (the replacement, the files beside the database moved, the empty database, a file that is not a database, a damaged one, the message of every command, the history named, exit 2), and each removal failed the test meant for it.
