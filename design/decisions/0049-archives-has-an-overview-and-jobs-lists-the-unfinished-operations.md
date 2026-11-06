---
status: accepted
date: 2026-11-06
amends: HANDOFF §B17 (Archives shows an overview above the archives, whose tiles filter the list, and Verify the attached archives that need it; the head of an archive's details stays in view while they scroll, with FULL verify and Cross-check...; Jobs lists the unfinished operations; the window calls a comparison a cross-check, takes neither archive for the original and lists each archive's last FULL verification in its report), decision 0020 (the window's words for a comparison and its differences), decision 0022 (where the unfinished operations show), decision 0025 (Abandon... under Unfinished operations in Jobs), decision 0046 (point 3: Archives above the list, the details' head and menu; point 4: Jobs; point 5: the comparison dialog)
---

# Archives has an overview, and Jobs lists the unfinished operations

Decision 0046 put the unfinished operations above the list of archives. A stopped verification there took the top of Archives and pushed the details of the archive chosen down, while the jobs button above every page already told what the window runs. The comparison was hard to find: Compare with another archive... was the first item of the details' menu (⋯), and with a stopped operation above the list, the menu was out of view. The details scrolled away with their head, so that further down the operator no longer saw which archive they were reading. The operator asked whether the unfinished operations are needed besides the jobs, and what the top of Archives could show instead, and approved an interactive proposal of it (sample data only).

An unfinished operation and a job are two states of one thing for the operator: something started that has not ended. A job is the window's and is gone once the window closes; an unfinished operation lives in the repository, wherever it was started, and stays until it finishes or is abandoned (GLOSSARY). A stopped job of the window is an unfinished operation from then on. So the unfinished operations stay, and are listed where the jobs are.

## Decision (operator, 2026-11-06)

1. **Jobs lists the unfinished operations**, in a group of their own below the jobs: each registration, verification and relocation that has not finished, wherever it was started, with its label, root, operation, progress and state, and Resume and Abandon... as decisions 0022, 0025, 0026 and 0033 give them, every rule, refusal and question kept. One that a job of the window works on is that job's row already and is not listed twice, unless it has stopped. The unfinished operations are read while Jobs or Archives is shown. The bar of a step of abandoning shows in that group. How many have stopped shows beside the jobs button's count and on the Jobs row of the sidebar, in the colour of warnings. Stop of a registration, verification or relocation says in its tooltip that the job keeps what it did, and that Resume under Unfinished operations goes on from there, reading again only the file it was reading: it works as a pause would, and keeps its name, as the file read when it stopped is read again (the operator chose so). Every sentence of the window that named "Unfinished operations in Archives" names Jobs.
2. **An overview above the list of Archives**: how many archives there are and how many need attention (OVERDUE, never FULL verified, the result its row shows FAILED, a device alert, or a manifest that cannot be used; none retired); how many are attached, once a read of the attached devices says, and the latest FULL verification of any archive, with its archive and how long ago; and a tile for each of All, Passed, not overdue, OVERDUE, Never FULL verified, Last result FAILED and Device alert, with how many archives it holds. **A tile chosen shows only its archives in the list**, All at first; the choice is kept while the window is open. A retired archive counts under All only, as one whose first registration has not finished. The tiles can be folded away; folded, the list shows every archive. Below them, "N unfinished operations have stopped: see Jobs", where any has.
3. **Verify the attached archives that need it (N)**, in the overview, starts a FULL verification of each archive that is OVERDUE, never FULL verified or whose last result FAILED, that is attached as recorded as the latest read found it, behind no other enclosure (whose verification asks first, decision 0047), with no job of the window and no unfinished operation; as FULL verify of its details starts it, so that those of one device run one after another (decision 0045). Its tooltip names them. Register Archive, Cross-check... and Refresh sit beside it.
4. **The head of an archive's details stays above them while they scroll**: its label and the result of its last FULL verification as its row shows it, the menu; its volume label and root, **FULL verify**, the split button of decision 0046, and **Cross-check...**. Scrolled, the label is smaller and a line sets the head off. The banner under it keeps View result, and says how long ago the last FULL verification was before when it ended.
5. **The window calls a comparison a cross-check** (核對完整性): Cross-check... beside FULL verify, in the overview and beside each copy in Copies; the menu (⋯) no longer lists it. A and B stand side by side and neither is taken for the original: the dialog's rows are Archive A and Archive B (B lists the recorded copies of A first, as before), and a difference is an entry only in A, only in B, or one that differs (the engine's Missing, New and Modified). **Its report also gives each archive's last FULL verification** of the revision cross-checked, as Archives listed it when the job started: its result with its mode, how long ago and when, OVERDUE where it is, or that there is none of that revision yet. Where the two hold the same content, it says either that both media held it at last FULL verifications that are not overdue, or which archives a FULL verification would have to show it of. The operator chose the name; the command line keeps `archive compare`, its words and `--json`.
6. No change to the command line, the engine, the manifests, the database or `--json`.

## Why

The operator chose the Jobs page for the unfinished operations, the overview and its filters, the one-click verification, the frozen head and the name. One place for everything started and not ended matches how the operator thinks of it; the details' notice of a stopped verification of the archive, with Resume, stays where the archive is. The overview answers what the operator opens the window for, which archives need a verification, and makes them one click away. A cross-check answers "are my copy and its original the same?" only together with what each disk held, so the report says both; it reads no media, so it never claims more than the FULL verifications it lists.

The name keeps 驗證 (verification) for reading media again: the operator first proposed 核對驗證 and 比對驗證, which use verification's own words (GLOSSARY: Comparison, _Avoid_: verification), and chose 核對完整性. "Cross-check" is its English, as the window's word; the GLOSSARY term stays Comparison.

Rejected alternatives:

- The unfinished operations in one line above the archives, leading to them: still two places for one thing.
- Renaming Stop to Pause: a stopped job does not hold its place in the file it read, which a resume reads again.
- A cross-check that FULL verifies both archives first: hours of reading for what the FULL verifications already listed beside it tell, and only while both are attached.
- 副本 (copy) in the name: neither archive is taken for the original.

## Consequences

- HANDOFF §B17: points 1 to 5.
- GLOSSARY: Comparison gives the window's word, Cross-check (核對完整性).
- The user guides: Archives with its overview, the frozen head and Verify the attached archives that need it; Jobs with the unfinished operations; "Cross-checking archives" in place of "Comparing archives"; every "Unfinished operations in Archives" now in Jobs. The pictures are taken again (`02-archive-list.png`, `04-verify.png`, `06-compare.png`, `08-jobs.png` show the changes).
- Tests: the overview's filters, tiles, heading and line, which archives need a FULL verification by their state (retired, not yet registered and unusable ones excluded); the cross-check's words for a difference, its report with each archive's last FULL verification (of the revision cross-checked, of an earlier one, OVERDUE, FAILED, none) and what it says of the media; the banner's line, how long ago first; every new text in Chinese; the window's refusals naming Jobs. That Jobs lists the unfinished operations and leaves out those its jobs work on, the tiles filter the list, the head stays in view and the one-click verification starts the archives it names is reviewed and seen in the screenshot tour on sample data, not tested.

### Interface terms (decision 0017)

| English | 繁體中文 |
|---|---|
| Cross-check, Cross-check... | 核對完整性、核對完整性… |
| Archive A, Archive B | 封存 A、封存 B |
| Only in A, Only in B, Differs | 只在 A、只在 B、兩邊不同 |
| Verify the attached archives that need it (N) | 驗證需要驗證的已連接封存（N） |
| All, Passed, not overdue, Never FULL verified, Last result FAILED, Device alert | 全部、通過且未逾期、從未完整驗證、上次失敗、裝置警訊 |
| N unfinished operations have stopped: see Jobs | N 個未完成的作業已停止：前往「工作」 |
