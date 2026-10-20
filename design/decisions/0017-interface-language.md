---
status: accepted
date: 2026-10-08
amends: HANDOFF §B7.3 (config keys), §B16 (global options), §B17 (Settings)
---

# The interface speaks English and Traditional Chinese

The operator works in Traditional Chinese; the specification, code, manifests and history stay in English. The CLI and the GUI show their text in English or Traditional Chinese (zh-TW). Anything a program reads stays English, so scripts behave the same in every language.

## What is translated

- Everything written for people: CLI text output, warnings, errors, usage, progress; every GUI label, button and report.
- Engine messages (errors, warnings, item details) are English sentences built in `core`. They are translated where they are shown, by matching catalog templates: `"cannot read the manifest: {}"` matches the sentence and keeps the value. Values (paths, labels, uuids, numbers) are never translated, except the last value of a template marked as a wrapper (`Match::Cause`), which is itself a message: `"cannot scan the root: {}"` around `"FindFirstFileExW failed (Win32 error 5)"`.
- Causes that come from libraries are translated when they are common: SQLite messages, JSON parser messages, Linux `strerror` texts. Windows system messages already arrive in the system's language.
- Status codes are shown translated (`PASSED` 通過, `Modified` 已修改); the result line keeps its mode (R-21: 完整驗證 通過).

## What is never translated

- `--json` output, exit codes, the codes inside JSON (`PASSED`, `Modified`, `MATCH_WEAK`).
- Manifests, the history database, config keys, file names, uuids, labels and paths.

## Times

Timestamps are stored and exported in UTC (`2026-10-14T00:13:53Z`). The CLI's text output and the GUI show them in the system's local time with the offset from UTC in force at that moment, daylight saving time included: `2026-10-14 08:13:53 (UTC+8)`, in Chinese `2026-10-14 08:13:53（UTC+8）`. The offset keeps the moment unambiguous when a report is read in another time zone. `--json` output keeps the UTC timestamps.

## Choosing the language

`--lang en|zh-TW` (CLI), then `VERIARCHIVE_LANG`, then the config file's `"language"`, then the operating system's display language (on Linux gettext's order: `LANGUAGE`, which may be a `:`-separated list and is ignored under the C or POSIX locale, then `LC_ALL`, `LC_MESSAGES`, `LANG`), then English. Command-line errors found before the command runs follow `--lang` and `--config` too.

The GUI's Settings view has a language drop-down; it saves `"language"` in the config file and rebuilds the window. What the user typed and chose, the window size and the reports shown are kept, and reports are worded again in the new language; the window position is not kept (GTK 4 cannot set it). The drop-down is unavailable while an operation runs, because the operation reports into the current window. The GUI sets `LANGUAGE` before GTK starts and resets GTK's translation cache when the language changes, so GTK's own texts follow; the Windows package ships GTK's zh_TW catalogs (`share/locale/zh_TW`). On Linux, glyph forms of Chinese text follow the language the GUI started with until it is restarted (Pango reads the language once).

On Windows the CLI writes text for a console window as UTF-16 (`WriteConsoleW`), so Chinese is readable in `cmd.exe`, PowerShell and Windows Terminal whatever the console's code page; the code page itself is never changed. Redirected or piped output is UTF-8. Windows PowerShell 5.1 decodes a program's output with `[Console]::OutputEncoding` (Big5 on a Traditional Chinese Windows), so before capturing, redirecting or piping CLI output there, run `[Console]::OutputEncoding = [Text.UTF8Encoding]::new()`.

If the GUI ends unexpectedly, it appends the reason to `veriarchive-gui.log` in the user config folder (`%APPDATA%\VeriArchive` on Windows); an exception in a button or other GTK callback is shown in the view instead of ending the program.

## Implementation

English is the source language: the code keeps writing English. `va::app::Text` translates a message (`text("Settings")`) or a template with values (`text.format("Registered {} revision {}", label, revision)`). The compiler checks each English template against its values, as `std::format` does; a translation that does not fit is replaced by the English text rather than failing.

Engine sentences are matched against the catalog's templates. Templates that start with fixed text are tried before templates that start with a value, longest first, so a wrapper such as `"RepositoryError: {}"` wins over a generic cause such as `"{} failed (Win32 error {})"`. Each entry may set its match mode: `Values` (the default), `Cause` (the last value is a message and is translated too) or `FormatOnly` (used only through `Text::format`, never matched against sentences).

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

- Config gains `"language"` (`"en"`, `"zh-TW"`; absent = follow the system).
- A new English message needs a catalog entry to appear in Chinese; until then it appears in English, never broken.
- Adding a language means one more catalog file and one more `Language` value.
