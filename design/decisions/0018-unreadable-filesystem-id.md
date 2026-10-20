---
status: accepted
date: 2026-10-14
amends: HANDOFF §B10 (identity decision table)
---

# A filesystem id that cannot be read falls back to the device and the volume size

§B10's table has rows for a `filesystem_uuid` that matches and one that differs, but none for a `filesystem_uuid` that cannot be read. On Linux the id comes from the udev database or `/dev/disk/by-uuid`; containers and minimal systems have neither, while the device serial is still readable from sysfs. The matcher treated the missing id as "differs", so with a matching serial every such volume was `MISMATCH` ("reformatted"): each verification failed, and R-97 no longer refused a second registration of the same root. The GitLab Linux CI jobs showed it.

## Decision

Two rows are added to the table:

| filesystem_uuid | device serial / wwn | volume capacity | Result |
|---|---|---|---|
| unavailable on either side | match | within 1 % | `MATCH_WEAK`, detail `filesystem-id-unavailable` |
| unavailable on either side | match | differs by more than 1 % | `MISMATCH` (another filesystem on a known device, e.g. a second partition) |

With no filesystem id and no matching device the result stays `UNKNOWN`.

## Why

Identity matching finds which archive the media is; whether the data is intact is decided by reading every registered byte. A disk that was in fact reformatted still fails the content comparison. The new row mirrors the existing one (`filesystem_uuid` match, serial unavailable → `MATCH_WEAK`): one strong piece of evidence present, the other missing. `MATCH_WEAK` caps the run at `PASSED_WITH_WARNINGS`, and the warning says that the filesystem id could not be read, so the weaker evidence is visible.

Treating the missing id as a difference made the program unusable where udev data is absent: every run failed for a reason that was not true. Refusing to work without the id was the other option; it would also have kept the Linux CI from testing registration and verification.

## Consequences

- The volume capacity is the only evidence about the filesystem in these rows. Two partitions of the same size on one device, both without readable ids, cannot be told apart: registration of the second asks for `--force-new-archive`, and verifying it against the first one's archive fails on content.
- `register` refuses a second registration of the same root again (R-97), and verification proceeds with a warning.
- The detail is recorded with the evidence (R-96) and appears in `--json` as `filesystem-id-unavailable`.
