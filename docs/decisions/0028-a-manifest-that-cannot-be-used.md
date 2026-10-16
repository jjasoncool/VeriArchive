---
status: accepted
date: 2026-10-16
amends: HANDOFF §B4.1 (R-62: line 1 of a recorded manifest is compared with the BLAKE3 recorded for it before it is parsed)
---

# What is done with a recorded manifest that cannot be used

A revision is recorded with the BLAKE3 of its manifest's line 1 and of its body, and the index keeps a copy of that line 1 (`archive_revisions.header_json`, §B8). Validated manifest bytes are the truth (R-14), so a recorded manifest can be used only while it is there and can be read, its line 1 and its body are the ones recorded, and the index's copy of its header is its line 1 (CLAUDE.md rule 3, never one in place of the other). A manifest that fails one of these cannot be used: it is gone, it cannot be read, it has changed, or the index holds its header otherwise. The operator decided how such a manifest is handled.

## Decision

1. **The first line before parsing.** Wherever the manifest of a recorded revision is read, its line 1 is compared with the BLAKE3 that the repository recorded for it before it is parsed (`ManifestReader::open` given that BLAKE3; `load_recorded_header`). A line 1 that differs is a changed manifest, whatever it holds: also one that no longer parses, one cut off before its newline, the line recorded without its newline (the body gone with it), and an empty file. Where a whole manifest is loaded (verification, `archive compare`, `export`, `export-sha256sums`, relocation), it fails with `ManifestHashMismatch`, as a body that does not match its header does (R-62), never as a malformed manifest or with `RepositoryError`; where line 1 alone is read (identity matching, the archive lists), its cause is that "the manifest differs from the repository record". A file that cannot be opened or read is no answer either way and stays a failure to read it. An import's source and `repo rebuild-index` have no record to compare line 1 with, and read it as before.

## Why

The BLAKE3 recorded for line 1 is what the repository knows of it. Other bytes are a changed manifest whether or not they still parse: a brace that a failing disk flipped, or a header cut short, says no more about its archive than a changed volume label does. Parsing first made such a change read as a defect of the manifest's format ("invalid manifest header: parse error ...", `RepositoryError`), as if the repository could not read what it had written, and hid that the manifest is not the one recorded.

Rejected alternatives:

- Parsing line 1 first and comparing its hash afterwards, as a whole manifest was loaded until now: a header changed into text that no longer parses never reaches the comparison.
- Comparing the hash in every reader of manifests: an import's source and `repo rebuild-index` read manifests that no record vouches for, where a line 1 that does not parse is what is wrong with them.

## Consequences

- No change to the manifest bytes, the database schema or the golden test.
- Core: `ManifestFailure::Kind::HeaderMismatch`; `ManifestReader::open` takes the BLAKE3 recorded for line 1; `load_recorded_manifest` gives it the record's, and fails with `ManifestHashMismatch` when line 1 differs.
- Tests: the reader given the BLAKE3 recorded opens the manifest recorded, and fails with `HeaderMismatch` for a line 1 that no longer parses, one cut off, the line recorded without its newline, an empty file and another header that parses, while a file that is gone stays an I/O failure and, without the BLAKE3, a line 1 that does not parse stays a malformed manifest; a recorded manifest whose header no longer parses, is cut off before its newline, or is empty fails `load_recorded_manifest`, `archive compare` and `export` with `ManifestHashMismatch` ("the manifest differs from the repository record"), and line 1 alone in the same words. A mutation check removed each guard in turn (the comparison before parsing, the comparison of a line 1 cut off or empty, the BLAKE3 given by `load_recorded_manifest`, its failure class, line 1 alone of a manifest cut off), and each removal failed the test meant for it.
