---
status: accepted
date: 2026-10-08
amends: HANDOFF §B3.1 (`l` hashing), §B4.4, R-66, R-44 (names, too, are judged by the registering OS: 2026-10-27)
---

# Link targets have one canonical form; absolute targets are compared exactly only on the registering OS

§B3.1 hashed "the link target string" without fixing its form. The same Windows-written NTFS link reads differently on Linux: `ntfs3` returns the reparse PrintName with every `\` turned into `/`, and rewrites absolute junction targets into relative Linux paths (`ntfs-3g` has its own rules). A healthy disk registered on Windows would fail a Linux verify with `Modified`, and R-66 golden manifests could not be byte-identical.

The canonical target is UTF-8. On Windows it is the reparse PrintName (or the SubstituteName without its `\??\` prefix when PrintName is empty) with `\` replaced by `/`. On Linux it is the `readlink` result unchanged (`\` is an ordinary character there, R-40).

When the verifying OS equals the revision's `created_by.os`, canonical targets must be byte-identical, else `Modified`. On a different OS, relative targets must still be identical, else `Modified`; when either side is absolute (`/…`, `X:/…`, `//…`), a difference is `MetadataMismatch` with detail `link-target-translated`.

## Consequences

- R-66 golden fixtures use relative links only; absolute links get their own cross-OS test.
- Under `--strict-metadata` a translated absolute target fails like any other metadata difference.
- The manifest stores only the hash of a link target, so registration marks an absolute target with `warn:["link-absolute"]`; at verify time that flag tells whether the recorded side was absolute.
- Names (2026-10-27, R-44): a recorded entry not found now is `Unrepresentable` (a warning), because its name cannot exist on the verifying OS, only where another OS recorded the revision (`created_by.os`). On the OS that recorded it the name existed there all the same: Windows reads from NTFS names that it lets no program create, such as reserved names (`AUX.txt`) and names that end in a dot or a space, which Linux drivers write. Such an entry gone is `Missing`, and the run fails, where it passed with a warning before. A relocation (decision 0026) ends on it either way.
- Amended by decision 0042: the registering OS, in the title, is the OS that recorded the revision, as its `created_by.os` gives it: that of its registration, or of the relocation or the verification adding chunk hashes that recorded it. A relocation records links as it read them (decision 0032, point 5), and a verification adding chunk hashes records the next revision only where every entry matched as recorded (decision 0033), so no entry it carries was read otherwise on another OS. The rules do not change; the Names bullet says so.
