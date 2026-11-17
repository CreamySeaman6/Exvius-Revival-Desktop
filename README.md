# Exvius Revival Desktop

Play **Final Fantasy Brave Exvius** (Global, version 9.1.0) again on a **Windows PC**, offline, in a window of its own.

## Already playing? How to update

Your save, your settings and the game data stay where they are. You do not choose the APK again, and only game data that is new is downloaded.

**From version 0.2.0 on, the start window does it for you.** When a newer version is out it says so and offers **Update now**, **Later** and **Skip this version**. Update now downloads the update, checks it, closes, replaces the program's own files and starts again. It is never required; see [Updates](#updates).

**Versions 0.1.3 and earlier have no updater**, so the step to 0.2.0 is made by hand, once:

1. **Close the game and the start window.**
2. Go to [Releases](../../releases) and download the newest `ExviusRevivalDesktop-win64-<version>.zip`.
3. Right-click the downloaded zip and choose **Extract All**. This gives you a new folder called `ExviusRevivalDesktop`.
4. Open that new folder. Press **Ctrl+A** to select everything in it, then **Ctrl+C** to copy.
5. Open the folder you already play from (the one with **Exvius Revival Desktop.exe**, `save` and `data` in it). Press **Ctrl+V** to paste.
6. Windows asks what to do with files that have the same name. Choose **Replace the files in the destination**.
7. Double-click **Exvius Revival Desktop.exe**. If the new version comes with new game data, the start window offers **Download new game data** (0.3 GB for 0.2.0); then click **Play**, as always.

You can delete the downloaded zip and the new folder afterwards. If you want to be extra safe, make a copy of your `save` folder before step 5.

New here? Start at [Quick start](#quick-start).

---

This is the desktop edition of [Exvius Revival for iOS](https://github.com/CreamySeaman6/Exvius-revival-ios). It runs the same game server, with the same historical replay of Global's summons, events and shops, and the same settings. The difference is where the game runs: instead of an iPhone talking to a server in Docker over Tailscale, the server and the game both run on your PC, out of one folder. There is no Docker, no Tailscale, no certificate to trust and no account to make.

The game itself is the Android version. The program takes the game library and assets out of **your own APK**, runs that library on the PC, and points it at a server on the same computer that only that computer can reach.

> **This repository and its downloads contain no game files.** No game library, no assets, no game data, no APK. You supply the APK; the game data is downloaded during setup from the data releases, exactly as the iOS edition does.

> **Status: public test release.** It is played on real Windows PCs and tested automatically on Linux and under Wine, but it is young. [Known issues](#known-issues) lists what does not work yet.

---

## What you need

- **Windows 10 or 11, 64-bit**, on an Intel or AMD processor.
- **Your Final Fantasy Brave Exvius 9.1.0 APK** (Android, Global version): the original game as it came from the app store. A single complete APK, or an `.xapk` / `.apks` bundle. It must contain the 64-bit ARM library.
  **The Exvius Offline app's APK does not work here.** Its game library is changed to run inside that app; this program recognises it and asks for the original instead.
  **Other versions of the game do not work either**, including older ones that a download site labels 9.1.0. To check an APK, open it with 7-Zip: `lib/arm64-v8a/libgame.so` in the 9.1.0 APK is exactly **60,410,728 bytes**. The program checks this too and says so when you choose an APK that is not it.
- **About 18 GB of free disk space** during setup, about 16 GB afterwards. The game data is a 14.9 GB download.
- **A graphics driver with OpenGL 2.1 or newer**, which is any PC from the last ten years.

## Quick start

1. Download `ExviusRevivalDesktop-win64-<version>.zip` from [Releases](../../releases).
2. Unpack it somewhere with room, for example `C:\Games\ExviusRevivalDesktop`. Keep the folder together.
3. Double-click **Exvius Revival Desktop.exe**. The start window opens.
4. Click **Choose APK or XAPK** and pick your 9.1.0 APK. The game is copied out of it into the folder.
5. Click **Download game data** (14.9 GB). You can close the window and carry on later; it continues where it stopped.
6. Click **Play**.

The first time you play, the title screen shows a few labels as codes (`BTN_INFO`). Tap the screen, choose **None** for account linking and accept the download prompt; the game fetches its text (about 40 MB) from the server on your PC and the title screen then reads normally.

Windows may warn that the program is from an unknown publisher, because it is not signed. Choose **More info → Run anyway**.

The full user guide is `README.txt` inside the download ([`docs/README.txt`](docs/README.txt) here).

## Playing

- **Mouse click and drag are touch.** Drag a list to scroll it and let go while moving to flick it, as on a phone. The mouse wheel scrolls the list under the pointer. A list's own scroll bar, the thin strip along its right edge, can be dragged too.
- The bar above the game has Back (also `Esc`), sound and volume (`F9` / `F10`), a camera (`F12`), full screen (`F11`) and a small menu.
- The windows have no Windows title bar: drag the bar to move the game window, drag its edges to resize it. If you prefer ordinary Windows frames, turn on **Windows title bar** in Settings.
- **Settings** in the start window has the server's switches in plain words: the history replay, the weekly change, Lapis and gifts, drops, units and summons, and so on. They are the same switches the iOS edition keeps in `docker-compose.yml`.

Your save is in the `save` folder next to the program. Nothing is installed anywhere else: no registry entries, no services, nothing in your user profile. **To remove it, delete the folder.**

## Updates

When the start window opens it asks GitHub for the newest release of this repository. If that is newer than what you have, the start window shows its version and a line about what changed, with three choices:

- **Update now** downloads the update (about 15 MB), checks its size and SHA-256 against the release, unpacks it inside the program's folder, closes, replaces the program's own files and starts the new version. If the new version comes with new game data, it then offers to download just that.
- **Later** asks again the next time you start.
- **Skip this version** does not offer that version again. A later one is offered.

It is never required, and **Settings → This computer → Check for a new version when the program starts** turns the question off.

- **What is sent:** the request for the release list, and nothing else. No identifier, no version number, nothing about your PC or your save.
- **What is replaced:** only the program's own parts: `app`, `runner`, `server`, `source`, the `.exe` you double-click and the three text files (and `python`, if a release ever needs a newer one). `client`, `save`, `data`, `logs`, `pictures` and `settings.json` are never touched; an update that names anything else is refused whole.
- **If it fails:** the replaced files are first moved to `update\backup\<old version>`. If any step fails, or the PC is switched off half-way, everything is put back and the old version starts, with a message saying why. `logs\update.log` has the details. The backup of the last update stays in `update\backup`; you can delete it.
- **Removing the program** is still just deleting the folder.

## How it works

```
Exvius Revival Desktop.exe
  └─ start window (Python)            choose the APK, download the data, Settings, Play
       ├─ game server                 the Exvius Revival server engine on 127.0.0.1, this computer only
       └─ runner.exe                  runs the Android game library
            └─ game.elf               a small Linux program that stands in for Android
                 └─ libgame.so        the game, from your APK
```

- **`runner.exe`** is an ARM64 machine in a Windows program: a JIT ([dynarmic](https://github.com/azahar-emu/dynarmic)) for the processor, the Linux system calls the game makes, its threads spread over a few host threads, OpenGL ES 2 passed on to desktop OpenGL, and SDL2 for the window, input and sound.
- **`game.elf`** is a static 64-bit ARM Linux program. It loads the game library and supplies what the library expects from Android: the C library differences, JNI and the Java helpers the game calls, OpenSL ES sound, text drawing, HTTP.
- **The server** is the Exvius Revival server engine, the same code the iOS edition runs in Docker, started by the launcher on a free local port with plain HTTP.
- **In-game web pages** (News notices, Drop Rates) are drawn by the server as pictures and shown by the runner in the place the game asks for them; no browser is embedded.
- **The game reads the server's data where it is.** It does not download a second copy of the 14.9 GB.
- **`Exvius Revival Desktop.exe`** starts the start window. It is also what puts an update in place, because it is the one part that runs while nothing else of the program does.

## Known issues

- **Movies do not play.** The trailers the game opens on the Home screen and on summon banners need Android's video decoder. They close at once and the game carries on.
- **Loading pauses.** Logging in, and now and then entering a screen, pauses for a few seconds while the game reads its tables. That is the game's own loading, running through the JIT.
- **Windows only for now.** The runner also builds for Linux (that is how most of it is tested), but there is no Linux package.
- **Unsigned.** Windows SmartScreen and some antivirus programs warn about new unsigned programs.

- **Antivirus.** `runner.exe` translates the game's ARM code into PC code while it runs, which some antivirus programs take for suspicious behaviour and stop. If the game closes right after starting, add the program's folder to your antivirus's exceptions and check that it did not move `runner.exe`, `SDL2.dll` or `game.elf` into quarantine.

If something goes wrong, the logs are in the program's folder: `client\runner.log` (the game), `logs\server.log` (the server), `logs\launcher.log` and `logs\launcher-fatal.log` (the start window).

## Building from source

Everything is built on Linux (Ubuntu 24.04) and cross-compiled for Windows. [`docs/BUILDING.txt`](docs/BUILDING.txt) has the details.

```
sh guest/build.sh                                   build/game.elf
sh build_linux.sh                                   build/linux/runner      (for testing)
sh build_windows.sh                                 build/windows/runner.exe
DEV=<Exvius-revival-ios checkout> sh pack.sh        dist/ExviusRevivalDesktop
DEV=<the same> sh release.sh 0.2.0                  final/release-0.2.0: the full zip, the update zip and update.json
```

The server is not in this repository. `pack.sh` copies it from a checkout of [Exvius Revival for iOS](https://github.com/CreamySeaman6/Exvius-revival-ios) when it assembles the package.

| Folder | What |
|---|---|
| `app/` | The start window, Settings, the local server start-up and the code that draws the in-game web pages (Python, Pillow, tkinter) |
| `runner/` | `runner.exe`: the JIT glue, system calls, scheduler, OpenGL, window, input, sound (C, one C++ file) |
| `guest/` | `game.elf`: the Android stand-in the game library runs inside (C) |
| `stub/` | The small program you double-click, which also puts a downloaded update in place |
| `tools/` | Generators for the OpenGL call table and the program icon |
| `test/` | Scripts that drive the game on Linux and under Wine |
| `docs/` | The user guide, build notes and third-party licences shipped in the package |
| `fonts/` | Roboto, Cinzel and DejaVu Sans, with their licences |

## Credits

- The server and the historical replay are from [Exvius Revival for iOS](https://github.com/CreamySeaman6/Exvius-revival-ios).
- The runner uses [dynarmic](https://github.com/azahar-emu/dynarmic) (azahar-emu fork), [SDL2](https://libsdl.org) and [stb](https://github.com/nothings/stb). The full list with licences is in [`docs/LICENSES.txt`](docs/LICENSES.txt).
- Exvius Revival Desktop is by rivbg.

Final Fantasy Brave Exvius belongs to Square Enix and gumi. This is an unofficial fan project, not affiliated with either.
