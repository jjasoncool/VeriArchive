# VeriArchive user guide

VeriArchive records file integrity information for archives so you can later check for modified, corrupted, deleted or added files. It only **detects** changes; it does not repair files, restore them or create backups.

The whole workflow is two steps:

1. **Register**: read every file of a folder or volume once and store its fingerprints (BLAKE3 and SHA-256) as the **baseline**.
2. **Verify**: later, re-read every registered byte from the disk, compare it with the baseline, and report what changed.

This guide uses screenshots of the graphical interface (`veriarchive-gui.exe`) to explain how to create a repository, register and verify archives, view results and history, save verification reports, relocate archives and cross-check copies against their originals. Most sections cover the GUI first, followed by the corresponding command-line (`veriarchive-cli.exe`) instructions.

### Sample data in the screenshots

The screenshots use sample files and substitute device information so that no real devices or private data appear. All sample archives are folders on one volume, under `D:\va-sample`:

| Folder | Archive label | Purpose |
|---|---|---|
| `原始資料` | `ARCH-0001` | The original archive |
| `備份資料` | `ARCH-0002` | A copy of the original archive |
| `舊版封存` | `ARCH-0003` | An archive registered by an older version, without chunk hashes (see "Adding chunk hashes to an archive registered earlier") |
| `新的資料` | `ARCH-0009` | The source used in the Register Archive screenshot |

When generating screenshots, the program substitutes the following information:

- Volume label, device model, serial and capacity: `SAMPLE-VOLUME`, `SAMPLE-DEVICE`, `SAMPLE0001` and 1.0 TiB.
- Devices page and dialogs: a fixed set of sample devices and volumes, such as `SAMPLE-DEVICE-01` and `/media/SAMPLE-VOLUME-1`, instead of this computer's actual devices.
- Device's own serial and SMART: `SAMPLE-OWN-SERIAL-01` and simulated SMART data, without requesting administrator permission from Windows.
- Computer name: `SAMPLE-HOST`.

Times, durations and read speeds are the actual measurements from the run used to generate the screenshots.

In normal use, you would usually register a whole volume, such as `H:\`, from the attached volumes (see "Registering an archive"). Archives then shows the actual volume label, model and capacity; the other operations work as shown.

[繁體中文版](user-guide.zh-TW.md)

## Installing (Windows)

VeriArchive is a portable program that needs no installation:

1. Make a folder for the program, for example `D:\VeriArchive`, and unzip `VeriArchive-<version>-windows-x86_64.zip` into it; nothing else (such as MSYS2) is needed.
2. Run `bin\veriarchive-gui.exe` in that folder (`D:\VeriArchive\bin\veriarchive-gui.exe` in the example).

The zip holds no folder of its own: its `bin`, `lib` and `share` go straight into the folder it is unzipped into.

- `bin/veriarchive-gui.exe`: the graphical interface
- `bin/veriarchive-cli.exe`: the command line
- `bin/veriarchive-device-identity.exe`: the device identity reader, which the window starts only when you ask it to read a device's own serial behind its enclosure or the SMART of the attached devices, with the administrator permission that Windows asks you for (see "A device moved to another enclosure" and "SMART"); it is not meant to be opened by hand
- `lib/` and `share/`: image loaders, icons, settings and translations GTK needs; `share/doc/VeriArchive/`: this guide and a short README; `share/licenses/`: licenses of the libraries the program uses

To update VeriArchive:

1. Close the program.
2. In Windows Explorer, right-click the new version's zip and choose "Extract All...".
3. Set the destination to **the existing program folder** (`D:\VeriArchive` in this example), then replace the files when prompted.

Explorer defaults to a new folder named after the zip. If you extract there, that copy of the program will not find your existing records. The zip contains neither `bin\data` nor `bin\config.json`, so replacing the program files does not overwrite your records. **Do not delete the old folder first.**

To check the running version, look for "Program version" under About at the bottom of Settings, or run `veriarchive-cli --version` or `veriarchive-cli repo info`.

### Moving the program folder

On first use, the repository (`data`) and the config file that remembers it (`config.json`) are created in the program's folder (`bin` in the zip), and the config file records the repository relative to that folder: `"repository": "data"`. Program, configuration and repository are therefore one folder. You can move or copy the whole unzipped folder, to another drive, to another computer, or onto a USB disk that gets another drive letter, and it keeps using its own repository. Backing up that folder also backs up the repository.

> **Back up the whole program folder regularly, especially `bin\data`.** Losing the repository loses the verification history with it.

- Copying the folder copies the repository; from then on each copy records its own verification history. Keep one of them as the folder you work with.
- A repository outside the program folder (`VeriArchive` in your home folder, or a place chosen with `--repository`) is remembered by its full path and does not move with the folder.
- A folder set up with an earlier version remembers the full path. To make it movable, open `bin\config.json` in a text editor and change the value of `"repository"` to `"data"`.

### After updating VeriArchive

A new version may need to upgrade the repository database. Earlier versions cannot open the upgraded database.

Before upgrading, VeriArchive saves a copy beside the database in the repository folder. The filename includes the old schema version and the date, for example `veriarchive-schema-1-before-upgrade-2026-10-17.db`.

The program reports the copy's location as follows:

- **CLI**: a note on standard error, such as `note: the repository database was upgraded from schema version 1 to 3, ...`.
- **GUI**: a notice on Settings, which opens first after the upgrade.
- **Later use**: Settings and `repo info` list each copy's location and purpose for as long as it remains in the repository folder. The notice reads `Copy of the database from before an upgrade: ... (an earlier version of VeriArchive can open only this copy; you can delete it once you no longer need that version)`. In `repo info --json`, these copies appear under `database_copies_before_upgrade`.

The copies are also listed after an error if the repository cannot be opened, including when a newer version has already upgraded it. Use the pre-upgrade copy if you need to return to the earlier version.

Copying a large database can take time. The GUI shows "Upgrading the repository database..." and an animated progress bar until the pages are ready. A second window opened during the upgrade waits with the same message. If the upgrade fails, the window explains why.

**Keep the copy until you are sure you will continue using the new version.** Verification history exists only in the database, and the earlier version can open only the pre-upgrade copy. If the copy cannot be created, for example because the disk is full, VeriArchive leaves the database unchanged and stops with exit code 11. Free up space, then open the repository again.

## The window

The window has a sidebar on the left and, beside it, the page you choose there:

- **Archives**: an overview of your archives, their list, and the details of the archive you choose. Here you verify, cross-check, relocate, export and retire an archive.
- **Devices**: the devices attached to this computer now, the health each reports, their volumes and the archives on them.
- **Jobs**: the registrations, verifications, cross-checks, relocations and exports started in this window, and the operations that have not finished.
- **History**: every recorded verification of every archive.
- **Settings**: the repository, the interface language, the verification policy and the program's version.

Above the sidebar, **+** opens Register Archive, as the Register Archive button in Archives does; beside it is the name of the repository's folder. At the top right of every page, the **Jobs** button shows how many jobs run or wait and how far they are, and with a pause sign how many unfinished operations have stopped, if any; it opens a list of them. A short message at the foot of the window says when an operation started or ended, often with a button such as View result or Details. Questions come in a small dialog over the window, with Cancel as the default answer. In a narrow window the sidebar and the page show one at a time, and Archives shows either its list or an archive's details.

## Interface language

![Language list](images/en/07-language.png)

The interface is available in English and Traditional Chinese. The first setting found wins:

1. `--lang en` or `--lang zh-TW` on the command line
2. the `VERIARCHIVE_LANG` environment variable
3. `"language"` in the config file
4. the Windows display language
5. English

In the GUI, the Settings page has the row "Language / 語言" under Interface (English / 繁體中文). The choice is saved in the config file and the window is rebuilt in the new language at once; the page shown, the window size, the result pages that are open and the jobs with their reports are kept, and the reports are shown in the new language. It cannot be changed while a job waits or works: the row says so.

`--json` output, exit codes and the codes inside JSON (`PASSED`, `Modified`, ...) are **always English**, so scripts behave the same in every language.

## First use: create the repository

![Settings page](images/en/01-settings.png)

The **repository** holds every manifest (baseline) and the verification history.

The first time you open the program, it shows the Settings page with "No repository is open." and the button **Initialise repository**. Press it, and VeriArchive creates the repository in the program folder. Once it is created, the page shows, in groups:

| Group | Item | Meaning |
|---|---|---|
| Repository | RepositoryRoot | the folder the repository is in |
| | Selected by | how the repository in use was chosen, for example "pinned" |
| | repository_uuid | this repository's identifier |
| | Config file | the config file in use |
| | Copy of the database from before an upgrade | shown only when the repository's database was upgraded and the copy kept before the upgrade is still there: where the copy is and what it is for, namely that an earlier version of VeriArchive can open only this copy, and that you can delete it once you no longer need that version (see the upgrade described above). Once the copy is deleted, it is no longer shown |
| Interface | Language / 語言 | see "Interface language" |
| Verification policy | Overdue after | after how many days without a FULL verify, or without any verification, an archive is marked OVERDUE |
| | Strict metadata | when on, a different modification time or attributes also fail a verification |
| | Read SMART when a FULL verification starts | Windows only, off by default: on, each FULL verification started in the window first reads the SMART of the attached devices, which Windows asks you to allow with administrator permission each time, and records it with the run (see "SMART") |
| | Trusted signing keys | signed manifests are not part of this version |
| About | Program version | the version of VeriArchive that runs, with what it detects |

A warning, such as that the program runs elevated, shows in the Repository group. The reminder under that group is important:

> **Back up RepositoryRoot.** Losing it loses all verification history; manifests can be recovered only from media copies, if they were written.

CLI: `veriarchive-cli repo init`

By default the repository is created in `data` next to the program, or in `VeriArchive` in your home folder when the program folder is not writable. Its location is remembered in the config file and used from then on. To choose it yourself: `veriarchive-cli --repository D:\VeriArchiveRepo repo init`.

If a configured repository is unavailable later (for example an external disk is not attached), VeriArchive stops with an error that names the repository's folder and the config file that names it; it never falls back to another location.

## Registering an archive

![Register Archive](images/en/03-register.png)

Registration reads every file of a volume once and records their fingerprints (BLAKE3 and SHA-256) as the **baseline** for later verifications and cross-checks. To register only part of a volume, you can register a single folder.

On the screen, "folder or volume" means these two. "Volume" is the program's word for a place such as `H:\`: a data device usually holds a single volume, so registering `H:\` registers the whole device; a device divided into several volumes is registered once per volume.

Press **+** above the sidebar, or **Register Archive** in Archives. The dialog Register Archive has three steps:

1. **Choose the source.** The attached volumes are listed, each with its label, where it is attached (`H:\`, or a GUID path for a volume that has no drive letter), its device, bus, capacity and the space in use. Choose a volume to register it as a whole. A volume that already holds the roots of archives cannot be registered as a whole, as the roots of archives on one volume may not contain each other, except retired ones (see "Relocating and retiring an archive"): its button "Choose a folder in H:\..." chooses a folder in it. To register a folder, press "Choose a folder...", or type or paste its path, for example `H:\Photos`, also a volume's GUID path. Press **Next**: VeriArchive reads what is there, and a folder or volume that cannot be registered (see below) is refused on this page, saying why.
2. **Options.** Only the options that apply to the source chosen are shown (the table below): the label, and, where it applies, a new revision or a separate archive, the resume of an interrupted registration, the media copy, which archive this is a copy of, and recording the device's own serial. The label is optional: `ARCH-` followed by four or more digits, for example `ARCH-0004`; left empty, the archive takes the repository's next label, which the page names, skipping the numbers that labels in the repository already have. Press **Next**.
3. **Confirm** says what the registration will read, the label, whether a media copy is written, what the archive is a copy of, and, where it applies, whether the device's own serial is read and recorded. Press **Start registration**.

The dialog closes and the registration runs as a job (see "Jobs"): a message at the foot of the window says that it started, with Details; Jobs and the Jobs button show it, with its share done, speed and time left (see "Reading the progress" below), and on a volume that holds no archive Devices shows "Being registered: <root>" in place of Register this volume; "Stop" stops it at any time. When it ends, the message says "Registered as ARCH-0009", with View archive, which shows the archive in Archives; its report is in the job's Details.

The screenshot shows the Options page for the sample folder `D:\va-sample\新的資料`, which no archive holds yet, with the label `ARCH-0009` typed in (the introduction says why it is a folder).

| Option | When to use it |
|---|---|
| Create a new revision | Shown where an archive is registered at this folder or volume already. You changed its content on purpose: make a new baseline. In Archives, "Create a new revision..." in the menu of the archive's details opens Register Archive with this chosen |
| Register as a separate archive even though this folder or volume is registered | Shown where an archive is registered there already, and you want another archive of it, as for a sector-by-sector clone of a device (see "Cross-checking an archive and its copy") |
| Resume the interrupted registration | Shown where a registration of this folder or volume was stopped halfway; it is then the only way on: it continues where it stopped, without reading again the files already hashed ("Resume" on its row under Unfinished operations in Jobs does the same). If the program that ran it still runs on this computer, it stops and says which one. A registration that an older version of VeriArchive left cannot be checked so: VeriArchive first asks the same question as Archives, saying what happens if that version still runs it; make sure that the older version's window or command has been closed, then press "Resume" in the question (see "Unfinished operations" below). A registration that can never be finished can be given up with "Abandon..." under Unfinished operations in Jobs |
| Also write a manifest copy to the media (.veriarchive) | Off by default. Leaves one more copy of the manifest on the registered volume (the `.veriarchive` folder). **Off, registration writes nothing at all to the archive** |
| Copy of which archive (optional) | This folder or volume is a copy of another archive: records it, for display only (see "Cross-checking an archive and its copy"). "Not a copy" by default |
| Record the device's own serial | Shown on Windows where the source is on a device behind a USB enclosure that does not pass the device's own serial on (the system sees only the enclosure's serial), and the registration resumes none. Off by default. On, **Start registration** first reads the device's own serial with the device identity reader, which Windows asks you to allow with administrator permission, and records it with the registration; when the device is later moved to another enclosure, VeriArchive then tells it by that serial (see "A device moved to another enclosure"). If you answer No, or the enclosure does not pass the command on, Confirm says so, and **Start registration without the serial** registers without it. Where the window still keeps a read of this device's own serial, made on the Devices page or in the question of a FULL verify (it forgets it once the device is detached or changed, or the window stopped watching the devices), Options shows "Device's own serial: ... (read)" in place of the switch, and the registration records that serial without asking Windows again. The report then says "Device's own serial recorded: ... (the archive's confirmed device from now on)" |

On the Devices page, "Register this volume" opens Register Archive with that volume chosen.

Keep in mind:

- **Do not modify the folder or volume while it is being registered**; otherwise the baseline records content that is changing.
- An HDD reads about 150 to 250 MB per second. Registering a volume of several TB can take more than ten hours.
- Several registrations can run at once: each is a job, and jobs that read the same device wait for one another (see "Jobs").

CLI:

```text
veriarchive-cli register K:\ --label ARCH-0012
```

Options:

| Option | Purpose |
|---|---|
| `--write-media-copy` | Also write the manifest onto the disk (`.veriarchive` folder) as a media copy. By default nothing is written to the archive. |
| `--new-revision` | The folder or volume is already registered and its content changed on purpose: create a new revision (a new baseline). |
| `--force-new-archive` | The folder or volume is already registered, but register it as a separate archive. |
| `--resume` | Continue an interrupted registration without re-reading hashed files. While the program that ran it still runs on this computer, it stops and says which one. A registration that an older version of VeriArchive left cannot be checked so: the command says what happens if that version still runs it and stops (exit code 12) without changing anything; make sure that no window or command of that version still runs it, then add `--confirm` (see "Unfinished operations" below). Scripts that resume registrations of this version are not affected. |
| `--io hdd` or `--io ssd` | Reading strategy: `hdd` reads sequentially, `ssd` in parallel. Usually detected automatically. A device whose type is reported wrongly, as by some USB enclosures, can be set once in the config file by its serial, which `veriarchive-cli media list` shows, and that applies on the command line and in the window alike: `"io": {"rotational_override": {"<serial>": "hdd"}}` (or `"ssd"`). `--io` wins over it. |
| `--lost-manifest <uuid>` | The last resort for an archive whose latest manifest is lost: register its next revision from what the folder or volume holds now (see "Archives and OVERDUE" below). It first only says what it would do; add `--confirm` to do it. |

Registering an already registered folder or volume again stops with exit code 12 and asks you to choose `--new-revision` or `--force-new-archive`.

A drive letter made with `subst` (such as `S:` for the folder `D:\Data`) is not a volume. A folder reached through one is registered by its real path: registering `S:\Photos` records `D:\Data\Photos` on the volume that holds it, and the reports show that path. It can be verified through the letter or by its real path.

VeriArchive registers and verifies folders on volumes attached to this computer. A folder on a network share is refused: a network path (`\\server\share\Photos`), a folder on a drive letter mapped to a share, and on Linux a folder on a network filesystem (NFS, SMB, sshfs) or on a share that GNOME or KDE opened (choosing such a share in their file dialogs, "Choose..." included, gives a folder below `/run/user/<uid>/gvfs` or `/run/user/<uid>/kio-fuse-...`). A phone or camera that GNOME shows there is refused as well. On Windows, a root written with `\\?\` in front (`\\?\D:\Photos`) is refused too: write it as `D:\Photos`. A volume that has no drive letter is the exception: Windows names it only by its GUID path, which `mountvol` lists, such as `\\?\Volume{...}\`. Type that path exactly as `mountvol` lists it, for the whole volume (`\\?\Volume{...}\`) or a folder on it (`\\?\Volume{...}\Photos`): it is registered, verified and relocated to like any other root, and is the same archive as through a drive letter the volume gets later. `verify --full <uuid>`, or FULL verify in the archive's details, finds an archive on such a volume too. For each refused root, `register`, `verify --full <root>` and `archive relocate` stop with exit code 10 before anything is read.

A folder or volume that holds VeriArchive's repository (by default `bin\data` in the program folder), or a folder inside the repository, is refused as well (exit code 10): the repository changes while the folder is read, so its verifications could never pass. Keep the program folder, with its repository, outside the folders and volumes you register.

Files that cannot be read at registration (permission, media errors) are recorded without hashes and are never reported as verified.

### Reading the progress

While files are hashed (registering, verifying, relocating), the job's bar in the window (on Jobs, in the Jobs button's list, and on the job's card in an archive's details) and the CLI show one line, for example:

```text
hashing: 2.16 TiB of 9.44 TiB (23%) · 240 MiB/s · about 8-9 hours left
```

The CLI writes it to stderr, and not at all with `--json`.

On the command line, Ctrl+C or Ctrl+Break stops a registration, verification or relocation safely, and closing its window, signing out or shutting down the computer does the same: what it has done is kept, and it can be resumed (see "Unfinished operations" under "Jobs"). After Ctrl+C its message gives the command that continues it, with the folder or volume written as you can type it.

- **The share done is exact**: it counts bytes, so a folder of large videos and a folder of small photos move it alike. It shows 100% only once the last file is done. Scanning comes first and shows only the entries found, as the total is not known yet.
- **The speed** is the average of the last three minutes.
- **The time left is an estimate**, shown as a range. The low end of the range picks the unit: minutes while it is under an hour, hours while it is under two days, then days ("about 55-175 minutes left", "about 47-96 hours left"). It counts each file's fixed cost (opening it; on an HDD, moving the head) as well as its bytes, so a stretch of small files does not make it jump. It reads "estimating time left" during the first minute and after minutes without progress, and the range widens when the speed varies or keeps dropping (an HDD reads its inner tracks more slowly). A pause, such as the computer sleeping, does not count as slow reading. Once every file is hashed, the line shows no time left while the results are written.

### While an operation runs

While the window runs an operation (a registration, verification, cross-check, relocation or export, abandoning an unfinished operation, retiring an archive, or upgrading the repository database), the computer does not go to sleep on its own: VeriArchive asks the system to stay awake until the operation ends. The system lists the request as "A long operation is running" (on Windows `powercfg /requests`, run as administrator; on Linux `systemd-inhibit --list`). The display may still turn off and the screen may lock; neither stops the operation. A sleep you ask for still happens: choosing Sleep, pressing the power button, or closing a laptop's lid. On Linux this needs logind (systemd); without it the computer sleeps as its settings say.

When an operation of the window ends while another window is in front, VeriArchive says so in a notification of the system with the first lines of its report, for example "FULL VERIFY PASSED" and the archive's label and revision. While the VeriArchive window is in front there is no notification: the report is in front of you.

The command line does neither. It often runs in scripts, in scheduled tasks and on servers, whose power settings decide. On a computer that sleeps when idle, set its power settings not to sleep (sleep after: Never) before a long command; on Linux, run the command under `systemd-inhibit`, for example `systemd-inhibit veriarchive-cli verify --full /media/archive`.

### Device alerts

VeriArchive watches for signs that a device may be failing. Such a sign, a **device alert**, is shown in red and led by ⚠, beside the result, which it never changes: a FULL VERIFY PASSED stays PASSED, with the same exit code.

- **The device's own health.** When a registration, a verification or a relocation starts, VeriArchive asks each device of the volume what it reports of its own health, without administrator rights: on Windows, the health that Windows keeps of the disk (what `Get-PhysicalDisk` shows); on Linux, what udisks2 read of its SMART data. A device that reports a warning or a failure, such as "predicting failure" or "bad sectors", gives a device alert at once: in the window on the job's card and its row in Jobs, above the progress, on the command line before the progress. The operation goes on; stop it if you would rather copy the archives off that device first. Many USB enclosures pass no health on: the report then says "cannot be read", which is no alert. The Devices page and `veriarchive-cli media list` show each device's health at any time.
- **Reading slower.** A FULL verification times how fast it reads each hundredth of the archive's content, and compares that with the last FULL verification of the same archive on the same device whose speed was usual. The whole read 30% slower or more, or a part of the content 50% slower or more, is a device alert, which says where ("from 0% to 12% of the content, 55% slower"). Another program reading the same device, or another device read through the same port or hub at the same time, also makes reading slower: to tell, verify the device again with nothing else reading. Every FULL verification's report gives its read speed ("Read speed: 182 MiB/s; the last FULL verification of this archive on this device, which finished 2026-11-01 14:03:00 (UTC+8), read 180 MiB/s"). A verification is compared only with one of the same content on the same device: the first FULL verification with this version of VeriArchive, after moving the disk to another enclosure, after a relocation, or after a new revision with other content, is compared with nothing.
- **SMART** (Windows, in the window). What Windows reports of a disk without administrator rights stays "Healthy" until the device itself predicts its failure. The counts that tell of a device going bad earlier, such as sectors it had to reallocate or cannot read, are its SMART data, which Windows gives only with administrator permission. Whenever the window asks for that permission (Read the SMART of all devices... on the Devices page, a read of a device's own serial, Record the device's own serial in Register Archive), the device identity reader reads the SMART of every attached device too, and the repository keeps each read. The latest read of a device gives a device alert where an attribute is at or below the threshold its maker set, where reallocated sectors, pending sectors, offline uncorrectable sectors or reported uncorrectable errors are above zero, or, for an NVMe device, where it reports a critical warning, its available spare is below its threshold, or it counts media errors. Each is compared with the read before of the same device: "Pending sectors increased (0 → 8)", "Reallocated sectors: 8 (as in the last read)". An alert stays while its count stays above zero, as reallocated sectors do for good; pending sectors back at zero alert no more ("Pending sectors cleared (8 → 0)", a gray hint), unless the device reallocated them, which raises its reallocated sectors. UltraDMA CRC errors that rose since the read before are a gray hint only: they usually come from a cable or an enclosure, not the device. Temperature is shown, never judged.

Archives then marks the archive's row with "⚠ device reports a problem" or "⚠ reading slower", and its details and the run's result page show the alert, until a later verification gives none; History marks the run in its row. A SMART alert marks the row with "⚠ SMART: ..." and the details with when it was read, until a later read of that device gives none; it is counted under Device alert in the overview too. What to do: copy the archives on that device to another device soon, and verify the copies.

### Jobs

![Jobs](images/en/08-jobs.png)

Every registration, verification, cross-check, relocation and export started in the window runs as a **job** in the background, so you can start the next one at once: register a second device while the first is verified, or cross-check two archives meanwhile.

- **Jobs that read the same device wait for one another**, in the order you started them, and say which one they wait for ("Waiting: Register H:\ reads the same device"). A device read in two places at once reads both slower than one after the other. Jobs of different devices run at once; a cross-check and an export read no archive and never wait.
- The **Jobs** page lists every job of the window in three groups, Running, Waiting and Ended, newest first, and below them the unfinished operations (see below): what it does, how far it is (share done, speed, time left), its device alert if any, and **Stop**. **Details** opens a dialog with the same and, once the job has ended, its report; an ended verification has **View result** too, which opens its result page. **Clear ended jobs** removes the ended ones from the page.
- The **Jobs** button at the top right of every page shows how many jobs run or wait and, with a pause sign, how many unfinished operations have stopped (as the Jobs row of the sidebar does); it opens a list of the jobs that run or wait and of the last three that ended, with "See all jobs".
- A verification or a relocation of an archive also shows as a card in the archive's details in Archives, with its progress, Stop and Details.
- **Stop** asks nothing: the job ends, and what it did is kept. A stopped registration, verification or relocation is listed under Unfinished operations on the same page, where "Resume" on its row goes on with it (a stopped verification or relocation also in the archive's details), without reading again what it read: Stop works as a pause would, except that the file it was reading is read again from its start.
- **Closing the window while jobs run asks first.** "Stop them and close" stops every job, each resumable; "Keep the window open" lets them go on.
- While a job works on an unfinished operation, that operation cannot be abandoned or resumed: stop the job first.
- For two devices at once, connect them to different ports, or to different USB controllers: two devices behind one USB hub share its speed.

The screenshot shows the jobs that were run to make the screenshots, ended: the registration of the sample folder `新的資料`, the FULL verification of `原始資料` and the cross-check of the two sample archives. No operation was unfinished then, so the page has no such group.

### Unfinished operations

Below the jobs, Jobs lists "Unfinished operations" while there are any: the registrations, verifications and relocations that have not finished (also those started from the command line; a retired archive can still be verified, and its verifications are listed too, except for a retired archive whose manifest is lost: its interrupted verifications can never be resumed, so they are not listed), each with its label, root, operation, progress and state. Pointing at the label shows the archive's UUID, and pointing at the state shows the run's id. Where the state is "stopped; it can be resumed", the operation can continue from where it stopped, without reading again the files already read:

- Press **Resume** on the row: for a verification, a registration and a relocation alike. This works wherever the operation was started, in this window, in another window or on the command line, and whether or not the program was closed after the interruption (for example by a restart). The operation runs as a job, shown above on the same page (a verification or relocation also on its card in the archive's details); "Stop" stops it if needed. The archive's details show a stopped verification or relocation of it too, with **Resume**. A verification finds the archive on the attached volumes by its UUID, so a volume that got another drive letter is no problem; but if more than one attached volume holds the archive (for example a device and a sector-by-sector clone of it), it cannot tell which one the run read, so it stops and names their roots: detach the other volumes and press "Resume" again, or continue at the root that the run read with `veriarchive-cli verify --full <root> --resume <RUN_ID>` on the command line. A registration reads its own folder or volume again, and a relocation its new root, the path the row shows; where the new carrier is attached at another drive letter or mount point now, "Relocate to a new carrier..." in the archive's details resumes the relocation at the root you give (see "Relocating and retiring an archive"). "Resume" is exactly the command-line resume (`verify --full <uuid> --resume <RUN_ID>`, `register <root> --resume` or `archive relocate <uuid> <root> --resume`) and takes no option: it writes no media copy, and uses strict metadata only where Settings shows "On" in the row "Strict metadata" (`strict_metadata` set under `policy` in the config file). To write a media copy when you resume a registration, choose that folder or volume in Register Archive: its options then offer "Resume the interrupted registration" beside the media copy.

A stopped registration that can never be finished, for example because its media failed, can be given up there with "Abandon..." (CLI: `veriarchive-cli archive abandon-registration <uuid> --confirm`): its saved progress is lost and it can never be resumed, while recorded revisions and history stay. A stopped relocation can be given up there the same way, for example when its new carrier failed (CLI: `veriarchive-cli archive abandon-relocation <uuid> --confirm`): its saved progress is lost and it can never be resumed, while the archive, its revisions and history stay. So can a stopped verification that adds chunk hashes (see "Adding chunk hashes to an archive registered earlier" below). A stopped FULL verification that adds none has "Abandon..." too, for example when you no longer mean to verify that archive or its device failed: it asks first, then its saved progress is deleted, it can never be resumed and it is no longer listed here, while the archive, its revisions and its history, the INCOMPLETE run included, stay. Without abandoning it, the next FULL verification started for that archive supersedes it, and it is no longer listed either. Some states ask you to abandon the operation, such as "new carrier differs; abandon it": the relocation found that what the new carrier holds differs from the archive, so it can never finish; abandon it, copy again, then relocate again. A registration or relocation of an archive that was retired since reads "archive is retired; abandon it": a retired archive takes no new revision, so it can only be abandoned.

Operations that still run, or may still run, have no "Resume", except a registration that an older version of VeriArchive left: that version never records when it last worked, so once it has made no progress for 3 minutes its row reads "no progress seen for N minutes; it may have stopped", and then has "Resume" and "Abandon..." as well. VeriArchive cannot tell whether that older version still runs it: the older version also makes no progress for minutes while it reads a large file or scans the folder or volume. So before you press either, make sure that no window or command of the older version still runs that registration. If one does, and it finishes after "Resume" has recorded the revision, it replaces the manifest that the resume recorded with its own: from then on every verification of the archive fails on that manifest, registering a new folder or volume and relocating stop on it until the archive gets a new revision, and unless a backup of the repository holds the manifest that the resume recorded, it is lost (see above). That is why "Resume" on such a row asks first, "Resume the unfinished registration of ...?": it says all this and asks you to make sure that the older version's window or command has been closed, and resumes only if you press "Resume" there; "Cancel" changes nothing. Register Archive asks the same when its options resume such a registration, and so does `register <root> --resume` on the command line, which says it and stops until you run it again with `--confirm`. A stopped registration's "Resume" does not ask: the registration has said that it stopped, or it has recorded when it last worked or which program runs it, and its version cannot record its own manifest over the one that the resume records. "Abandon..." does not stop the older version either: it may still record the revision. While a job of the window works on the operation, "Resume" and "Abandon..." ask you to stop that job first, or to wait for it to end.

`archive list` and `archive show` print an archive's unfinished registration, its unfinished relocation and its unfinished verification that adds chunk hashes as one more line each under the archive: the revision, the folder or volume it reads, what it has done so far and its state, in the words of Jobs (for example "stopped; it can be resumed" or "new carrier differs; abandon it"). As in Jobs, the unfinished verification of a retired archive whose manifest is lost is left out.

How many of them have stopped shows with a pause sign on the Jobs button at the top right of every page and on the Jobs row of the sidebar, and the overview of Archives says "N unfinished operations have stopped: see Jobs".

## Archives and OVERDUE

![Archives](images/en/02-archive-list.png)

The Archives page, or `veriarchive-cli archive list`, shows for each archive:

- **Last FULL verify**: result and date of the last complete read
- **Last verify**: result and date of the last verification of any mode
- **OVERDUE**: no FULL verify for 365 days or no verification for 180 days (adjust under `policy` in the config file)

At the top of Archives is an **overview**:

- Its first line says how many archives there are and how many need attention: OVERDUE, never FULL verified, last FAILED, with a device alert, or with a manifest that cannot be used (none retired). The next says how many are attached now, and which archive was FULL verified last and how long ago.
- **"Verify the attached archives that need it (N)"** starts a FULL verification of each archive that is OVERDUE, never FULL verified or last FAILED and is attached as recorded; those on one device run one after another. Pointing at the button names them. An archive whose device is seen behind another enclosure (which is asked about first, see "A device moved to another enclosure"), or that has a job or an unfinished operation, is not among them. Beside it are Register Archive, Cross-check... and Refresh.
- A row of tiles: how many archives are All, "Passed, not overdue", OVERDUE, "Never FULL verified", "Last result FAILED" and "Device alert". **Choosing a tile lists only those archives below**; All lists every archive again. A retired archive counts under All only. The arrow at the right folds the tiles away or shows them; folded, the list shows every archive.
- Where any unfinished operation has stopped, "N unfinished operations have stopped: see Jobs".

Below the overview, Archives lists the archives, and beside the list shows the details of the archive you choose. Each row of the list is an archive:

| In the row | Meaning |
|---|---|
| The dot | the state in its colour: a failure, a device alert or a manifest that cannot be used; warnings or OVERDUE; never verified; or passed |
| Label, volume label | the archive's label, and the volume label that its current revision records |
| The line below | where the archive is attached now: its folder or volume (in the screenshot `D:\va-sample\原始資料`), "Not attached", "The enclosure differs from the one recorded" (see "A device moved to another enclosure"), or "The device differs from the one recorded" (see "Devices") |
| Model · capacity · Last FULL verify ... ago | the device model and capacity that the archive's current revision records, those of the carrier it was read from (after a relocation, the new carrier), and how long ago the last FULL verify ended |
| The result | the result of the last FULL verify, PASSED, PASSED_WITH_WARNINGS or FAILED, or "Never verified"; beside it OVERDUE, "⚠ device reports a problem", "⚠ reading slower", RETIRED and REGISTRATION_INCOMPLETE (its first registration has not finished), where they hold |

The details of the archive show:

- at the top, its head: its label, the result of its last FULL verify, the menu **⋯**, its volume and root, **FULL verify** and **Cross-check...** (see "Verifying an archive" and "Cross-checking an archive and its copy"). The head stays in view while you scroll the details, so you always see which archive they are of. The menu holds Create a new revision..., Copy archive UUID, Export manifest..., Relocate to a new carrier... and Retire archive... (see the sections below); each item says under it what it does, or why it does not apply to this archive;
- a banner with the result of the last FULL verify with its mode, for example "FULL VERIFY PASSED", how long ago and when it ended, the last verification of another mode where that is later, and OVERDUE with the limit that was passed; beside it **View result**;
- the device alerts of its last verification, the card of a verification or relocation of it while one waits or works, and a notice with **Resume** where a verification or relocation of it stopped;
- **Device**: the volume, capacity, model, bus and serial that its current revision records, the health and the read speed of its last verification, and where it is attached now ("Attached volume", "Attached at"); where its enclosure differs from the one recorded, "Enclosure serial" with the serial recorded and the one seen now;
- **Content**: its root, files and size, its revision and when that was recorded, whether it has chunk hashes, and its archive UUID;
- **Copies**: the archive it is a copy of and those recorded as its copies, each with Cross-check...;
- **Recent verifications**, each leading to its result page, and a link to all of them in History.

A result always comes with its mode and its time, because it shows the state only **at that time**. The screenshot shows the sample archive `ARCH-0001` chosen, never verified yet.

An archive whose manifest in the repository cannot be used (missing, unreadable or changed) is listed too, with a line that says what is wrong and how to fix it (in Archives, in red under its row), and `archive list` then exits with code 2 (not for a retired archive whose manifest is lost, see below). Verifying that archive (by its uuid, or with FULL verify in its details) records the failure in its history. A manifest that is missing, unreadable or changed is put back with `veriarchive-cli import <folder>/r0002.vam` (the file of that revision), from a backup of the repository, from an export of that revision, or from its media copy (in the `.veriarchive` folder on the archive's media, where registration wrote one): the import checks that the file is the one the repository recorded, and puts it back.

If none of these holds the manifest of the archive's latest revision, the manifest is lost. Where its first line cannot be used (the manifest is missing, its first line cannot be read, or its first line has changed), registering a new folder or volume and relocating stop on it until it is back, or until the archive is retired (below), as finding which archive a folder or volume holds reads the first line of every archive's latest manifest; a new revision of another archive's folder (`register --new-revision`, or Create a new revision in Register Archive) is still registered where the folder is plainly that archive's, on the volume and the device its manifest records. A manifest changed only in its body stops neither, though verifying its archive fails on it. The last resort is to register the archive's next revision from what its folder or volume holds now: `veriarchive-cli register <root> --lost-manifest <uuid>`. Without `--confirm` it only says what it would do (exit code 12); run it again with `--confirm` to do it. What the folder or volume holds then becomes the new baseline without being compared with the lost revision, so damage since that revision was recorded would not be found. The archive keeps its verification history and never goes back to an earlier revision. Where the lost manifest's first line cannot be used, the lost revision leaves its list of revisions; a manifest changed only in its body stays listed, as an earlier revision. It refuses a manifest that is not lost (one that can be used, or one where only the index differs: rebuild the index), a root that belongs to another archive (a folder containing another archive's folder or inside it, on the volume and the device that archive's manifest records), and a root whose `.veriarchive` folder still holds the lost manifest as a media copy (import that file instead). It takes another archive's very folder, on the volume and the device that archive's manifest records, or on a volume that may be that archive's or a sector-by-sector clone of it (the same filesystem on another device, or no device serial to compare), but names that archive before you confirm: VeriArchive cannot tell whether that folder was registered on purpose as both archives (`--force-new-archive`), so confirm only where it was, or, on a volume that may be a clone, where the volume is not that archive's. Where the folder contains another archive's folder or lies in it, on a volume that may be a sector-by-sector clone of that archive's volume, or that volume itself, it names that archive as possibly a clone before you confirm: confirm only where the folder is not that archive's. Archives names this command in the problem line; it runs on the command line only.

If the device that held the archive is gone as well (broken, lost or wiped), no folder or volume is left to register its next revision from. Retire the archive then: "Retire archive..." in its details (see "Relocating and retiring an archive"), or `veriarchive-cli archive retire <uuid>`; the problem line names the command too, after the last resort, as `archive retire <uuid> --confirm`. Where its lost manifest stops registering and relocating (above), the window says what retiring means and asks you to confirm that the manifest is lost for good and the device is gone, and the command first only says it (exit code 12); run it again with `--confirm` to do it. The archive keeps its verification history and its hash chain, but can no longer be verified or take a new revision, and it no longer stops registering a new folder or volume or relocating: retired, it is no longer taken as a possible owner of a folder or volume, as VeriArchive cannot tell from its lost manifest where it is. Should its device turn up again, nothing recognises it, and registering it makes a new archive. An archive whose manifest changed only in its body stops nothing: it is retired at once, as any archive, and is still recognised when its media is attached. `archive list`, `archive show` and Archives show the archive as retired, with a line "manifest lost: ..." in place of the problem (in Archives, not in red: it needs nothing more of you), and `archive list` no longer exits with code 2 for it. While a registration, a relocation or a verification that adds chunk hashes of the archive is unfinished, retiring it is refused, for every archive: finish or abandon that operation first. An archive retired earlier whose manifest is lost later is treated the same way, and a retired archive takes no registration over its lost manifest. Should the manifest turn up after all, `import` puts it back, and the archive is recognised again as a retired archive.

The manifest of an earlier revision can be lost too: `archive show` marks that revision, and registering a new folder or volume, or verifying a folder, may stop on it where VeriArchive looks among the archive's previous carriers. Put it back with `import` as above; if nothing holds it, `veriarchive-cli repo rebuild-index` leaves that revision out of the index, after which a volume that held only that revision is no longer recognised as a previous carrier of the archive. Its verifications stay in the history.

Times are shown in this computer's local time with the offset from UTC, for example `2026-10-14 08:13:53 (UTC+8)`. `--json` output and the history database always use UTC, for example `2026-10-14T00:13:53Z`.

More archive commands:

```text
veriarchive-cli archive show <uuid>               # details and every revision
veriarchive-cli archive retire <uuid>             # no longer expected to be verified; history is kept
                                                  # (--confirm where its manifest is lost, see above)
veriarchive-cli archive relocate <uuid> <root>    # after copying an archive to a new carrier: read it in full and accept it
veriarchive-cli archive compare <uuid> <uuid>     # do two archives hold the same content? (see below)
```

Relocating and retiring in the window, and how a relocation is resumed or abandoned: see "Relocating and retiring an archive" below.

`archive show`, `retire`, `relocate`, `compare`, `history`, `export`, `export-sha256sums`, `verify --full` and `register --copy-of` accept the first 8 or more characters of a uuid, as long as no other archive starts with them.

## Devices

![Devices](images/en/10-devices.png)

The Devices page lists the devices attached to this computer now: each with its model, bus, capacity, serial (or WWN) and the health it reports (see "Device alerts"); the volumes on it, with their label, where they are attached, capacity and space in use; and the archives whose roots lie on each volume, each leading to the archive in Archives. A volume that holds no archive has "Register this volume", which opens Register Archive with that volume chosen. The page reads all this again every 30 seconds while it or Archives is shown, and at once with Refresh; these reads need no administrator rights, and nothing is written to the devices.

Many USB enclosures report their own serial in place of the serial of the device inside them. Where VeriArchive can tell so, the device shows **Enclosure serial** in place of Serial, and a row **Device's own serial**: "This enclosure does not pass the device's own serial on." On Windows the row has **Read the device's own serial...** (with a shield): it starts the device identity reader, which needs administrator permission, so Windows' User Account Control first asks you whether to allow it (see "A device moved to another enclosure"). The reader reads only the model, serial and WWN that the device reports, and nothing of its data. Once read, the row shows the serial with "(read)", and either "the device of ARCH-0001 (confirmed)", where it is an archive's confirmed device, or "no archive records this serial yet". A serial read here is only shown, not recorded by itself: it is recorded when you then register the device, as Register Archive takes this read in place of the switch "Record the device's own serial" (see "Registering an archive"), or when the FULL verification after a change of enclosure, which reads the serial again, passes. The window keeps the read while it watches the device, and forgets it once the device is detached or changed. USB flash memory and card readers have no enclosure, and are not shown so (USB flash memory that tells the system its medium is fixed may be: a read of it then gives nothing); Linux has no device identity reader, so there the row has no button.

From the same read, Archives tells where each archive is attached:

- its folder or volume, where the volume and its device are those that its current revision records (MATCH), or the device has no serial to compare (MATCH_WEAK);
- "Not attached", where no attached volume holds it; its details say that it can be verified once it is attached;
- "The enclosure differs from the one recorded", where the archive's filesystem is attached on a device behind another USB enclosure than the one recorded (MIGRATED): the archive's device in another enclosure, or a sector-by-sector clone of it. Its details offer "Confirm and FULL verify...", and FULL verify first asks you whether it is the same device (see "A device moved to another enclosure");
- "The device differs from the one recorded", where the archive's filesystem is attached on another device in any other way (MIGRATED): a sector-by-sector clone of the archive's device, or the archive's device taken out of its enclosure and attached directly. FULL verify does not read it there. If you moved the archive to that device on purpose (a clone you made to replace the device), or it is the archive's device without its enclosure, relocate the archive to it with "Relocate to this carrier..." in its details (see "Relocating and retiring an archive"); a clone is registered as an archive of its own in Register Archive;
- "Several volumes look like this archive", as a device and a clone of it would: verify the one you mean with "Verify a folder or volume..." (see "Verifying an archive").

Attached is what this computer sees now, not part of an archive's identity: nothing of it is recorded, and an archive that is not attached keeps its identity and history.

The screenshot shows the end of the page: the last of the fixed sample devices and volumes that the screenshots list (with the volumes `/media/SAMPLE-VOLUME-*`), `SAMPLE-DEVICE-03` among them, shown behind a sample enclosure that does not pass its own serial on, with Read the device's own serial... (not pressed), and below them the volume of the sample folders, shown with the sample identity, holding the three sample archives.

### SMART

![SMART on Devices](images/en/12-smart.png)

On Windows the head of the page says when SMART was last read and how many attached devices give a SMART alert, with **Read the SMART of all devices...** (with a shield). It starts the device identity reader, which Windows' User Account Control asks you to allow (see "A device moved to another enclosure"); the reader reads the model, serial and SMART of every attached device side by side, and nothing of their content: no self-test runs and nothing is changed, but a sleeping hard disk spins up, and the read may take up to two minutes. The same happens whenever you read a device's own serial. Each read is recorded in the repository, which is why a repository opened by this version (1.3.0, schema version 4) can no longer be opened by an earlier one; the copy of the database from before the upgrade is kept beside it.

Each device's card then shows a second pill beside its health: "SMART alert", "SMART OK", "SMART cannot be read", "SMART not read" or "SMART to read again" (see below), and a section SMART: when and how it was read ("through its enclosure (SAT)", "directly (ATA)", "NVMe health log"), its alerts in red with what to do, its gray hints, the values most looked at (reallocated, pending and offline uncorrectable sectors, UltraDMA CRC errors, power cycles, power-on hours and temperature; for NVMe the available spare, the percentage of its rated endurance used, media errors, unsafe shutdowns and data written), and, folded, **All SMART attributes** (ID, attribute, current, worst, threshold and raw value, as smartctl and CrystalDiskInfo show them) and **Reads** (the earlier reads of the same device, newest first). Where a read gave nothing, the section says why: the enclosure passes no ATA command on, or passes the identification but not SMART, the device has SMART turned off, or it did not answer in time. USB enclosures for NVMe devices take commands of their own maker, which VeriArchive does not send: their devices cannot be read.

A device is found among the reads by what Windows tells of it. Behind a USB enclosure that does not pass the device's own serial on, Windows tells only the enclosure's, so it cannot tell which device is inside: another one may have been put into the same enclosure since. There the card shows "SMART to read again" and no earlier read, and Archives marks no archive on it, until the device is read again in this window, which reads its own serial too; the window then shows the reads of that device, found by its own serial, also those made behind another enclosure. It forgets that read once the device is detached or changed, or the window stopped watching the devices. Linux has no device identity reader: there the page shows no SMART box, and the device's health comes from udisks2 (see "Device alerts").

These reads describe the devices; a verification records none of them unless you turn on **Read SMART when a FULL verification starts** in Settings (off by default). Then each FULL verification started in the window first reads the SMART of the attached devices, which Windows asks you to allow each time (a read made in the last five minutes, as by the question after a change of enclosure, serves instead, without asking again), and records the reads of the devices it reads with the run, in the hash-chained history: its result page lists them beside the devices ("SMART OK · read ..." or "SMART alert: ..."), and a read that alerts among its device alerts, as its verification report does. If you decline, or the read fails, the verification starts without SMART, and a message says so. Leave it off where verifications are started and left unattended: Windows' question would wait for you.

The screenshot shows the card of `SAMPLE-DEVICE-02`, a sample device, after the sample read: 8 reallocated and 8 pending sectors, a made-up SMART alert.

## Verifying an archive

![Verifying an archive](images/en/04-verify.png)

A FULL verification reads every registered byte from the device again and compares it with the baseline.

In Archives, choose the archive and press **FULL verify** in its details. VeriArchive finds the archive on the attached volumes by its UUID, so a volume that got another drive letter is no problem, and the verification runs as a job: its card in the details shows its progress, with Stop and Details. When it ends, a message at the foot of the window says its result, with View result, and the details' banner shows it.

The arrow beside FULL verify opens other ways to verify:

| Menu item | What it does |
|---|---|
| FULL verify, and add chunk hashes | Only for an archive that has no chunk hashes: see "Adding chunk hashes to an archive registered earlier" below |
| FULL verify with strict metadata | A FULL verify in which a different modification time or attributes fail the run, whatever Settings says |
| Verify a folder or volume... | Verifies the archive at the folder or volume you choose, typed, pasted or chosen, with strict metadata if you turn it on. Use it where more than one attached volume holds the archive, as a device and a sector-by-sector clone of it do: FULL verify then does not choose one, but stops and names their folders. Or detach the other volumes and verify it again |

While a job of the archive waits or works, FULL verify is not offered for it: a second verification would read its whole device once more. Where Archives shows the archive as not attached, FULL verify still looks for it, as that may have been read up to 30 seconds ago. Where it shows "The enclosure differs from the one recorded", FULL verify and FULL verify with strict metadata first ask whether it is the same device (see "A device moved to another enclosure"); Verify a folder or volume... of such an archive does not ask, and stops, saying to use FULL verify in its details.

The screenshot shows Archives after a FULL verification of the sample archive `ARCH-0001` (its message at the foot of the window), with the details of the sample archive `ARCH-0003` and the menu beside FULL verify open: as that archive has no chunk hashes, the menu offers to add them.

CLI, by path or by archive uuid:

```text
veriarchive-cli verify --full K:\
veriarchive-cli verify --full 46d4c696-4999-4c7d-80eb-b626da614e77
```

With a uuid, or its first 8 or more characters, VeriArchive finds the archive on the attached disks. If more than one attached volume holds it, as a device and a sector-by-sector clone of it do, it does not choose one: it stops (exit code 12) and names their folders. Verify one of them by its folder, or detach the other volumes and verify it again. A folder or volume that is not there, and that is no archive's uuid either, is reported as not found (exit code 10); where no volume is attached as its drive letter, the message asks you to check that the device that holds it is attached. A uuid that names no registered archive, and an archive whose disk is not attached, are reported as not found as well (exit code 10); where the archive's disk is attached but its folder is not there (moved or renamed), the message names that folder. Where the archive's filesystem is attached on another device than the one recorded and its folder is there (MIGRATED: the archive's device in another enclosure, or a sector-by-sector clone of it), the verification stops on it with exit code 12 and names `archive relocate`; earlier versions reported such an archive as not found (exit code 10). An empty folder that is no registered archive, such as a folder on Linux where a disk is mounted when it is attached, is reported the same way (exit code 10): the message asks you to check that the device is attached.

### Reading the result

![A verification's result](images/en/09-result.png)

**View result**, in an archive's details, on a row of Recent verifications or History, on an ended verification in Jobs, or in the message that a verification ended, opens the run's **result page**:

- **The banner**: the result with its mode, for example "FULL VERIFY PASSED", the archive, and when the run ended; a PASSED run adds "100% of the registered content was read and matched". Beside it, **Save report...** saves the run's verification report (see "Saving a verification report" below).
- The run's device alerts, in red.
- For a FAILED or PASSED_WITH_WARNINGS run, its items come first: every entry with a problem, with its status and detail.
- **The figures**: read and matched, entries compared, duration and average read speed.
- **Read speed by position in the content**: a chart of how fast each hundredth of the content was read, beside the FULL verification it was compared with (dashed), where the run timed at least one second of reading; below it, the read speed and how it compares. A run that read less says "This verification read too little to chart its read speed by position in the content", as in the screenshot, whose sample archive is read in a moment; a run of a version of VeriArchive that did not record its read speed says so.
- **Items**: the entries that were not verified as recorded, 200 at a time ("Show N more items").
- **Device and identity** and **Technical details**, opened with their arrow: the media identity and each device's health; the run's id, mode and revision, the run it resumed, when it started and finished, the computer and program version that ran it, and what VeriArchive detects.

An INCOMPLETE run says how far it came, and has **Resume** where it can still be resumed. The back button at the top left, named for the page it came from, returns to Archives or History. While the verification's job is listed in Jobs, its Details hold the run's whole report, read as the command line's below.

The command line's report:

```text
FULL VERIFY FAILED  ARCH-0001 (46d4c696-...) revision 1
  root: K:\
  identity: MATCH
  coverage: 7 of 13 registered bytes read and matched
  items:  Modified 1  New 1  Verified 1
  New  new.txt
  Modified  readme.txt  (content differs)
```

How to read the report:

- **The first line** is the result, for example "FULL VERIFY PASSED".
- **Identity**: whether VeriArchive recognised the disk, for example "MATCH".
- **Coverage**: how many bytes were read and matched.
- **Items**: how many items have each status; every entry with a problem is listed below.
- **Note** (Linux only): "driver: ntfs-3g" when the volume was read through ntfs-3g rather than the kernel's ntfs3 driver; it changes nothing in the result.

A result always carries its mode and date: only FULL VERIFY PASSED means 100% of the registered content was read and matched.

| Result | Meaning | Exit code |
|---|---|---|
| PASSED | every registered byte was read and matched | 0 |
| PASSED_WITH_WARNINGS | content matched, but something needs attention (no hardware serial to compare, an unreadable filesystem id, metadata differences, names in one folder that differ only in upper and lower case or in Unicode normalisation, which a volume or system that does not tell them apart cannot keep all, or a hash recorded for a file that cannot be relied on: the file is fine, and a new revision fixes the hash) | 2 |
| FAILED | files were modified, missing or unreadable, or the media identity is wrong, or the archive's manifest in the repository cannot be used (missing, unreadable or changed: then the media is not read at all, and the result says how to fix the manifest) | 3 |
| INCOMPLETE | the verification was interrupted (cancelled, or the device went away or stopped answering); it can be resumed | 4 |

Item statuses:

| Status | Meaning |
|---|---|
| Verified | content matches the baseline |
| Modified | content differs from the baseline |
| Missing | in the baseline, not found now |
| New | a file the baseline does not have |
| Unreadable | could not be read (permission, media error, ...) |
| MetadataMismatch | same content, different modification time (a warning by default); with strict metadata ("Strict metadata" turned on, "FULL verify with strict metadata" in the menu, or `--strict-metadata`), the permission mode and attributes are compared too, and a difference fails the run |
| TypeChanged | for example a file became a folder, or a folder became a file or a link; what such a folder holds is listed beside it, as New where the folder is new, as Missing where it went |
| Unbaselined | unreadable at registration, so there is no hash to compare. When everything else matched and the archive is not retired, the report suggests registering a new revision, which takes a baseline for it (`register <root> --new-revision`, or "Create a new revision..." in the menu of the archive's details) |
| Unrepresentable | the name cannot exist on this operating system, and the revision verified was recorded on another one (for example a name with `?`, registered on Linux and verified on Windows): a warning. Verified on the system that recorded the revision, such an entry that is gone is Missing |
| SkippedMountPoint | another volume is mounted at this folder: it is not part of the archive and is not read. Where the archive recorded nothing there (the volume was mounted there at registration already), it changes no result; where it covers recorded files or folders, those are listed with this status and the run fails |

### Saving a verification report

A **verification report** is one file that says what one verification found: to keep with the archive's papers, or to send to someone who does not use VeriArchive. It is an HTML file that any browser opens, also offline. It holds no script and loads nothing from elsewhere, so opening it runs nothing.

In the window, press **Save report...** in the banner of the run's result page (see "Reading the result" above). Only a run that reached a result (PASSED, PASSED_WITH_WARNINGS or FAILED) has a report; the banner of an INCOMPLETE run's page has no Save report... and says "A report cannot be saved for a verification that has not finished." under its result. The save dialog proposes a name such as `ARCH-0001 verification report 2026-11-04 PASSED.html` (the archive's label, the date the run ended and its result, in the interface language), in your Documents folder, or in the folder where you last saved a report while the window has been open; it asks before it replaces a file of that name. The message at the foot of the window then says "Saved report: ..." with **Open**, which opens the file in your browser; where the report cannot be saved, it says "The report ... was not saved:" and why.

What the report holds:

- What the result page shows, in the same words: the result with its mode, the archive and when the run ended, the device alerts, the figures, the read speed with its chart, every item (not 200 at a time), Device and identity, and Technical details with the run's id and what VeriArchive detects.
- For a run that went on past a changed enclosure after a read of the device's own serial (see "A device moved to another enclosure"): the model, serial and WWN that were read, under "The device's own identity, read behind its enclosure". The result page does not show them.
- "Runs of this archive": every recorded run of the archive, newest first, with this run marked "This run".
- "History chain when this report was made": the result of the same check as "Check history chain" on the History page, for the history of every archive, and "This run's hash in the history chain (row_hash)".
- At its end, when, on which computer and by which version of VeriArchive it was made, and how to check it later.

On screen it follows your system's light or dark style. Printed, it fits A4 and needs no background colours, and a long list of items repeats its heading on each page.

**Where it cannot be saved.** Not in the root of an archive that is attached now, nor in any folder inside that root: the report would be a new file there, which that archive's next FULL verification reports as New, so that the verification fails. Where an archive is a whole volume, such as `H:\`, that is every folder of that volume. VeriArchive refuses such a folder and names the archive, for example "H:\Reports lies in the root of ARCH-0001 (H:\): a report saved there would add a file that its next FULL verification reports as added. Choose a folder outside every archive.", also when you reach the folder through a symbolic link or a junction to it, through a drive letter made with `subst`, or by a short name such as `PROGRA~1`. Save reports elsewhere, for example in Documents.

**Checking a report later.** The file is not signed, so it cannot show by itself that someone changed it after it was made: the repository is the record, and the report says what the repository recorded when it was made. To check a report, make a new report of the same run from the repository (Save report... on its result page, or `veriarchive-cli report` below) and compare the two: they must say the same of this run (its result, its figures and its items), this run's hash in the history chain (row_hash) must be the same, and the history chain must still be intact (it then counts the runs recorded since as well). The same hash shows only that the repository's record of the run has not changed; whether the old file was changed shows only in what the two say. The end of the report says how to check it as well.

On the command line:

```text
veriarchive-cli report <run_id> --out D:\Reports\ARCH-0001.html
```

`<run_id>` is the run's whole id, as `veriarchive-cli history <uuid>` lists it, `verify --json` gives it (`run_id`) and the result page shows it under Technical details ("Run"). The command makes the same file as the window, in the language of `--lang` (without it, as "Interface language" above describes). `report` records nothing in the repository: it adds no run and changes no history (as with every command, the first one that a new version runs upgrades an older database; see "After updating VeriArchive"). It says "Wrote the verification report of run ... (ARCH-0001: FULL VERIFY PASSED) to ..."; with `--json` it gives `run_id`, `archive_uuid`, `result` and `out` (the file written).

| Exit code | When |
|---|---|
| 0 | the report was written |
| 10 | nothing was written: a usage error (no run id, no `--out`); the file is there already ("... exists already; a report never replaces a file, so name another one"); the run is not recorded ("no run ..."); the run is INCOMPLETE ("run ... did not finish (INCOMPLETE); a report is made only of a run that reached a result"); the file's folder lies in the root of an attached archive (see above) |
| 11 | the repository cannot be opened or read, or the file cannot be written |

`report` does not replace a file: it refuses one that is there, also one that another program makes under that name while the report is being made, unlike `export-sha256sums`, which replaces its file without asking; in the window, the save dialog asks you first. Only on a Linux file system that offers neither a rename that replaces nothing nor hard links, such as exFAT through FUSE, can a file made in the moment before the report gets its name be replaced.

### Resuming an interrupted run

An interrupted verification ends INCOMPLETE and can be continued; files already verified are not read again:

- GUI: press **Resume** in the archive's details, where its stopped verification shows, on the verification's row under Unfinished operations in Jobs (see "Unfinished operations"), or on the INCOMPLETE run's result page. This works also after the window that ran it was closed.
- CLI: `veriarchive-cli verify --full <archive_uuid> --resume <RUN_ID>`, for any interrupted verification, also one started in the GUI. Under Unfinished operations in Jobs, pointing at the label of its row shows the archive's uuid, and pointing at its state shows the run's id.

If more than one attached volume holds the archive, as a device and a sector-by-sector clone of it do, a resume by the archive's uuid cannot tell which one the run read: it stops (exit code 12) and names them, with the root the run read. Resume it at the root on the volume it read with `veriarchive-cli verify --full <root> --resume <RUN_ID>`, or detach the other volumes and resume it again.

A device that is unplugged or switched off during a verification ends it INCOMPLETE, not FAILED: before VeriArchive reports a file as unreadable, it checks that the volume is still attached, so a device that went away is not taken for damage, and the result says what happened. A device that stops answering while the computer still lists it (a USB enclosure that hangs) fails every read with an input/output error, as damage does: when 10 entries (files or folders) in a row cannot be read because of such errors, with no entry read between them, the verification ends INCOMPLETE as well, lists none of them, and says that the device stopped answering; fewer in a row are reported unreadable, as damage. Attach the device again (unplug it first if it is still connected) and resume the run as above; what was verified is not read again. If it stops again at the same place, the device may be damaged: damage that spans 10 entries in a row stops the run in the same way instead of failing it. A registration and a relocation stop the same way and are resumed the same way: with "Resume" under Unfinished operations in Jobs, or with `register <root> --resume` and `archive relocate <uuid> <root> --resume`.

### Adding chunk hashes to an archive registered earlier

Registering records, besides the BLAKE3 and SHA-256 of each whole file, the hash of each 64 MiB chunk of every larger file, so that a FULL verify can say where a large file differs ("content differs: chunk 1 of 3 differs, from byte 67,108,864"). An archive that an earlier version of VeriArchive registered has no chunk hashes. They are added to it once, by a FULL verify that adds them:

- GUI: in Archives, choose the archive, open the menu beside **FULL verify** in its details, and choose **FULL verify, and add chunk hashes**. The menu has it only for an archive without chunk hashes, and it can be chosen only where they can be added (not for a retired archive); otherwise the line under it says why. To add them with strict metadata, turn strict metadata on in the policy (`strict_metadata` under `policy` in the config file), which every verification then uses.
- CLI: `veriarchive-cli verify --full <archive_uuid> --add-chunk-hashes`, or the folder or volume in place of the uuid.

The screenshot at the start of "Verifying an archive" shows this item offered for the sample archive `ARCH-0003`, not chosen yet. That sample archive was registered the way earlier versions registered archives, without chunk hashes; an archive you registered with an earlier version shows the same.

It is a FULL verify in every respect: it reads the whole archive once, taking as long as any FULL verify, and its result is the verification's. The chunk hashes come from that same read. Only when every file matched does it record a new revision of the archive, holding the same entries with their chunk hashes, and the result says so: "chunk hashes added: revision 2 records them (chunk size 64 MiB, ...)". When anything differs, such as a modified, missing or new file or another modification time, it records the verification alone, and the result says why: "chunk hashes not added: ...". Such an archive gets chunk hashes from its next registration ("Create a new revision..." in the menu of its details; on the command line, `register --new-revision`), which takes it as it is now. Once an archive has chunk hashes, every FULL verify compares them, and the menu no longer offers to add them.

An interrupted one keeps what it has read, and Jobs lists it under Unfinished operations as "Verification adding chunk hashes". To continue it, press "Resume" on its row or in the archive's details, also after the window that ran it was closed, or run `veriarchive-cli verify --full <archive_uuid> --resume <RUN_ID>`; files already read are not read again. Where the window shows the archive's uuid and the run's id: see "Resuming an interrupted run" above. To give it up, for example when its device failed, press "Abandon..." on its row, or run `veriarchive-cli archive abandon-verification <archive_uuid> --confirm`: its saved progress is lost, while the archive, its revisions and the history stay. Until it is finished or abandoned, a new registration of the archive (`--new-revision`), a relocation of it, a second verification that adds chunk hashes to it and its retirement are refused (exit code 12), each saying how to finish or abandon it. In the window, the refusal names the window's own buttons: first "Resume" and "Abandon..." on that operation's row under Unfinished operations in Jobs, then "Resume" in the archive's details, or, for a registration, choosing its root in Register Archive, whose options then resume it; a relocation is resumed the same way, with "Resume" on its row. While that operation still runs, it says so and to wait for it.

### When VeriArchive needs your decision (exit code 12)

| Message | Situation | What to do |
|---|---|---|
| UNKNOWN | not a registered archive | check that the right disk is attached, or register it |
| MIGRATED | the filesystem matches but the device changed (the archive's device in another enclosure, or a clone on a new device); also `verify --full <uuid>` of an archive attached only so, with its folder there, which earlier versions reported as not found (exit code 10) | the archive's device in another USB enclosure: in the window, FULL verify in its details asks whether it is the same device and verifies it (see "A device moved to another enclosure"); the command line has no such question. Otherwise, or on the command line, if it is the archive's device or you moved the archive on purpose: "Relocate to this carrier..." or "Relocate to a new carrier..." in its details, or `archive relocate <uuid> <new root>` (see "Relocating and retiring an archive"); a clone: register it as an archive of its own |
| AMBIGUOUS | several archives match (cloned disks) | choose one with `verify --full <archive_uuid>`, or FULL verify in its details; where the volumes of those archives are attached together, detach all but the volume of the one to verify, then verify it |
| AMBIGUOUS: archive ... is on more than one attached volume | a verification by the archive's uuid finds it on several attached volumes (a device and a clone of it) | verify one of them by its folder: "Verify a folder or volume..." in the menu beside FULL verify, or `verify --full <root>`; or detach the other volumes and verify again |
| AMBIGUOUS: archive ... is on more than one attached volume ...; run ... | a resume by the archive's uuid finds it on several attached volumes | resume at the root on the volume the run read: `verify --full <root> --resume <RUN_ID>`, or detach the other volumes and resume again |
| PREVIOUS_CARRIER | this is the old disk of a relocated archive | the old disk is no longer verified; verify the new carrier |
| run the command again with --confirm to resume it | `register <root> --resume` of a registration that an older version of VeriArchive left, which may still run it | make sure that no window or command of that version still runs it, then run the command again with `--confirm` |

## A device moved to another enclosure

Many USB enclosures and docks report their own serial in place of the serial of the device inside them. Put such a device into another enclosure, or attach it through another dock, and VeriArchive sees the archive's filesystem with another serial. From that alone it cannot tell whether this is the archive's device in another enclosure or a sector-by-sector clone of it, which holds exactly the same content and passes a FULL verify too. So it asks you before it verifies.

In Archives the archive's row then reads **"The enclosure differs from the one recorded"**, and its details say "The enclosure of H:\ differs from the one recorded. Before it is verified, you are asked whether it is the same device." (naming the volume where it is attached), with **Confirm and FULL verify...**; under Device, "Enclosure serial" shows the serial recorded and the one seen now. On the Devices page, the archive under that volume reads "The filesystem belongs to ARCH-0001, but the enclosure differs from the one recorded (MIGRATED)". Confirm and FULL verify..., FULL verify and FULL verify with strict metadata all open the same question. (Chunk hashes are not added by this verification: verify the archive first, then add them.)

**The question.** It says that the filesystem is the archive's while the serial its enclosure reports has changed, and lists that serial as recorded and as seen now. Where the archive has a device's own serial recorded (see below), it says that reading the serial now tells whether it is the same device; otherwise it says that this may be a clone, and that you compare a serial read now with the serial on the device's label. Its answers:

- **Read the device's own serial** (with a shield): VeriArchive starts its device identity reader, `veriarchive-device-identity.exe`, which needs administrator permission, so Windows' User Account Control first asks whether to allow it. VeriArchive's programs are not signed, so Windows names the publisher as unknown; on an account that is not an administrator's, Windows asks for an administrator's password. While Windows asks and the reader reads, the dialog waits, with Cancel only. The reader sends each attached device only commands that ask for its identity and its SMART (ATA IDENTIFY DEVICE, SMART READ DATA and SMART READ THRESHOLDS, or their NVMe counterparts), reads only the model, serial, WWN and SMART that the devices report, and reads or writes none of the data on them; then it ends (see "SMART"). VeriArchive itself never runs with administrator permission.
- **Confirm without reading the serial...**: offered where the archive has no device's own serial recorded, after a read that gave nothing, and where no read can be made.
- **Cancel**, the default, changes nothing.

Every FULL verify of such an archive asks again and reads the device again, also when you have just read its serial on the Devices page: another device of the same model put into the same enclosure looks exactly alike to the system.

**After the read.**

- **It is the archive's confirmed device** (its own serial was recorded at registration or by an earlier change of enclosure): "The device's own serial matches (...)", and the FULL verification starts at once, with no more questions.
- **The archive has no device's own serial recorded yet**, as every archive registered without it: "What the device reports" shows the model, the device's own serial and the WWN that were read. Compare the serial with the one on the device's label, then answer **It is the same device, start the FULL verify**, or **No, it is another device**; neither is chosen for you.
- **It is another device**: "This is not the device of ARCH-0001", with the serial recorded and the one read (after **No, it is another device** on "What the device reports", the page is headed "Another device", without the serials): most likely a sector-by-sector clone. Nothing was recorded. **Register as another archive...** opens Register Archive on that folder or volume with "Register as a separate archive even though this folder or volume is registered" turned on and ARCH-0001 chosen as the archive it is a copy of; then cross-check the two (see "Cross-checking an archive and its copy").
- **You answered No to Windows' question**: the question comes back, saying that administrator permission was not given, and offers Confirm without reading the serial....
- **The serial could not be read**: some enclosures pass no ATA command on to the device; the question comes back saying so ("this enclosure does not pass ATA commands on"), or why else nothing was read. Try another enclosure, or confirm it yourself.

**Confirm without reading the serial...** asks once more: do this only if you are sure that you moved this device into the new enclosure yourself. Whatever the result, the history records that you confirmed it without reading its serial, and a sector-by-sector clone passes the verification too, so VeriArchive cannot find such a mistake for you. **It is the same device, start the FULL verify** starts it.

**What is recorded.** How it was confirmed (the serial match, or your own confirmation, with the serial read where you read it) is written into the verification history with the FULL verification it starts, whatever the result, and the history's hash chain covers it (see "History"), so that it can be audited later, a wrong "same device" too. While the verification waits or works, the archive's details say how it was confirmed and that the new enclosure is recorded only if it passes; a stopped one is resumed as any verification, and goes on with the same confirmation. When it ends, the message says what was recorded:

- **PASSED** (or PASSED_WITH_WARNINGS), with every entry matched as a relocation requires: "New enclosure recorded: revision 2 records it (...)". VeriArchive records the next revision of the archive with the new enclosure, as a relocation does, and, where you read the device's own serial, that serial as the archive's confirmed device. Later verifications recognise the device in this enclosure with no question and no administrator permission; when the enclosure changes again, a read of the serial tells by itself.
- **FAILED**: "New enclosure not recorded: the verification did not pass". Nothing about the carrier changes, and the next FULL verify asks again; the run, with your confirmation, is in the history.
- A verification that stopped records nothing about the enclosure until it is resumed and passes.

The result page's Device and identity says "MIGRATED: the enclosure differs from the one recorded (the device's serial matches)", "(confirmed by you after reading the serial)" or "(confirmed by you, serial not read)", and the report says whether the new enclosure was recorded; History notes the run "enclosure changed, same device by its serial", "enclosure changed, confirmed by the operator after reading the device's serial" or "enclosure changed, confirmed by the operator without the serial".

Where no serial can be read, the question says why and offers Cancel and Confirm without reading the serial... only: on Linux, which has no device identity reader; where the reader is missing from the program's `bin` folder (an antivirus program may have removed it); and for a device that reports its own serial through its enclosure, which a read would only repeat.

To have the device's own serial recorded before an enclosure ever changes, turn on "Record the device's own serial" when you register the device (see "Registering an archive").

A device taken out of its enclosure and attached directly, by SATA for example, is no change of enclosure: its own serial is seen now where the enclosure's was recorded. Archives then says "The device differs from the one recorded", and the archive is relocated to it (see "Relocating and retiring an archive"). "Relocate to a new carrier..." in the menu of the details accepts a device in another enclosure too, as before.

On the command line, `verify --full <uuid>` of such an archive stops with exit code 12 (MIGRATED) and names `archive relocate`: the command line never asks for administrator permission and has no option for this question. A confirmed verification started in the window and stopped can be resumed with `verify --full <uuid> --resume <RUN_ID>`, and goes on with its confirmation. `history` shows the notes above, `history --json` gives each run `carrier_confirmed` (null, or how it was confirmed, the device read, the two enclosure serials and the revision it was given against), and `verify --json` gives it with the revision that records the new enclosure, or why none does.

## History

![History](images/en/05-history.png)

The History page, or `veriarchive-cli history <uuid>`, lists the recorded verifications.

History lists the runs of every archive, newest first; choose an archive in "Archive" to list its runs alone, or "All archives" to list them all again. Each row gives the result with its mode, the archive and when the run ended, what it read and matched, how long it took and how fast it read, and its notes: what else the run is (see below) and its device alerts. A row opens the run's result page (see "Reading the result"), which gives its revision, failures and run id. A long history is listed 200 runs at a time ("Show N more runs"). An archive's details in Archives show its recent verifications, with a link to all of them here.

An interrupted `archive relocate` is listed there too, as an INCOMPLETE FULL verification of the revision it compared the new carrier with, marked "interrupted relocation attempt": it read the new carrier, not the archive's carrier. While the relocation is unfinished, continue it with "Resume" under Unfinished operations in Jobs or `archive relocate <uuid> <root> --resume`; `verify --resume` does not continue it, and says so. The FULL verification with which a relocation recorded the archive's next revision is marked "relocation from r1", r1 being the revision it compared the new carrier with. The window shows these marks among the notes of the run's row; `history --json` gives each run `relocation`: null, or the relocation's `run_id`, `from_revision` and `interrupted_attempt`.

A FULL verification that went on past a changed enclosure is marked with how you confirmed it, whatever its result: "enclosure changed, same device by its serial", "enclosure changed, confirmed by the operator after reading the device's serial" or "enclosure changed, confirmed by the operator without the serial" (see "A device moved to another enclosure"); `history --json` gives each run `carrier_confirmed`, null for every other run.

The history is a hash chain. Checking it detects a recorded run that was changed or removed, but not the removal of the newest runs, nor runs changed with the chain computed again; history signatures, which would detect those, are not part of this version. To check it:

- GUI: press "Check history chain" on the History page: the page then says what it found, for the history of every archive whatever the filter lists, and what the check cannot detect. The screenshot shows the sample repository's history after the FULL verification of `ARCH-0001`, with the chain checked.
- CLI: `veriarchive-cli repo verify-history`

## Cross-checking an archive and its copy

![Cross-check](images/en/06-compare.png)

Say you copied volume H to volume J with your own tools (VeriArchive never makes backups) and want proof that J holds exactly what H holds. Verifying J alone does not show that: it compares J with J's own baseline. Instead, register each volume as an archive of its own and **cross-check** the two archives (on the command line, compare them: `archive compare`). The cross-check takes every entry (file, folder, link) of both manifests and confirms that it is in both, with the same type and the same content (BLAKE3 and SHA-256). It reads no media, only the manifests in the repository, so it takes seconds. Modification times and attributes are not cross-checked, because copy tools do not always keep them.

### 1. Register both volumes

Register them once the copy is complete; each registration reads its whole disk.

GUI: in Register Archive, register `H:\` first, then `J:\`; on the Options page of `J:\`, choose H's archive in "Copy of which archive (optional)".

CLI:

```text
veriarchive-cli register H:\
veriarchive-cli register J:\ --copy-of <uuid of H's archive>
```

- `--copy-of`, or "Copy of which archive", is optional: it records that J is a copy of H, for display only; Archives shows it under Copies in the details of both archives. On the command line it takes the uuid of H's archive, or its first 8 or more characters, as `archive list` shows them.
- Register the folder that corresponds to H's root, so the paths line up: if the copy of H sits in `J:\Backup\H`, register `J:\Backup\H`. Anything else in the folder registered on J shows up as New.
- A sector-by-sector clone of H has H's filesystem id, so registering it stops with exit code 12 (MIGRATED). Add `--force-new-archive` to register it as an archive of its own. Register Archive offers "Register as a separate archive even though this folder or volume is registered" for it, with H's archive chosen as the one it is a copy of.

### 2. Cross-check

GUI: in Archives, choose H's archive and press "Cross-check..." at the top of its details, or "Cross-check..." beside J's archive under Copies; the overview of Archives has "Cross-check..." too. In the dialog Cross-check:

1. "Archive A" is H's archive.
2. In "Archive B" choose J's archive; the archives recorded as copies of A are listed first, and the first is chosen. A and B stand side by side: neither is taken for the original.
3. Press "Cross-check". The dialog closes and the cross-check runs as a job: a message at the foot of the window says that it started, and the report is in the job's Details in Jobs; "Stop" stops it at any time.

How to read the report:

- **The first line**: "Cross-check: IDENTICAL", "Cross-check: IDENTICAL_WITH_WARNINGS" or "Cross-check: DIFFERENT".
- **The lines A and B**: which archive each is, which of its revisions is cross-checked, and when that revision was recorded: by a registration, by a relocation or by adding chunk hashes.
- **Entries** and **Differences**: how many entries each side has, how many of them are the same, and how many differences there are of each kind.
- Below them, every entry that differs is listed: "Only in A", "Only in B" or "Differs" (see "Reading the cross-check" below).
- **The last FULL verification of each**: its result, how long ago and when it ended, and OVERDUE where it is; where the last FULL verification is of an earlier revision, that the revision cross-checked has none yet. This is as Archives listed it when the cross-check started. Where the result is IDENTICAL, a last line says whether both media held this content at last FULL verifications that are not overdue, or which archives a FULL verification would still have to show it of (see "What the cross-check proves" below).

Choosing the same archive twice is refused, in the dialog.

The screenshot shows the dialog with the sample archive `ARCH-0001` (`原始資料`) as Archive A and its copy `ARCH-0002` (`備份資料`) as Archive B, as "Cross-check..." beside the copy opens it. The sample copy lacks one file, has one file more, and holds one file with other content, so the result is DIFFERENT.

CLI, `archive compare` with archive A first, then archive B. The first 8 or more characters of each uuid are enough:

```text
veriarchive-cli archive compare 0d0ce48d a82ebd4e
```

```text
COMPARE DIFFERENT  ARCH-0001 (0d0ce48d-9445-4e7a-ae49-8fb42ef05201) r1  vs  ARCH-0002 (a82ebd4e-7766-4291-9795-d1a8d29efadd) r1
  A: ARCH-0001, revision 1 recorded 2026-10-15 10:18:09 (UTC+8), 6 entries
  B: ARCH-0002, revision 1 recorded 2026-10-15 10:18:09 (UTC+8), 6 entries
  same: 4  Missing 1  Modified 1  New 1
  Modified  專案/budget 預算.csv  (size 26 -> 41)
  New  新增的檔案.txt
  Missing  會議紀錄.txt
  This compares what each registration read. A FULL verify of each archive proves that its carrier still holds that content.
```

### Reading the cross-check

| Result | Meaning | Exit code |
|---|---|---|
| IDENTICAL | A and B hold the same entries, each with the same type and content | 0 |
| IDENTICAL_WITH_WARNINGS | nothing differs, but the entries listed could not be compared (see Unbaselined and MetadataMismatch below) | 2 |
| DIFFERENT | at least one entry is in one archive only, differs, or is of another type | 3 |

The window takes neither archive for the original; the command line sees B from A. So they name a difference in their own words:

| Window | Command line | Meaning |
|---|---|---|
| Only in A | Missing | in A, not in B |
| Only in B | New | in B, not in A |
| Differs | Modified | different content; `size 26 -> 41` gives the size in A and in B (bytes) |
| TypeChanged | TypeChanged | for example a file in A is a folder in B |
| Unbaselined | Unbaselined | no hash to compare: it was unreadable when A or B was registered (the detail says which and why), or it lies in a folder one of them could not read |
| MetadataMismatch | MetadataMismatch | a link whose absolute target differs where one revision was recorded on Windows and the other on Linux; a warning only |

If the cross-check cannot run, for example because a manifest in the repository was changed or damaged or an archive's first registration never finished, it reports the reason and exits 3; a cancelled one exits 4. `--json` gives the result and every difference, always in English.

### What the cross-check proves

It compares what each registration read, not what their media hold today. For proof that J holds H's content now:

1. The cross-check says IDENTICAL: the two baselines hold the same entries with the same content.
2. A FULL verify of each archive (`verify --full H:\`, `verify --full J:\`) says PASSED: the media of each still hold its baseline. The window's report lists the last FULL verification of each and says which one is still needed; "Verify the attached archives that need it" in the overview of Archives verifies every attached archive that is OVERDUE, never FULL verified or last FAILED at once.

Devices can still fail later. To prove that both volumes **still** hold that content now, run a FULL verification of each archive regularly (see "Verifying an archive" above).

If H changed between its registration and the copy, the cross-check shows the change. Register H again with `--new-revision` ("Create a new revision..." in its details) and cross-check again. A cross-check writes nothing: it is not part of the verification history and does not change "Last FULL verify" or OVERDUE, so you can run it as often as you like.

## Relocating and retiring an archive

![Relocate to a new carrier](images/en/11-relocate.png)

**Relocation** takes an archive to a new carrier and keeps its UUID and its history: after you copied its files to another device with your own tools, or took its device out of its enclosure, or put it in another enclosure (which the window also accepts with a FULL verify, once you confirmed that it is the same device: see "A device moved to another enclosure"). VeriArchive copies nothing. It reads the new carrier in full and accepts it only when every entry matches the archive's latest revision; it then records the next revision with the new carrier, and the old carrier is no longer verified (it is recognised as PREVIOUS_CARRIER).

In the window:

1. Copy the archive's files to the new carrier, keeping their paths below the archive's root.
2. In Archives, choose the archive and "Relocate to a new carrier..." in the menu of its details. Where Archives found the archive's filesystem on another device than the one recorded (see "Devices"), its details offer "Relocate to this carrier..." with that root chosen; where only the enclosure differs, they offer "Confirm and FULL verify..." in its place, and the menu still offers "Relocate to a new carrier...".
3. The dialog lists the attached volumes: choose the one that holds the copy, or under "New root" choose, type or paste the folder that holds it. A volume that holds other archives' roots cannot be the new root as a whole. "What the relocation does" says what it will read and record.
4. Press **Start relocation**. VeriArchive first checks the new root, as the relocation will, and a refusal stays in the dialog. The relocation then runs as a job, which waits for the jobs of the new carrier's devices and of the archive, and shows as a card in the archive's details. A message says how it ended; its report, with every entry that differs if the new carrier does not hold the recorded content, is in the job's Details.

A stopped relocation shows under Unfinished operations in Jobs and in the archive's details, with "Resume". Where the new carrier is attached at another drive letter or mount point now, "Relocate to a new carrier..." opens the dialog to resume it at the root you give there. "Abandon..." on its row gives it up.

The screenshot shows the relocation dialog of the sample archive `ARCH-0002`, with the sample volumes `/media/SAMPLE-VOLUME-*` listed and the sample folder `/media/SAMPLE-VOLUME-1/備份資料` typed in as the new root, before Start relocation.

An interrupted `archive relocate` keeps what it has matched and is listed with the unfinished operations: continue it with "Resume" or `archive relocate <uuid> <root> --resume`, or give it up with "Abandon..." or `archive abandon-relocation <uuid> --confirm`. A relocation that finds an entry on the new carrier that differs from the archive, such as a modified or missing file, ends instead, also when it is interrupted after that, and its saved progress is removed: copy again, and the next relocation reads the whole new carrier. It lists every entry that differs, and names the 64 MiB chunks of a large file that differ, as a FULL verify does. A relocation accepts what a FULL verify accepts: a file whose recorded hash cannot be relied on while the file itself is fine (see PASSED_WITH_WARNINGS above) does not end it, and the relocation finishes with the same warning (exit code 2); the revision it records keeps that hash as it was recorded, until `register --new-revision` records the file again. An entry that cannot be read ends it only once everything else has been read; if the new carrier is unplugged before that, the relocation is interrupted, and its resume reads that entry again. If the saved progress of a relocation that ended cannot be removed, the relocation stays listed as "new carrier differs; abandon it": abandon it, then relocate again.

**Retiring** an archive says that you no longer expect to verify it: it is never OVERDUE, keeps its verification history and is still recognised when attached, but takes no new revision, and it cannot be made active again. In Archives, choose the archive and "Retire archive..." in the menu of its details: VeriArchive first reads what retiring will do, then asks, with Cancel as the default. For an archive whose latest manifest is lost (see "Archives and OVERDUE"), it explains what retiring it means, and offers "Retire" only once you turn on "I confirm that this manifest is lost for good and the device is gone". Retiring is refused while a registration, a relocation or a verification that adds chunk hashes of the archive is unfinished: finish or abandon it first. A retired archive can still be verified.

A retired archive's root no longer keeps another root from containing it or lying in it. So to verify a whole volume where you registered only a folder of it, retire the folder's archive first, then register the volume: it becomes a new archive, and the folder's archive keeps its history, listed as retired. An archive cannot be deleted: its verification history is part of one hash chain with every other archive's, which a deletion would break, and it is the record of what was verified and when.

CLI:

```text
veriarchive-cli archive relocate <uuid> <root>             # read the new carrier in full and accept it
veriarchive-cli archive relocate <uuid> <root> --resume    # continue a stopped relocation
veriarchive-cli archive abandon-relocation <uuid> --confirm
veriarchive-cli archive retire <uuid>                      # --confirm where its manifest is lost
```

## Export and import

In the window, "Export manifest..." in the menu of an archive's details writes the manifest of its current revision and its `identity.json` into a folder named for the archive (its label and the start of its UUID) inside the folder you choose, so that exports of several archives into one folder never replace each other. A folder inside the root of an attached archive is refused, also when you reach it through a symbolic link or a junction, a drive letter made with `subst` or a short name: its next FULL verification would report the files as new (a verification report is refused there too; see "Saving a verification report"). The export runs as a job; Stop cancels it and writes nothing. Importing, and the list for `sha256sum`, are on the command line:

```text
veriarchive-cli export-sha256sums <uuid> --out sums.txt   # sha256sum-format list
veriarchive-cli export <uuid> --out D:\export             # the current revision's manifest
veriarchive-cli --repository D:\OtherRepository import D:\export\r0001.vam   # into another repository
```

`export` writes the manifest of the archive's current revision (`r0001.vam` is revision 1) and its `identity.json`, not those of earlier revisions: to keep an export of every revision, export the archive again after each new one. `import` imports into the repository in use; to import into another repository, name it with `--repository`, and create it first if it does not exist yet (`veriarchive-cli --repository D:\OtherRepository repo init`). A repository named with `--repository` is used for that command only: without it, VeriArchive uses the repository that the config file remembers, and says nothing about the other one.

`sums.txt` lists every file by its path inside the archive, so `sha256sum -c sums.txt` checks the archive without VeriArchive when it runs in the archive's root folder. Windows itself has no `sha256sum`; Git Bash (part of Git for Windows), WSL and Linux have it. In Git Bash, for an archive at `H:\` and the list at `D:\sums.txt`: `cd /h && sha256sum -c /d/sums.txt`.

## More

- If the repository's database (`veriarchive.db`) is lost or damaged, the commands say so (a damaged part of it, by the first command that reads that part), and `veriarchive-cli repo rebuild-index` makes it again from the manifests: it renames the damaged file `veriarchive.db.damaged-<date and time>` and leaves it beside the new one. The verification history was kept only in the database and cannot be made again: afterwards every archive is listed as never verified until it is verified again, retired archives are listed as registered, and unfinished registrations and relocations are gone. If a backup of the repository holds `veriarchive.db`, put that file back instead. If the rebuild stops before it has finished (Ctrl+C, an error), the new database is left unfilled: every command says so, and running `repo rebuild-index` again finishes it and names the file put aside once more. Manifests are the truth; the database is an index.
- Every command accepts `--json` for machine-readable output.
- Exit codes: 0 passed, 2 passed with warnings, 3 failed, 4 incomplete, 10 usage, 11 repository, 12 needs your decision.
- VeriArchive detects accidental corruption and unintended change; tamper evidence (signed manifests) is not part of this version. Nor are QUICK and DEEP verification, which would read only a sample of the content: every verification of this version reads everything (FULL).
- VeriArchive needs no administrator rights and should not be run elevated. Only reading a device's own serial behind its USB enclosure needs them, on Windows, and only for the device identity reader, which the window starts when you ask it to and Windows asks you to allow each time (see "A device moved to another enclosure"); the command line never asks for them.
- In PowerShell, run `[Console]::OutputEncoding = [Text.UTF8Encoding]::new()` before capturing, redirecting (`>`) or piping CLI output; otherwise Chinese text is garbled. Output shown directly in the window is not affected.
- If the GUI closes unexpectedly, the reason is written to `veriarchive-gui.log` in the user config folder (`%APPDATA%\VeriArchive` on Windows, `~/.config/veriarchive` on Linux). Include it when reporting a problem.
- The screenshots are made automatically from sample data by the tool in `tools/screenshots/`. When the interface changes, they can be made again; see `tools/screenshots/README.md` in the source code.
