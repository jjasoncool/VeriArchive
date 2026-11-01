---
status: accepted
date: 2026-11-02
amends: HANDOFF §B16 (`archive compare`: each archive's line says which revision is compared and when it was recorded; `--json` names that time `created`), §B17 (Compare Archives: the same words), decision 0020 (its output: the `A:` and `B:` lines, the lines of Compare Archives and the `--json` member `registered`), decision 0040 (its consequence for Compare Archives and the GUI)
---

# A comparison says when each compared revision was recorded

A comparison (decision 0020) reads the current revision of each archive, and its line for each archive gave the time of that revision, the `created` of its manifest's header, as the time the archive was registered ("registered", 註冊於). A revision is not always recorded by a registration: a relocation records the archive's next revision, with the content hashes of the files and streams of the one before (decision 0004), and so does a FULL verify that adds chunk hashes, with the entry lines of the one before (decision 0033). The operator's archives, registered by V1 and given chunk hashes later, were then shown by Compare Archives as registered on the day their chunk hashes were added, while `archive show` gives the end of their registration (decision 0040): two registration times for one archive, which decision 0040 had set out to end.

## Decision (operator, 2026-11-02)

1. **Each archive's line says which revision is compared and when it was recorded.** `archive compare` (sample values): `A: ARCH-0001, revision 2 recorded 2025-01-16 09:00:00 (UTC+8), 6 entries` (`A：ARCH-0001，版次 2 記錄於 2025-01-16 09:00:00（UTC+8），共 6 個項目`), and so `B:`. Compare Archives: `A (reference): ARCH-0001 (<uuid>) revision 2, recorded 2025-01-16 09:00:00 (UTC+8)` (`A（參考）：ARCH-0001（<uuid>）版次 2，記錄於 2025-01-16 09:00:00（UTC+8）`), and so `B (copy):`. The time is still the compared revision's `created`: when the registration, relocation or verification adding chunk hashes that recorded it ended. An archive's registration is what `archive show` gives (decision 0040).
2. **`--json` names that time as `archive show --json` names a revision's: `created`**, in `archive_a` and `archive_b`, where it was `registered`. The value is unchanged. A script that read `registered` from version 1.0.0 reads `created`: the member is renamed, not given a second name beside the first.
3. The scope sentence stays: a comparison compares files and streams by their whole hashes only (decision 0032), which a relocation and a verification adding chunk hashes keep from the revision before (decisions 0004, 0033), so the content it compares is still what each registration read. A link, which has no content, is compared by the target its revision records, which a relocation records as it read it (decisions 0008, 0032 point 5).
4. No change to the manifests, the database or the golden test.

## Why

The time a comparison shows is that of the baseline it reads, and the line now names that baseline and says what the time is. The operator chose it over the archive's registration, which `archive show` already gives.

Rejected alternatives:

- Showing each archive's registration, its first revision's time, as `archive show` does: a comparison of a later revision would not say when that revision was recorded, though the revision itself is named either way (`rN` in the first line of `archive compare`, `revision` in its `--json`, `revision N` in the lines of Compare Archives); and after a registration of a new revision (`register --new-revision`), which reads a new baseline, the line would give a time older than the content compared.
- Keeping "registered": it is wrong for every revision recorded by a relocation or by adding chunk hashes.
- Keeping `registered` in `--json`, or giving `--json` both members: the words and the member would name one time two ways.

## Consequences

- `archive show` gives an archive's registration, and a comparison the time each compared revision was recorded, under that name; the two no longer give one archive two registration times.
- Decision 0020: its quoted output, the `A:` and `B:` lines, the lines of Compare Archives and the `--json` member, say so.
- Decision 0040: no GUI view shows an archive's registration, and Compare Archives gives it only while the first revision is the one compared; its consequence said that the archive's details, in `archive show` and the GUI, and Compare Archives show the same time.
- Tests: the engine gives the time of each compared revision, also where B's is its second revision, recorded a second after its first; the Chinese words of each archive's line in the CLI and in the GUI; the CLI end to end: both lines of an identical copy, in English and in Chinese, both lines after the copy's new revision (`revision 1 recorded` for A, `revision 2 recorded` for B), and `created` in `--json` equal to the time that `archive show --json` gives revision 2, with no `registered` beside it. A mutation check removed each part in turn (the words "revision N recorded" in English, in either line, the revision in the line, the Chinese words of the CLI's and of the GUI's lines, the `--json` member, the time of the compared revision), and each removal failed the test meant for it. The GUI's lines have no unit test of their own (GTK): their words are the catalog's, which the tests cover, and the screenshot tour stops where the report of its comparison does not say "revision 1, recorded".
- The user guides' example of `archive compare` and their pictures of Compare Archives (`06-compare.png`, in both languages, taken again with `tools/screenshots`) show the new lines.

### Interface terms (decision 0017)

| English | 繁體中文 |
|---|---|
| A: L, revision N recorded T, C entries | A：L，版次 N 記錄於 T，共 C 個項目 |
| B: L, revision N recorded T, C entries | B：L，版次 N 記錄於 T，共 C 個項目 |
| A (reference): L (U) revision N, recorded T | A（參考）：L（U）版次 N，記錄於 T |
| B (copy): L (U) revision N, recorded T | B（副本）：L（U）版次 N，記錄於 T |
