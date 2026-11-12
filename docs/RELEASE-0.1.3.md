# Exvius Revival Desktop 0.1.3

Fixes for the summon screen, the Vortex and APKs that cannot run, and the game server brought up to date with the
iPhone release.

## Fixed

- **Summon: "A connection error has occurred", then back to the title.** Some summon banners (value and step-up
  deals, free daily summons, and the banners from late 2023 on) have pictures the game server downloads from a web
  archive after it starts, a few at a time. A banner whose picture had not arrived yet made the game report a lost
  connection. Such a banner now shows a plain titled picture until the real one is downloaded; the next time you
  open the summon screen after that, the real picture replaces it.
- **The Vortex closed the game** the first time you opened it after setting up, or after an update. The game
  downloads its changed data at log-in but only read it at the next start, so the Vortex found part of its data
  missing. The data is now read as soon as it is downloaded. If something is still missing, the game restarts with
  a message instead of closing.
- **"Rotate summons weekly" off showed almost no banners.** It now shows every banner (over a thousand), and it
  also turns the history replay off, so no summon, shop or Vortex event is held back by date. Open the game twice
  after changing it. The first time the summon screen shows every banner, the game downloads their pictures
  (about 150 MB from the server on your own PC, a minute or two).
- **An APK that is another version of the game made the game close at once**, with "guest stopped: invalid memory
  write at 0x0" in `client\runner.log`. Some download sites label older versions as 9.1.0. The start window now
  checks the APK and says what is wrong with it, also for a game set up with an earlier version of this program.

## New from the iPhone release

The game server is the one from Exvius Revival for iOS as of 2026-11-12:

- The friend point Standard Summon has a 10x button: 10 units for 2,000 friend points.
- Clearing a quest gives LB EXP to every unit in the party, as on Global.
- Awakening a 6★ unit to 7★ with an Omniprism works.
- Kira's abilities have English names and descriptions.
- Viewing the Metal Gigantuar's vision card in the VIP Shop (and others sold or won the same way) no longer
  crashes the game.
- With the history replay, banners and events stay up for their whole Global run, so long ones last several weeks.
- Global's banners from September 2023 to the end of service, and the missing Yuffie, Ayaka and Elly banners.

Settings has the new switches: "Banners and events stay up for their whole run" (History replay), "Global's last
year of banners" and "Also the unconfirmed 2024 step-ups" (Units and summons).

After updating, the game downloads its changed data once when you log in (about 40 MB, a minute or so).

## Updating

Your save, your settings and the game data stay where they are. Close the game, unpack this zip, copy everything
from the unpacked `ExviusRevivalDesktop` folder into the folder you already play from, and choose **Replace the
files in the destination**. The README has the steps one by one.

## Which APK

The original Final Fantasy Brave Exvius 9.1.0 APK, Global version, as it came from the app store. Not the APK of
the Exvius Offline Android app, and not another version. To check one, open it with 7-Zip:
`lib/arm64-v8a/libgame.so` in the 9.1.0 APK is exactly 60,410,728 bytes.
