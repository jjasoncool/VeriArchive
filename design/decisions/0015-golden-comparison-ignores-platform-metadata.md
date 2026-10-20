---
status: accepted
date: 2026-10-08
amends: HANDOFF R-66, §B4.3 (position of `copy_of`)
---

# Cross-OS golden comparison also blanks `mode` and `attrs`

R-66 requires the same tree registered on Windows and on Linux to produce byte-identical manifests after blanking `created`, `created_by`, `identity` and `root_hint`. But R-53 records `mode` (POSIX bits) only where POSIX permissions exist and `attrs` (Windows attribute bits) only on Windows, so the two manifests can never be identical while those keys are present. Both are metadata compared only under `--strict-metadata`, so the cross-OS golden helper blanks them too. Content, paths, sizes, mtimes, link hashes and every other key must still match byte for byte.

## Consequences

- `copy_of` (decision 0005) sits directly after `supersedes` in the header key order.
- The unit-level golden manifest (`tests/golden/golden_r0002.vam`) has fixed input and must be byte-identical on every OS without blanking.
