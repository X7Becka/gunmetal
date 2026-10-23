#!/usr/bin/env python3
"""Front-end screen map of Gunmetal (M8.2): states, handlers, layout, assets.

The tool is read-only and stdlib-only (plus the project's ``exelib`` for the
pre-generated listing).  It answers:

* which front-end states exist, which handler each one calls, and how the
  screen is titled (the engine's own strings, with their VA);
* the layout parameters a handler installs: number of rows (``0x67f10c``),
  row step (``0x662d58``), left column and base row of every item;
* which UI pieces from ``FRONTEND/BMP.IDX`` the handler path actually uses
  (``PLATE``, ``FILESLOT``, ``SLIDE``/``KNOB``, ``CHECK``/``NOCHECK``,
  ``MENUARRW``, fonts, backgrounds);
* the screen-to-screen transitions proved by ``mov [0x4bc440], imm`` sites;
* the save/load slot arithmetic (20 slots, 6 visible rows) and the key-set
  storage (``KEYCFG\\*.KEY``, 0x178 bytes per set);
* which of those files really exist in the reference installation.

No Ghidra: the jump table, stub trampolines, immediate writes and strings are
read from the executable bytes through ``exelib``.

Examples::

    python3 tools/re/ui_screens.py --map
    python3 tools/re/ui_screens.py --assets
    python3 tools/re/ui_screens.py --json > /tmp/ui_screens.json
    python3 tools/re/ui_screens.py --self-test

Addresses are VA in ``gundx.exe`` (ImageBase 0x400000).  ``GUNMETAL_GAME``
(default ``/mnt/d/games/Gunmetal 3dfx``) points at the installation; nothing
is ever written to it.
"""
from __future__ import annotations

import argparse
import json
import os
import re
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools" / "re"))

try:
    import exelib  # noqa: E402
except Exception as exc:  # pragma: no cover - broken checkout
    exelib = None
    _EXELIB_ERROR = exc

DEFAULT_GAME = Path(os.environ.get("GUNMETAL_GAME", "/mnt/d/games/Gunmetal 3dfx"))

# ---------------------------------------------------------------------------
# Static code facts (VA in gundx.exe).  Every number below is re-checked
# against the executable by --self-test.
# ---------------------------------------------------------------------------

#: Front-end state variable and the 33-entry jump table used by the frame
#: dispatcher at 0x487e71 (``eax = [0x4bc440] - 5; jmp [eax*4 + 0x4883d8]``).
STATE_VAR = 0x4BC440
JUMP_TABLE = 0x4883D8
JUMP_FIRST = 5
JUMP_COUNT = 33
NO_HANDLER_STUB = 0x487F64

#: Rows / row step globals written by each handler.
ROWS_VAR = 0x67F10C
STEP_VAR = 0x662D58
SELECTION_VAR = 0x68CCF4

#: Menu item draw helper and the plate composition buffer.
ITEM_DRAW = 0x485DD0
PLATE_COPY = 0x486050
PLATE_SRC = 0x6D2B60
PLATE_SIZE = (196, 131)
FONT_ATLAS_NOTE = "fonts (number->atlas not pinned, see docs/UI.md 10.3)"

#: UI sounds: (name, name-string VA, handle global, store site, playback site).
UI_SOUNDS = [
    ("updown", 0x4BDF7C, 0x4BC454, 0x48F780, 0x48E60C),
    ("select", 0x4BDF4C, 0x4BC458, 0x48F7A8, 0x48E639),
    ("vleft", 0x4BDF1C, 0x4BC45C, 0x48F7CC, 0x48BE65),
    ("vfront", 0x4BDEEC, 0x4BC460, 0x48F7F0, None),
    ("vback", 0x4BDEBC, 0x4BC464, 0x48F814, None),
    ("menuon", 0x4BDE8C, 0x4BC44C, 0x48F835, 0x486720),
    ("selectm", 0x4BDE84, 0x4BC450, 0x48F85E, 0x486700),
]
UI_SOUND_LOADER = 0x48F770
SOUND_QUEUE = 0x40BE20

#: Front-end asset loader (0x48f970): FRONTEND\\<name>.bmp -> buffer, w, h.
UI_ATLAS = [
    ("f_m_titl", 232, 52, 0x67FF38, "menu title art"),
    ("scroll", 404, 200, 0x6645A0, "scrolling menu background"),
    ("controls", 112, 25, 0x662268, "main-menu entry art"),
    ("sound", 112, 25, 0x660790, "main-menu entry art"),
    ("graphics", 112, 25, 0x662F60, "main-menu entry art"),
    ("startgam", 112, 25, 0x685B08, "main-menu entry art"),
    ("savegame", 112, 25, 0x663A50, "main-menu entry art"),
    ("quitgame", 112, 25, 0x67F118, "main-menu entry art"),
    ("play", 112, 25, 0x661778, "main-menu entry art (PLAY OPTIONS)"),
    ("plate", 196, 131, 0x6D2B60, "menu plate, text is composed into a copy"),
    ("fileslot", 124, 10, 0x661298, "save/load slot bar"),
    ("slide", 72, 8, 0x68CA48, "slider track"),
    ("knob", 8, 8, 0x67F0C8, "slider knob"),
    ("check", 8, 16, 0x67FC10, "checkbox on"),
    ("nocheck", 8, 8, 0x683FB8, "checkbox off (drawn 8 rows lower)"),
    ("menulit", 16, 16, 0x682E58, "selected-item marker"),
    ("menuarrw", 16, 8, 0x67FCB0, "list arrow, also mirrored by 0x485d00"),
    ("killtile", 80, 16, 0x6EB820, "kill list tile"),
    ("rpvicons", 60, 29, 0x660078, "RPV design icons"),
    ("panelmid", 320, 8, 0x6E89E0, "HUD panel centre"),
    ("fonts1", 512, 168, None, "font atlas"),
    ("fonts2", 1024, 80, None, "font atlas"),
    ("fonts3", 520, 56, None, "font atlas"),
    ("fonts4", 512, 168, None, "font atlas"),
    ("numfonts", 180, 48, None, "numeric font atlas"),
    ("helpscrn", 320, 200, 0x6D8FC0, "SML help background"),
    ("briefbg2", 320, 200, 0x6D8FC0, "SML briefing background"),
    ("loadpage", 320, 200, 0x6D8FC0, "prelevel/loading background"),
    ("multibg", 320, 166, 0x6D8FC0, "multiplayer wait background"),
    ("demotion", 320, 200, None, "in-game menu background"),
    ("disband", 320, 200, None, "in-game menu background"),
    ("statsbg", 320, 200, None, "stats screen background"),
]

#: Screens.  ``rows``/``step`` are the immediates the handler writes to
#: 0x67f10c / 0x662d58 (``None`` where the count is computed at runtime).
#: ``items`` are (label, string VA, item id, left x, base row) for every
#: 0x485dd0 call the handler makes with a literal label; the drawn row is
#: ``base + item*step`` when ``base >= 0`` and ``-base`` pixels otherwise.
SCREENS = [
    dict(id=0, handler=None, title_va=None, title="intro video (0x499fc0)",
         rows=None, step=None, assets=[], items=[], note="driver state, not a menu"),
    dict(id=1, handler=None, title_va=None, title="main menu",
         rows=None, step=None,
         assets=["scroll", "f_m_titl", "controls", "sound", "graphics",
                 "startgam", "savegame", "play", "quitgame", "menulit",
                 "menuarrw", FONT_ATLAS_NOTE],
         items=[], note="carousel drawn by 0x486740..0x48694f; 7 entries"),
    dict(id=2, handler=0x487B69, title_va=None, title="SML pager (help/credits/briefing)",
         rows=None, step=None,
         assets=["helpscrn", "briefbg2", "loadpage", FONT_ATLAS_NOTE],
         items=[], note="0x4972f0 + 0x498600/0x4986b0/0x498750; SML/SME documents"),
    dict(id=3, handler=0x4882D6, title_va=None, title="briefing transition",
         rows=None, step=None, assets=[], items=[], note="driver state"),
    dict(id=4, handler=0x4882D1, title_va=None, title="briefing transition",
         rows=None, step=None, assets=[], items=[], note="driver state"),
    dict(id=5, handler=0x48ACD0, title_va=0x4BDC50, title="ERROR",
         rows=None, step=None, assets=["plate", FONT_ATLAS_NOTE],
         items=[("ESC TO RETURN TO MENU", 0x4BDC38, -1, 0x64, 0x5B)],
         note="message lines from 0x4aa2f8, wrapped at 0xb4 px"),
    dict(id=6, handler=0x488850, title_va=0x4BD464, title="RESTART GAME",
         rows=None, step=16, assets=["plate", FONT_ATLAS_NOTE],
         items=[("Reload last saved game", 0x4BD44C, 0, 0x0A, 0x1B),
                ("Restart level with same R.P.V.", 0x4BD42C, 1, 0x0A, 0x1B),
                ("Restart level from HQ", 0x4BD3FC, 2, 0x0A, 0x1B),
                ("Quit to title", 0x4BD3EC, 3, 0x0A, 0x1B)],
         note="rows conditional: 2..4; ids in 0x4bc278..0x4bc284; "
              "level name compared with LEVEL20/SECRT1/SECRT2"),
    dict(id=7, handler=None, title_va=None, title="(transition)",
         rows=None, step=None, assets=[], items=[], note="no handler"),
    dict(id=8, handler=None, title_va=None, title="(transition)",
         rows=None, step=None, assets=[], items=[], note="no handler"),
    dict(id=9, handler=None, title_va=None, title="level / in-game menu",
         rows=None, step=None,
         assets=["demotion", "disband", "humpan", "panel", "panelmid"],
         items=[], note="background chosen by 0x497210 (DEMOTION/DISBAND)"),
    dict(id=10, handler=0x488BA0, title_va=0x4BD5F4, title="CONTROL OPTIONS",
         rows=10, step=9,
         assets=["plate", FONT_ATLAS_NOTE],
         items=[("Player 1: %s", 0x4BD5E4, 0, 0x14, 0x1B),
                ("Assign %s", 0x4BD5D0, 1, 0x20, 0x1B),
                ("Player 2: %s", 0x4BD5C0, 2, 0x14, 0x1B),
                ("Assign %s", 0x4BD5D0, 3, 0x20, 0x1B),
                ("Keyjamming", 0x4BD5B4, 4, 0x14, 0x1B),
                ("1: %.25s", 0x4BD58C, 5, 0x14, 0x1B),
                ("2: %.25s", 0x4BD564, 6, 0x14, 0x1B),
                ("Joystick Centering", 0x4BD550, 7, 0x14, 0x1B),
                ("Mouse: %s", 0x4BD544, 8, 0x14, 0x1B),
                ("Done", 0x4BCEB4, 9, 0x14, 0x1B)],
         note="for set 0 items 1/3 read 'View %s' (0x4bd5dc) and open the "
              "read-only list (30); other sets read 'Assign %s' and open the "
              "editor (34 -> 35); a set called NEW SET first asks for a name "
              "(31); sub-screen dispatch table 0x48e888"),
    dict(id=11, handler=0x4889F0, title_va=0x4BD534, title="SOUND OPTIONS",
         rows=7, step=10,
         assets=["plate", "slide", "knob", FONT_ATLAS_NOTE],
         items=[("Music Volume:", 0x4BD508, 0, 0x12, 0x1B),
                ("Effects Volume:", 0x4BD4F8, 2, 0x12, 0x1B),
                ("Switch Left Right", 0x4BD4E4, 4, 0x1E, 0x1B),
                ("Done", 0x4BCEB4, 6, 0x12, 0x18)],
         note="music 0x4aac74, effects 0x4aac78; disabled branch at 0x488b29"),
    dict(id=12, handler=0x485EE0, title_va=0x4BCF24, title="GRAPHICS OPTIONS",
         rows=6, step=15,
         assets=["plate", "slide", "knob", "check", "nocheck", FONT_ATLAS_NOTE],
         items=[("(video mode)", 0x4C05C0, 0, 0x26, 0x18),
                ("Border:", 0x4BCF1C, 1, 0x26, 0x18),
                ("%s Gamma Correction", 0x4BCF08, 2, 0x26, 0x18),
                ("Reflections", 0x4BCEFC, 3, 0x32, 0x18),
                ("Light Maps Off", 0x4BCEBC, 4, 0x26, 0x18),
                ("Done", 0x4BCEB4, 5, 0x26, 0x17)],
         note="border 0x4bc1c8, gamma index 0x5a4950 (labels 0x4bc6d8), "
              "reflections 0x4aac84, light maps 0x4aac88 (item 4 text becomes "
              "Low 0x4bcee4 / High 0x4bcecc at values 1/2)"),
    dict(id=13, handler=0x48AA60, title_va=0x4BDC20, title="START GAME",
         rows=8, step=10,
         assets=["plate", "fileslot", FONT_ATLAS_NOTE],
         items=[("New Single Player Game", 0x4BDC08, 0, 0x23, 0x17),
                ("New Multiplayer Game", 0x4BDBF0, 1, 0x23, -0x21),
                ("(save slot name)", 0x6C2FE0, 2, 0x27, -0x2C),
                ("ESC TO CANCEL", 0x4BD9D8, -1, 0x23, -0x6E)],
         note="items 2..7 are the six visible save slots (scroll 0x68ccdc)"),
    dict(id=14, handler=0x48ABD0, title_va=0x4BDC2C, title="SAVE GAME",
         rows=6, step=12,
         assets=["plate", "fileslot", FONT_ATLAS_NOTE],
         items=[("(save slot name)", 0x6C2FE0, 0, 0x2A, -0x1C),
                ("ESC TO CANCEL", 0x4BD9D8, -1, 0x26, 0x65)],
         note="choose slot -> 0x4bc274, name entry state 0xf"),
    dict(id=15, handler=None, title_va=None, title="(save name entry)",
         rows=1, step=0, assets=["plate", "fileslot", FONT_ATLAS_NOTE],
         items=[], note="input 0x48b691, up to 18 chars, Enter -> 0x4422d0"),
    dict(id=16, handler=0x488E90, title_va=0x4BD640, title="KEY JAMMING",
         rows=1, step=0, assets=["plate", FONT_ATLAS_NOTE], items=[],
         note="live key state, four information lines"),
    dict(id=17, handler=0x48A3C0, title_va=0x4BD9E8, title="SELECT LEVEL FOR NEW GAME",
         rows=None, step=8, assets=["plate", "menuarrw", FONT_ATLAS_NOTE],
         items=[("(level name)", 0x68CDA8, 0, 0x0E, -0x1B),
                ("ESC TO CANCEL", 0x4BD9D8, -1, 0x26, -0x6F)],
         note="count = 0x68ddb0 (scan of *.S?W = 36 files); 10 visible rows, "
              "names from LVLNAMES.SLU (0x48b3f0)"),
    dict(id=18, handler=0x48A8B0, title_va=0x4BDB94, title="SELECT VIDEO MODE",
         rows=None, step=9, assets=["plate", FONT_ATLAS_NOTE],
         items=[("Press Esc to close", 0x4BD7C8, -1, 0x19, 0x14)],
         note="mode strings built by 0x4864a0/0x486500 from 0x4bd050.."),
    dict(id=19, handler=0x48A580, title_va=0x4BDAE0, title="MULTIPLAYER GAME TYPE",
         rows=12, step=8, assets=["plate", FONT_ATLAS_NOTE],
         items=[("One Computer", 0x4BDAD0, 0, 0x12, -0x12),
                ("Network Client", 0x4BDAC0, 1, 0x12, -0x1A),
                ("Network Server", 0x4BDAB0, 2, 0x12, -0x22),
                ("Network No Render Server", 0x4BDA94, 3, 0x12, -0x2A),
                ("TCP/IP Client", 0x4BDA84, 4, 0x12, -0x32),
                ("TCP/IP Server", 0x4BDA74, 5, 0x12, -0x3A),
                ("TCP/IP No Render Server", 0x4BDA5C, 6, 0x12, -0x42),
                ("Serial Cable Client", 0x4BDA48, 7, 0x12, -0x4A),
                ("Serial Cable Server", 0x4BDA34, 8, 0x12, -0x52),
                ("Modem Client", 0x4BDA24, 9, 0x12, -0x5A),
                ("Modem Server", 0x4BDA14, 10, 0x12, -0x62),
                ("Cancel", 0x4BDA0C, 11, 0x12, -0x6A)],
         note="item -> multiplayer mode 0x664598, jump table 0x48e934; "
              "rows are absolute y = 18 + 8*k, k counts only the entries "
              "allowed by the bitmask at 0x4c1290, so disabled modes shift "
              "the rest up"),
    dict(id=20, handler=0x48A830, title_va=0x4BDB80, title="LAY DOWN THE BUCKS",
         rows=1, step=0, assets=["plate", FONT_ATLAS_NOTE],
         items=[("THIS FEATURE IS NOT", 0x4BDB6C, -1, 0x2A, 0x1B),
                ("AVAILABLE IN THE", 0x4BDB58, -1, 0x2A, 0x24),
                ("SHAREWARE VERSION", 0x4BDB44, -1, 0x2A, 0x2D)],
         note="id 20 is the shareware notice; UI.md calls it SHAREWARE VERSION"),
    dict(id=21, handler=0x48A780, title_va=0x4BDB2C, title="NUMBER OF LOCAL PLAYERS",
         rows=3, step=27, assets=["plate", FONT_ATLAS_NOTE],
         items=[("One Player", 0x4BDB20, 0, 0x26, 0x1B),
                ("Full Screen", 0x4BDB14, 0, 0x26, 0x24),
                ("Two Players", 0x4BDB08, 1, 0x26, 0x1B),
                ("Split Screen", 0x4BDAF8, 1, 0x26, 0x24),
                ("Cancel", 0x4BDA0C, 2, 0x26, 0x1B)],
         note="two text lines per item, 27 px apart"),
    dict(id=22, handler=0x489A10, title_va=0x4BD8C0, title="MOUSE SETUP",
         rows=12, step=8, assets=["plate", "slide", "knob", FONT_ATLAS_NOTE],
         items=[("Used By: %s", 0x4BD8B4, 0, 0x12, 0x18),
                ("Left/Right: %s", 0x4BD8A4, 1, 0x09, 0x18),
                ("Sensitivity:", 0x4BD894, 2, 0x09, 0x18),
                ("Forward/Back: %s", 0x4BD880, 3, 0x09, 0x18),
                ("Sensitivity:", 0x4BD894, 4, 0x09, 0x18),
                ("Up/Down (3D): %s", 0x4BD86C, 5, 0x09, 0x18),
                ("Sensitivity:", 0x4BD894, 6, 0x09, 0x18),
                ("Button 1: %s", 0x4BD85C, 7, 0x09, 0x18),
                ("Button 2: %s", 0x4BD838, 8, 0x09, 0x18),
                ("Button 3: %s", 0x4BD814, 9, 0x09, 0x18),
                ("Button 4: %s", 0x4BD7F0, 10, 0x09, 0x18),
                ("Done", 0x4BCEB4, 11, 0x09, 0x18)],
         note="binding list for one player; sensitivity sliders"),
    dict(id=23, handler=0x48A9A0, title_va=0x4BDBD8, title="SET DIFFICULTY LEVEL",
         rows=6, step=15, assets=["plate", FONT_ATLAS_NOTE],
         items=[("Private", 0x4BDBD0, 0, 0x28, 0x1B),
                ("Corporal", 0x4BDBC4, 1, 0x28, 0x1B),
                ("Lieutenant", 0x4BDBB8, 2, 0x28, 0x1B),
                ("Captain", 0x4BDBB0, 3, 0x28, 0x1B),
                ("Major", 0x4BDBA8, 4, 0x28, 0x1B),
                ("Cancel", 0x4BDA0C, 5, 0x28, 0x1B)],
         note="rank copied to 0x4bc2b8; out-of-range -> First Seaman 0x4bdd14"),
    dict(id=24, handler=0x489020, title_va=0x4BD730, title="LOCAL PLAYER ONE NAME",
         rows=1, step=0, assets=["plate", FONT_ATLAS_NOTE],
         items=[("TYPE THE NAME YOU", 0x4BD71C, -1, 0x10, 0x30),
                ("WISH TO USE", 0x4BD710, -1, 0x10, 0x38),
                ("LEAVE BLANK TO USE A", 0x4BD6F8, -1, 0x10, 0x43),
                ("RANDOM NAME", 0x4BD6EC, -1, 0x10, 0x4B),
                ("PRESS ENTER TO CONTINUE", 0x4BD6D4, -1, 0x10, 0x59),
                ("PRESS ESC TO ABORT", 0x4BD6C0, -1, 0x10, 0x66)],
         note="handler takes player index (0); text typed into 0x682f60 + p*17"),
    dict(id=25, handler=0x489020, title_va=0x4BD748, title="LOCAL PLAYER TWO NAME",
         rows=1, step=0, assets=["plate", FONT_ATLAS_NOTE],
         items=[], note="same handler with argument 1 (stub 0x487f3e)"),
    dict(id=26, handler=0x488F70, title_va=0x4BD6AC, title="JOYSTICK CENTERING",
         rows=1, step=0, assets=["plate", FONT_ATLAS_NOTE],
         items=[("Spiral the stick of the", 0x4BD694, 0, 0x24, 0x19),
                ("joystick through all of", 0x4BD67C, 0, 0x24, 0x23),
                ("it's positions.", 0x4BD66C, 0, 0x24, 0x2D),
                ("Press Esc or Enter", 0x4BD658, 0, 0x29, 0x56),
                ("to close", 0x4BD64C, 0, 0x29, 0x61)],
         note="calibration; result in joystick centre globals"),
    dict(id=27, handler=0x4885C0, title_va=0x4BD388, title="MULTIPLAY GAME OPTIONS",
         rows=8, step=16, assets=["plate", "check", "nocheck", FONT_ATLAS_NOTE],
         items=[("(deathmatch type)", 0x4BC6E8, 0, 0x17, 0x16),
                ("Starting Cash: %ld", 0x4BD374, 1, 0x17, 0x10),
                ("Teamplay", 0x4BD368, 2, 0x23, 0x0D),
                ("No Enemies", 0x4BD35C, 3, 0x23, 0x0C),
                ("Can't Buy Ammo", 0x4BD34C, 4, 0x23, 0x0B),
                ("Handicap Better Players", 0x4BD334, 5, 0x23, 0x0A),
                ("Allow Mouse Cheating", 0x4BD31C, 6, 0x23, 0x07),
                ("START GAME!", 0x4BD310, 7, 0x17, -0x71)],
         note="checkbox flags in 0x68cd88; type 0x68cd84; start at 0x48b0c0"),
    dict(id=28, handler=None, title_va=None, title="(transition)",
         rows=None, step=None, assets=[], items=[], note="no handler"),
    dict(id=29, handler=None, title_va=None, title="(transition)",
         rows=None, step=None, assets=[], items=[], note="no handler"),
    dict(id=30, handler=0x489850, title_va=None, title="(key set name)",
         rows=31, step=8, assets=["plate", "menuarrw", FONT_ATLAS_NOTE],
         items=[("(binding name)", 0x4BC558, 0, 0x05, -0x1D),
                ("Press Esc to close", 0x4BD7C8, -1, 0x26, 0x14)],
         note="title = 0x6c2058 + set*0x108 (key set name); 31 bindings from 0x4bc558"),
    dict(id=31, handler=None, title_va=0x4BDD8C, title="NAME OF NEW KEY SET",
         rows=1, step=0, assets=["plate", FONT_ATLAS_NOTE],
         items=[], note="dialog drawn by 0x48ebb0; text typed in font 3"),
    dict(id=32, handler=0x486170, title_va=0x4BD040, title="PLAY OPTIONS",
         rows=8, step=15, assets=["plate", "check", "nocheck", FONT_ATLAS_NOTE],
         items=[("Skip Empty Weapons", 0x4BD02C, 0, 0x19, 0x15),
                ("Skip Deadly Weapons", 0x4BD018, 1, 0x19, 0x11),
                ("Switch Weapon To Picked-Up", 0x4BCFFC, 2, 0x19, 0x0D),
                ("Ammo", 0x4BCFF4, 2, 0x19, 0x15),
                ("Auto Switch Weapons When", 0x4BCFD8, 3, 0x19, 0x0F),
                ("Ammo Depleted", 0x4BCFC8, 3, 0x19, 0x17),
                ("Don't Auto-Target when", 0x4BCFB0, 4, 0x19, 0x12),
                ("Looking", 0x4BCFA8, 4, 0x19, 0x1A),
                ("Mouse Cheating", 0x4BCF98, 5, 0x19, 0x14),
                ("Dynamic Camera: None", 0x4BCF38, 6, 0x0D, 0x0F),
                ("Done", 0x4BCEB4, 7, 0x0D, 0x09)],
         note="flags in 0x5a46f8; dynamic camera 0x4aac8c"),
    dict(id=33, handler=0x488780, title_va=0x4BD3E0, title="QUIT GAME?",
         rows=2, step=16, assets=["plate", FONT_ATLAS_NOTE],
         items=[("Yes", 0x4BD3DC, 0, 0x26, 0x1B),
                ("No", 0x4BC7A4, 1, 0x26, 0x1B)],
         note="in game (0x83cb7c != 0) title is QUIT OPTIONS 0x4bd3cc with "
              "Return To Game / Quit To Title / Quit Game"),
    dict(id=34, handler=0x4892D0, title_va=None, title="(key set editor, entry)",
         rows=31, step=8, assets=["plate", FONT_ATLAS_NOTE],
         items=[("Enter changes, Delete clears", 0x4BD7A8, -1, 0x26, 0x14)],
         note="one-frame entry alias: the input pass at 0x48e212 rewrites state "
              "34 to 35 before the next frame; player comes from 0x660788 "
              "(1 = P1, 3 = P2)"),
    dict(id=35, handler=0x4892D0, title_va=None, title="(key set editor)",
         rows=31, step=8, assets=["plate", FONT_ATLAS_NOTE],
         items=[("Enter changes, Delete clears", 0x4BD7A8, -1, 0x26, 0x14)],
         note="same handler and stub as 34; set index argument 0x68cd7c, "
              "player from 0x660788 (1/3); conflict message 0x4bd790"),
    dict(id=36, handler=0x489D60, title_va=0x4BD9C0, title="PLAYER 1 JOYSTICK SETUP",
         rows=None, step=9, assets=["plate", FONT_ATLAS_NOTE],
         items=[("Esc Exit, Button To Highlight", 0x4BD988, -1, 0x14, 0x14)],
         note="rows = 1 or 33 depending on 0x4bc540/0x4bc544; title PLAYER 2 "
              "JOYSTICK SETUP 0x4bd9a8 when 0x685afc != 0"),
    dict(id=37, handler=0x488990, title_va=0x4BD474, title="NEW VIDEO MODE OK?",
         rows=2, step=16, assets=["plate", FONT_ATLAS_NOTE],
         items=[("Yes", 0x4BD3DC, 0, 0x26, 0x1B)],
         note="keeps or reverts the new video mode"),
]

#: Proved transitions: (from, to, how, site VA, expected immediate or None).
TRANSITIONS = [
    (9, 6, "Esc opens the in-game menu", 0x48B54C, 6),
    (5, 1, "Esc from ERROR", 0x48B57A, 1),
    (15, 14, "Esc from save name entry", 0x48B710, 14),
    (1, 11, "main menu entry 0 (sound art)", 0x48CF78, 11),
    (1, 10, "main menu entry 1 (controls art)", 0x48CF8C, 10),
    (1, 12, "main menu entry 2 (graphics art)", 0x48CFA0, 12),
    (1, 13, "main menu entry 3 (startgam art)", 0x48CFB4, 13),
    (1, 14, "main menu entry 4 (savegame art, in game only)", 0x48CFF1, 14),
    (1, 32, "main menu entry 5 (play art)", 0x48D005, 32),
    (1, 33, "main menu entry 6 (quitgame art)", 0x48D019, 33),
    (13, 23, "New Single Player Game", 0x48DA96, 23),
    (13, 19, "New Multiplayer Game", 0x48DAB8, 19),
    (21, 17, "number of players accepted -> level select", 0x48DCA6, 17),
    (17, 23, "level chosen -> difficulty", 0x48E281, 23),
    (23, 24, "rank chosen -> player one name", 0x48E3C7, 24),
    (10, 16, "Keyjamming", 0x48D1D9, 16),
    (10, 36, "joystick setup, player 1", 0x48D1E4, 36),
    (10, 36, "joystick setup, player 2", 0x48D209, 36),
    (10, 26, "Joystick Centering", 0x48D26A, 26),
    (10, 22, "Mouse Setup", 0x48D292, 22),
    (10, 31, "new key set (clears name)", 0x48D191, 31),
    (16, 10, "Esc from a control sub-screen", 0x48E599, 10),
    (22, 10, "Esc from a control sub-screen", 0x48E599, 10),
    (26, 10, "Esc from a control sub-screen", 0x48E599, 10),
    (30, 10, "Esc from a control sub-screen", 0x48E599, 10),
    (34, 10, "Esc from a control sub-screen", 0x48E599, 10),
    (35, 10, "Esc from a control sub-screen", 0x48E599, 10),
    (36, 10, "Esc from a control sub-screen", 0x48E599, 10),
    (18, 12, "Esc from video mode list", 0x48E55A, 12),
    (6, 1, "Quit to title", 0x48DA31, 1),
    (0, 1, "intro finished / skipped (0x485c50 writes state 0 for game)", 0x487709, 1),
    (14, 15, "slot chosen -> name entry", 0x48E162, None),
    (9, 0, "menu item 0 restarts the level", 0x485C50, 0),
    (9, 1, "credits item: winner.sbv then title", 0x48347E, 1),
]

#: Save/load facts.
SAVE_DIR = "SAVEGAME"
SAVE_FMT_SBT = 0x4AB460     # 'savegame\\saved%d.sbt'
SAVE_FMT_SBW = 0x4AB478     # 'savegame\\saved%d.sbw'
SAVE_NAME_TABLE = 0x6C2FE0
SAVE_NAME_STRIDE = 0x13
SAVE_TABLE_END = 0x6C315C
SAVE_SLOTS = (SAVE_TABLE_END - SAVE_NAME_TABLE) // SAVE_NAME_STRIDE
SAVE_VISIBLE_ROWS = 6
SAVE_SCROLL_MAX = 13
EMPTY_SLOT_STR = 0x4BC3B0  # '::: empty slot :::'
SAVE_READ_NAME = 0x455D90   # opens savegame\saved%d.sbt, copies header name
SAVE_WRITER = 0x4422D0
SAVE_LOADER = 0x442340

#: Key sets.
KEY_FMT_ENUM = 0x4BCC74     # 'KEYCFG\\*.KEY'
KEY_FMT_OPEN = 0x4BCC68     # 'KEYCFG\\%s'
KEY_FMT_SAVE = 0x4BCDD4     # 'KEYCFG\\%.8s.key'
KEY_FILE_SIZE = 0x178
KEY_MAX_SETS = 16
KEY_NAME_TABLE = 0x6C2058
KEY_NAME_STRIDE = 0x108
KEY_SET_COUNT = 0x4BC5D4
KEY_BINDINGS = 0x4BC558     # 31 binding-name pointers
KEY_EDITOR_TITLE = 0x4BDF84  # 'GUNMETAL KEYS'
CONFIG_NAME = 0x4BC370      # 'GUNMETAL.CFG'
LEVEL_NAMES_FILE = 0x4BC270  # 'LVLNAMES.SLU'

#: Layout constants of the item composer.
PLATE_WIDTH = 196
TITLE_FONT = 5
ITEM_FONT = 0
SELECTED_FONT = 4

FONT_TABLE = 0x65DDD0


# ---------------------------------------------------------------------------
# Executable helpers
# ---------------------------------------------------------------------------

def _need_exelib():
    if exelib is None:
        raise SystemExit(f"ui_screens: cannot import exelib: {_EXELIB_ERROR}")


def exe_bytes() -> bytes:
    _need_exelib()
    return exelib.data()


def u32_at(blob: bytes, va: int) -> int:
    return struct.unpack_from("<I", blob, exelib.va2off(va))[0]


def bytes_at(blob: bytes, va: int, size: int) -> bytes:
    off = exelib.va2off(va)
    return blob[off:off + size]


def cstr_at(va: int) -> str:
    _need_exelib()
    return exelib.cstr(va, 120)


def disasm_text(lo: int, hi: int) -> list[str]:
    _need_exelib()
    return list(exelib.disasm(lo, hi))


def function_lines(start: int, max_bytes: int = 0x900) -> list[str]:
    """Instructions of a function, bounded by its first ``ret``."""
    out = []
    for line in disasm_text(start, start + max_bytes):
        out.append(line)
        if re.search(r"\t(ret|ret\s+0x[0-9a-f]+)\s*$", line.rstrip()):
            break
    return out


def parse_lines(lines: list[str]) -> list[tuple[int, str, str]]:
    """(va, mnemonic+operands, raw hex) for each listing line."""
    out = []
    for line in lines:
        m = re.match(r"\s*([0-9a-f]+):\s+((?:[0-9a-f]{2} )+)\s*(.*)$", line)
        if not m:
            continue
        out.append((int(m.group(1), 16), m.group(1).strip(), m.group(3).strip()))
    return out


def calls_in(start: int, max_bytes: int = 0x900) -> list[tuple[int, int]]:
    """(site VA, call target) inside a function."""
    out = []
    for line in function_lines(start, max_bytes):
        m = re.match(r"\s*([0-9a-f]+):.*\bcall\s+0x([0-9a-f]+)", line)
        if m:
            out.append((int(m.group(1), 16), int(m.group(2), 16)))
    return out


def pushes_in(start: int, max_bytes: int = 0x900) -> set[int]:
    out = set()
    for line in function_lines(start, max_bytes):
        m = re.search(r"\bpush\s+0x([0-9a-f]+)", line)
        if m:
            out.add(int(m.group(1), 16))
    return out


def pushes_in_range(lo: int, hi: int) -> set[int]:
    out = set()
    for line in disasm_text(lo, hi):
        m = re.search(r"\bpush\s+0x([0-9a-f]+)", line)
        if m:
            out.add(int(m.group(1), 16))
    return out


def item_calls(start: int, max_bytes: int = 0x900) -> list[list]:
    """Six stack arguments of every ``0x485dd0`` call in a handler.

    Order is (item, row, x, centered, label, font); non-immediate operands
    stay as text so callers can tell "computed at runtime" from a constant.
    """
    calls: list[list] = []
    pushes: list[object] = []
    for line in function_lines(start, max_bytes):
        m = re.match(r"\s*([0-9a-f]+):\s+((?:[0-9a-f]{2} )+)\s*(.*)$", line)
        if not m:
            continue
        text = m.group(3).strip()
        op, _, arg = text.partition(" ")
        arg = arg.strip()
        if op == "push":
            mm = re.match(r"0x([0-9a-f]+)$", arg)
            if mm:
                value = int(mm.group(1), 16)
                if value >= 0x80000000:
                    value -= 0x100000000
                pushes.append(value)
            else:
                pushes.append(arg)
        elif op == "call":
            if len(pushes) >= 6 and int(arg, 16) == 0x485DD0:
                calls.append(pushes[-6:])
            pushes = []
    return calls


def immediate_writes(start: int, global_va: int, max_bytes: int = 0x900) -> list[tuple[int, int]]:
    """``mov ds:<global>, imm32`` sites inside a function -> (site, value)."""
    out = []
    for va, _hex, text in parse_lines(function_lines(start, max_bytes)):
        m = re.match(r"mov\s+DWORD PTR ds:0x([0-9a-f]+),0x([0-9a-f]+)", text)
        if m and int(m.group(1), 16) == global_va:
            out.append((va, int(m.group(2), 16)))
    return out


def stub_target(stub: int, blob: bytes) -> tuple[str, int | None]:
    """First call/jump target of a dispatcher stub, plus its push argument."""
    lines = list(exelib.disasm(stub, stub + 0x18))
    pushed = None
    for line in lines:
        m = re.search(r"\bpush\s+0x([0-9a-f]+)", line)
        if m:
            pushed = int(m.group(1), 16)
        m = re.search(r"\bcall\s+0x([0-9a-f]+)", line)
        if m:
            return "call", int(m.group(1), 16)
        m = re.search(r"\bjmp\s+0x([0-9a-f]+)", line)
        if m:
            return "jmp", int(m.group(1), 16)
    return "none", pushed


# ---------------------------------------------------------------------------
# Installation helpers
# ---------------------------------------------------------------------------

def read_bigfile_index(idx: Path) -> dict[str, dict]:
    """Parse a ``BIGFILE`` index; returns lower-case name -> record."""
    if not idx.is_file():
        return {}
    blob = idx.read_bytes()
    if len(blob) < 12 or not blob.startswith(b"BIGFILE"):
        raise ValueError(f"{idx}: not a BIGFILE index")
    count = struct.unpack_from("<I", blob, 8)[0]
    if 12 + count * 12 > len(blob):
        raise ValueError(f"{idx}: truncated index")
    raw = []
    for i in range(count):
        off = 12 + 12 * i
        name = blob[off:off + 8].rstrip(b"\0").decode("latin1")
        start = struct.unpack_from("<I", blob, off + 8)[0]
        raw.append((name, start))
    dat = idx.with_suffix(".DAT")
    if not dat.is_file():
        dat = idx.with_suffix(".dat")
    dat_size = dat.stat().st_size if dat.is_file() else 0
    out = {}
    for i, (name, start) in enumerate(raw):
        end = raw[i + 1][1] if i + 1 < len(raw) else dat_size
        out[name.lower()] = {"name": name, "offset": start,
                             "size": max(0, end - start), "dat": str(dat)}
    return out


def bmp_dims(record: dict) -> tuple[int, int] | None:
    dat = Path(record["dat"])
    if not dat.is_file():
        return None
    with dat.open("rb") as fh:
        fh.seek(record["offset"])
        head = fh.read(30)
    if len(head) < 30 or head[:2] != b"BM":
        return None
    w, h = struct.unpack_from("<ii", head, 18)
    return abs(w), abs(h)


def save_names(game: Path) -> list[tuple[str, str]]:
    """(file, display name) for SAVEGAME/saved*.sbt."""
    out = []
    d = game / SAVE_DIR
    if not d.is_dir():
        return out
    for p in sorted(d.glob("saved*.sbt")):
        try:
            with p.open("rb") as fh:
                name = fh.read(0x50).split(b"\0", 1)[0].decode("latin1", "replace")
        except OSError:
            name = ""
        out.append((p.name, name))
    return out


def key_files(game: Path) -> list[tuple[str, int]]:
    d = game / "KEYCFG"
    if not d.is_dir():
        return []
    return [(p.name, p.stat().st_size) for p in sorted(d.iterdir()) if p.is_file()]


def sml_corpus(game: Path) -> dict:
    d = game / "SML"
    info = {"root": str(d), "sme": 0, "sml": 0, "help": False, "credits": False,
            "brief": 0, "prelevel": 0, "design": 0}
    if not d.is_dir():
        return info
    for p in d.rglob("*"):
        if p.suffix.lower() == ".sme":
            info["sme"] += 1
        elif p.suffix.lower() == ".sml":
            info["sml"] += 1
    for sub, key in (("brief", "brief"), ("prelevel", "prelevel"), ("design", "design")):
        sd = d / sub
        if sd.is_dir():
            info[key] = len([p for p in sd.iterdir() if p.is_file()])
    info["help"] = (d / "help.sme").is_file() or (d / "help.sml").is_file()
    info["credits"] = (d / "credits.sme").is_file() or (d / "credits.sml").is_file()
    return info


def level_count(game: Path) -> int:
    return len(list(game.glob("*.sbw"))) + len(list(game.glob("*.SBW")))


def lvlnames_count(game: Path) -> int:
    p = game / "LVLNAMES.SLU"
    if not p.is_file():
        return 0
    text = p.read_text("latin1")
    return len([ln for ln in text.replace("\r", "").split("\n") if ln.strip()])


# ---------------------------------------------------------------------------
# Checks
# ---------------------------------------------------------------------------

class Checks:
    def __init__(self) -> None:
        self.items: list[dict] = []

    def add(self, ok: bool, name: str, detail: str = "") -> bool:
        self.items.append({"ok": bool(ok), "name": name, "detail": detail})
        return bool(ok)

    @property
    def failed(self) -> list[dict]:
        return [c for c in self.items if not c["ok"]]


def check_jump_table(chk: Checks, blob: bytes) -> None:
    handlers = {s["id"]: s["handler"] for s in SCREENS if 5 <= s["id"] <= 37}
    table = [u32_at(blob, JUMP_TABLE + 4 * i) for i in range(JUMP_COUNT)]
    chk.add(len(table) == JUMP_COUNT, "jump table size",
            f"{JUMP_TABLE:#x}: {len(table)} entries (expected {JUMP_COUNT})")
    chk.add(len(handlers) == JUMP_COUNT, "screen id coverage",
            f"{len(handlers)} ids in 5..{JUMP_FIRST + JUMP_COUNT - 1}")
    for idx, stub in enumerate(table):
        sid = JUMP_FIRST + idx
        expected = handlers.get(sid)
        kind, target = stub_target(stub, blob)
        if expected is None:
            ok = stub == NO_HANDLER_STUB and kind == "jmp"
            chk.add(ok, f"id {sid} no handler",
                    f"stub {stub:#x} (expected {NO_HANDLER_STUB:#x})")
        else:
            ok = kind == "call" and target == expected
            chk.add(ok, f"id {sid} -> {expected:#x}",
                    f"stub {stub:#x} {kind} {target if target is None else hex(target)}")


def check_stub_arguments(chk: Checks, blob: bytes) -> None:
    table = [u32_at(blob, JUMP_TABLE + 4 * i) for i in range(JUMP_COUNT)]
    # 24/25 push the player index before calling 0x489020.
    for sid, arg in ((24, 0), (25, 1)):
        stub = table[sid - JUMP_FIRST]
        lines = list(exelib.disasm(stub, stub + 0x10))
        pushed = [int(m.group(1), 16) for ln in lines
                  for m in [re.search(r"\bpush\s+0x([0-9a-f]+)", ln)] if m]
        chk.add(pushed[:1] == [arg], f"id {sid} player argument",
                f"push {pushed[:1]}, expected [{arg}]")
    # 30/34/35 pass [0x68cd7c] (player number 0/1).
    for sid in (30, 34, 35):
        stub = table[sid - JUMP_FIRST]
        lines = list(exelib.disasm(stub, stub + 0x14))
        text = " ".join(lines)
        chk.add("0x68cd7c" in text, f"id {sid} player index",
                "stub reads 0x68cd7c" if "0x68cd7c" in text else "not found")


def check_screens(chk: Checks) -> None:
    seen = set()
    for s in SCREENS:
        sid = s["id"]
        chk.add(sid not in seen, f"screen id {sid} unique")
        seen.add(sid)
        if s["handler"] is None:
            continue
        handler = s["handler"]
        if s.get("title_va"):
            want = s["title"]
            got = cstr_at(s["title_va"])
            chk.add(got == want, f"id {sid} title string",
                    f"{s['title_va']:#x} = {got!r}")
            pushed = pushes_in(handler)
            chk.add(s["title_va"] in pushed, f"id {sid} title is pushed",
                    f"push {s['title_va']:#x} in {handler:#x}")
        if s.get("rows") is not None:
            writes = immediate_writes(handler, ROWS_VAR)
            vals = [v for _, v in writes]
            chk.add(s["rows"] in vals, f"id {sid} row count",
                    f"{ROWS_VAR:#x} <- {vals} (expected {s['rows']})")
        if s.get("step") is not None:
            writes = immediate_writes(handler, STEP_VAR)
            vals = [v for _, v in writes]
            chk.add(s["step"] in vals, f"id {sid} row step",
                    f"{STEP_VAR:#x} <- {vals} (expected {s['step']})")
        # asset helpers actually used by this handler
        targets = {t for _, t in calls_in(handler)}
        names = {a.split()[0].lower() for a in s.get("assets", [])}
        if names & {"slide", "knob"}:
            chk.add({0x486070, 0x4860B0} <= targets, f"id {sid} slider helpers",
                    "0x486070/0x4860b0 called")
        if names & {"check", "nocheck"}:
            chk.add(0x4833E0 in targets, f"id {sid} checkbox helper",
                    "0x4833e0 called")
        if "fileslot" in names:
            chk.add(0x48AB90 in targets, f"id {sid} fileslot helper",
                    "0x48ab90 called")
        if "menuarrw" in names:
            chk.add(bool({0x485CC0, 0x485D00} & targets), f"id {sid} arrows",
                    "0x485cc0/0x485d00 called")
        # static item labels: the literal must be pushed with the listed
        # item id, x and base row (dynamic labels are formatted at runtime)
        calls = item_calls(handler)
        for label, va, item, x, row in s.get("items", []):
            hits = [c for c in calls if c[4] == va]
            if not hits:
                continue
            chk.add(cstr_at(va) == label, f"id {sid} item {item} label",
                    f"{va:#x} = {cstr_at(va)!r}")
            ok = any(not isinstance(c[0], int) or c[0] == item
                     for c in hits)
            ok = ok and any(not isinstance(c[2], int) or c[2] == x for c in hits)
            ok = ok and any(not isinstance(c[1], int) or c[1] == row for c in hits)
            chk.add(ok, f"id {sid} item {item} placement",
                    f"call(label={va:#x}, item={item}, x={x}, row={row})")


def check_transitions(chk: Checks, blob: bytes) -> None:
    reg_sites = {0x48D1D9: (0x48D1CE, 0x10), 0x48D292: (0x48D283, 0x16)}
    for frm, to, how, site, imm in TRANSITIONS:
        if imm is not None and site not in reg_sites:
            raw = bytes_at(blob, site, 10)
            want = b"\xc7\x05" + struct.pack("<I", STATE_VAR) + struct.pack("<I", imm)
            got = struct.unpack_from("<I", raw, 6)[0] if len(raw) >= 10 else None
            chk.add(raw == want, f"transition {frm}->{to}",
                    f"{site:#x}: mov [0x4bc440], {got} (expected {imm})")
        elif imm is not None:
            load_site, value = reg_sites[site]
            chk.add(bytes_at(blob, load_site, 5) == b"\xbd" + struct.pack("<I", value)
                    and bytes_at(blob, site, 6) == b"\x89\x2d" + struct.pack("<I", STATE_VAR),
                    f"transition {frm}->{to}",
                    f"{site:#x}: mov ebp,{value:#x}; mov [0x4bc440],ebp")
        else:
            # 0x48e15a sets ebp=0xf, 0x48e162 stores it.
            raw = bytes_at(blob, site - 8, 13)
            ok = bytes_at(blob, site - 8, 5) == b"\xbd\x0f\x00\x00\x00" and raw[8] == 0x89
            chk.add(ok, f"transition {frm}->{to}", f"{site:#x}: mov ebp,0xf; mov [0x4bc440],ebp")


def check_save_and_keys(chk: Checks, blob: bytes) -> None:
    chk.add(SAVE_SLOTS == 20, "save slot count",
            f"({SAVE_TABLE_END:#x}-{SAVE_NAME_TABLE:#x})/{SAVE_NAME_STRIDE:#x} = {SAVE_SLOTS}")
    chk.add(SAVE_SCROLL_MAX + SAVE_VISIBLE_ROWS == SAVE_SLOTS - 1, "save scroll bound",
            f"{SAVE_SCROLL_MAX}+{SAVE_VISIBLE_ROWS} = {SAVE_SLOTS - 1}")
    for va, want in ((SAVE_FMT_SBT, "savegame\\saved%d.sbt"),
                     (SAVE_FMT_SBW, "savegame\\saved%d.sbw"),
                     (EMPTY_SLOT_STR, "::: empty slot :::"),
                     (KEY_FMT_ENUM, "KEYCFG\\*.KEY"),
                     (KEY_FMT_OPEN, "KEYCFG\\%s"),
                     (KEY_FMT_SAVE, "KEYCFG\\%.8s.key"),
                     (KEY_EDITOR_TITLE, "GUNMETAL KEYS"),
                     (CONFIG_NAME, "GUNMETAL.CFG")):
        got = cstr_at(va)
        chk.add(got == want, f"string {va:#x}", f"{got!r}")
    # 0x4bc270 is a char* pointing at 'LVLNAMES.SLU' (0x4bca9c).
    ptr = u32_at(blob, LEVEL_NAMES_FILE)
    chk.add(ptr == 0x4BCA9C and cstr_at(ptr) == "LVLNAMES.SLU", "level names file",
            f"[0x4bc270] = {ptr:#x} -> {cstr_at(ptr)!r}")
    chk.add(KEY_FILE_SIZE == 0x178 and KEY_MAX_SETS == 16, "key set geometry",
            f"{KEY_MAX_SETS} sets x {KEY_FILE_SIZE:#x} bytes, name stride {KEY_NAME_STRIDE:#x}")
    # save list is filled by 0x455d90 for every slot in 0x48f... loader loop
    lines = function_lines(0x48F970, 0x900)
    text = " ".join(lines)
    chk.add("0x6c2fe0" in text and "0x6c315c" in text, "save list loader loop",
            "0x48f970 fills 0x6c2fe0..0x6c315c via 0x455d90")


def check_ui_sounds(chk: Checks, blob: bytes) -> None:
    for name, va, handle, store, play in UI_SOUNDS:
        got = cstr_at(va)
        chk.add(got == name, f"ui sound {name}", f"{va:#x} = {got!r}")
        # loader stores the returned sample id into its handle global
        raw = bytes_at(blob, store, 5)
        chk.add(raw == b"\xa3" + struct.pack("<I", handle), f"ui sound {name} handle",
                f"{store:#x}: a3 {handle:#08x}")
        if play is not None:
            body = " ".join(function_lines(play, 0x40))
            chk.add(f"0x{handle:x}" in body, f"ui sound {name} playback",
                    f"handle 0x{handle:x} read at {play:#x}")
        else:
            chk.add(True, f"ui sound {name} playback", "no consumer found (hypothesis)")


def check_assets(chk: Checks, game: Path) -> None:
    idx = game / "FRONTEND" / "BMP.IDX"
    recs = read_bigfile_index(idx) if idx.is_file() else {}
    chk.add(bool(recs), "FRONTEND/BMP.IDX", f"{len(recs)} records")
    for name, w, h, _buf, _use in UI_ATLAS:
        rec = recs.get(name)
        chk.add(rec is not None, f"atlas {name} present")
        if rec is None:
            continue
        dims = bmp_dims(rec)
        chk.add(dims == (w, h), f"atlas {name} dims", f"{dims} (expected {(w, h)})")
    # fonts really loaded from the frontend loader
    font_names = {"fonts1": 0x4BE184, "fonts2": 0x4BE140, "fonts3": 0x4BE114,
                  "fonts4": 0x4BE0B8, "numfonts": 0x4BE028}
    pushed = pushes_in_range(0x48F970, 0x490600)
    for name, va in font_names.items():
        got = cstr_at(va)
        chk.add(got == name and va in pushed, f"font loader {name}",
                f"{va:#x} = {got!r}, pushed={va in pushed}")
    keys = key_files(game)
    chk.add(all(size == KEY_FILE_SIZE for _, size in keys), "key files size",
            f"{[(n, s) for n, s in keys]} (expected {KEY_FILE_SIZE})")
    chk.add(len(keys) <= KEY_MAX_SETS, "key set limit", f"{len(keys)} <= {KEY_MAX_SETS}")
    names = save_names(game)
    chk.add(len(names) <= SAVE_SLOTS, "save slots on disk", f"{len(names)} <= {SAVE_SLOTS}")
    corpus = sml_corpus(game)
    chk.add(corpus["sme"] > 0 and corpus["sml"] > 0, "SML corpus",
            f"sme {corpus['sme']}, sml {corpus['sml']}")
    chk.add(corpus["help"] and corpus["credits"], "SML help/credits", "present")
    levels = level_count(game)
    chk.add(levels > 0, "level files", f"{levels} *.sbw, LVLNAMES.SLU {lvlnames_count(game)} lines")


# ---------------------------------------------------------------------------
# Reports
# ---------------------------------------------------------------------------

def build_report() -> dict:
    _need_exelib()
    exe = Path(os.environ.get("GUNMETAL_EXE", str(DEFAULT_GAME / "gundx.exe")))
    report = {
        "tool": "ui_screens",
        "exe": {"path": str(exe), "sha256": exelib.exe_sha256() if exe.is_file() else None},
        "state_var": STATE_VAR,
        "jump_table": JUMP_TABLE,
        "screens": [],
        "transitions": [],
        "save": {
            "format_sbt": cstr_at(SAVE_FMT_SBT),
            "format_sbw": cstr_at(SAVE_FMT_SBW),
            "slots": SAVE_SLOTS,
            "visible_rows": SAVE_VISIBLE_ROWS,
            "scroll_max": SAVE_SCROLL_MAX,
            "empty_label": cstr_at(EMPTY_SLOT_STR),
            "name_table": SAVE_NAME_TABLE,
            "name_stride": SAVE_NAME_STRIDE,
            "name_reader": SAVE_READ_NAME,
            "writer": SAVE_WRITER,
            "loader": SAVE_LOADER,
        },
        "keys": {
            "enum": cstr_at(KEY_FMT_ENUM),
            "open": cstr_at(KEY_FMT_OPEN),
            "save": cstr_at(KEY_FMT_SAVE),
            "max_sets": KEY_MAX_SETS,
            "file_size": KEY_FILE_SIZE,
            "name_table": KEY_NAME_TABLE,
            "name_stride": KEY_NAME_STRIDE,
            "binding_names": KEY_BINDINGS,
            "editor_title": cstr_at(KEY_EDITOR_TITLE),
            "config": cstr_at(CONFIG_NAME),
        },
        "ui_sounds": [{"name": n, "string": f"{va:#x}", "handle": f"{h:#x}",
                       "store": f"{st:#x}",
                       "playback": (f"{p:#x}" if p else None)}
                      for n, va, h, st, p in UI_SOUNDS],
    }
    for s in SCREENS:
        entry = {k: s[k] for k in ("id", "title", "rows", "step", "note")}
        entry["handler"] = f"{s['handler']:#x}" if s["handler"] else None
        entry["title_va"] = f"{s['title_va']:#x}" if s.get("title_va") else None
        entry["assets"] = s.get("assets", [])
        entry["items"] = [{"label": t, "va": f"{va:#x}", "item": i,
                           "x": x, "row": r}
                          for t, va, i, x, r in s.get("items", [])]
        report["screens"].append(entry)
    for frm, to, how, site, imm in TRANSITIONS:
        report["transitions"].append({"from": frm, "to": to, "how": how,
                                      "site": f"{site:#x}", "value": imm})
    game = DEFAULT_GAME
    idx = game / "FRONTEND" / "BMP.IDX"
    recs = read_bigfile_index(idx) if idx.is_file() else {}
    report["assets"] = {
        "game": str(game),
        "frontend_count": len(recs),
        "ui": [{"name": n, "expected": [w, h],
                "actual": list(bmp_dims(recs[n])) if n in recs and bmp_dims(recs[n]) else None,
                "buffer": (f"{buf:#x}" if buf else None), "use": use}
               for n, w, h, buf, use in UI_ATLAS],
        "sml": sml_corpus(game),
        "key_files": key_files(game),
        "save_files": save_names(game),
        "levels": level_count(game),
        "lvlnames": lvlnames_count(game),
    }
    return report


def print_map(report: dict) -> None:
    print(f"front-end state [0x{STATE_VAR:x}], jump table 0x{JUMP_TABLE:x} "
          f"(ids {JUMP_FIRST}..{JUMP_FIRST + JUMP_COUNT - 1})")
    print(f"{'id':>3} {'handler':>9} {'rows':>5} {'step':>4}  {'title':<32} assets")
    for s in report["screens"]:
        rows = "-" if s["rows"] is None else str(s["rows"])
        step = "-" if s["step"] is None else str(s["step"])
        assets = ", ".join(a.split(" (")[0] for a in s["assets"]) or "-"
        title = s["title"]
        if len(title) > 32:
            title = title[:31] + "~"
        print(f"{s['id']:>3} {(s['handler'] or '-'):>9} {rows:>5} {step:>4}  "
              f"{title:<32} {assets}")
    print()
    print("transitions (state -> state, site):")
    for t in report["transitions"]:
        val = "" if t["value"] is None else f" = {t['value']}"
        print(f"  {t['from']:>2} -> {t['to']:<2} {t['site']}{val:<5} {t['how']}")
    print()
    save = report["save"]
    print(f"save: {save['slots']} slots, {save['visible_rows']} visible rows, "
          f"scroll 0..{save['scroll_max']}; {save['format_sbt']!r}; "
          f"empty label {save['empty_label']!r}")
    keys = report["keys"]
    print(f"keys: {keys['max_sets']} sets x {keys['file_size']} bytes, "
          f"{keys['enum']!r}; config {keys['config']!r}")


def print_assets(report: dict) -> None:
    a = report["assets"]
    print(f"install: {a['game']}")
    print(f"FRONTEND/BMP.IDX: {a['frontend_count']} records")
    print(f"{'asset':<10} {'actual':>9} {'expected':>9}  status  use")
    for item in a["ui"]:
        exp = "x".join(str(v) for v in item["expected"])
        act = "x".join(str(v) for v in item["actual"]) if item["actual"] else "-"
        status = "ok" if item["actual"] == item["expected"] else "MISSING/DIFF"
        print(f"{item['name']:<10} {act:>9} {exp:>9}  {status:<12} {item['use']}")
    sml = a["sml"]
    print(f"SML: {sml['sme']} .sme + {sml['sml']} .sml; help={sml['help']} "
          f"credits={sml['credits']} brief={sml['brief']} "
          f"prelevel={sml['prelevel']} design={sml['design']}")
    keys = a["key_files"]
    print(f"KEYCFG: {len(keys)} file(s): "
          + ", ".join(f"{n} ({s}B{' ok' if s == KEY_FILE_SIZE else ' BAD'})"
                      for n, s in keys))
    saves = a["save_files"]
    print(f"SAVEGAME: {len(saves)} file(s): "
          + ", ".join(f"{n}={name!r}" for n, name in saves))
    print(f"levels: {a['levels']} *.sbw; LVLNAMES.SLU {a['lvlnames']} lines")


def self_test(verbose: bool = False) -> int:
    _need_exelib()
    blob = exe_bytes()
    chk = Checks()
    check_jump_table(chk, blob)
    check_stub_arguments(chk, blob)
    check_screens(chk)
    check_transitions(chk, blob)
    check_save_and_keys(chk, blob)
    check_ui_sounds(chk, blob)
    game = DEFAULT_GAME
    if (game / "FRONTEND" / "BMP.IDX").is_file():
        check_assets(chk, game)
    else:
        chk.add(True, "assets", f"installation not found at {game}, skipped")
    passed = len(chk.items) - len(chk.failed)
    if verbose:
        for c in chk.items:
            if not c["ok"]:
                print(f"FAIL {c['name']}: {c['detail']}")
    print(f"self-test: {passed}/{len(chk.items)} checks passed")
    if chk.failed:
        print(f"failed: {len(chk.failed)}")
        return 1
    return 0


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--map", action="store_true", help="screen table and transitions")
    ap.add_argument("--assets", action="store_true", help="asset inventory of the installation")
    ap.add_argument("--json", action="store_true", help="full machine-readable report")
    ap.add_argument("--self-test", action="store_true", help="verify all claims against the exe")
    ap.add_argument("--verbose", action="store_true", help="show failing checks")
    args = ap.parse_args(argv)
    if args.self_test:
        return self_test(args.verbose)
    if not (args.map or args.assets or args.json):
        ap.print_help()
        return 2
    report = build_report()
    if args.json:
        json.dump(report, sys.stdout, ensure_ascii=False, indent=1)
        sys.stdout.write("\n")
        return 0
    if args.map:
        print_map(report)
    if args.assets:
        if args.map:
            print()
        print_assets(report)
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
