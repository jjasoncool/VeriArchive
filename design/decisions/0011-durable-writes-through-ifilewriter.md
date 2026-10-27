---
status: accepted
date: 2026-10-08
amends: HANDOFF §B9 (adds IOutputFile / IFileWriter), §B7 (repository initialisation order; amended 2026-10-27: `repo init` run again pins a complete repository that was never pinned)
---

# Durable writes go through `IFileWriter`

R-61 requires manifests to be written to a temporary file, flushed (`fsync`) and renamed, and M1 requires crash-safe repository and config publication. Standard C++ cannot flush a file to stable storage, and `core`/`common` may not call the OS (R-04), so §B9 gains `IOutputFile` (write, then `commit()` = flush + atomic replace; destruction without commit deletes the temporary file) and `IFileWriter` (create, remove). Windows uses `FlushFileBuffers` + `MoveFileExW(REPLACE_EXISTING | WRITE_THROUGH)`; Linux uses `fsync`, `rename`, and an `fsync` of the directory.

## Consequences

- `common` only serialises (`config_to_json`); callers write through `IFileWriter`.
- Repository initialisation order is: directories → schema in one SQLite transaction → `repository.json` → config pin. `repository.json` marks a complete repository, so re-running `repo init` after a crash at any step finishes the job.
- Amended 2026-10-27: finishing the job includes the pin. An init stopped between `repository.json` and the pin left a complete repository that no config file pins, and `repo init` run again opened it and stopped there, so every later command said that no repository was configured, and every init after it did the same. Now an init that finds `repository.json` opens the repository and, where it pins (the first use, or `repo init --repository P` while nothing is pinned, §B7.4 step 3), writes the pin and its record of the repository's uuid (R-83 step 5), as an init that creates the repository does; one that does not pin (`repo init --repository P` beside an existing pin, decision 0037) writes no config file. A repository that a lost config file left unpinned is pinned again the same way. Tests: an init stopped before the pin, in the portable folder and at a place named with `--repository`, run again: the repository keeps its uuid, the config file pins it with that uuid as its record, and the next command opens it as pinned; a mutation check that left the pin out of the init that finds `repository.json` failed them.
