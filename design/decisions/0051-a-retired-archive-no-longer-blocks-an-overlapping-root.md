---
status: accepted
date: 2026-11-06
amends: decision 0001 (overlapping archive roots on one volume are refused, but for a retired archive's root), decision 0003 (a retired archive is still matched at its own root, but keeps no other root from containing it or lying in it), HANDOFF §B10 (as decision 0001 amends it), HANDOFF §B17 (Register Archive and the relocation dialog judge it alike before they start)
---

# A retired archive no longer blocks an overlapping root

Decision 0001 refuses an archive root that contains another archive's root on the same volume, or lies in it, and decision 0003 keeps a retired archive among the candidates of matching. Together they left an operator who had registered a folder of a volume, and later wanted to verify the whole volume, with no way on: the whole volume was refused while the folder's archive existed, retired or not, and an archive cannot be deleted. The operator asked whether an archive can be removed for that case, and chose, of the ways offered, that retiring the folder's archive lets the whole volume be registered as a new archive (option A), the folder's archive keeping its history.

## Decision (operator, 2026-11-06)

1. **A retired archive's root no longer refuses a root that contains it or lies in it**, on the same volume or on one of its earlier carriers: a registration (`check_existing_archives`, also a registration over a lost manifest) and a relocation's new root (`relocate`) take such a root, as `register` and `archive relocate` on the command line do. An archive that is not retired still refuses it, with the words of decision 0001.
2. **At its very root a retired archive still decides as before** (decision 0003): a new revision of it is refused, and the same root is registered apart from it only as a separate archive. Matching still finds it at its own root, so it is still verified there, by its folder or its uuid.
3. **Register Archive and the relocation dialog judge it alike before they start**: a volume whose only archive roots are retired archives' folders can be chosen whole, naming them as retired ("Holds ARCH-0001 (H:\Photos, retired)"), and a folder in a retired archive's root is no longer refused.
4. **An archive is never deleted.** The verification history is one hash chain across every archive of the repository (each run's `prev_hash` is the row before it, whatever its archive), which `repo verify-history` checks: removing an archive's runs would break the chain at every later run, of every archive, and look exactly like the tampering it detects. Retiring is the way to stop expecting an archive, and keeps what was verified and when.
5. No change to the manifests, the database or `--json`.

## Why

The operator chose to retire and register anew rather than to let an archive grow to a larger root (which would carry its history on under one archive, but change what a revision is), and was told why an archive is not deleted. Retiring already says that an archive is no longer expected to be verified; holding its root against every other root served nothing then, while a root that is not retired keeps decision 0001's reason: two live archives of one volume whose roots contain each other would read the same files twice and be matched by one root.

Rejected alternatives:

- Deleting an archive with its history: it breaks the one hash chain of every archive (point 4).
- Letting an archive's next revision take a larger root: one archive for the volume, but a revision would no longer verify what the one before recorded, and relocation and matching would have to learn it.
- Lifting decision 0001 altogether: live archives with nested roots would be read twice by each verification of the outer one, and verified as two.

## Consequences

- HANDOFF §B10 (decision 0001's amendment) and §B17.
- GLOSSARY: Archive root.
- The user guides: under "Relocating and retiring an archive", how to go from a folder's archive to the whole volume, and why an archive is not deleted; the note in Register Archive's first step.
- Tests: a folder's archive refuses the whole volume until it is retired, after which the volume registers as a new archive, a folder in it is still refused (the volume's root is not retired), and the retired archive is still verified at its folder; Register Archive's options no longer refuse a folder in a retired archive's root; a volume holding only a retired archive's folder can be chosen whole, naming it as retired, in English and Chinese. The relocation dialog's rule and the relocation engine's are reviewed, not tested on their own.
