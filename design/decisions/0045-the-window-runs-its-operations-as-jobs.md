---
status: accepted
date: 2026-11-03
amends: HANDOFF §B9 (judge_attempt asks whether this process runs the very operation that a plan or record is of), HANDOFF §B17 (the window runs its registrations, verifications and comparisons as jobs, several at once, a page Jobs lists them, closing the window with jobs asks first)
---

# The window runs its operations as jobs

The window ran one operation at a time: while a FULL verification read one disk for 16 hours, nothing else could start in it, not a registration of another disk, not a comparison, which reads no media at all. The operator started a second copy of the program to read a second disk, as the command line had verified two disks at once. The operator asked that an operation become a job that runs in the background, so that several run from one window, each with its share done, a way to stop it, and its details.

## Decision (operator, 2026-11-03)

1. **Every registration, verification and comparison started in the window runs as a job of its own** (`gui::Jobs`), on a thread of its own; a Resume in the Archive List or in Verify Archive starts one too. Starting one never waits for another: Register, Verify (FULL) and Compare stay available while jobs run. Abandoning an unfinished operation, and the upgrade of the repository's database as it opens, stay one step at a time besides the jobs.
2. **Jobs that read the same device run one after another, in the order they were started; jobs of different devices run at once.** A disk read in two places at once reads both slower than one after the other, and two operations of one archive (which lies on one volume) never run together. A job's devices are told on its thread before its operation starts, and before it writes anything: each device of the volume of its root by its serial, else its WWN, else the system's name for it while it is attached, and the volume by its filesystem id; for an archive named by its uuid, the devices and volume that its latest revision records as its carrier, and the archive. A job that waits says which job reads its device ("Waiting: Register H:\ reads the same device"); one stopped while it waits ends without starting. A comparison reads no media and waits for nothing.
3. **A page Jobs**, after the Archive List, lists the jobs of the window, newest first: what each does, its state (waiting, running, stopping, or ended and its result), its progress as the view shows it (the share done, the speed and the time left), its device alert (decision 0044), **Stop** while it waits or works, and **Details**, a window of its own with the same, and once the job ended its report. "Clear ended jobs" removes the ended ones from the page. The view that started a job shows it as it showed its one operation, until it starts another; its Cancel stops that job. A stopped job is resumable as a cancelled operation is (R-104).
4. **Closing the window while jobs wait or work asks first**: "N jobs are still running. Closing the window stops them. Each can be resumed later, without reading again what it read." Keep the window open is the default; Stop them and close closes it, and the program stops every job and waits for each to record where it stopped, as it did for its one operation.
5. Each job keeps the computer awake while it works, and one that ends while no window of the program is active is told in a notification of its own (decision 0043); the language cannot be changed while a job waits or works.
6. **The program tells which of its jobs runs an operation.** A plan or a record that names this very process was, before, of an attempt that had ended unless the window ran *an* operation (judge_attempt, decision 0024). The window now asks whether a job of its own that waits or works has the root or the archive that the plan or record names (`runs_this_operation`). The Archive List judges each row so: a job stopped while others run shows stopped, with its Resume; and abandoning or resuming an operation that a job of the window works on is refused, saying to stop the job in Jobs first, as the check that abandons judges this process's attempts ended.
7. No change to the command line, the manifests, the database or `--json`.

## Why

The operator chose the queue by device, asking before closing, and a page of jobs with Stop and Details. A queue by device gives what running several disks at once gives without the cost of reading one disk twice at once, which also misleads the read-speed comparison (decision 0044). The judgement of an attempt had to learn which job runs what: with one operation per process, "this process runs an operation" was enough; with several jobs, a stopped job would read as running while another job runs, and the abandon of a registration would take a running job's attempt for one that ended.

Rejected alternatives:

- Every job at once, whatever its device: a hard disk read in two places reads both at a fraction of its speed, and the speeds recorded would read as a failing device.
- Keeping the program running in the notification area once its window is closed: a notification-area icon of its own on Windows and on Linux, for little gain over asking.
- A second copy of the program for each disk: possible all along, but each copy shows only its own operation, and two copies of one archive are not kept apart.

## Consequences

- Amended by decision 0046: a relocation and an export are jobs too, an export waiting for no device, as a comparison. Jobs is a page of the sidebar after Devices, its jobs grouped Running, Waiting and Ended, and the header of every page has the jobs button, a ring of the running jobs' progress with their number, whose popover lists the jobs that run and wait and the last three that ended. Register Archive and Compare Archives are dialogs that close once their job starts, so it is not the view that started a job that shows it: Jobs shows every job, the archive's details show a verification's or relocation's job as a card with Stop and Details, and a toast says when a registration, comparison or export started and how a job ended, leading to its details or its result.
- HANDOFF §B9: `judge_attempt(plan, processes, host, runs_this_operation)` asks of the plan or record whether this process runs its operation; `AttemptCheck::runs_this_operation` likewise.
- HANDOFF §B17: views as point 1; the page Jobs and the window of a job's details as point 3; the question on closing as point 4; the Archive List as point 6. The screenshot tour also takes `08-jobs.png`.
- The user guides: "Jobs", after "While an operation runs".
- Tests: the devices a job reads, from a volume and from an archive's recorded carrier; jobs of one device take their turns in the order they asked, others at once, one stopped while it waits gives up its place and holds nothing; which operation a job runs, from a plan's or a record's root and archive; two verifications of one repository at once, in one process, both recorded in a chain that holds; the judgement of an attempt asks about the plan judged; every new text in Chinese. That the window lists, stops and details its jobs, and asks before closing, is reviewed, and seen in the screenshot tour on sample data; two jobs of one device waiting in the window is reviewed, not tested: a sample archive is read before a second job could wait.

### Interface terms (decision 0017)

| English | 繁體中文 |
|---|---|
| Jobs | 工作 |
| Stop | 停止 |
| Details | 詳細 |
| Clear ended jobs | 清除已結束的工作 |
| Waiting: … reads the same device | 等候中：… 正在讀取同一個裝置 |
