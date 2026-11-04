# Screenshots for the user guides

`make_screenshots.cmake` makes the pictures of the GUI that the user guides show: `docs/images/en/` for the English guide (`docs/user-guide.md`) and `docs/images/zh-TW/` for the Chinese one (`docs/user-guide.zh-TW.md`), with the same file names in both. It builds sample data, registers it as two archives with `veriarchive-cli`, adds a third archive registered without chunk hashes, as an earlier version of VeriArchive registered archives, then starts `veriarchive-gui` in its screenshot mode. The GUI tours its views as a reader of the guide would: it fills in forms and starts a verification and a comparison with the buttons' own handlers. It saves one PNG per view and quits.

| File | What it shows |
|---|---|
| `01-settings.png` | Settings: the repository, the config file, the policy |
| `02-archive-list.png` | Archives with the three sample archives and the sample identity (`SAMPLE-VOLUME`, `SAMPLE-DEVICE`, 1.0 TiB), archive A selected and its details beside the list |
| `03-register.png` | Register Archive with archive A's folder and label typed in (not started) |
| `04-verify.png` | Archives after a FULL verification of archive A by its folder (its toast at the bottom), then the details of archive C with the menu of FULL verify open, which offers "FULL verify, and add chunk hashes" (not started), as an archive registered by an earlier version shows it |
| `05-history.png` | History: the runs of every archive, newest first, after the verification of archive A and the comparison, with the history chain checked |
| `06-compare.png` | Compare Archives: A (`原始資料`) against its copy B (`備份資料`) |
| `07-language.png` | Settings with the language list open |
| `08-jobs.png` | Jobs: the verification and the comparison that the tour started, ended |
| `09-result.png` | The result page of archive A's FULL verification, the tour's, opened with View result in its details |

The pictures are 1100 × 720 and show the window only, title bar included. GTK draws them itself from what the window drew, so other windows on the desktop never appear in them.

The user guides and the repository's `README.md` link to these files by name. Keep the names: a new picture saved under the same name replaces the old one in every document, with no change to the documents.

## Making them again

You need a build with the GUI and a desktop session: the window appears for a few seconds while the tour runs. On Windows, run from the MSYS2 UCRT64 shell (or put the GTK DLLs on `PATH` some other way), once for each guide:

```sh
cmake -DCLI=build/windows-ucrt64-debug/veriarchive-cli.exe \
      -DGUI=build/windows-ucrt64-debug/veriarchive-gui.exe \
      -DWORK=D:/va-sample -DOUT=docs/images/zh-TW -DLANG=zh-TW \
      -P tools/screenshots/make_screenshots.cmake
cmake -DCLI=build/windows-ucrt64-debug/veriarchive-cli.exe \
      -DGUI=build/windows-ucrt64-debug/veriarchive-gui.exe \
      -DWORK=D:/va-sample -DOUT=docs/images/en -DLANG=en \
      -P tools/screenshots/make_screenshots.cmake
```

`LANG` is `zh-TW` (the default) or `en`. Run once per language, each into its own `OUT`: `docs/images/zh-TW` with `LANG=zh-TW`, `docs/images/en` with `LANG=en`.

- `WORK` holds the sample data, a config file and a repository. It is emptied on every run, so the script accepts only an empty folder or one it made itself (it leaves a `.veriarchive-screenshots` marker there).
- The pictures show the paths under `WORK`, so a short `WORK` gives short paths. A drive letter made with `subst` does not shorten them: the program records the folder it stands for (decision 0027), so the pictures show the real path.
- Both guides name the folder that their pictures show, `D:\va-sample\原始資料`: make both sets with the same `WORK`, and with another `WORK` change that sentence in both guides.
- `OUT` must not be inside `WORK`.

## Sample data only

The repository is public, and so are these pictures. The script makes its own sample data, a few small files with Chinese and English names. Archive B is a copy of A with one file changed, one deleted and one added, so the comparison has differences to show. Archive C (`舊版封存`, `ARCH-0003`) stands for an archive that an earlier version registered, which has no chunk hashes: this program registers every archive with them, so the script registers C's folder in a scratch repository under `WORK`, writes its exported manifest again as an earlier version wrote it (chunk size 0, no Merkle root) under the sample uuid `c0ffee00-0000-4000-8000-000000000003`, its files and their hashes as they are, and imports it. Never point the tool at real folders or at a real repository.

- The GUI finds its config file in `WORK`: the script points `APPDATA` (Windows) or `XDG_CONFIG_HOME` (Linux) there and clears `VERIARCHIVE_REPOSITORY`.
- A portable install (a `config.json` next to `veriarchive-gui`) would use its own repository instead, so the script refuses such a GUI.
- The sample folders lie on a real volume, the one `WORK` is on, so what the registrations record about their carrier is that volume's own identity: its label, its device's model and serial, its capacity. The screenshot mode never shows it: every view shows a fixed sample identity instead (see below).
- The pictures still show the paths under `WORK`, its drive letter included, and the computer's time zone. Check them before publishing.
- The identity in the pictures is sample data, but what the tour's own runs measured is not: the health that the device under `WORK` reported, the read speed and the duration of the verification (in the details of archive A, under Device and Recent verifications, in History and on its result page), and every time shown come from the machine that took the pictures, as do the roots, which are the sample folders' real paths under `WORK`. Check them before publishing too. The name of the computer that recorded a run, which a result page shows under Technical details, is shown as `SAMPLE-HOST`.

## The screenshot mode

The script sets these variables for the GUI. Setting them by hand works too, against any repository the GUI opens; the views then show that repository's archives with the sample identity, so use a sample repository only.

| Variable | Meaning |
|---|---|
| `VERIARCHIVE_GUI_SCREENSHOTS` | Folder for the PNG files; turns the mode on |
| `VERIARCHIVE_SCREENSHOT_REFERENCE` | uuid of archive A: verified, compared, its runs in History, the result page of its FULL verification shown |
| `VERIARCHIVE_SCREENSHOT_COPY` | uuid of archive B: compared with A |
| `VERIARCHIVE_SCREENSHOT_EARLIER` | uuid of archive C, registered without chunk hashes: its details shown in Archives with the menu of FULL verify open, which offers "FULL verify, and add chunk hashes" |
| `VERIARCHIVE_SCREENSHOT_FOLDER` | archive A's folder: typed into Register Archive, and verified as "Verify a folder or volume..." verifies one |
| `VERIARCHIVE_LANG` | The interface language of the pictures |

While the mode runs, every view shows the same sample media identity in place of the recorded one, in every language: volume label `SAMPLE-VOLUME`, device model `SAMPLE-DEVICE`, serial `SAMPLE0001`, capacity 1.0 TiB, and sample values in every other identity field (filesystem type and id, volume GUID, WWN, bus); whether the device rotates (HDD or SSD) is unknown. One function decides which identity the views show: `displayed_identity` in `src/gui/displayed_identity.cpp`. Normal use always shows what was recorded, and the smoke test (`VERIARCHIVE_GUI_SMOKE_TEST`), which takes precedence, shows nothing.

The GUI exits with status 1 and writes the reason to stderr when a step fails, for example when an archive is not in the repository, the verification does not reach a result, or "FULL verify, and add chunk hashes" is not offered for archive C. The tour is in `src/gui/screenshots.cpp`.
