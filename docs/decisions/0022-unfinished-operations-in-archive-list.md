---
status: accepted
date: 2026-10-15
amends: HANDOFF §B8 (what the `settings` records of an unfinished registration or verification hold), §B17 (Archive List: the unfinished operations)
---

# The Archive List shows the registrations and verifications that have not finished

Registering an archive that is a whole volume reads every byte on it and takes many hours, and so does its FULL verification. The operator often starts them from the command line, which shows their progress only in its own window, and the GUI showed nothing of them: an archive whose first registration is under way appeared as `REGISTRATION_INCOMPLETE` with revision 0, and a verification not at all until it ended. The repository already holds what they have done: both checkpoint into it (R-104) and keep a record in `settings` so they can be resumed.

## Decision

1. **Section.** Above the archives, the Archive List view (§B17) shows the registrations and verifications that have not finished, wherever they were started (this window, another one, the command line): label, root, operation, progress and state, one row per operation. The section is hidden when there are none. It scrolls when it holds more than about three rows, so it never pushes the archives out of the window. The operations that run, or may, come first, then those that stopped; the newest first within each. When an operation ends or begins (its record is gone or new, or it was recorded as stopped), the archives are read again as well.
2. **Reading.** While the view is shown, every 5 seconds, on a thread of its own, through a read-only connection: watching never creates a database (a missing one is an error) and never writes to one. A read that took long spaces out the next ones, so reading keeps the database busy at most a tenth of the time. In WAL mode (R-91) one read transaction gives a consistent picture. In the rollback-journal mode, which a database keeps where WAL cannot be turned on, each query reads on its own: there a long read would keep the operations from committing their checkpoints. A verification recorded `INCOMPLETE` never changes, so its checkpoints are counted once.
3. **Progress.** Done is what the checkpoints hold: for a registration the files indexed under the revision being created, for a verification the entries in `verify_progress`, and their bytes. The totals come from the operation's record (below). Until a registration's scan has counted them, the total of a registration whose root is a whole volume is the space in use on that volume, shown as "of about"; for records written before this decision a verification's totals are its manifest header's counts. The share counts bytes when their total is known, otherwise files. It stays below 100 % until every file is done, and an approximate total never shows 100 %. Speed and time left come from the steps of the counts (app::ProgressEstimate); they are shown only while the counts moved in the last 3 minutes, and the estimate starts afresh after a gap that is long for the operation's own pace (at least 10 minutes) or after the operation was seen stopped, so a stop is not taken for slow work. The pace is learned from every gap between steps but one in which the operation was seen stopped, and the estimate counts at least the last three steps however far apart they come (one large file each), so steps minutes apart still give a speed and a time left.
4. **State**, worded so that it claims no more than the repository shows:

   | State | When |
   |---|---|
   | `waiting for progress...` | no `updated`; first seen, and not yet seen to move |
   | `running` | its record's `updated` is at most 3 minutes old; without `updated`, its counts moved in the last 3 minutes |
   | `no progress seen for N minutes` (`hours`) `; it may have stopped` | no `updated`, and the counts have not moved for 3 minutes or more since this window first looked |
   | `stopped; it can be resumed` | a verification recorded `INCOMPLETE`, a registration whose plan says it stopped (`stopped`), or its record's `updated` is more than 3 minutes old |
   | `stopped; it cannot be resumed (the archive has a newer revision)` | a verification never recorded whose record's `updated` is more than 3 minutes old, and whose archive has another revision now (R-104); once that `updated` is 5 minutes old it is no longer listed, nor is one recorded `INCOMPLETE` (see 5) |

5. **Which verifications are listed.** Every interrupted verification keeps its record for `verify --resume`. One is not listed when nothing can finish it any more:
   - its archive has another revision now (R-104 refuses the resume), and it was recorded `INCOMPLETE` or, never recorded, its record's `updated` is 5 minutes old (its process was killed);
   - a later verification of the same archive supersedes it: one recorded after it, or one started after it was recorded, or, for a run that was never recorded (its process was killed), one that started, worked (its `updated`) or was recorded when the run's record had said nothing for 5 minutes. A run that works writes its `updated` every 30 seconds, so it is never left out this way. A run of an older version never says it works; for it only a verification started 5 minutes or more after it counts.

   The history keeps every run (R-18); `verify --resume` of a run not listed still works where R-104 allows it.
6. **The records** in `settings` (§B8) are JSON objects. Members are added, none is removed or changes its meaning, and a reader ignores members it does not know:

   | Record | Members before | Added |
   |---|---|---|
   | `registration:<archive_uuid>` (the plan) | `archive_uuid`, `revision`, `label`, `created`, `new_archive`, `root`, `copy_of` | `files_total`, `bytes_total`: after the scan, every readable file it found, those an interrupted attempt hashed included (they count as done); `updated`; `stopped`: when an attempt ended without finishing (cancelled, its device removed, a failure), written last by that attempt; the attempt that `register --resume` starts writes the plan without it |
   | `run:<run_id>` | `archive_uuid`, `revision`, `started` | `root`: where the run reads (UTF-8); `files_total`, `bytes_total`: the files and streams of the revision whose content has hashes, which are what the checkpoints count (an entry unreadable at registration has none, decision 0010); `updated` |

   `updated` is a UTC timestamp in the form of the manifest's `created` (`2026-10-15T08:30:00Z`): when the operation last worked; `stopped` has the same form. The engines write it when the record is made or taken up again by a resume, when the totals are known, and while they work at least every 60 seconds: every 30 seconds, with the progress reports, which come often enough (scanning reports at least once a second while it finds entries, hashing about five times a second, also while one large file is read). A write is one small `UPDATE` of the record and best effort: one that fails is left out, and the operation goes on. It only changes a record that exists, so it cannot bring back the record of an operation that has finished.

## Why

The operator needs to see how far an operation of many hours is, and whether it still works, without the window that started it. The Archive List is where the archives' state is shown (R-21), and an archive under registration is part of it.

The checkpoints are the only progress the repository has, and they move only when a file finishes: a large file or the scan holds them back for minutes. The state therefore says only what can be known. `updated` is the operation's own sign of life; without it (an operation of an older version), quiet minutes say only that it *may* have stopped, counted from when this window looked. A cancelled verification is recorded `INCOMPLETE` and reads as stopped at once; a cancelled registration keeps only its plan, whose `updated` is then still fresh, so the plan says itself that the attempt stopped (`stopped`).

Watching must not change the repository it watches. A read-write connection creates an empty database where the file is missing (moved or being restored), and the last one to close writes the WAL into the database file.

Every cancelled verification keeps its record for a resume, so a list of all of them grows with every Ctrl+C and pushes what runs out of sight. A finished later verification gives the archive a newer result, and a newer attempt is the one to follow.

The totals must count what the done figures count. A manifest header counts the entries unreadable at registration as files, and they are never checkpointed, so a verification never reached its total; the files reused by a resumed registration are in the checkpoints, so they belong in its total.

Rejected alternatives:

- A table for progress: the records already exist for the resume and are read by it; a table would be a schema change for display only.
- Writing the counts done into the record with every checkpoint, so a read costs nothing: the counts would have to follow the checkpoint rows exactly through re-hashed files and resumed runs. Counting the rows is exact; a read costs one pass over an operation's own checkpoints.
- Notifying the GUI from the operation (IPC): another channel between processes for what the repository already holds.
- Deleting the records of superseded runs: watching writes nothing, and the run may still be resumed by hand.

## Consequences

- No change to the database schema, the manifests or the golden test; the records gain members, and records written before this decision are still read (totals from the volume's used space or the manifest header, state from the counts).
- An operation of an older version, such as one under way when the program was updated, has no `updated`: it shows `no progress seen ...; it may have stopped` after quiet minutes, even while it works.
- A verification whose process was killed is listed as stopped until it is resumed or a later verification of its archive supersedes it; when its archive has a newer revision, until its record has said nothing for 5 minutes.
- A registration or verification whose process was killed writes nothing more: it reads as running until its `updated` is 3 minutes old. One cancelled, whose device was removed or that failed reads as stopped at once.
- A single directory listing that takes minutes (a network share, a failing device) gives no progress report, so its operation may read as stopped meanwhile.
- `updated` is the clock of the computer doing the work, compared with that of the computer showing the list; they are the same computer unless a repository is shared, and then a clock off by minutes shows the state wrong.
- Fixed along the way: a checkpoint whose commit failed left its transaction open, so the next checkpoints and the final record of the run went into it and failed. A failed checkpoint is now rolled back and its entries are tried again with the next one; the operation goes on.
- Tests: reading the records (totals, roots, a damaged record passed over), which verifications are listed (also a killed run after a restart that finished or still works, and one of an older revision once silent), the read-only open, reading in the rollback-journal mode, the counts of a stopped run read once, the state and its words in both languages, a cancelled registration read as stopped at once, 100 % only when done, the estimate after a stop and from steps 2 and 12 minutes apart, the order of the rows, the engines' records while they work and after they stop, a late write that does not bring a record back, and the rolled-back transaction.

### Interface terms (decision 0017)

| English | 繁體中文 |
|---|---|
| Unfinished operations | 未完成的作業 |
| running · waiting for progress... | 執行中 · 等待進度更新…… |
| no progress seen for N minutes; it may have stopped | 已 N 分鐘未見進度，可能已停止 |
| stopped; it can be resumed · it cannot be resumed | 已停止；可以繼續 · 無法繼續 |
