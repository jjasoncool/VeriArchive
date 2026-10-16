---
status: accepted
date: 2026-10-16
resolves: HANDOFF Part D D2
amends: HANDOFF §B4.1 (R-61: the chunk lines' side file), §B4.3 (`hash.chunk_size`, `counts.chunks`, `merkle_root`), §B4.4 (which files have chunk lines; how a chunk line finds its file; strict reading), §B6 (which entries give leaves)
---

# Chunk lines and the Merkle root

V1 hashes every file whole. A FULL verify that finds a 200 GB disk image `Modified` cannot say where, and a sample (QUICK, DEEP, §B11) needs pieces smaller than a file to read. §B4.4 and §B6 fixed the shape of the V1.1 answer, chunk lines and a Merkle root over the content hashes, and left open the chunk size (D2), which entries have chunk lines, how a reader ties a chunk line to its file when two files share a `p`, what a reader does with chunk lines that do not fit, and how the writer, which streams the entries as they come (decision 0023), puts the chunk lines after all of them. The operator decided the size, the terms and the approach; this record states them and the rules that follow, and the first stage builds the pieces: the Merkle tree, the chunk hashes in the read pass, and the writer and reader of chunk lines. The engines keep writing `chunk_size` 0 until a later stage switches them on, so a build with this stage alone behaves like V1.

## Decision

1. **64 MiB, fixed per archive (resolves D2).** Real archives use a chunk size of 64 MiB (67108864 bytes), set when the archive is registered and kept by its revisions. A FULL verify of a later stage names the chunks of a file that differ. A manifest's `hash.chunk_size` is 0 (no chunk lines: every V1 manifest) or a positive multiple of 64 KiB (65536) up to 1 GiB (1073741824); any other value makes the manifest malformed. Sizes below 64 MiB are for tests: the golden manifest with chunk lines uses 64 KiB.

2. **Which files have chunk lines.** A file entry (`t:"f"`) with a content hash (`b3`) whose `size` is larger than the chunk size. Its chunk n is its bytes from n × chunk_size, the last chunk shorter when the size is not a multiple (R-31). A file no larger than the chunk size is one chunk, the whole file, whose hash is its `b3`, and has no chunk lines (R-31). Streams (`t:"s"`), links, directories and entries without a content hash (unreadable at registration, decision 0010; special files, decision 0014) have none.

3. **One read pass.** The hash pipeline gives such a file its chunk hashes from the read that gives its BLAKE3 and SHA-256 (R-101). Chunks end at multiples of the chunk size from the start of the file, never where a buffer or a short read ends. The first chunk's hash is the file's BLAKE3 as it stands at the chunk size, so a file no larger than one chunk is not hashed twice. A file that cannot be read to its end gets no chunk hashes, as it gets no file hashes.

4. **Chunk lines** (§B4.4): `{"t":"c","p":…,"idx":…,"offset":…,"length":…,"b3":…}`, keys in that order, after all entries, in entry order and then `idx` ascending. A chunk line names its file by `p` alone, and two files can share a `p` (R-41: two names that are not valid Unicode, such as Linux names in Big5, can have the same display form). So a chunk line finds its file by position: the chunk lines are the last `counts.chunks` lines of the body, and each file with chunk lines owns the next ⌈size / chunk_size⌉ of them, `idx` 0 to n − 1, each with the file's `p`, `offset` idx × chunk_size, and as `length` the chunk size or, for the last, what is left of the file.

5. **Header.** `hash.chunk_size` is the chunk size, `counts.chunks` the number of chunk lines, and `merkle_root` the root of §B6 as 64 lowercase hex digits, present exactly when the chunk size is not 0. The writer fills in all three from what it wrote, as it fills in the other counts and the body fields; it is given the chunk size when it is made.

6. **Merkle leaves** (§B6, made exact). In entry order: the chunk hashes of a file with chunk lines, in `idx` order; the `b3` of every other file, and of every link and stream; nothing for a directory, nor for an entry without a content hash. A leaf is BLAKE3(0x00 ‖ raw32(b3)), a node BLAKE3(0x01 ‖ left ‖ right); an unpaired node is promoted unchanged; with no leaves the root is BLAKE3(0x00). The root is built while the leaves stream in, keeping one hash per level of the tree, never the leaves.

7. **Writing.** As each entry of a file with chunk lines arrives, the writer streams its chunk lines into a side file of its own, `rNNNN.vam.chunks-<uuid>` (the uuid of its body file, decision 0023), and copies them after the entries; the body hash and length cover entries and chunk lines. The side file has the body file's lifetime and leftover rules: the writer removes both when it finishes, whatever the outcome; a crashed attempt leaves them under names no other attempt uses, which never block one. The writer refuses chunk hashes where point 2 has none, a number of them that the size does not give, a hash that is not 64 lowercase hex digits, and a chunk size outside point 1.

8. **Reading, strictly.** A manifest with a chunk size is malformed when its chunk size is outside point 1; its `merkle_root` is missing or not 64 lowercase hex digits; `counts.chunks` is larger than `body.lines`; a chunk line stands among the entries, or an entry among the chunk lines; a chunk line is not canonical JSON (R-64), lacks a key, or has a `b3` that is not 64 lowercase hex digits; a chunk line of a file is missing or does not fit it (`p`, `idx`, `offset`, `length`); a chunk line is left after the last entry; or an entry's `b3` or `sha256` is not 64 lowercase hex digits, since a FULL verify compares both as they are. Content hashes that do not give `merkle_root` fail as a body that does not match its hash does: `ManifestHashMismatch`. These checks come after the hash checks (R-62, R-14), the header's too: a header changed since its revision was recorded is a changed manifest (`ManifestHashMismatch`), not a malformed one. The reader reads the chunk lines from a second position in the file while it returns the entries, gives each file its chunk hashes, and reports the end of the entries only once every chunk line has found its file and the root matches; after a failure it gives the same failure again. Each position's read is held to the stretch of the body that the reader validated for it, the entries and the chunk lines, so that the second read of the body, which the reader checks at its end (decision 0030), covers all of it; that check comes before the root's, and a chunk line that the second position read otherwise and that no longer parses is a manifest that changed while it was read, not a malformed one, as decision 0028 has it for entries. A manifest with chunk size 0 is read exactly as before: chunk lines are skipped and `merkle_root` is ignored, as §B4.3 has a verifier do.

9. **Format version 1 stays.** Every key was in §B4 already. A V1 reader skips chunk lines and ignores `merkle_root`, so it reads a manifest with chunk lines.

10. **One design for every archive.** Archives registered by V1 keep their manifests, with chunk size 0. They get chunk hashes later through a FULL verify that adds them (a later stage), not through a second format or an index of their own.

## Why

**64 MiB.** §B11 already counts with it: QUICK samples 64 chunks, about 4 GiB. A chunk is read in under half a second at HDD speed, and a 10 TB archive has at most about 150 000 chunk lines, some 25 MB of manifest. 16 MiB would point to damage four times finer for four times the chunk lines and leaves.

**Multiples of 64 KiB.** A chunk then starts at an offset aligned for uncached reads (R-100) on every sector size the pipeline's buffers serve, so a later stage can read one chunk on its own. The bound keeps a chunk a part of a file and its offsets far from overflow; 1 GiB is 16 times the chosen size.

**By position.** `p` alone cannot tell two files apart, and adding `raw` to chunk lines would change §B4.4's keys, which V1.1 must not (HANDOFF Part C). The Merkle root binds each chunk hash to its file: chunk lines moved between two files of the same `p` and size change the order of the leaves, and so the root.

**Strict reading, but not of V1 manifests.** A later stage names damaged chunks from these lines; a reader that skipped what does not fit could name the wrong chunk, or none. Every manifest written so far has chunk size 0 and is read as it was.

**A side file.** Chunk lines follow all entries, but each is known when its file's entry is written. Held in memory, they would grow with the archive; the body already streams through a side file with rules that cover attempts running at once (decision 0023).

**A Merkle mismatch is a hash mismatch.** The bytes match their body hash, but the header claims a root they do not give: the header does not describe the body, as with a body that does not match its hash (R-14).

Rejected alternatives:

- 16 MiB (D2's other option): finer pointing for four times the chunk lines and leaves.
- A chunk size of any number of bytes: chunks could not be read on their own uncached.
- A format version 2: no key is new, and V1 readers already skip chunk lines.
- `raw` (or `stream`) in chunk lines: a key §B4.4 does not have.
- Matching chunk lines to files by `p`: two files can share it.
- Holding chunk lines in memory until the end.
- Reading chunk lines leniently, skipping those that do not fit.
- Chunk hashes for archives registered by V1 in an index or a format of their own: a second mechanism beside the manifest.

## Consequences

- Amended by decision 0032: registration and relocation write chunk lines, at 64 MiB for a new archive and for the next revision of one registered by V1, at the chunk size an archive has otherwise, and at rN's in a relocation, which carries rN's chunk hashes; the index holds them (`chunks`, `archive_revisions.merkle_root`, schema version 2). So the engines no longer write chunk size 0, a relocation no longer drops the chunk lines of the revision it verifies, and a V1 program refuses a repository that V1.1 has opened. A FULL verify compares by chunk: it names the chunks of a file whose content differs and the byte where the first begins (point 1), and warns where a file's chunk hashes do not match content that matches its file hashes. Still to come: QUICK and DEEP, and the FULL verify that adds chunk hashes to an archive registered by V1 (point 10).
- The format version stays 1. A V1 build reads manifests with chunk lines (verify, export, import, rebuild-index, compare), but a revision it writes from one does not keep them: relocation copies the header, so its revision would claim a chunk size without chunk lines or `merkle_root`, which V1.1 rejects as malformed. After upgrading, only V1.1 should operate the repository.
- The V1 golden manifest (`tests/golden/golden_r0002.vam`) is unchanged. `tests/golden/golden_chunks_r0003.vam` (64 KiB chunks) is new and byte-identical on every OS; its chunk hashes, body hash and Merkle root were also checked outside the code base, with a BLAKE3 written in Python from its specification and checked against the official BLAKE3 test vectors.
- The first stage (library): `src/core/merkle`, `HashPipelineOptions::chunk_size` and `HashResult::chunks`, `Entry::chunks`, the chunk size given to `ManifestWriter`, `ManifestFailure::Kind::MerkleMismatch`; registration and relocation write chunk size 0. A later stage switches the engines on, fills the index (`chunks`, `archive_revisions.merkle_root`) and verifies by chunk.
- The reader's and writer's new messages have Traditional Chinese translations (decision 0017).
- Tests: Merkle roots of 0, 1, 2, 3, 4, 5, 7, 8 and 300 leaves computed with the same independent BLAKE3, and the builder against two references, §B6 level by level and RFC 6962's recursive split, for 0 to 300 leaves; chunk hashes at file offsets through buffers that divide the chunk size, that do not and that hold several chunks, through short reads, and without a chunk size; the golden manifest and round trips; each rule of point 8 broken in turn; the Merkle root telling apart the chunk lines of two files with the same `p` and size; two writers of one revision keeping their own chunk lines; the side files gone when the manifest cannot be written; an import, and the load of a recorded revision, of a manifest whose content hashes do not give its root failing with `ManifestHashMismatch`, and the load of a recorded manifest whose header was changed to a chunk size no manifest may have failing the same way. A mutation check bent or removed each rule in turn (57 mutations: the Merkle rules, the chunk boundaries of the read pass, which files have chunk lines and which entries give leaves, the writer's guards and side files, every check of the reader and their order, the failure classes), and each one failed the tests; its first run found one rule without a test (chunk lines that end before the last file has all of its own), which now has one.

### Interface terms (decision 0017)

| English | 繁體中文 |
|---|---|
| chunk | 區塊 |
| chunk line | 區塊行 |
| Merkle root, in the interface | 總碼 |
| Merkle root, in technical output (manifest messages, the `merkle_root` field) | Merkle 根雜湊 |
