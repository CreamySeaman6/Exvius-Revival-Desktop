# Exvius Revival Desktop 0.2.0

Scrolling with the mouse wheel and by dragging is fixed, Convert to Prism and Fragments work, and the game server is
the iPhone release's of 2026-11-13. From this version on, the start window offers new versions itself.

**0.1.3 and earlier have no updater, so this version is installed by hand, once** (see "Updating" below). After
that the start window tells you when a new version is out and can install it for you.

## Fixed

- **Scrolling ran the wrong way or jumped.** Every list in the game has its own scroll bar, a thin strip along its
  right edge, and a touch on it moves the list to that place and then the other way round, as a scroll bar does.
  The mouse wheel works by sliding a finger where the pointer is, so with the pointer near a list's right edge
  turning the wheel down sent the list up. The wheel now always moves the list, wherever on it the pointer is. Dragging
  the scroll bar itself with the mouse button still works as a scroll bar: down is down.
- **The wheel could press what was under the pointer.** A small turn (a touch pad, a wheel that spins freely) was
  a slide too short for the game to count as scrolling, so it counted as a tap. A turn of the wheel never presses
  anything now, and a turn too small to scroll waits for more.
- **The wheel did nothing, or pressed again and again, with the pointer near the window's edge.** It scrolls from
  there too.
- **A list let go in mid-flick stopped dead; one that had been stopped glided away.** The game works out how fast
  a finger was going from the last places it was told of. It was told every report of the mouse, up to a thousand a
  second, where a touch screen tells it once a frame. Now a flick lets the list glide on as it does on a phone, and
  a list you stop before letting go stays where you put it.
- **A quick flick did nothing, or opened the banner under the pointer.** A press made while the mouse is moving is
  held back for a moment so that clicking units in a battle is not read as a swipe. On a list that held back the
  whole flick. On a scrolling list the hold is now short; in battles it is as before.
- **A quick click could be lost** when the mouse moved a few pixels while the button was down: the button lit up
  and nothing happened. Found while testing the above; such clicks count now.
- **Convert to Prism / Fragments was empty**, and the awakening page said **"Unit Not in Possession"** for a unit
  you had another copy of. Both read a table the game never fetched when it was started fresh. They work now, from
  the first start.
- **"A connection error has occurred" at the first log-in after an update**, when the Vortex's banners had changed.
- **Stars showed as empty boxes** in the game's own lettering, for example "Guaranteed Ticket (5★/NV)" in a shop,
  and so did the "until" sign in front of a summon's end date.

## New from the iPhone release

The game server is the one from Exvius Revival for iOS as of 2026-11-13. Its change log has everything; the larger
points:

- **Summon art comes with the game data.** One new data pack (0.3 GB) holds every summon picture as a finished
  file. The server no longer downloads pictures from the internet while you play, and 151 more summons have real
  art instead of a plain titled banner. The start window offers **Download new game data** after you update; only
  the new pack is downloaded.
- Summon pages show their art across the frame, step-up summons keep their art on every step, and the larger
  summon screen has the price inside its buttons.
- Summons no longer give a unit's Brave Shift form as a unit of its own (a copy that could not be awakened). Such
  copies you already have turn into the real unit the next time you log in.
- The battle item set: items used in battle stay used, the set no longer refills from items you do not have, and
  the Items tab shows what is not in the set.
- Town quests stay done after a restart; the Quest Report lists them.
- Treasure chests with equipment, materia or key items give their item; ones you opened before arrive by mail once.
- The Big Bridge, The Enchanted Maze and 85 other explorations have the enemies their fixed fights were missing.
- Kilika Woods and three other events offer their later quests; three explorations have their random battles.
- Bulk Fusion fuses every unit you picked. The results screen shows only the EXP each unit earned.
- The 5★ Select Summon lists the limited and collaboration units; summon pools, raid boxes and exchange shops were
  corrected in many places.

Settings: "Mail treasure chest items that never arrived" (Lapis and gifts) and "Check for a new version when the
program starts" (This computer) are new. "Download deal banner art once" is gone: that art is in the game data now.

## Updates from now on

When the start window opens it asks GitHub for the newest release. If there is one, it shows the version and a line
about it, with **Update now**, **Later** and **Skip this version**. Update now downloads about 15 MB, checks it,
closes, replaces the program's own files and starts again. Your save, settings, game data and the game files from
your APK are never touched. If anything fails, the old version is put back and tells you why. It is never
required, nothing but the request itself is sent, and "Check for a new version when the program starts" in
Settings turns it off.

## Updating

By hand, this once. Your save, your settings and the game data stay where they are.

1. Close the game and the start window.
2. Download `ExviusRevivalDesktop-win64-0.2.0.zip` below and unpack it (right-click, Extract All).
3. Open the unpacked `ExviusRevivalDesktop` folder, select everything in it (Ctrl+A) and copy (Ctrl+C).
4. Open the folder you already play from, paste (Ctrl+V), and choose **Replace the files in the destination**.
5. Start **Exvius Revival Desktop.exe**. It says "New game data": click **Download new game data** (0.3 GB), then
   **Play**.
6. At the first log-in the game downloads its changed tables (about 40 MB, a minute or so).

`ExviusRevivalDesktop-update-0.2.0.zip` and `update.json` are what the updater uses; you do not need them.

## Which APK

The original Final Fantasy Brave Exvius 9.1.0 APK, Global version, as it came from the app store. Not the APK of
the Exvius Offline Android app, and not another version. To check one, open it with 7-Zip:
`lib/arm64-v8a/libgame.so` in the 9.1.0 APK is exactly 60,410,728 bytes.
