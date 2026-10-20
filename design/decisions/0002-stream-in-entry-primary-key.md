---
status: accepted
date: 2026-10-05
amends: HANDOFF §B8
---

# `stream` is part of every entry-keyed primary key from schema v1

An NTFS alternate data stream entry (`t:"s"`, V1.1) shares `p`/`raw` with its owning file and is distinguished only by `stream` (§B3.1, R-43). The v1 tables `files`, `chunks`, `verify_items` and `verify_progress` were keyed on `(…, path, raw_hex)` only, so `report.pdf` and its `Zone.Identifier` stream would collide, and R-92 forbids V1.1 from altering V1 columns to fix it. We added `stream TEXT NOT NULL DEFAULT ''` to all four tables and to their primary keys before schema v1 ships. Non-stream entries use `''`, so V1 behaviour is unchanged.
