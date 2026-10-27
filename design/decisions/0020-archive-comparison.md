---
status: accepted
date: 2026-10-15
amends: HANDOFF §B15 (repository structure), §B16 (CLI: `archive compare`), §B17 (GUI: the Compare Archives view), §B3.1 (R-50: a directory with contents is told from its descendants, in a comparison and, since 2026-10-27, in a verification)
---

# A copy is compared with its archive through their manifests

The operator backs up one disk to another with their own tools (VeriArchive makes no backups, R-01), for example disk F to disk G, and needs proof that G holds what F holds. Decision 0005 registers each copy as an archive of its own, and `copy_of` is display only. Verification therefore proves something else: a FULL verify of G compares G with G's own baseline, so it shows that G has not changed since G was registered, not that G equals F. Nothing compared two archives.

Both registrations already read every byte of their disk and recorded BLAKE3 and SHA-256 for every entry. Comparing the two manifests answers the question without reading either disk again.

## Decision

`veriarchive-cli archive compare <uuid_a> <uuid_b> [--json]` and the GUI view Compare Archives compare two registered archives. Archive A is the reference (the original), archive B the copy to check. Each archive is named as in `archive show`: a full uuid, or a unique prefix of at least 8 characters.

### What is compared

- The manifests of both archives' **current** revisions, loaded with the same checks as verification: the header and body BLAKE3 must equal the repository record, the body must match its header, and the revision must not be older than the highest known one (R-14, R-15, R-62). A manifest that fails is refused, never compared.
- Entries are matched by path (`p`, and `raw` for names that are not valid Unicode) and `stream`, the key of decision 0002. The type is not part of the key, so a file that became a directory is reported once, as `TypeChanged`, instead of as `Missing` plus `New`. Both manifests are in canonical order (R-43), so the two lists are merged in one linear pass and the differences come out in canonical order.
- **Content**: a file, link or stream is the same when both its BLAKE3 and its SHA-256 are equal. A directory has no content; being there is all.
- **Type** and **presence**: an entry recorded in one archive only is `Missing` or `New`.
- A manifest records a directory only when it is empty or could not be listed (R-49), so a directory with contents is recognised from its descendants in the other archive. File `x` in A and `x/a.txt` in B is `x` `TypeChanged` (`f -> d`) plus `x/a.txt` `New`. An empty directory `x` in A and `x/a.txt` in B is the same directory, and only `x/a.txt` `New` is reported.
- **Not compared**: metadata (`mtime_ns`, `mode`, `attrs`), because copies made by ordinary tools need not keep it and content is the integrity criterion (R-51); the media identity, because a copy lives on other media by definition; labels and the other header fields.

### Item statuses

A is the reference, so the statuses are verification's words (§A6), read as "B compared with A":

| Status | Meaning | `detail` |
|---|---|---|
| `Missing` | in A, not in B | empty |
| `New` | in B, not in A | empty |
| `Modified` | same type, different content | `size <a> -> <b>` when both sizes are known and differ, otherwise `content differs`; for a link `link target differs` |
| `TypeChanged` | a file, directory or link in one archive is another type in the other | `f -> d`, `d -> l`, … (`f` file, `d` directory, `l` link) |
| `Unbaselined` | no content hash to compare: the entry was unreadable at registration (decision 0010) in one or both archives, or it lies in one archive inside a directory the other could not list | `unreadable in A (<reason>)`, `unreadable in B (<reason>)` (reason `permission`, `media` or `other`), `unreadable in both archives`, `inside a directory unreadable in A`, `inside a directory unreadable in B` |
| `MetadataMismatch` | a link whose target differs, where the two revisions were registered on different operating systems and either side recorded the target as absolute (`warn` `link-absolute`): a driver may have translated it (decision 0008) | `link-target-translated` |

`TypeChanged` takes precedence over `Unbaselined`. An entry that is entirely absent from the other archive stays `Missing` or `New`, even when it was unreadable in its own.

### Result

| Result | Meaning | CLI exit code |
|---|---|---|
| `IDENTICAL` | every entry of A is in B with the same type and content, and B has no other entries | `0` |
| `IDENTICAL_WITH_WARNINGS` | nothing differs, but some entries are `Unbaselined` or `MetadataMismatch`: their content is not proven the same | `2` |
| `DIFFERENT` | at least one entry is `Missing`, `New`, `Modified` or `TypeChanged` | `3` |

The words are not verification's `PASSED` / `FAILED`: those claim that the medium was read (§A6, R-21), and a comparison reads none.

Comparing an archive with itself is allowed in the engine and the CLI: `IDENTICAL`, or `IDENTICAL_WITH_WARNINGS` when the archive has known-unreadable entries (each is `Unbaselined`, `unreadable in both archives`). The GUI refuses it (see below).

### Failures and exit codes

The comparison does not run, and no result is reported, when:

| Situation | `failure_class` | CLI exit code |
|---|---|---|
| a manifest whose bytes are not the recorded ones | `ManifestHashMismatch` | `3` |
| a manifest older than the highest known revision | `ManifestRevisionRollback` | `3` |
| an archive whose first registration never finished (`archive <uuid> has no revision yet`), a manifest file that cannot be read | `RepositoryError` | `3` |
| cancelled (Ctrl+C, the GUI's Cancel) | `Cancelled` | `4` |

A failure while loading names the revision: `cannot read the manifest of <label> revision <n>: <cause>`. The CLI prints `<failure_class>: <message>`, as `export` does, and for `Cancelled` the message alone (`the comparison was cancelled`). The usual codes apply otherwise: `10` for a usage error (not exactly two archives, an option the command does not take, `no archive <word>`, `several archives start with <word>`), `11` when the repository cannot be opened.

### Nothing is written

A comparison writes no `verify_runs` row, no history, no media evidence and nothing to the media. It is not a verification and does not change "Last FULL verify" or OVERDUE (R-21, R-105). Because it writes nothing and is quick, it has no checkpoint and no resume (R-104 does not apply); a cancelled comparison is simply started again.

### Progress and cancel (R-94)

The engine takes `IProgressSink&` and `CancelToken&`. Progress counts manifest entries, not bytes: `items_total` is the number of entries in A plus in B, reported once at the start, at most every 250 ms during the merge, and once at the end. Loading the manifests comes first and is the slow part (seconds for millions of entries); it reports no progress and cannot be interrupted midway, because `load_recorded_manifest` takes no token. The token is checked before each load and every 4 096 entries of the merge. Both manifests are held in memory whole, a few hundred bytes per entry each.

## What a comparison proves

It compares what each registration read, not what the disks hold today. To show that G holds the same content as F now:

1. Register F and register G after the copy is complete. Each registration reads its whole disk.
2. Compare F's archive (A) with G's archive (B): `IDENTICAL` proves that the two baselines hold the same entries with the same content.
3. FULL verify each archive: `FULL VERIFY PASSED` proves that each disk still holds its baseline.

Both reports end with a sentence that says so. If F changed after it was registered and before the copy was made, the comparison shows the change; register F again with `--new-revision` and compare again. `register <G> --copy-of <F uuid>` records the relation for display (decision 0005); the comparison does not need it.

## Output

### CLI text

The first line says what was done and its result in every language, like verification's result line. The `A:` and `B:` lines tell which archive is which, because details name the archives `A` and `B`. Then the number of entries that are the same, the count per status, one line per difference (`<status>  <path>[:<stream>]  (<detail>)`), and the scope sentence. Progress goes to stderr: `comparing: reading the manifests`, then `comparing: <done>/<total> entries`.

```text
COMPARE DIFFERENT  ARCH-0001 (99188c9d-3467-495e-98f8-f0bc7769e053) r1  vs  ARCH-0002 (94bbff52-39fa-458a-8831-9b30fb0cadf1) r2
  A: ARCH-0001, registered 2026-10-15 09:47:40 (UTC+8), 5 entries
  B: ARCH-0002, registered 2026-10-15 09:47:40 (UTC+8), 6 entries
  same: 4  Missing 1  Modified 1  New 1
  New  extra.txt
  Modified  readme.txt  (size 7 -> 14)
  Missing  中文/資料.txt
  This compares what each registration read. A FULL verify of each archive proves that its carrier still holds that content.
```

### JSON

English, as every `--json` output (decision 0017); keys are in alphabetical order; `registered` is the UTC `created` of the compared revision's header; `counts` is `{}` when nothing differs; `differences` are in canonical order.

```json
{"archive_a":{"label":"ARCH-0001","registered":"2026-10-15T01:47:40Z","revision":1,"uuid":"99188c9d-3467-495e-98f8-f0bc7769e053"},
 "archive_b":{"label":"ARCH-0002","registered":"2026-10-15T01:47:40Z","revision":2,"uuid":"94bbff52-39fa-458a-8831-9b30fb0cadf1"},
 "counts":{"Missing":1,"Modified":1,"New":1},
 "differences":[{"detail":"","p":"extra.txt","status":"New","stream":""},
                {"detail":"size 7 -> 14","p":"readme.txt","status":"Modified","stream":""},
                {"detail":"","p":"中文/資料.txt","status":"Missing","stream":""}],
 "entries_a":5,"entries_b":6,"result":"DIFFERENT","same":4}
```

Failures use the usual `{"error": "...", "exit_code": n}`.

### GUI

A sixth view, **Compare Archives**, follows Verify Archive in the sidebar. It has two drop-downs, "Reference archive (A)" and "Copy to check (B)" (B starts on the second archive in the list), Compare and Cancel buttons, a progress bar that counts entries (it shows "Starting..." while the manifests load) and the report. The comparison runs as a job off the GTK main thread, like registration and verification, and the language drop-down is unavailable meanwhile (decision 0017).

- The view refuses A = B ("A and B are the same archive; choose the copy to check as B."): there an `IDENTICAL` result would say nothing about the copy the operator meant to check.
- The report starts with `ARCHIVES <result>`, a sentence explaining `IDENTICAL` or `IDENTICAL_WITH_WARNINGS`, the A (reference) and B (copy) lines with label, uuid, revision and registration time, the entry counts, the number of differences per status, and the differences themselves, then the scope sentence. It lists at most 2 000 differences ("... and N more differences, not listed here"); the counts cover all of them.
- A failure shows "Not compared: <message>" and, unless the comparison was cancelled, "Failure: <failure_class>".

### Interface terms (decision 0017)

| English | 繁體中文 |
|---|---|
| comparison, compare, Compare Archives | 比較、比較封存 |
| reference archive (A), copy (B) | 參考封存（A）、副本（B） |
| IDENTICAL / IDENTICAL_WITH_WARNINGS / DIFFERENT | 相同 / 相同（有警告）/ 不同 |

### Pictures for the user guide

The GUI has a screenshot mode, as it has a smoke-test mode (decision 0016). `VERIARCHIVE_GUI_SCREENSHOTS=<folder>` turns it on; `VERIARCHIVE_SCREENSHOT_REFERENCE` (archive A's uuid), `VERIARCHIVE_SCREENSHOT_COPY` (archive B's uuid) and `VERIARCHIVE_SCREENSHOT_FOLDER` (archive A's folder) are also required, and `VERIARCHIVE_GUI_SMOKE_TEST` takes precedence. The GUI then tours its views at 1100 × 720, starts a FULL verification of A and the comparison of A with B through the buttons' own handlers, saves `01-settings.png` … `07-language.png` into the folder, and quits; a step that fails exits with status 1 and names the picture and the reason on stderr. `tools/screenshots/make_screenshots.cmake` builds sample data (an original and a changed copy), registers both with the CLI and runs the tour against that repository only; `tools/screenshots/README.md` describes it.

## Why

The manifests are the validated truth (§A5); a comparison of two of them is as trustworthy as each one, and needs neither disk attached. Reusing verification's item statuses keeps one vocabulary for "what differs" across the program, and keeping copies as separate archives (decision 0005) leaves matching, history and overdue policy untouched.

Rejected alternatives:

- Reading both disks and comparing them file by file: both disks must be attached at once and read again in full, and the result is a one-off statement with no baseline behind it. Registration plus comparison plus FULL verification gives the same proof, and each step is recorded or repeatable.
- Verifying B's disk against A's manifest through `copy_of`: it would make `copy_of` take part in verification, against decision 0005, and mix two archives' identity and history in one run.
- Reporting `PASSED` / `FAILED`: these claim a reading of the medium.
- Comparing metadata: `mtime` and attributes are not kept by every copy tool, and a difference would hide the content result behind warnings. Content is the criterion (R-51).

## Consequences

- No change to the manifest format, the database schema or the golden manifests: the comparison only reads.
- New result words `IDENTICAL`, `IDENTICAL_WITH_WARNINGS`, `DIFFERENT`; new detail texts listed above; new failure class `Cancelled` for the comparison. All are English codes in `--json` and translated in text output (decision 0017).
- Only current revisions are compared. Comparing an older revision would need a `--revision` option; nothing asks for it yet.
- Verification tells a directory with contents the same way (2026-10-27; R-50), against what the scan of the media finds now: a file or a link that became a folder with contents is `TypeChanged` (`f -> d`, `l -> d`) beside that folder's entries `New`, and a folder with contents that became a file or a link is `TypeChanged` (`d -> f`, `d -> l`) beside its recorded entries `Missing`, where it reported `Missing` plus `New` before; the run fails either way. An empty directory that now holds files is still the same directory. Both share the rule (`holds_directory`, `src/core/engine/item_rules.h`). A relocation onto such a carrier ends as before (decision 0026): the new carrier does not hold the recorded content.
- The comparison is pure core code (`src/core/engine/compare_archives.*`) with no platform calls, so it behaves the same on Windows and Linux. An archive registered on Windows can be compared with one registered on Linux; absolute link targets are then only warnings (decision 0008).
- Tests: unit tests of the engine (identical copy with other mtimes; each difference in canonical order; directories with contents; modified details; absolute links across OSes; unreadable files and directories in either archive; an archive with itself; a tampered manifest; an unknown uuid; progress and cancel) and a CLI end-to-end case (a copy is `IDENTICAL`, then one file changed, one deleted and one added gives `DIFFERENT` with exit 3, archives named by 8-character prefixes).
