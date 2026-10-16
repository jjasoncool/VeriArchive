---
status: accepted
date: 2026-10-16
amends: HANDOFF §A4 R-09 (`QueryDosDeviceW` on Windows), §B4.3 (what `root_hint` records)
---

# A folder reached through a drive letter made with subst is taken by its real path

`subst S: D:\Data` makes the drive letter `S:` stand for the folder `D:\Data`. The letter is not a volume, and Windows reports no volume for it: `GetVolumePathNameW` on `S:\Photos` returns `S:\Photos\` itself (on `S:\` it fails), and `GetVolumeNameForVolumeMountPointW` then fails with Win32 error 4390. Registering or verifying a folder through the letter stopped with "cannot identify the volume of the root: GetVolumeNameForVolumeMountPointW failed (Win32 error 4390)", which does not say that the letter was the cause.

Finding a volume for the letter would not be enough. An archive is known by its volume and its `root_subpath`, the root's place measured from where that volume is mounted (decision 0001). Through the letter, that place would be measured from `S:\` (or from `S:\Photos` itself, which `GetVolumePathNameW` takes for the mount point), not from the root of the volume that holds `D:\Data`: `S:\Photos` is `Data/Photos` on that volume. The same folder would be two archives, one through the letter and one by its real path.

## Decision

1. **Resolved before the volume is looked up.** When an archive root starts with a drive letter made with subst, the letter is replaced by the folder it stands for, as `QueryDosDeviceW` reports it for the letter: `\??\D:\Data` is `D:\Data`, and `\??\UNC\server\share\dir` is `\\server\share\dir`. When that folder is itself reached through such a letter, it is resolved again. The command line (`register`, `verify --full`, `archive relocate`) and the GUI (Register Archive, Verify Archive) do this to the root the operator gives, where they already make it a path; registration, verification and relocation then see only the real path.
2. **One archive, on its real volume.** The archive is recorded on the volume that holds the folder, with `root_subpath` measured from that volume's root. Through the letter and by its real path it is the same archive: registering it again either way is refused (R-97), and either path verifies it.
3. **`root_hint` records the real path** (`D:\Data\Photos`), never the letter, and so do the root in a registration's plan and in a verification's run record (decision 0022). Reports show that path.
4. **Everything else is unchanged.** Volume letters, network drives, UNC paths and paths that do not start with a drive letter and a separator are not touched. A letter that `QueryDosDeviceW` does not report, or letters that stand for each other in a loop, are left as they are; the volume lookup then fails as before.

`QueryDosDeviceW` reads the drive letters of the caller's logon session, where `subst` makes them, and needs no privilege (R-09).

## Why

**The letter is an alias, the folder is the archive.** A letter made with subst exists only in the logon session that made it, and is gone at sign-out or restart unless it is made again. What identifies an archive must not depend on which name of its folder the operator typed; resolving the letter gives the identity the real path gives.

**`root_hint` is read later.** It is for display (R-45), shown in history and in the list of unfinished operations, often after the letter is gone or in another session. The real path still names the folder as long as its volume keeps its letter. One folder also keeps one hint, and one plan root: `register --resume` finds an interrupted registration by its root, so it is found whichever of the two paths the operator gives.

**Where the path is made.** The command line and the GUI already turn what the operator typed into the root path that the engines receive (made absolute on the command line). Resolving the letter there keeps the engines unchanged: they compute `root_subpath` from the root and the volume's mount point, so a volume lookup that resolved the letter on its own (`IMediaEnumerator::volume_for_path`) would return a mount point the root is not on. The resolution itself is platform code, in `platform/windows`, reached through the platform services that the composition roots already use; on Linux it changes nothing.

Rejected alternatives:

- Recording the letter in `root_hint`: it is what the operator typed, but it names nothing once the letter is removed, and one folder would carry two hints.
- Refusing such roots with a clearer message: the operator would have to look up the folder that Windows already knows.
- A new method on `IMediaEnumerator` that gives the engines the real path: it changes a frozen interface (§B9) and every fake of it, for a step the user interfaces already take when they make the root path.

## Consequences

- Registering or verifying `S:\Photos` reports and records `D:\Data\Photos`; `media list` is unchanged, since letters made with subst never were volumes.
- A folder registered through a letter can be verified after the letter is gone, by its real path or by archive uuid.
- Unit tests resolve paths against sample `QueryDosDeviceW` answers on every OS (volumes, network drives, letters for folders, for whole volumes, through other letters, for network folders, and a loop). On Windows, when `VERIARCHIVE_TEST_SUBST` is `1` (the GitLab Windows job sets it), the CLI end-to-end test makes a letter from `S:` to `V:` with `subst`, registers and verifies a sample folder through it, removes the letter at once, checks that `subst` no longer lists its mapping, and then checks the recorded `root_hint`, `root_subpath` and that the real path names the same archive. The letter appears in the logon session that runs the test, so without the variable the test skips this and says why.
