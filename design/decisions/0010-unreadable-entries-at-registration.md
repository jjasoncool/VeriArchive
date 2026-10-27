---
status: accepted
date: 2026-10-08
amends: HANDOFF §B4.4 (entry keys), §A6 (item status, run result), decision 0004
---

# Entries unreadable at registration are recorded without hashes and kept distinct from new damage

R-10 forbids silently skipping unreadable files, but §B4.4 required `b3`/`sha256` on every file entry, so a manifest could not say "exists, could not be read". Refusing registration would leave a disk with one bad sector (the typical old archive disk) entirely unprotected. Instead, an entry that cannot be read at registration is written with the optional key `unreadable` (`"permission"` | `"media"` | `"other"`) in place of its hashes; registration completes, lists these entries, and exits 2.

| At registration | At verify | Item status | Effect on run result |
|---|---|---|---|
| hashed | unreadable | `Unreadable(reason)` | `FAILED` |
| `unreadable` | unreadable | `Unreadable(reason)`, detail `known-since-registration` | at most `PASSED_WITH_WARNINGS` |
| `unreadable` | readable | `Unbaselined` (new item status) | at most `PASSED_WITH_WARNINGS`; report suggests `register --new-revision` |

Known, unchanged damage is not `FAILED`, because an archive that is always red hides new damage. It is never `PASSED`, because "100 % of registered content read and matched" would be false.

## Consequences

- §B4.4 key order becomes `…,"sha256","unreadable","mode",…`. `size` and `mtime_ns` are still recorded from the directory listing. The key is omitted when absent, so existing golden manifests are unchanged; a golden case with an unreadable entry is added.
- A directory that cannot be listed is recorded as `d` with `unreadable`; entries later found under it are `Unbaselined`, not `New`.
- `removed` is not a recorded reason: a device that disappears makes registration `REGISTRATION_INCOMPLETE` (R-104).
- (2026-10-27) Nor is it an item status: a device that disappears during a verification ends the run `INCOMPLETE` (§A6), resumable, and is never reported as an entry. So §A6's `Unreadable(removed)` is never reported; `Unreadable` gives `permission`, `media` or `other`.
- Relocation (decision 0004): entries already `unreadable` in rN do not block relocation. If present on the new carrier they are carried into r(N+1) exactly as recorded in rN, still without hashes, because a copy of a file that could not be read cannot become a trusted baseline. If missing, relocation aborts like any other `Missing`.
