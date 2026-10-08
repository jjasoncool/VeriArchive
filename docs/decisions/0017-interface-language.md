---
status: accepted
date: 2026-10-08
amends: HANDOFF §B7.3 (config keys), §B16 (global options), §B17 (Settings)
---

# The interface speaks English and Traditional Chinese

The operator works in Traditional Chinese; the specification, code, manifests and history stay in English. The CLI and the GUI show their text in English or Traditional Chinese (zh-TW). Anything a program reads stays English, so scripts behave the same in every language.

## What is translated

- Everything written for people: CLI text output, warnings, errors, usage, progress; every GUI label, button and report.
- Engine messages (errors, warnings, item details) are English sentences built in `core`. They are translated where they are shown, by matching catalog templates: `"cannot read the manifest: {}"` matches the sentence and keeps the value. A value after a final `": "` is a cause and is translated too.
- Status codes are shown translated (`PASSED` 通過, `Modified` 已修改); the result line keeps its mode (R-21: 完整驗證 通過).

## What is never translated

- `--json` output, exit codes, the codes inside JSON (`PASSED`, `Modified`, `MATCH_WEAK`).
- Manifests, the history database, config keys, file names, uuids, labels and paths.

## Choosing the language

`--lang en|zh-TW` (CLI), then `VERIARCHIVE_LANG`, then the config file's `"language"`, then the operating system's interface language, then English. The GUI's Settings view has a language drop-down; it saves `"language"` in the config file and rebuilds the window. The GUI also sets `LANGUAGE` before GTK starts, so GTK's own texts follow; the Windows package ships GTK's zh_TW catalogs (`share/locale/zh_TW`).

On Windows the CLI switches the console to UTF-8 (`SetConsoleOutputCP`), so Chinese output is readable in `cmd.exe` and PowerShell.

## Implementation

English is the source language: the code keeps writing English. `va::app::Text` translates a message (`text("Settings")`) or a template with values (`text.format("Registered {} revision {}", label, revision)`). The catalog is C++ (`src/app/text_zh_tw.cpp`), not gettext `.po`/`.mo`: no extra build tool (`msgfmt`) or runtime lookup of files is needed, and a unit test checks every entry (same values in both languages, valid format strings, no duplicates, templates specific enough to match only their sentence). A new message without a translation is shown in English.

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
