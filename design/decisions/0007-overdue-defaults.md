---
status: accepted
date: 2026-10-08
resolves: HANDOFF Part D D3
amends: R-105 (default of `policy.any_verify_max_age_days`)
---

# Overdue defaults are 365 days (FULL) and 180 days (any); OVERDUE never blocks

V1 has only FULL verification (QUICK/DEEP are V1.1), so the former 90-day "any verify" default meant a FULL read of every archive every three months: about ten hours for an 8 TB HDD. For cold-stored media that frequency gets ignored, and an ignored warning protects nothing. The defaults are `full_verify_max_age_days = 365` and `any_verify_max_age_days = 180`. `OVERDUE` is a flag only: it never blocks an operation and never changes a run result. Revisit the 180-day default when V1.1 makes a QUICK verify take minutes.
