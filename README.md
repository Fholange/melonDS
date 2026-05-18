# melonDS Switch — WebDAV Save Sync Fork

This is a fork of [Gheovgos's melonDS Switch port](https://github.com/Gheovgos/melonDS), which itself is a continuation of RSDuck's standalone melonDS port for Nintendo Switch. This fork adds automatic WebDAV cloud save synchronization, designed to keep DS save files in sync across devices that use RetroArch cloud sync — including PC, iOS, and Switch.

---

## What this adds

Automatic save sync over WebDAV on ROM launch and app exit, with a manual sync button available in both the settings menu and the in-game pause menu. Progress is shown on screen during sync so you always know what is happening.

---

## Prerequisites

### RetroArch compatibility

For saves to stay in sync between this app and RetroArch on other devices, the following must be true:

- RetroArch must use the **melonDS DS** core (not the legacy melonDS core). Save files must use the `.srm` extension, which this core produces by default.
- RetroArch's save directory must be explicitly set. The remote path configured in melonDS must match where RetroArch stores its saves on the WebDAV server. For example, if RetroArch is configured to sync saves to `/retroarch/cores/savefiles/`, that same path must be set as the remote path in melonDS.
- RetroArch cloud sync must be enabled and pointed at the same WebDAV server. [Koofr](https://koofr.eu) works well as a free provider with built-in WebDAV support. Configure it under Settings → Saving → Cloud Sync.
- The game must be the same ROM on both sides. RetroArch names save files after the ROM filename. melonDS on Switch will look for and upload a file with the same name.

### melonDS configuration

Add the following to `/switch/melonds/melonds.ini` on your SD card:

```ini
WebDAV.URL = https://your-webdav-server/path
WebDAV.Username = your@email.com
WebDAV.Password = yourpassword
WebDAV.RemotePath = /retroarch/cores/savefiles/
```

These fields are also editable from within the app under Settings → WebDAV Save Sync.

---

## How save version decisions are made

When a sync runs, the app compares three things: the local save file's modification time, the remote file's last-modified timestamp (fetched via HTTP HEAD), and the time of the last successful sync stored in the config.

**Remote newer, local unchanged** — the remote save is downloaded and replaces the local file. This happens when another device saved and synced since the last time melonDS ran.

**Local newer, remote unchanged** — the local save is uploaded to the server. This happens on exit after playing.

**Both changed since last sync** — this is a conflict. The file with the newer modification time wins. The losing file is backed up before being overwritten.

**Neither changed** — nothing happens.

---

## Safety measures

Before any file is overwritten, a timestamped backup of the existing local save is written to `/switch/melonds/backups/` on the SD card. Backups are named `<romname>.sav-YYMMDD-HHMMSS`.

By default backups are kept forever. You can set a limit under Settings → WebDAV Save Sync → **Max backups (0 = unlimited)**. When a limit is set, the oldest backups for that game are deleted automatically after each new backup is created, keeping only the most recent N.

Downloads are written to a temporary file first and only renamed into place if the transfer completes successfully. A failed or interrupted download will never leave a partial or corrupted save file.

When syncing from the in-game pause menu or settings, the sync is **upload-only**. The app will never download and overwrite a save file while a game is actively running, since the emulator holds the save in memory and would overwrite any downloaded file on exit anyway.

The app also updates RetroArch's `manifest.server` file on the WebDAV server after every upload so that RetroArch on other devices correctly detects the new version. This means RetroArch may report "finished with conflicts" when it next syncs — this is expected and harmless. With RetroArch's sync mode set to server-wins, it will always resolve to the correct save.

---

## Sync timing

- **On ROM launch** — syncs before the game loads. A progress screen is shown during this time.
- **On app exit** — syncs the current save when melonDS is fully closed.
- **Manual** — available in Settings and in the in-game pause menu. Upload-only.

---

## Building

Follow the build instructions from the [original Gheovgos repository](https://github.com/Gheovgos/melonDS) with two differences specific to this fork:

**1. Install the mbedtls package** (used for MD5 hashing):

```bash
dkp-pacman -S switch-mbedtls
```

`switch-curl` is already required by the original build, so no change there.

**2. The CMakeLists is already updated** in this repo — `mbedtls`, `mbedcrypto`, and `mbedx509` are linked automatically and `WebDAVSync.cpp` is included in the build. No manual changes needed.

---

## License

[![GNU GPLv3 Image](https://www.gnu.org/graphics/gplv3-127x51.png)](http://www.gnu.org/licenses/gpl-3.0.en.html)

melonDS is free software: you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, either version 3 of the License, or (at your option) any later version.
