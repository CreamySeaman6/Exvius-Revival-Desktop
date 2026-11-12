# Exvius Revival Desktop 0.1.1

Fixes for the first reports from other people's PCs. To update, unpack this over your existing folder (your save,
game files and data stay) or start fresh; nothing else needs doing.

## Fixed

- **"Download game data" failed at once on some PCs** although the internet worked. The bundled Python only trusted
  the certificates already in the Windows store, and a PC that had not yet fetched the download server's root, or
  whose antivirus scans HTTPS, refused the connection. Downloads are now checked the way your browser checks them.
  Thanks to alexis-jayden824 for finding and fixing this.
- **The Exvius Offline app's APK made the game window open and stay black**, after which Windows reported that
  runner.exe had stopped working. That APK carries a game library changed to run inside that app. The start window
  now recognises it and asks for the original 9.1.0 APK, also if it was installed with 0.1.0.
- **The game window could be marked "Not Responding"** while the game was loading (starting up, logging in). The
  window now stays alive during those pauses.
- If the runner itself crashes, what happened is written to `client\runner.log` instead of leaving only the Windows
  dialog.

## Which APK

The original Final Fantasy Brave Exvius 9.1.0 APK, Global version, as it came from the app store. Not the APK of
the Exvius Offline Android app.

## If your antivirus stops the game

`runner.exe` translates the game's ARM code into PC code while it runs, which some antivirus programs take for
suspicious behaviour. Add the folder to its exceptions and check that it did not move `runner.exe`, `SDL2.dll` or
`game.elf` into quarantine; unpack the download again if it did.
