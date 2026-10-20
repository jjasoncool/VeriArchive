# VeriArchive agent rules

1. Read `design/HANDOFF.md` §A3, §A6, §B4, §B7.4, §B9 before writing code. Part B is frozen; contract changes require a decision record in `design/decisions/`. Accepted records override the HANDOFF text they amend; each amended section names its records.
2. `core` / `common` never include OS headers and never link `veriarchive_platform`. OS selection lives in CMake only.
3. Validated manifest bytes are the truth; SQLite is an index.
4. Use `std::filesystem::path`, `operator/`, and `u8string()` persistence. `path::string()` and `current_path()` are banned in `src/`.
5. Repository selection is explicit → pinned → first-run fallback. Configured-but-unavailable is an error.
6. Platform fallible calls return `std::expected<T, IoError>`; exceptions never cross the platform boundary.
7. Long operations take `IProgressSink&` and `CancelToken&`.
8. Windows uses `wmain`, `-municode`, `longPathAware`, and UTF-8 `activeCodePage`.
9. V1 is unprivileged. Elevated operations belong behind V1.1 helper interfaces.
10. Manifest byte changes require golden tests and possibly a format-version decision.
11. Name things with `GLOSSARY.md` terms in identifiers, docs, and CLI/GUI text; its _Avoid_ words are rejected synonyms.