"""make_icon.py <out.ico>: the program's own icon (the crystal on a blue tile) for the Windows programs' file icons.

The window and taskbar icon while the program runs is made at run time (app/launcher_ui.py: a Chocobo from the user's
own game files when they are installed). A file's icon has to be inside the file, so it is this program's own drawing.
"""
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / 'app'))
import launcher_ui  # noqa: E402

icon = launcher_ui.crystal_icon(256)
icon.save(sys.argv[1], format='ICO', sizes=[(16, 16), (20, 20), (24, 24), (32, 32), (40, 40), (48, 48), (64, 64), (128, 128), (256, 256)])
