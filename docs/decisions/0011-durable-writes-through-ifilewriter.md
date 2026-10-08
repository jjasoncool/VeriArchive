---
status: accepted
date: 2026-10-08
amends: HANDOFF §B9 (adds IOutputFile / IFileWriter), §B7 (repository initialisation order)
---

# Durable writes go through `IFileWriter`

R-61 requires manifests to be written to a temporary file, flushed (`fsync`) and renamed, and M1 requires crash-safe repository and config publication. Standard C++ cannot flush a file to stable storage, and `core`/`common` may not call the OS (R-04), so §B9 gains `IOutputFile` (write, then `commit()` = flush + atomic replace; destruction without commit deletes the temporary file) and `IFileWriter` (create, remove). Windows uses `FlushFileBuffers` + `MoveFileExW(REPLACE_EXISTING | WRITE_THROUGH)`; Linux uses `fsync`, `rename`, and an `fsync` of the directory.

## Consequences

- `common` only serialises (`config_to_json`); callers write through `IFileWriter`.
- Repository initialisation order is: directories → schema in one SQLite transaction → `repository.json` → config pin. `repository.json` marks a complete repository, so re-running `repo init` after a crash at any step finishes the job.
