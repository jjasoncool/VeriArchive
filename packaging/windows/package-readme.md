# VeriArchive

VeriArchive records integrity evidence for archive disks and later proves whether the archived files were modified, corrupted, deleted, or added to. It only **detects**: it never repairs, restores, or makes backups.

[繁體中文](README.zh-TW.md)

## Starting the program

The folder the zip was unzipped into, for example `D:\VeriArchive`, holds the whole program; nothing else needs to be installed.

- `bin\veriarchive-gui.exe` is the graphical interface: double-click it.
- `bin\veriarchive-cli.exe` is the command line, for PowerShell or the Command Prompt; run without a command, it lists its commands.
- `bin\veriarchive-device-identity.exe` is the device identity reader. The window starts it only when you ask it to read a device's own serial behind its enclosure, and Windows first asks whether to allow it administrator permission. It is not meant to be opened by hand.

## The guides

The user guides are beside this file, with pictures of the graphical interface: [user-guide.md](user-guide.md) in English and [user-guide.zh-TW.md](user-guide.zh-TW.md) in Traditional Chinese. Start there: they take you from creating the repository to verifying an archive and comparing it with its copy, in the window and on the command line.

## Where the records are

VeriArchive keeps the baseline of each archive (its manifest) and every verification in its **repository**. On first use it creates the repository in `bin\data` and remembers it in `bin\config.json`; where the program folder cannot be written to, it uses `VeriArchive` in your home folder instead and remembers that in `%APPDATA%\VeriArchive\config.json`. The Settings view shows which repository is in use.

**Back up the repository.** Losing it loses the verification history. Backing up the whole program folder backs up `bin\data` with it.

To update the program, close it and unzip the new version's zip into the same folder: the zip holds no `bin\data` and no `bin\config.json`, so your records stay. "Installing (Windows)" in the guide says how.

`NOTICE.txt` beside this file names the libraries the program uses; `share\licenses` holds their licenses.
