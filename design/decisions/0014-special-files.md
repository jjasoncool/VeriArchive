---
status: accepted
date: 2026-10-08
amends: HANDOFF §B9 (`EntryType`), §B3.1
---

# Special files are recorded as unreadable entries and never opened

§B3.1 knows regular files, directories, links and streams, but a Linux tree can also contain FIFOs, sockets and device nodes. Opening a FIFO to hash it blocks forever, and a device node would read the device, not archive content. `EntryType` gains `Special`; the scanner records such an entry as `t:"f"` with `unreadable:"other"` (decision 0010) and never opens it, so it is listed rather than silently skipped (R-10) and verification reports it as known-unreadable.
