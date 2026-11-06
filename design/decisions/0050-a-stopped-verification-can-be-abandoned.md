---
status: accepted
date: 2026-11-06
amends: HANDOFF §B17 (Abandon... on every stopped verification under Unfinished operations), decision 0022 (a stopped verification that adds no chunk hashes may be abandoned, besides being superseded by the next verification), decision 0033 (abandoning a verification names its run, whether it adds chunk hashes or not)
---

# A stopped verification can be abandoned

A FULL verification that adds no chunk hashes and has stopped offered Resume alone. It blocked nothing (decision 0022): the next verification of its archive superseded it, and it left the list. Decision 0049 then counted the unfinished operations that stopped on the jobs button and the Jobs row, in the colour of warnings. A verification that nobody means to resume, of an archive that will not be verified again soon or whose device failed, kept that count lit, and the operator had no way to clear it but to verify the archive again. The operator asked to remove such a verification ("停止之後不能移除欸 只有繼續", "意思就是放棄那次驗證").

## Decision (operator, 2026-11-06)

1. **Abandon... on every stopped verification under Unfinished operations in Jobs**, one that adds no chunk hashes as one that adds them (decision 0033). It reads first what abandoning removes, asks with Cancel as the default ("Abandon the stopped verification of X?", its root and its saved progress), and abandons it as its step besides the jobs, with the checks again, refused while a job of the window works on it (decision 0045) or while an attempt of it still runs (decision 0024).
2. **Abandoning removes the run's record (`run:<run_id>`) and its saved progress (its checkpoints) in one transaction, and nothing else**: the archive, its revisions, their manifests and index, the media evidence and the verification history stay, the run's `INCOMPLETE` row included, which the hash chain covers (R-90). The run can never be resumed. Unabandoned, the next verification of the archive still supersedes it, as before.
3. **The engine abandons a verification named by its run** (`VerificationAbandonRequest::run_id`), whether it adds chunk hashes or not; one that adds them is still the archive's unfinished verification that adds them at its current revision, and a run whose record is gone is refused as no longer the one shown. Without a run, the archive's verification that adds chunk hashes, as before.
4. **The command line does not change**: `archive abandon-verification <uuid> --confirm` abandons the archive's verification that adds chunk hashes. The command line lists no stopped verification that adds none (`archive list` lists only those that block another operation), and `verify --full ... --resume` or the next verification goes on from one.

## Why

The operator chose to abandon it in the window. The count of stopped operations is a call to act; one that cannot be cleared trains the operator to ignore it. Superseding by the next verification stays, so nothing is asked of an operator who simply verifies again. History is append-only: abandoning never removes the `INCOMPLETE` run, which records that an attempt was made and how far it came.

Rejected alternatives:

- Leaving the stopped verification out of the count: it would still be listed, and still wait for a Resume that nobody means to give.
- Dropping a stopped verification by itself after some time: it would discard progress the operator may still resume, without asking.
- `archive abandon-verification <uuid> --run <run_id>` on the command line: the command line lists no such verification, so it would name a run it never showed; it can be added when it does.

## Consequences

- HANDOFF §B17: point 1.
- The user guides: abandoning a stopped verification, under "Unfinished operations" in Jobs.
- Tests: the engine abandons a stopped verification that adds no chunk hashes named by its run (its record and checkpoints gone, its `INCOMPLETE` run kept, the archive at its revision, its resume refused, a second abandon refused as no longer the one shown), and without a run names only one that adds chunk hashes; a run that adds them named by a run that is gone keeps its own refusal; the list offers Abandon... on a stopped verification that adds none; every new text in Chinese. The question and the report in the window are reviewed, not tested (GTK).

### Interface terms (decision 0017)

| English | 繁體中文 |
|---|---|
| Abandon the stopped verification of X? | 要放棄 X 已停止的驗證嗎？ |
| Abandoned the stopped verification of X (uuid). | 已放棄 X（uuid）已停止的驗證。 |
