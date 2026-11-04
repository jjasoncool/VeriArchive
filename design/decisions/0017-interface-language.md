---
status: accepted
date: 2026-10-08
amends: HANDOFF §B7.3 (config keys), §B16 (global options; amended 2026-10-27: usage errors follow `--json`, and messages in plain words), §B17 (Settings); R-02 (§A1: how reports and the GUI word the guarantee, 2026-10-27)
---

# The interface speaks English and Traditional Chinese

The operator works in Traditional Chinese; the specification, code, manifests and history stay in English. The CLI and the GUI show their text in English or Traditional Chinese (zh-TW). Anything a program reads stays English, so scripts behave the same in every language.

## What is translated

- Everything written for people: CLI text output, warnings, errors, usage, progress; every GUI label, button and report.
- Engine messages (errors, warnings, item details) are English sentences built in `core`. They are translated where they are shown, by matching catalog templates: `"cannot read the manifest: {}"` matches the sentence and keeps the value. Values (paths, labels, uuids, numbers) are never translated, except the last value of a template marked as a wrapper (`Match::Cause`), which is itself a message: `"cannot scan the root: {}"` around `"access denied (FindFirstFileExW, Windows error 5)"`.
- Causes that come from libraries are translated when they are common: SQLite messages, JSON parser messages, Linux `strerror` texts. Windows system messages already arrive in the system's language.
- Status codes are shown translated (`PASSED` 通過, `Modified` 已修改); the result line keeps its mode (R-21: 完整驗證 通過).

## What is never translated

- `--json` output, exit codes, the codes inside JSON (`PASSED`, `Modified`, `MATCH_WEAK`).
- Manifests, the history database, config keys, file names, uuids, labels and paths.

## Plain words

Amended 2026-10-27 (operator: messages without internal references, plain words for common system errors and for item details; `--json` codes stay as they are). What people read says what happened in words an ordinary user understands, in English as in Chinese:

- **No internal references.** No message names a decision record, a requirement or a failure class: "(decision 0003)", "(R-97)" and "(ManifestHashMismatch)" are gone from the engines' messages ("a retired archive takes no new revisions", "already registered as archive L (root "R"); pass --new-revision to re-baseline it or --force-new-archive", "manifest body does not match its header"). The result and identity words (`FAILED`, `MATCH_WEAK`, `MIGRATED`) and the statuses (`Modified`) stay, as the guides explain them.
- **A failure class in plain words.** A command whose failure has a failure class in front of its message in `--json` (`"error": "ManifestHashMismatch: ..."`, from `archive compare`, `export`, `export-sha256sums`, `import`, `archive relocate`) says the message alone in text (`app::without_failure_class`). A run's failure classes are said in plain words in text, in a verification's report ("failure: the manifest is not the one recorded") and in the history, separated by "; ": `MediaIdentityMismatch` "the media is not the one recorded", `MediaIdentityAmbiguous` "the media matches more than one archive", `ManifestSignatureFailure` "the manifest's signature does not verify", `ManifestHashMismatch` "the manifest is not the one recorded", `ManifestRevisionRollback` "a revision older than one already known", `ChangeTrackerContinuityLost` "the record of changes on the volume was interrupted", `FilesystemAccessError` "files could not be read", `RepositoryError` "the repository could not be read or written" (`app::failure_class_words`). `--json` keeps the classes, `failure_classes` and the `error` of such a failure as they were.
- **An item's detail in plain words.** The repository and `--json` keep an item's detail as a code; text says it in words, in English too (`app::item_detail_words`): a change of type "d -> f" as "folder -> file" (files, folders, links, streams), the metadata that differs "mtime,mode,attrs" as "modification time, permissions, attributes", "link-target-translated" as "link target converted by this system", "known-since-registration" as "unreadable since registration", "name cannot exist on windows" as "name cannot exist on Windows". A detail that is a sentence already ("content differs") or a system's message stays as it is.
- **What matching noted in plain words.** A verification's warning says what identity matching noted of the media in words, "identity: the volume label differs from the one recorded" (the capacity, the filesystem type, the device model; "the device was replaced", "the filesystem id cannot be read", "the archive is retired", "the identity file was copied from other media"), where it gave the code ("identity: volume-label-differs"); `identity.details` in `--json` keeps the codes.
- **One in the singular.** A count of one thing is said in the singular in English, where every count was plural ("1 files, 1 empty directories"): "content: 4 files, 1 empty directory, 0 links, 39 bytes", "1 byte", "1 entry", "1 archive", "1 revision", "1 day", "1 of 1 file (100%)", "... and 1 more difference, not listed here", "History intact: 1 verification run, ...", "less than a minute left" (`app::count_of`, and two templates where a count stands alone). Chinese has no plural: both forms are translated alike.
- **System errors in plain words.** An error that Windows reports says what went wrong first, then the call and Windows' number for whoever looks further: "access denied (CreateFileW, Windows error 5)", where it said "CreateFileW failed (Win32 error 5)"; for not found, no such drive letter (amended 2026-10-27: "no such drive", where "drive" is a word the glossary keeps from naming a device or volume), write-protected media, a device not ready, reporting an error, damaged, not attached, in use or locked by another program, a full volume, a network path not found, a name that exists already, is not valid or is too long, not a folder, a folder not empty, not permitted, no answer in time, not supported, aborted before it finished (amended 2026-10-30: "cancelled", where nobody had cancelled anything: Windows aborted the request, decision 0034). Any other error names Windows' number alone: "Windows error 1234 (CreateFileW)". Linux's errors keep the system's own words ("open failed: Permission denied"), plain already, translated where common.

Tests: an item's detail and a run's failure classes in words, in both languages, a sentence and a system's message kept, an unknown class kept, a message without the class that leads it; each of Windows' errors in plain words, translated, and one without; a verification of a volume whose label differs, warning in words while its details keep the code; the catalog's translations of the engines' messages without references; end to end, a folder made a file in a verification's text ("TypeChanged  empty  (folder -> file)", "類型改變  empty  （資料夾 → 檔案）") and code (`d -> f`), the failure in words in a verification's report and in the history, and an export of a changed manifest whose text has no failure class while its `--json` keeps it. A mutation check removed in turn the words of an item's detail and of a failure class from the command line's report, of the failure classes from the history, the stripping of the failure class, a type's words, the words of what matching noted, the plain words of Windows' errors, and put a decision number back in a message, and each removal failed the tests. The GUI shows the same words through the same functions; its views are not tested beyond starting. Tests of the singular: each count for one and for more, in both languages; end to end, the content of an archive with one empty directory ("1 empty directory"). A mutation check made every count plural again, and the test failed.

## Times

Timestamps are stored and exported in UTC (`2026-10-14T00:13:53Z`). The CLI's text output and the GUI show them in the system's local time with the offset from UTC in force at that moment, daylight saving time included: `2026-10-14 08:13:53 (UTC+8)`, in Chinese `2026-10-14 08:13:53（UTC+8）`. The offset keeps the moment unambiguous when a report is read in another time zone. `--json` output keeps the UTC timestamps.

## Choosing the language

`--lang en|zh-TW` (CLI), then `VERIARCHIVE_LANG`, then the config file's `"language"`, then the operating system's display language (on Linux gettext's order: `LANGUAGE`, which may be a `:`-separated list and is ignored under the C or POSIX locale, then `LC_ALL`, `LC_MESSAGES`, `LANG`), then English. Command-line errors found before the command runs follow `--lang` and `--config` too, and `--json` (amended 2026-10-27): with `--json` among the arguments, a parse error ("option --out needs a value", "--lang takes en or zh-TW"), an unknown command ("unknown command X") and no command at all ("a command is needed; veriarchive-cli without arguments shows the commands") are printed as every command's failure is, JSON on stdout with `error` and `exit_code` (10), in English, where they were text, or the usage. Without `--json`, an unknown command says so ("error: unknown command X") before the usage. `media list` checks its options as every command does: an unknown one is a usage error, where it was taken silently. Tests: the CLI end to end, each of these with `--json`, an unknown option of `media list` and an unknown command in text; a mutation check removed the JSON of a parse error, of an unknown command, of no command, and the option check of `media list`, in turn, and each removal failed the test.

The GUI's Settings view has a language drop-down; it saves `"language"` in the config file and rebuilds the window. What the user typed and chose, the window size and the reports shown are kept, and reports are worded again in the new language; the window position is not kept (GTK 4 cannot set it). The drop-down is unavailable while an operation runs, because the operation reports into the current window. The GUI sets `LANGUAGE` before GTK starts and resets GTK's translation cache when the language changes, so GTK's own texts follow; the Windows package ships GTK's zh_TW catalogs (`share/locale/zh_TW`). On Linux, glyph forms of Chinese text follow the language the GUI started with until it is restarted (Pango reads the language once).

On Windows the CLI writes text for a console window as UTF-16 (`WriteConsoleW`), so Chinese is readable in `cmd.exe`, PowerShell and Windows Terminal whatever the console's code page; the code page itself is never changed. Redirected or piped output is UTF-8. Windows PowerShell 5.1 decodes a program's output with `[Console]::OutputEncoding` (Big5 on a Traditional Chinese Windows), so before capturing, redirecting or piping CLI output there, run `[Console]::OutputEncoding = [Text.UTF8Encoding]::new()`.

If the GUI ends unexpectedly, it appends the reason to `veriarchive-gui.log` in the user config folder (`%APPDATA%\VeriArchive` on Windows); an exception in a button or other GTK callback is shown in the view instead of ending the program.

## What this version does not do (2026-10-27)

The texts of the CLI and the GUI, the catalog, the user guides and the README never name a version of the program (V1, V1.1, 1.0.0) for what it does or does not do. The specification's milestones are not the numbers of the programs: 1.1.0 holds only the first stage of V1.1, so "tamper evidence begins in V1.1" was untrue in the very version it named. What this version does not do yet is said of "this version", which stays true in every version that lacks the feature, whatever its number, and goes when the feature comes:

- R-02's guarantee, in every verify report of the CLI and the GUI: "VeriArchive detects accidental corruption and unintended change; tamper evidence (signed manifests) is not part of this version." `--json` gives the same English sentence in `guarantee`.
- The refusal of `verify --quick|--deep` (exit 10): "QUICK and DEEP verification are not available in this version; use --full".
- Settings: "Trusted signing keys: signed manifests are not part of this version."
- After a check of the history chain: "... history signatures, which would detect those, are not part of this version."

A unit test fails on a catalog entry that names a version in either language ("V" and a digit at the start of a word); the end-to-end test checks the guarantee, its `--json` member and the refusal in both languages.

Signed history checkpoints (R-76) are history signatures in the interface, 歷程簽章 in Chinese, never 簽章檢查點: 檢查點 is the checkpoint of a resume (operator's plan, item 4-3).

## Implementation

English is the source language: the code keeps writing English. `va::app::Text` translates a message (`text("Settings")`) or a template with values (`text.format("Registered {} revision {}", label, revision)`). The compiler checks each English template against its values, as `std::format` does; a translation that does not fit is replaced by the English text rather than failing.

Engine sentences are matched against the catalog's templates. Templates that start with fixed text are tried before templates that start with a value, longest first, so a wrapper such as `"cannot scan the root: {}"` wins over a generic cause such as `"{} failed: {}"`. Each entry may set its match mode: `Values` (the default), `Cause` (the last value is a message and is translated too) or `FormatOnly` (used only through `Text::format`, never matched against sentences).

The catalog is C++ (`src/app/text_zh_tw.cpp`), not gettext `.po`/`.mo`: no extra build tool (`msgfmt`) or runtime lookup of files is needed, and a unit test checks every entry (same values in both languages, plain `{}`/`{N}` placeholders, no duplicates, `Cause` entries end in `": {}"`, templates specific enough to match only their sentence). A new message without a translation is shown in English.

## Terms

| English | 繁體中文 |
|---|---|
| archive | 封存 |
| Archive List | 封存列表 |
| repository / RepositoryRoot | 儲存庫 / 儲存庫根目錄 |
| manifest | 清單檔 |
| media copy | 媒體副本 |
| revision | 版次 |
| register, baseline | 註冊、基準 |
| verify, FULL verify | 驗證、完整驗證 |
| run, resume, checkpoint | 執行、繼續、檢查點 |
| history, history chain | 歷程、歷程鏈 |
| history signature (a signed history checkpoint, R-76) | 歷程簽章 |
| tamper evidence, signed manifest, trusted key | 竄改證據、簽章清單檔、受信任的金鑰 |
| volume, disk, carrier, media | 磁碟區、磁碟、載體、媒體 |
| link (symlink or junction), symlink | 連結、符號連結 |
| identity | 身分 |
| relocate, retire | 搬移、停用 |
| export, import | 匯出、匯入 |
| label | 標籤 |
| coverage | 涵蓋範圍 |
| metadata | 中繼資料 |
| overdue | 逾期 |
| PASSED / PASSED_WITH_WARNINGS / FAILED / INCOMPLETE | 通過 / 通過（有警告）/ 失敗 / 未完成 |
| Verified / Modified / Missing / New | 已驗證 / 已修改 / 遺失 / 新增 |
| Unreadable / MetadataMismatch / TypeChanged | 無法讀取 / 中繼資料不符 / 類型改變 |
| Unrepresentable / SkippedMountPoint / Unbaselined | 無法表示的名稱 / 略過掛載點 / 無基準 |

## Consequences

- Amended by decision 0046: the window's pages are Archives (封存) and History (歷程), where the table above says Archive List (封存列表) and the catalog said Verification History (驗證歷程), whose entries left the catalog; the language drop-down is a row of the Settings page, locked while a job waits or works (decision 0045), which says why; the terms of the new pages and controls are in decision 0046's table.
- Amended by decision 0042: the words for `MediaIdentityMismatch` and for what matching noted compare the media with the identity that the verified revision records, which a relocation or a verification adding chunk hashes may have recorded: "the media is not the one recorded", "identity: the volume label differs from the one recorded" (and so the capacity, the filesystem type and the device model), where they said "registered". The words quoted above say so.
- Config gains `"language"` (`"en"`, `"zh-TW"`; absent = follow the system).
- A new English message needs a catalog entry to appear in Chinese; until then it appears in English, never broken.
- Adding a language means one more catalog file and one more `Language` value.
