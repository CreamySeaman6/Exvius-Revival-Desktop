# Exvius Revival Desktop 0.1.0

The first public test release. Final Fantasy Brave Exvius (Global, 9.1.0) on a Windows PC, offline, as the desktop
edition of [Exvius Revival for iOS](https://github.com/CreamySeaman6/Exvius-revival-ios): the same server, the same
historical replay, with the game and the server both running on your PC out of one folder.

**The download contains no game.** You need your own FFBE 9.1.0 APK (Android, Global). The game data (14.6 GB) is
downloaded during setup from the [Exvius Offline](https://github.com/Elvlin/exvius-offline) releases.

## Quick start

1. Download `ExviusRevivalDesktop-win64-0.1.0.zip` below and unpack it somewhere with about 31 GB free.
2. Double-click **Exvius Revival Desktop.exe**. If Windows says the publisher is unknown: **More info → Run anyway**.
3. **Choose APK or XAPK** and pick your APK.
4. **Download game data**. It can be closed and continued later.
5. **Play**.

The first time, the title screen shows some labels as codes. Tap, choose **None** for account linking, accept the
download prompt, and the title screen reads normally.

`QUICK START.txt` and `README.txt` in the folder have the rest: the controls, the settings, where everything is
kept, and how to remove it (delete the folder; nothing is installed anywhere else).

## What works

- Played so far on a PC: the first run and tutorial, story quests and their battles, summons, the Shop and the
  Home screen, with sound. The rest of the game runs on the same server as the iOS edition but has had less play here.
- The News notices and Drop Rates pages inside the game.
- A resizable window, full screen, a volume control, a screenshot button, the mouse wheel in lists.
- Every server setting of the iOS edition, on plain-language pages in Settings.

## Known issues

- Movies do not play (they need Android's video decoder); the game skips them.
- Logging in pauses for a few seconds while the game reads its tables.
- The program is unsigned, so Windows SmartScreen and some antivirus programs warn about it.
- Windows only.

If something goes wrong, the logs are in the folder: `client\runner.log`, `logs\server.log`, `logs\launcher.log`.
