---
status: accepted
date: 2026-11-03
amends: HANDOFF §A6 (a device alert is told beside the run result and never changes it), HANDOFF §B1.2 (a device's health, observed at runtime and never identity), HANDOFF §B8 (what a run's counts and its media evidence keep of its devices' health and of its read speed; no schema change), HANDOFF §B9 (IMediaEnumerator::device_health), HANDOFF §B16 (media list, the reports, archive list, archive show and history say device alerts; --json gives them), HANDOFF §B17 (the window shows a device alert above the progress at once, in its report and notification, and marks it in the Archive List and the Verification History)
---

# Device alerts: a device's health and a slower read

The operator keeps archives on data disks and verifies them FULL, which reads every registered byte for 10 to 16 hours. Nothing told them when a device was about to fail. A FULL verification of their second disk read at about half its speed for its first two hours, and nothing said so: the run passed, and its average over 16 hours hid it. A device that is failing often first takes longer to read, retrying a part of its surface, or reports its own state through the system before it loses data.

## Decision (operator, 2026-11-02 and 2026-11-03)

1. **A device alert** is a sign that a device may be failing: the health that the device reports of itself, or a FULL verification that read the archive markedly slower than the last usual one on that device. It is told beside the run result and never changes it: `FULL VERIFY PASSED` stays `PASSED`, the exit codes stay as they are, and a result's warnings (§A6) are something else.
2. **A device's health** is asked of every device of the volume where a registration, a verification or a relocation starts, before anything of the archive is read, and of every device that `media list` lists. Without elevation (R-08, R-09):
   - Windows: `IOCTL_STORAGE_QUERY_PROPERTY` with `StorageDeviceManagementStatus`, on a handle to the disk opened without access rights, as the enumeration's other queries are: the health and the operational status that the storage stack keeps of the disk, which `Get-PhysicalDisk` shows as `HealthStatus` and `OperationalStatus`. MinGW's headers do not define its structure; the platform reads it by its documented layout.
   - Linux: udisks2 on the system bus, which reads SMART as root and shows it to every program: `SmartFailing`, `SmartNumAttributesFailing` and `SmartNumBadSectors` of an ATA drive's `Drive.Ata`, `SmartCriticalWarning` of an NVMe controller's `NVMe.Controller`. udisks2 is not started for it.
   - It alerts when the device reports itself `Warning` or `Unhealthy`, or a condition that tells of a failure (predicting failure, a hardware error, not usable, a transient error, bad sectors, SMART attributes past their threshold, spare capacity low, temperature too high, reliability degraded, turned read-only, volatile memory backup failed); a repair or a rebuild under way does not. A device whose health cannot be read (many USB enclosures pass nothing on, a server may have no udisks2) is one quiet line, "cannot be read", and no alert.
   - It is kept with the operation's media evidence (each device's `health` in `volume_json`), and in the counts of each run that a verification or a relocation records (`device_health`), in the hash chain (R-90).
   - The window shows the alert at once, in red above the operation's progress, and the operation goes on: the operator may cancel it, or let it finish, as an unattended run does. The command line prints it on stderr at once and goes on; scripts and scheduled tasks have no one to answer.
3. **The read speed** of a FULL verification:
   - is timed by hundredths of the revision's content (the bytes of every file and stream it records with hashes, in canonical order), from the progress that the hash pipeline reports five times a second; time in which no byte was read goes to the hundredth being read, where a device that retries a read spends it. A resumed run times what it reads, in its place in the content, and a verification is its last run with the runs it resumed (R-104).
   - is kept in the run's counts (`read_speed`: the content's bytes, the device read, by its bus and its serial or WWN, or the volume's filesystem id where no device has one, and for each hundredth read its bytes and milliseconds).
   - once the verification reached a result, is compared with the latest earlier FULL verification of the archive that reached a result, read the same device and the same content (as many bytes), and was not itself slower (no comparison of it alerted). A run that failed on its content read it all the same; one that read no media is none. A new enclosure, a relocation or a new revision whose content differs starts over: its first verification is compared with nothing. The history recorded before this decision holds no speeds, so the first comparison comes with the second FULL verification after it.
   - alerts when the whole read, over the hundredths that both verifications read, is 30 % slower or more, where each spent a minute or more on them; or when a hundredth is 50 % slower or more, where each spent 10 seconds or more on it, told as stretches of such hundredths ("from 0% to 12% of the content, 55% slower", at most five named). The comparison is kept in the run's counts (`read_compared`), with the run compared with and whether it alerted.
   - The report says the read speed and, where it was compared, the other's ("Read speed: 182 MiB/s; the last FULL verification of this archive on this device read 180 MiB/s (date)"); the alert says how much slower, where, and that another program reading the device at the same time, or another device read through the same port or hub, also makes reading slower.
4. **Where alerts are told**, each line led by "⚠ ":
   - the report of the registration, verification or relocation, right below its result and archive, and each device's health among its details; the window's notification that an operation ended (decision 0043) adds every alert line to its two lines.
   - the Archive List, beside the archive's last verification, in red, until a later one gives none: "⚠ device reports a problem" for the health of the devices in the archive's latest run (an interrupted one included: it asked them as it started), "⚠ reading slower" for the read of its latest FULL verification that reached a result. `archive list` and `archive show` print the same marks below the last verification.
   - the Verification History and `history`, beside each run, after a relocation's note.
   - `--json`: `media list` gives each device's `health` (`status`, `conditions`); `register`, `verify` and `archive relocate` give `device_health`; `verify` gives `read_speed` (`bytes`, `milliseconds`) and `read_compared`; `archive list`, `archive show` and each run of `history` give `device_alerts` (`health`, `slower`).
5. No setting turns either off, and no threshold is configured.
6. No change to the manifests, the database's schema or the golden tests: the volume JSON of the media evidence and a run's counts gain members, which older versions ignore; the manifest's identity is written field by field and never holds a health.

## Why

The operator chose each part: the health checked wherever an operation starts to read, a failing device not stopping the window's operation, the speed compared by hundredths, and the Archive List keeping the mark until the next verification. A comparison by hundredths finds a stretch that an average hides: two hours at half speed in a 16-hour read change its average by about 6 %. The last *usual* verification is the baseline so that a device that stays slow keeps alerting, where a comparison with the one just before would alert once and then take the slow read for the usual one. Comparing only the same device and content keeps a new enclosure, another connection or other content from reading as a failing device. The health is the storage stack's own reading on Windows because the SMART data behind it needs elevation there.

Rejected alternatives:

- Comparing with the verification just before: a slowdown that persists would alert once.
- One average speed for the whole read: it hides a slow stretch, as above.
- SMART attributes on Windows (`IOCTL_STORAGE_PREDICT_FAILURE`, `MSFT_StorageReliabilityCounter`): they need elevation (R-08).
- WMI's `MSFT_PhysicalDisk`: the same health through COM; the IOCTL is what the enumeration already asks.
- Asking the operator in the window before reading a device that reports failing: a run left overnight would wait for the answer.
- A table of its own for the speeds: a run's counts already hold what the run says of itself, in the hash chain, and need no new schema version.
- The read speed of a relocation: it reads the new carrier, which no earlier verification read; the first FULL verification there is compared with nothing, as with any new device.

## Consequences

- Amended by decision 0046: the report's comparison names when the compared run finished, in the window and on the command line: "Read speed: 182 MiB/s; the last FULL verification of this archive on this device, which finished 2026-11-01 14:03:00 (UTC+8), read 180 MiB/s", where point 3 quotes it with "(date)" at its end. In the window an alert shows on the job's card and in Jobs, in the archive's details and as a mark of its row in Archives, on the run's result page, which also charts each hundredth's speed against the run compared with (only for a run that timed at least 1 s of reading), and among the notes of the run's row in History; Devices shows each attached device's health, asked once per device each time it reads them, through the function `media list` uses.
- HANDOFF §A6: a device alert is told beside the result and never changes it.
- HANDOFF §B1.2: `HealthStatus`, `DeviceHealth`, and `PhysicalDeviceInfo::health`, runtime only, never identity.
- HANDOFF §B8: a run's counts may hold `device_health`, `read_speed` and `read_compared`; `media_evidence.volume_json` holds each asked device's `health`.
- HANDOFF §B9: `IMediaEnumerator::device_health(const PhysicalDeviceInfo&)`, unprivileged, Unknown where nothing tells; the core's `observe_device_health`, `ReadSpeedMeter` and the comparison use it and the hash pipeline's progress, with a clock that tests replace.
- HANDOFF §B16 and §B17 as point 4.
- Tests: the Windows answer (healthy, warning with predicting failure, unhealthy, a later and an older version, too short) and udisks2's properties (ATA failing, attributes past threshold, bad sectors, SMART not read; NVMe warnings) from sample answers, on both systems; which healths alert; the media evidence and counts kept and read back, also from counts changed by hand; a registration, a verification and a relocation that ask before they read, record, and keep their result; the meter's hundredths with a clock of the test's, also of a resumed read, a file without hashes and a stopped read; the comparison's thresholds and stretches; which earlier verification is compared with; three verifications of a sample archive, the second read in twice the time; the marks of the Archive List and the history; every new text in English and Chinese; the notification's body. A mutation check let a verification that was itself slower be compared with, and three checks failed. That a real device reports a failure, that udisks2 answers on a desktop, and how the window's red line and the notification look, are reviewed, not tested: no test computer has a failing device, and no CI runner has a system bus or a desktop.

### Interface terms (decision 0017)

| English | 繁體中文 |
|---|---|
| Device alert | 裝置警訊 |
| device health | 裝置健康狀態 |
| healthy / warning / unhealthy / cannot be read | 健康 / 警告 / 不健康 / 無法讀取 |
| read speed | 讀取速度 |
| device reports a problem | 裝置回報異常 |
| reading slower | 讀取變慢 |
