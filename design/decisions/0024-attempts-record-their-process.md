---
status: accepted
date: 2026-10-16
resolves: HANDOFF Part D D10
amends: HANDOFF §B8 (a registration's plan names the process that runs its attempt), §B9 (`IProcessTable`), §B11.4 (`register --resume` while that process runs), §B16 (`register --resume` exits `12` while that process runs), §B17 (Archive List: the state of a registration from its process)
---

# A registration attempt records which process runs it

Decision 0023 lets two attempts of one registration run at once, as `register --resume` while an attempt still runs, or two resumes: the first to record the revision wins, and the others stop (exit 12) and record nothing. That keeps the manifest safe, but the losing attempt has read the archive for nothing, possibly for hours, and nothing told the operator whether an attempt still ran. The plan's `updated` (decision 0022) goes quiet for minutes both when the attempt's process was killed and when one read from a failing device takes that long, so the Archive List showed a killed registration as running for three minutes, and a hung one as resumable. HANDOFF D10 asked whether a resume should be refused while another attempt of its registration may still run; it needs a record of the running process, or an operator override. The operator chose the record (option c).

## Decision

1. **The record.** Every registration attempt writes into its registration's plan (the setting `registration:<archive_uuid>`, decision 0022) which process runs it, as the member `process`:

   ```json
   "process": {"id": 4242, "start": "134365891881292941", "host": "ws-01", "program": "veriarchive-cli.exe"}
   ```

   - `id`: the process id.
   - `start`: when the process started, in the operating system's own terms; compared, never shown. Windows: its creation time (`GetProcessTimes`, 100-nanosecond intervals since 1601-01-01 UTC), fixed by Windows when the process starts. Linux: the id of the boot (`/proc/sys/kernel/random/boot_id`) and the clock ticks since that boot at which the process started (`/proc/<pid>/stat`, field 22), as `<boot id>:<ticks>`; the ticks start again at every boot. It reads the same while the process runs, and differs for a later process given the same id.
   - `host`: the computer, as manifests record it (`created_by.host`): a process id means something only on the computer that gave it out.
   - `program`: the file name of the process's program, for people.

   A new registration writes the record with its plan. `register --resume` writes its own in the transaction of point 3, without the stop mark (`stopped`) of the attempt before. The heartbeat (decision 0022) repeats it with every write. When an attempt cannot read its own process, its plan names none, like a plan written by an older version.

2. **Judging an attempt.** The plan is read in this order:

   | The plan | The attempt |
   |---|---|
   | says it stopped (`stopped`, the attempt's last write) | ended, whether its process runs or not: the GUI's window goes on after its registration is cancelled |
   | names no process (a version before this decision), or a process on another computer | not known |
   | names this very process | ended (see below) |
   | names a process that no process has now, one that has ended (on Linux a zombie, on Windows an exited process whose object a handle keeps), or a process with that id but another start | ended |
   | names a process that runs, with that start | running |
   | names a process the process table cannot show | not known |

   A plan that names the process judging it is of an attempt that has ended: a process runs one registration at a time (the CLI runs one command, the GUI one operation at a time), and it judges another attempt only before it starts one (a resume) or while it runs no registration (abandoning, decision 0025, the window's one operation while it runs). This covers an attempt that the GUI cancelled but whose stop mark could not be written (the database stayed locked).

3. **`register --resume`** (the CLI's, and the GUI's resume). Before it reads anything of the media, in one write transaction, which waits for the write lock as a record does (decision 0023):
   - the plan must still be the one it found (the same revision, made at the same `created`); otherwise "there is no interrupted registration of this root to resume";
   - a plan whose revision is recorded goes, as decision 0023 says;
   - an attempt judged running keeps the registration: the resume stops with exit 12, "this registration is still running in process 4242 (veriarchive-cli.exe) on this computer; resume it once that process has ended", and changes nothing;
   - otherwise the plan names this attempt's process from then on, and the resume goes ahead at once, also when the plan said a moment ago that the attempt before worked.

   The judgement and the takeover are one transaction, so two resumes started at once on one computer cannot both go on: the second finds the first's process running. Where the attempt before is not known (a plan of an older version, another computer), the resume goes ahead as before this decision: the first attempt to record the revision wins (decision 0023), but for an attempt of a version before decision 0023, which names its manifest before it records it: finishing after the resume recorded the revision, it replaces the manifest that the resume recorded (decision 0023, consequences; decision 0022, point 7). So a plan that never says when it worked (decision 0022) and names no process, as the versions before decision 0022 write it, is taken over only once the resume is confirmed (`--confirm`, or Resume in the GUI's question; decision 0022, point 7, operator, 2026-10-20); without that, the transaction changes nothing, and the resume stops (exit 12), saying what it would take up.

4. **The Archive List** (decision 0022) judges each registration's attempt in the same way, through this computer's process table, on every read:

   | Attempt | `updated` | State |
   |---|---|---|
   | ended | any | `stopped; it can be resumed`, at once |
   | running | at most 3 minutes old | `running` |
   | running | older | `no progress reported for N minutes; its process is still running` (or hours): held up, not stopped, and not offered for a resume, which would be refused |
   | not known | | as decision 0022 |

   The window's own process counts as ended only while the window runs no operation; while it runs one, a plan naming it may be the registration it runs. Whether the window runs one is asked as such a plan is judged, after the read, not when the read starts: a read runs on a thread of its own and takes seconds on a large repository, and a registration that the window starts meanwhile writes a plan naming the window's process, which would otherwise read as stopped, with **Abandon...** (decision 0025), until the next read. A registration judged ended whose checkpoints then move under the same plan has been taken up by an older version's `register --resume`, which writes no plan: it runs again, judged by its counts, as one taken up after a stop mark (decision 0022). The state's tooltip names the process (`Process: 4242 (veriarchive-cli.exe)`).

5. **Platform interface.** §B9 gains `IProcessTable`: `current()`, this process, and `find(id)`, the process that has `id` now or nothing, both `std::expected<..., IoError>` (an error when it cannot tell). Windows: `OpenProcess` with `PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE` (only the first, and `GetExitCodeProcess`, where the second is refused), `WaitForSingleObject` with no wait, `GetProcessTimes`, `QueryFullProcessImageNameW`. Linux: `/proc/<pid>/stat` and the boot id. Both read only what the system shows an unprivileged process (R-08); the attempts are the operator's own processes. Tests use a fake table (R-93).

## Why

A process id alone is not enough: ids are given out again, on Windows soon after a process ends, and after a reboot from the start. The start time tells a later process given the same id from the attempt's, and is fixed for the life of the process. On Linux the start is counted from the boot, so the boot's id goes with it; the wall-clock start time computed from the boot time would move when the clock is set.

A process id means nothing on another computer. A repository on removable media, or one shared through `allow_network_repository`, holds plans of attempts that ran elsewhere: the record says where, and there the judgement is "not known".

Only certain refusals are added. A resume is refused only while the very process that runs the attempt is seen running on this computer; everything else goes ahead as before, and decision 0023 keeps the manifest safe where an attempt cannot be seen. The Archive List claims no more than it knows (decision 0022): a process that has ended means the attempt stopped, and a process that runs means it has not, though its plan is quiet.

The stop mark goes first because an attempt writes it last, also from a process that goes on (the GUI). A process judging a plan that names itself must not wait for itself: it cannot run that attempt while it judges.

Rejected alternatives:

- An operator override (`register --resume --force`) instead of a record: the operator chose the record. An override still could not tell a killed attempt from a working one, and taking a registration from a working attempt costs that attempt's work.
- A lock held by the running attempt (an operating-system lock on a file in the repository): a crashed process's lock is released only where the system notices, which on network filesystems may take long or never; it adds a second place, beside the plan, that attempts must agree on, and the Archive List, which only reads (decision 0022), could not test it without taking it.
- Judging by `updated` alone, with a longer wait: a read from a failing device can stall an attempt for longer than any wait, and a killed attempt would read as running all that time.
- The program's path or command line in place of the start time: two runs of the same program are not told apart.
- Refusing a resume while a plan from another computer, or of an older version, said it worked in the last minutes: the operator asked that a plan without the record keep today's behaviour, and the plan's `updated` cannot tell a hung attempt from a killed one.

## Consequences

- Amended by decision 0030: the transaction of point 3 refuses a plan made for an older revision than its archive has now, as after an import of a later revision, once the plan is still the one it found and before it judges the attempt before: such a registration can no longer finish, and the resume stops (exit 12), changes nothing and names only the abandon: "the unfinished registration of archive U (root "R") would create revision N, but the archive now has a later revision (M); the registration can no longer finish: abandon it with `archive abandon-registration U`".
- Amended by decision 0022 (point 7, operator, 2026-10-17): the Archive List offers **Resume** and **Abandon...** on the row of a registration whose plan names no process and never says when it worked (an older version's) once it reads `no progress seen ...; it may have stopped`. Its resume goes ahead as for any plan that names no process (point 3). Such a plan is of a version before decision 0023 as well, which names its manifest before it records it (decision 0023, consequences): where that version still runs the registration and finishes after the resume recorded the revision, it replaces the manifest that the resume recorded, and every later verification of the archive fails on that manifest. So the row's Resume asks first, saying so, and resumes only once the operator chose Resume there (decision 0022, point 7; operator, 2026-10-20).
- Amended by decision 0022 (point 7, operator, 2026-10-20): the transaction of point 3 takes over a plan that never says when it worked (no `updated`) and names no process only once the resume is confirmed, asked last: a plan whose revision is recorded still goes (decision 0023), and one that is no longer the one the resume read, is made for an older revision (decision 0030), is over the lost manifest of an archive retired since (decision 0028) or is run by a process on this computer is refused as before; unconfirmed, it changes nothing, and `register <root> --resume` stops (exit 12), naming the registration and saying what happens where the older version still runs it, until it is run again with `--confirm`. The GUI's resumes carry the confirmation once the operator chose Resume in its question. A plan that says when it worked or names a process, also one of another computer, goes ahead as before; one that names a process but never says when it worked is of a version since this decision whose writes of `updated` all failed.
- Amended by decision 0022 (point 7, operator, 2026-10-17): the Archive List offers **Resume** and **Abandon...** on the row of a registration whose plan names no process and never says when it worked (an older version's) once it reads `no progress seen ...; it may have stopped`. Its resume goes ahead as for any plan that names no process (point 3). Such a plan is of a version before decision 0023 as well, which names its manifest before it records it (decision 0023, consequences): where that version still runs the registration and finishes after the resume recorded the revision, it replaces the manifest that the resume recorded, and every later verification of the archive fails on that manifest. Whether the row keeps Resume is an operator decision still open (decision 0022, point 7).
- Amended by decision 0033: the record of a FULL verify that adds chunk hashes names its process, which its resume, its abandoning and the Archive List judge as a registration's plan is judged, a run recorded `INCOMPLETE` having stopped; other verifications still name none.
- §B8: a registration's plan gains `process`; no schema change, and the manifests and the golden test are unchanged. A reader ignores members it does not know (decision 0022), so older versions read these plans.
- §B9: `IProcessTable`, with `platform::make_process_table()`, implemented on Windows and Linux; the composition roots pass it to registration (`RegisterServices::processes`) and to the Archive List's reads.
- A process that hangs keeps its registration: the resume says which process it is, and the operator ends it (Task Manager, `kill`) and resumes, or abandons the registration (decision 0025).
- Versions mixed on one registration: a plan written before this decision names no process, and a resume goes ahead as before. An attempt of the version before this decision that resumes a plan written by this version keeps the process record of the attempt before it in the plan, so this version takes it for ended once that process has ended. Decision 0023's advice stands: do not resume a registration with an older version while an attempt of it may still run, in either direction.
- A repository shared between computers: an attempt on another computer is not known, and is judged as before this decision, by first to record and by `updated`.
- In Linux containers with a process namespace of their own, a process of another container cannot be seen; with the same host name it reads as ended, and a resume goes ahead as before this decision.
- On Windows, a process whose owner refuses `SYNCHRONIZE` is asked for its exit code instead; one that ended with exit code 259 (`STILL_ACTIVE`) then reads as running while its object lives.
- Verifications are unchanged: their records (`run:<run_id>`) name no process, and `verify --resume` of a run that still works stays as it was.
- Tests: the record of a new registration and of a resume; a resume refused while the attempt before runs (naming it, reading nothing, the plan unchanged), and going ahead once its process ended, when the id belongs to a later process (also after a reboot), for a plan without the record, from another computer, with a stop mark, naming this process, and when the table cannot tell; two resumes started at once on one computer, the second refused; the Archive List's states from the process (ended at once, running, held up in both languages, this window's own process, asked only for a plan naming it and as it is judged, another computer, no table, taken up after its process ended); and with this computer's real processes: this process found with the same start, an unused id ended, this process's id with another start ended. A mutation check removed each guard in turn (refusal while running, going ahead when ended, the start comparison, the stop mark first, this process's own plan, the question whether this process runs an operation, ignored or asked of every plan, the resume naming its own process, the Archive List's stopped and held-up states), and each removal failed the test meant for it.

### Interface terms (decision 0017)

| English | 繁體中文 |
|---|---|
| process | 處理程序 |
| this registration is still running in process N (P) on this computer; resume it once that process has ended | 此註冊仍在這台電腦的處理程序 N（P）中執行；請等該處理程序結束後再繼續 |
| no progress reported for N minutes; its process is still running | 已 N 分鐘未回報進度；其處理程序仍在執行 |
| Process: N (P) | 處理程序：N（P） |
