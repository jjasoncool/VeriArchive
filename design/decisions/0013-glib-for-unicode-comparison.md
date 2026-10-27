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
- The collisions that registration finds are recorded with the entries (`warn`: `nfc-collision`, `case-collision`), and every FULL verification of that revision reports them (2026-10-27), as §A6 counts portability warnings: the run is at most `PASSED_WITH_WARNINGS`, with one plain warning line saying that some names in one folder differ only in upper and lower case or in Unicode normalisation, which a volume or system that does not tell them apart cannot all keep. They are taken from the manifest, not from the scan of the media, so the same revision reads the same on every system. A link's `link-absolute` (decision 0008) is no portability warning.
