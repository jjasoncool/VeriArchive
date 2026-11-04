---
status: accepted
date: 2026-11-02
amends: HANDOFF §B9 (the platform keeps the computer awake for an operation of the window, without elevation), HANDOFF §B17 (an operation of the window keeps the computer awake, and one that ends while no window of the program is active is told in a notification of the system)
---

# The window keeps the computer awake and says when an operation ended

A FULL verification of the operator's 10.1 TB archives read for 10 to 16 hours. VeriArchive asked nothing of the system's power management, and Windows decides that a computer is idle from what its user does and from the requests of programs, not from how busy its disks are: the operator kept the computer awake with a script of their own. A sleep in the middle of a verification pauses it at best; a device on USB may not come back as it was, which interrupts the run (decision 0034). Nothing told the operator, who had left the computer, when the run ended.

## Decision (operator, 2026-11-02)

1. **An operation of the window keeps the computer awake** while it runs: every operation that the window runs (`gui::Job`: a registration, a verification, a verification adding chunk hashes, a comparison, abandoning an unfinished operation, the upgrade of the repository database). The platform makes the request without elevation (R-08), as `platform::KeepAwake`:
   - Windows: a power request that the system is required (`PowerCreateRequest`, `PowerSetRequest` with `PowerRequestSystemRequired`), named by its reason, which `powercfg /requests` lists.
   - Linux: a logind inhibitor lock of `sleep` in `block` mode (`Inhibit` on the system bus, through GDBus), held by "VeriArchive" for its reason, which `systemd-inhibit --list` lists. A call that would need authentication fails at once instead of asking.
   - The reason is "A long operation is running" (長時間作業執行中), in the interface's language.
   - The display may still turn off and the session lock, which does not stop the operation. On Windows a sleep the operator asks for still happens (Sleep, the power button, a laptop's lid). On Linux logind refuses a suspend that a program asks for while the lock is held, and closing a laptop's lid still suspends it, as logind's default (`LidSwitchIgnoreInhibited`) says.
   - Where the request cannot be made (Linux without logind or a system bus, as in a container), the operation runs as before and says nothing.
2. **The window says when an operation ended.** When an operation of the window ends (finished, failed, refused, cancelled or interrupted) while no window of the program is the active one, the window sends a notification of the system titled "VeriArchive", its body the first two lines of the operation's report that hold text, as the view shows it: "FULL VERIFY PASSED" and the archive's label, uuid and revision; a registration's label and revision and what it hashed; a refusal's reason. Through GLib's `GNotification`: on Windows its notification-area backend, on Linux the desktop's notification service. A later operation's notification replaces the earlier one. While a window of the program is active there is none: its report is in front of the operator. The screenshot mode and the smoke test send none.
3. **The command line does neither.** It runs in scripts, in scheduled tasks and on servers, whose power settings decide; servers are not set to sleep. The user guides say to set a computer that sleeps when idle not to sleep before a long command, and on Linux to run it under `systemd-inhibit`.
4. No setting turns either off.
5. No change to the manifests, the database, the golden test or `--json`.

## Why

The operator chose that every long operation of the window keeps the computer awake by itself, with no setting: nothing gains from a sleep in the middle of a verification, and a setting left off would cost a run. The notification is the window's alone: the command line's report is on its output, read by the script that ran it.

The command line first held the same request around every operation that Ctrl+C cancels (R-94). An antivirus's on-access scan deleted that build of it as soon as it was written (Bitdefender's engine, `Gen:Variant.Ulise.609087`), while the window and the unit tests, built with the same request, were kept: a generic detection by the file's likeness to others, made before the program ever ran, not by what it does. The operator chose to leave the request out of the command line rather than reshape its build until no scan flags it: the command line's operations run where the power settings decide.

Rejected alternatives:

- `SetThreadExecutionState`: it holds for the calling thread and names no reason, while a power request is made by any thread and named where the system lists it.
- A notification of the program's own (a notification-area icon, a Windows toast with a registered application id): GLib's `GNotification`, which GTK already brings, shows one on both systems.
- A notification while the window is active: its report shows there, and the operator who runs a short comparison in the window would be told twice.

## Consequences

- Amended by decision 0046: a relocation and an export are operations of the window too, run as jobs (decision 0045): each keeps the computer awake while it works and is told in a notification when it ends while no window of the program is active; retiring an archive, and reading first what it would do, run as the window's step, as abandoning does.
- HANDOFF §B9: the platform offers `KeepAwake` beside the interfaces of the core; the core never uses it.
- HANDOFF §B17: the window keeps the computer awake while an operation runs, and tells an operation's end in a notification as above.
- The Linux build links GIO's `gio-unix-2.0`, which every distribution's GLib development package already holds.
- The user guides: "While an operation runs", after "Reading the progress".
- Tests: on Windows the request is held and the system's execution state requires the system while it is (`CallNtPowerInformation`, without elevation), two at once and one again after them, and no longer once the last went, unless another program holds one; on Linux the test makes and releases it, held or not, as a CI container has no system bus. The reason's words in English and in Chinese. The notification's body from sample reports: a verification's result and archive past the blank line between them, in English and in Chinese, a registration's two lines, a refusal's one, lines of spaces skipped, an empty report. That each system shows the notification, and that logind takes the lock, is reviewed, not tested: no CI runner has a desktop or a system bus. A mutation check made the request without `PowerSetRequest`, and the Windows test failed, the system no longer required; one took one line of the report, and the body's test failed.

### Interface terms (decision 0017)

| English | 繁體中文 |
|---|---|
| A long operation is running | 長時間作業執行中 |
