Exvius Revival Desktop 0.1.0 (first public test release)
========================================================

Runs FINAL FANTASY BRAVE EXVIUS 9.1.0 on a Windows PC, offline, against the same game server the
iPhone release (Exvius Revival for iOS) uses. The server runs on this computer and only this
computer can reach it. There is no Docker, no Tailscale and no account to make.

In a hurry: "QUICK START.txt" next to this file is the short version.
Source and new versions: https://github.com/CreamySeaman6/Exvius-Revival-Desktop

This program contains no game files. On first start it asks for your own 9.1.0 APK and copies the
game out of it.


What you need
-------------
- Windows 10 or 11, 64-bit, on an Intel or AMD processor.
- Your FINAL FANTASY BRAVE EXVIUS 9.1.0 APK (Android, Global version). A single complete APK, or
  an .xapk / .apks bundle, both work. It must contain the 64-bit ARM library.
- About 31 GB of free disk space during setup, about 15 GB afterwards (the game data is 14.6 GB;
  the game reads it where it is and keeps only a few hundred MB of its own).
- A graphics driver with OpenGL 2.1 or newer (any PC from the last ten years).


How to start
------------
1. Unpack this folder somewhere you have room, for example C:\Games\ExviusRevivalDesktop.
   Keep the folder together; the program looks for its parts next to itself.
2. Double-click "Exvius Revival Desktop.exe". The start window opens.
3. "Choose APK or XAPK": pick your APK. The game library and assets are copied into the "client"
   folder.
4. "Download game data". It can be closed and continued later; it carries on where it stopped.
5. "Play". The game window opens. Close the window to stop; the server stops with it.

The first run takes a few minutes and goes like this:
  - The title screen shows some labels as codes (for example BTN_INFO). That is the game before
    its text has arrived.
  - Tap the screen, choose "None" for account linking, and accept the download prompts. The game
    fetches its tables and text (about 40 MB) from the server on this computer.
  - The title screen now reads normally. Tap to start.
  - Everything else the game reads straight from the "data" folder. Small download prompts still
    appear now and then (banner pictures for a new week, a table the server has updated); they
    come from the server on this computer and take a moment.


The game window
---------------
- Mouse click and drag = touch.
- A click made while the mouse is still moving counts as a tap on the spot where the button went
  down, not as a slide, so units can be clicked one after the other without stopping the mouse
  on each. A slide (pulling a unit's panel for abilities or to guard) starts from a pointer at
  rest, or is held a little longer than a click. If this gets in your way, add the line
  tap_guard=0 to client\desktop.ini.
- The mouse wheel scrolls: turning it over the game slides a finger over that spot, so lists move
  as they do under a thumb. Hold Shift to scroll sideways. (In a battle the same slide over a unit
  is the game's own swipe. "Mouse wheel scrolls the game" in Settings turns it off.)
- When the game asks for text (a name, a search), type it; Enter confirms.
- The window has no Windows title bar. The bar along its top is the title bar: press on a free
  part of it and drag to move the window, drag the window's edges to resize it. The game's picture
  scales to fit.

The bar above the game, left to right:
  Back        the Android Back button (Esc and Backspace do the same)
  Speaker     sound on or off
  Slider      volume; the game starts at 25% and remembers what you set (F9 quieter, F10 louder)
  Camera      saves a picture of the game to the "pictures" folder (F12)
  Corners     full screen and back (F11, or Alt+Enter)
  Gear        a small menu: show the frame rate, smooth picture, match the screen's refresh,
              sound while another window is in front, window size, full screen, open the
              pictures folder
  _  []  X    minimise, fill the screen (the taskbar stays in view), close

The start window and Settings are made the same way: no Windows title bar, their own three buttons
in the top right corner, and pressing on a free part of the page (or the strip along the top of
Settings) moves the window. If you would rather have the ordinary Windows frame around all three
windows, turn on "Windows title bar" in Settings, Game window.

The icon in the taskbar and Alt+Tab is a Chocobo taken from your own game files the first time the
program starts after the APK is set up (it is kept as client\icon.rgba). Before that, and on the
.exe files themselves, the icon is this program's own crystal.

"Match the screen's refresh" is off at first. On a 60 Hz screen it makes each frame wait for the
screen instead of the game's own clock, which removes the occasional repeated or skipped frame.
On a screen that is not at 60 Hz it does nothing. Try it and keep whichever feels smoother.

The trailers the game shows unasked on the Home screen and on a summon banner do not play: they
need Android's own video decoder, which a PC does not have. They close at once and the game
carries on. Other movies in the game were not tried and most likely behave the same.

Pages the game opens inside itself (the News notices, the Drop Rates pages, reward lists) are
drawn by the server on this computer and shown in the game. Scroll them with the mouse wheel or by
dragging; links can be clicked. Page Up, Page Down, Home and End work too.


Settings
--------
"Settings" in the start window has pages for:
  Game window                 size, full screen, volume and the other things in the bar's menu
  History replay              following Global's history, where it starts, how fast it goes
  Weekly change and rotation  the day, hour and time zone the week turns over
  Lapis and gifts             the Lapis multiplier, daily quests, weekly tickets, first-clear rewards
  Drops and trust             event currency, enemy drops, trust from quests
  Units and summons           story units, limited units, what summons can give
  Other players and the Arena made-up friends and Arena opponents
  Events and the Vortex       how archived events are listed
  This computer               processor cores, reading the data in place, log sizes
  Other settings              any other server switch, as NAME=VALUE lines

These are the same switches the iPhone release keeps in docker-compose.yml, in plain words.
server\README.md lists every switch the server has. Changes are saved as you make them and apply
the next time you press Play. What you choose is kept in settings.json and client\desktop.ini in
this folder; delete those two files to go back to the defaults.


Where everything is
-------------------
Everything this program creates is inside this folder:

  client\     the game library and assets from your APK, the game's own downloads, runner.log,
              desktop.ini (the game window's settings), icon.rgba (the window icon)
  data\       the server's game data
  save\       your save (save\state) and the server's records
  logs\       server.log, launcher.log, launcher-fatal.log
  pictures\   pictures taken with the camera button
  settings.json   what was chosen in Settings

Nothing is installed anywhere else: no registry entries, no services, no start-up items, no
files in your user profile.


Removing it
-----------
Close the game, then delete this folder. That removes all of it.
To keep your save, copy the "save" folder somewhere first.
To start the game over without removing anything else, delete the "save" folder and the
"client\game\files" folder and the files "client\game\prefs.tsv" and
"client\game\files-removed.txt".

If you used an earlier test build, "client\game\files" holds about 5 GB of copies the game no
longer needs. To get that space back without losing your save, delete "client\game\files",
"client\game\prefs.tsv" and "client\game\files-removed.txt" together (not "save"). The next
start goes through the short first-run prompts again and then continues with your save.


If something goes wrong
-----------------------
- "The game stopped unexpectedly": the details are in client\runner.log.
- Clicks or scrolling in the game misbehave: in Settings, Other settings, add the line
  FFBE_TOUCH_LOG=1 and play again. client\runner.log then lists every touch the game was given,
  which is what is needed to see what went wrong.
- The server did not start: the details are in logs\server.log.
- The start window or Settings closes by itself or does not react: the details are in
  logs\launcher.log and logs\launcher-fatal.log.
- Settings does not open, or a window cannot be moved: turn on "Windows title bar" in Settings,
  or add the line system_frame=1 to client\desktop.ini, to get ordinary Windows frames.
- Black window or no picture: update the graphics driver.


What is in this folder
----------------------
  Exvius Revival Desktop.exe   starts app\launcher.py
  app\         the start window, its settings pages, the local server start-up and the code that
               draws the in-game web pages (Python), with the fonts they use
  runner\      runner.exe (runs the game library), game.elf (the Android stand-in the library runs
               inside), SDL2.dll, font.ttf
  python\      a private copy of Python 3.12 used by the launcher and the server
  server\      the game server (the same engine as the iPhone release)
  source\      the source of runner.exe and game.elf, and the scripts that build them
  LICENSES.txt, QUICK START.txt


Credits
-------
The game engine, the preserved game data and the original offline Android app are Exvius Offline by
Elvlin (https://github.com/Elvlin/exvius-offline). The game data is downloaded from that project's
releases. The server and its history replay are from Exvius Revival for iOS
(https://github.com/CreamySeaman6/Exvius-revival-ios). Exvius Revival Desktop is by rivbg.

FINAL FANTASY BRAVE EXVIUS belongs to Square Enix and gumi. This is an unofficial fan project, not
affiliated with either.
