---
status: accepted
date: 2026-10-08
amends: HANDOFF §B15 (dependencies)
---

# GLib provides Unicode normalisation and case folding

Three rules compare names the way a filesystem would rather than byte by byte: NFC/NFD and case collisions within a directory (R-42), case-insensitive overlap of archive roots (decision 0001), and `root_subpath` narrowing during matching. Standard C++ has no Unicode normalisation or full case folding, and hand-written tables would be large and easy to get wrong. GLib (`g_utf8_normalize`, `g_utf8_casefold`) is portable C, already required by the GTK4 GUI on both platforms, and contains no OS API, so `common` links it privately behind `common/unicode.h`.

## Consequences

- `glib-2.0 ≥ 2.72` joins the dependency table for every build, including the CLI.
- Stored names are never normalised (R-42); GLib only produces comparison keys.
