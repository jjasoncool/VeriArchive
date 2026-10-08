---
status: accepted
date: 2026-10-08
amends: HANDOFF §B3.1 (`l` hashing), §B4.4, R-66
---

# Link targets have one canonical form; absolute targets are compared exactly only on the registering OS

§B3.1 hashed "the link target string" without fixing its form. The same Windows-written NTFS link reads differently on Linux: `ntfs3` returns the reparse PrintName with every `\` turned into `/`, and rewrites absolute junction targets into relative Linux paths (`ntfs-3g` has its own rules). A healthy disk registered on Windows would fail a Linux verify with `Modified`, and R-66 golden manifests could not be byte-identical.

The canonical target is UTF-8. On Windows it is the reparse PrintName (or the SubstituteName without its `\??\` prefix when PrintName is empty) with `\` replaced by `/`. On Linux it is the `readlink` result unchanged (`\` is an ordinary character there, R-40).

When the verifying OS equals the revision's `created_by.os`, canonical targets must be byte-identical, else `Modified`. On a different OS, relative targets must still be identical, else `Modified`; when either side is absolute (`/…`, `X:/…`, `//…`), a difference is `MetadataMismatch` with detail `link-target-translated`.

## Consequences

- R-66 golden fixtures use relative links only; absolute links get their own cross-OS test.
- Under `--strict-metadata` a translated absolute target fails like any other metadata difference.
