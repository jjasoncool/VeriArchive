---
status: accepted
date: 2026-10-08
amends: HANDOFF §B15 (GUI dependency), §B17
---

# The GUI uses the GTK4 C API instead of gtkmm

§B15 names gtkmm-4.0 for the GUI. gtkmm is a C++ wrapper over the same GTK4 toolkit; it was not installed in the development environment and would add a second binding dependency on every platform and CI image. The GUI calls the GTK4 C API directly from C++ (captureless lambdas as signal handlers, `g_idle_add` to return to the main thread). The toolkit, look, behaviour and R-08/R-12 constraints are unchanged.

## Consequences

- Build dependency is `gtk4 >= 4.10` (for `GtkFileDialog`); the GUI target is built only when it is found (`VERIARCHIVE_BUILD_GUI`).
- Packaging (R-121) ships the GTK4 DLLs, schemas and icon theme as before, without gtkmm, glibmm and sigc++.
- `VERIARCHIVE_GUI_SMOKE_TEST=1` builds every view without showing the window, so CI checks the GUI on Windows without user interaction.
- Amended by decision 0046: the GUI is built with libadwaita (≥ 1.5) on GTK4 ≥ 4.14, still through the C API, each API pinned to that version, so that a call of a later one fails the build; the package bundles libadwaita with its Traditional Chinese texts and its license (R-121). The smoke test builds every page of the window (Archives, Devices, Jobs, History, Settings) in either language, and rebuilds it in the other.
