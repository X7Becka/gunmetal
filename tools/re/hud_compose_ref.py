#!/usr/bin/env python3
"""Reference composition of the ``level0`` cockpit panel (M4.4.5).

The goal is a *second, independent* Python path that draws the cockpit HUD
layer ``320x78`` from the game's own assets, so that ``gm/render.py --hud`` can
later be built by moving this file into ``gm/``.  Nothing here writes to the
game, the frames or the repository: every asset is opened read-only.

What is drawn
-------------

| элемент | ассет | как |
|---|---|---|
| фон | ``FRONTEND/BMP.IDX:PANEL`` (320x78) | блит в панель, индекс 0 — ключ (прозрачный) |
| подписи ``MACHINE GUN`` / ``KILL`` / ``DAMAGE`` / ``THROTTLE`` / ``HEADLIGHTS`` | ``FONTS3`` (шрифт 6) | 8x8 клетки, обрезка по чернилам, ``advance = ширина + 1`` |
| патроны, ``KILL``, число экипировки | ``NUMFONTS`` (шрифты 10 / 9) | клетки 8x10 без обрезки |
| урон ``%`` | ``NUMFONTS`` шрифт 9 (зелёный) + ячейка ``%`` | 8x10 |
| тяга | не спрайт, заливка палитрой 243 | ``round(throttle*53)`` px, 4 строки |
| полосы гексагона ``DAMAGE`` | заливка 243 | ``round(charge*21)`` / ``round(charge*20)`` |
| иконки оружия/экипировки | ``WICON1`` / ``WICON7`` 24x24 | блит без ключа (гипотеза) |

Coordinate convention
---------------------

All public coordinates are **screen** coordinates in the game's 320x200
framebuffer; the returned layer is rows 122..199, i.e. panel-local
``y = screen_y - 122``.  Text uses the engine's *bottom-left* origin: the call
``0x47faf0(font, text, buf, x, y, pitch, 0)`` in ``0x4926a0`` passes
``DAMAGE (141,150)``, ``KILL (52,186)``, the weapon name ``(23,162)``, and the
glyph row 0 (the bottom row of the atlas cell) lands on ``y``, higher rows on
``y-1, y-2, ...`` — that is what the measured ink boxes show.

Honesty
-------

The in-game cockpit behind the panel is **3D** (the bezel, windows and icons
are textured quads), while ``PANEL`` is a flat 320x78 bitmap of the same
layout.  Its geometry lines up with the frame at ``(0,122)`` (hexagon, LED
strips, window outlines), but its colours do not: the engine's 3D lighting
lifts the metal to ~40..80 while the flat art keeps the 23..143 ramp.
Therefore ``--diff`` reports high structural agreement on text/gauges and low
colour agreement on the background.  Icons are 24x24 atlas entries drawn onto
3D quads (the frame shows ~28x27 with perspective), so their placement is a
hypothesis: ``WICON1`` is the gold ammo-belt seen in ``win2`` and ``WICON7``
the equipment bulb, but a flat NCC against the frame only reaches 0.1..0.3 —
the atlas is not blitted 1:1 in the game.

``gm/sprite.py`` does not exist in this tree (``tools/re/sprite_goldens.py``
reads ``.sbm`` directly); ``gm/render.py`` is imported but the panel/font
assets are BMPs inside ``FRONTEND/BMP.IDX``, so ``gm/bigfile`` + ``gm/bmp`` is
the right reader and no ``.sbm`` code is involved.

Usage
-----

    python3 tools/re/hud_compose_ref.py --self-test
    python3 tools/re/hud_compose_ref.py --json
    python3 tools/re/hud_compose_ref.py --png /var/tmp/hud.png
    python3 tools/re/hud_compose_ref.py --diff out/oracle/win2.png
    python3 tools/re/hud_compose_ref.py --diff out/oracle/win2.png --json

Frames and the game are found in this order: explicit flag,
``$GUNMETAL_ORACLE`` / ``$GUNMETAL_GAME``, the mirror
``~/.cache/gunmetal-re/game``, the installed game ``/mnt/d/games/Gunmetal 3dfx``
and (for frames) ``out/oracle`` of the main repository.
"""
from __future__ import annotations

import argparse
import json
import math
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
if ROOT not in sys.path:
    sys.path.insert(0, ROOT)
if os.path.join(ROOT, "tools", "re") not in sys.path:
    sys.path.insert(0, os.path.join(ROOT, "tools", "re"))

from gm import bigfile  # noqa: E402
from gm import bmp  # noqa: E402
from gm import palette as gpal  # noqa: E402
from gm import png  # noqa: E402
# ``gm.render`` is imported (never modified) so the future port reuses the same
# modules; the panel/icon path below deliberately uses ``gm.bmp`` because these
# assets are loose BMPs inside ``FRONTEND/BMP.IDX``, not ``.sbm`` textures.
from gm import render as gmrender  # noqa: E402,F401

SCREEN_W = 320
SCREEN_H = 200
PANEL_W = 320
PANEL_H = 78
PANEL_Y = 122                 # screen row of the panel's first line

#: ``PANEL`` palette index 0 is the cyan key in the BMP; the engine's GAMEPAL
#: maps it to black.  It is the only transparent index of the panel blit.
PANEL_KEY = 0

#: Colour of the throttle/charge fills: ``memset 0xf3`` in ``0x492f40``..
#: ``0x492f7f``; GAMEPAL index 243 is pure red.
FILL_INDEX = 243

#: Maximum fill length of the throttle bar: ``cmp edx,0x35`` at ``0x492f2e``.
THROTTLE_MAX_PX = 53

BRIGHT_THRESHOLD = 70         # same as tools/re/hud_audit.py
#: Our flat panel art is brighter than the engine's 3D-lit bezel, so a plain
#: ``>=70`` mask merges whole strips of the panel.  For cluster matching we use
#: an *ink* threshold (text highlights, LEDs, white bars) on our layer and the
#: oracle's ``static & bright`` mask as the reference.
INK_THRESHOLD = 150

#: Default state is the ``win2`` frame: MACHINE GUN, 1999 rounds, 0 damage,
#: 0 throttle, HEADLIGHTS equipment, 0 kills.
DEFAULT_STATE = {
    "weapon_name": "MACHINE GUN",
    "ammo": 1999,
    "damage": 0,
    "throttle": 0.0,
    "equipment_name": "HEADLIGHTS",
    "equipment_value": 0,
    "kills": 0,
    "headlights": 0,
    "charge": 0.0,
    "weapon_icon": "WICON1",
    "equipment_icon": "WICON7",
    "icons": True,
}


# --------------------------------------------------------------------------
# Fonts
# --------------------------------------------------------------------------
class FontSpec:
    """One record of the engine font table ``0x65ddd0`` (see TEXT_LAYOUT §2.3).

    ``origin_x/origin_y`` are **buffer** coordinates (the atlas is read
    bottom-up: ``image_row = height - 1 - y``), exactly like the loader
    ``0x47fe30`` records them.
    """

    __slots__ = ("number", "atlas", "cell_w", "cell_h", "first", "count",
                 "columns", "col_pitch", "row_pitch", "origin_x", "origin_y",
                 "space", "extra_before", "extra_after", "trim", "label")

    def __init__(self, number, atlas, cell_w, cell_h, first, count, columns,
                 col_pitch, row_pitch, origin_x, origin_y, space,
                 extra_before, extra_after, trim, label):
        self.number = number
        self.atlas = atlas
        self.cell_w = cell_w
        self.cell_h = cell_h
        self.first = first
        self.count = count
        self.columns = columns
        self.col_pitch = col_pitch
        self.row_pitch = row_pitch
        self.origin_x = origin_x
        self.origin_y = origin_y
        self.space = space
        self.extra_before = extra_before
        self.extra_after = extra_after
        self.trim = trim
        self.label = label


#: Reconstructed from the ``0x47fe30`` calls at ``0x490145``..``0x49054f``.
#: Font 6 is *present* in the loader (``push 6`` at ``0x490279``): it is the
#: FONTS3 face at origin y=0, identical to font 7 except for the origin row.
#: ``docs/TEXT_LAYOUT.md`` §2.3 omits it — see ``docs/HUD_COMPOSE.md``.
FONT_SPECS = {
    6: FontSpec(6, "FONTS3", 8, 8, 32, 64, 32, 16, 16, 8, 0, 4, 0, 1, True,
                "HUD labels"),
    9: FontSpec(9, "NUMFONTS", 8, 10, 48, 10, 10, 16, 16, 8, 6, 8, 0, 1,
                False, "green digits (damage)"),
    10: FontSpec(10, "NUMFONTS", 8, 10, 48, 10, 10, 16, 16, 0, 22, 8, 0, 1,
                 False, "red digits (ammo, kill, equipment)"),
    11: FontSpec(11, "NUMFONTS", 8, 8, 48, 10, 10, 16, 8, 8, 32, 8, 0, 1,
                 False, "small red digits"),
    12: FontSpec(12, "NUMFONTS", 8, 8, 48, 10, 10, 16, 8, 0, 40, 8, 0, 1,
                 False, "small white digits"),
}


class Glyph:
    __slots__ = ("advance", "width", "pixels", "cell_h")

    def __init__(self, advance, width, pixels, cell_h):
        self.advance = advance
        self.width = width
        self.pixels = pixels          # width*cell_h, row 0 = bottom
        self.cell_h = cell_h


class Font:
    """Glyph atlas extracted from a ``FRONTEND/BMP.IDX`` entry."""

    def __init__(self, spec, image):
        self.spec = spec
        self.width = image.width
        self.height = image.height
        self.pixels = image.pixels
        self.glyphs = {}
        self.by_code = {}
        for i in range(spec.count):
            code = spec.first + i
            gx = spec.origin_x + (i % spec.columns) * spec.col_pitch
            gy = spec.origin_y + (i // spec.columns) * spec.row_pitch
            self.by_code[code] = self._extract(i, gx, gy)
        if spec.number == 9:
            # The ``%`` of ``"0 %"`` is an extra cell right of the ten digits
            # (NUMFONTS is 180 px wide, 10 columns declare only 160 px).
            extra_x = spec.origin_x + spec.columns * spec.col_pitch
            self.glyphs["%"] = self._extract(10, extra_x, spec.origin_y,
                                             trim=False)

    # -- extraction -----------------------------------------------------
    def _pixel(self, x, y):
        if not (0 <= x < self.width and 0 <= y < self.height):
            return 0
        return self.pixels[(self.height - 1 - y) * self.width + x]

    def _extract(self, index, gx, gy, trim=None):
        spec = self.spec
        trim = spec.trim if trim is None else trim
        code = spec.first + index
        cw, ch = spec.cell_w, spec.cell_h
        if trim and code == 32:
            # 0x47ff5b: the space advance comes from the call argument.
            return Glyph(spec.space + spec.extra_before + spec.extra_after,
                         0, b"", ch)
        if trim:
            nonzero = [
                c for c in range(cw)
                if any(self._pixel(gx + c, gy + r) for r in range(ch))
            ]
            low, high = (nonzero[0], nonzero[-1]) if nonzero else (0, -1)
            width = max(0, high - low + 1)
        else:
            low, width = 0, cw
        pixels = bytearray()
        for r in range(ch):
            for c in range(width):
                pixels.append(self._pixel(gx + low + c, gy + r))
        if trim:
            advance = width
        else:
            advance = spec.space if code == 32 else width
        return Glyph(advance + spec.extra_before + spec.extra_after,
                     width, bytes(pixels), ch)

    # -- drawing --------------------------------------------------------
    def glyph(self, ch):
        if ch in self.glyphs:
            return self.glyphs[ch]
        code = ord(ch)
        return self.by_code.get(code)

    def measure(self, text):
        total = 0
        for ch in text:
            g = self.glyph(ch)
            if g is not None:
                total += g.advance
        return total

    def draw(self, canvas, x, y_bottom, text):
        """Draw text with a bottom-left origin; index 0 is transparent."""
        pen = x
        for ch in text:
            g = self.glyph(ch)
            if g is None:
                continue
            if g.width and g.pixels:
                for r in range(g.cell_h):
                    y = y_bottom - r
                    if not (0 <= y < PANEL_H):
                        continue
                    row = r * g.width
                    for c in range(g.width):
                        value = g.pixels[row + c]
                        if value == 0:
                            continue
                        px = pen + c
                        if 0 <= px < PANEL_W:
                            canvas[y * PANEL_W + px] = value
            pen += g.advance
        return pen


# --------------------------------------------------------------------------
# Assets
# --------------------------------------------------------------------------
class Assets:
    """Everything the composer reads from the game (read-only)."""

    def __init__(self, game_dir=None):
        self.game_dir = game_dir
        self.panel = None             # Bmp 320x78
        self.palette = None           # 256 RGBX quads
        self.fonts = {}
        self._archive = None
        self._icons = {}
        self.missing = []
        if game_dir:
            self._load(game_dir)

    # -- loading --------------------------------------------------------
    def _open(self, game_dir):
        index = os.path.join(game_dir, "FRONTEND", "BMP.IDX")
        if not os.path.exists(index):
            alt = os.path.join(game_dir, "frontend", "bmp.idx")
            index = alt if os.path.exists(alt) else index
        if not os.path.exists(index):
            raise FileNotFoundError(f"no FRONTEND/BMP.IDX under {game_dir!r}")
        return bigfile.BigFile(index)

    def _entry(self, name):
        wanted = name.upper()
        for entry in self._archive.entries:
            if entry.name.upper() == wanted:
                return entry
        return None

    def _bmp(self, name):
        entry = self._entry(name)
        if entry is None:
            return None
        try:
            return bmp.parse(self._archive.read(entry))
        except (ValueError, OSError):
            return None

    def _load(self, game_dir):
        try:
            self._archive = self._open(game_dir)
        except (OSError, ValueError) as exc:
            self.missing.append(f"archive: {exc}")
            return
        self.panel = self._bmp("PANEL")
        if self.panel is None:
            self.missing.append("PANEL")
        try:
            self.palette = gpal.installed(game_dir)
        except (OSError, ValueError, FileNotFoundError) as exc:
            self.missing.append(f"GAMEPAL: {exc}")
        for number, spec in FONT_SPECS.items():
            image = self._bmp(spec.atlas)
            if image is None:
                self.missing.append(spec.atlas)
                continue
            try:
                self.fonts[number] = Font(spec, image)
            except (IndexError, ValueError) as exc:
                self.missing.append(f"{spec.atlas}: {exc}")

    # -- icons ----------------------------------------------------------
    def icon(self, name):
        if not name or self._archive is None:
            return None
        key = name.upper()
        if key in self._icons:
            return self._icons[key]
        image = self._bmp(key)
        if image is None or image.width != 24 or image.height != 24:
            self._icons[key] = None
            return None
        self._icons[key] = image
        return image

    # -- colours --------------------------------------------------------
    def colour(self, index):
        if self.palette is not None and len(self.palette) >= 1024:
            return self.palette[index * 4: index * 4 + 3]
        # Deterministic fallback: no palette on disk -> grey ramp.
        return bytes((index, index, index))


# --------------------------------------------------------------------------
# Composition
# --------------------------------------------------------------------------
def _normalise(state):
    out = dict(DEFAULT_STATE)
    if state:
        out.update({k: v for k, v in state.items() if v is not None})
    out["ammo"] = max(0, min(99999, int(out.get("ammo", 0))))
    out["damage"] = max(0, min(100, int(round(float(out.get("damage", 0))))))
    out["throttle"] = max(0.0, min(1.0, float(out.get("throttle", 0.0))))
    out["charge"] = max(0.0, min(1.0, float(out.get("charge", 0.0))))
    out["kills"] = max(0, min(9999, int(out.get("kills", 0))))
    out["equipment_value"] = max(0, min(9999, int(out.get("equipment_value", 0))))
    out["headlights"] = 1 if out.get("headlights") else 0
    out["icons"] = bool(out.get("icons", True))
    for key in ("weapon_name", "equipment_name"):
        value = out.get(key, "")
        out[key] = str(value)[:40]
    return out


def _blit(canvas, x, y, width, height, pixels, key=None):
    for row in range(height):
        py = y + row
        if not (0 <= py < PANEL_H):
            continue
        base = row * width
        for col in range(width):
            px = x + col
            if not (0 <= px < PANEL_W):
                continue
            value = pixels[base + col]
            if key is not None and value == key:
                continue
            canvas[py * PANEL_W + px] = value


def compose_indices(state=None, assets=None):
    """Draw the panel layer as 8-bit palette indices (320x78)."""
    st = _normalise(state)
    canvas = bytearray(PANEL_W * PANEL_H)

    # 1. Flat panel art.  In the engine this is the 320x78 ``PANEL`` blit at
    #    (0,122) with index 0 as the transparency key; the 3D cockpit shows
    #    through the holes (see docs/HUD_COMPOSE.md §2).
    if assets is not None and assets.panel is not None:
        panel = assets.panel
        _blit(canvas, 0, 0, min(PANEL_W, panel.width),
              min(PANEL_H, panel.height), panel.pixels, key=PANEL_KEY)

    # 2. Gauges: the only non-sprite primitives of the cockpit.
    #    Throttle: 4 rows of 0xf3 starting at (202,155); the run length is
    #    ``round(engine_power*53)`` clamped by the engine at 0x492f2e.
    fill = int(round(st["throttle"] * THROTTLE_MAX_PX))
    if fill > 0:
        for row in range(4):
            y = 155 - PANEL_Y + row
            for col in range(fill):
                x = 202 + col
                if 0 <= x < PANEL_W and 0 <= y < PANEL_H:
                    canvas[y * PANEL_W + x] = FILL_INDEX

    #    Damage hexagon bars: top (146,150,21x3), bottom (147,176,20x3).
    #    Both fills are 0xf3; width from ``charge_accumulated (+0x2FA8)`` is a
    #    hypothesis (docs/HUD_AUDIT.md §3.2).
    charge = st["charge"]
    top = int(round(charge * 21))
    bottom = int(round(charge * 20))
    for row in range(3):
        y = 150 - PANEL_Y + row
        for col in range(top):
            x = 146 + col
            if 0 <= x < PANEL_W and 0 <= y < PANEL_H:
                canvas[y * PANEL_W + x] = FILL_INDEX
    for row in range(3):
        y = 176 - PANEL_Y + row
        for col in range(bottom):
            x = 147 + col
            if 0 <= x < PANEL_W and 0 <= y < PANEL_H:
                canvas[y * PANEL_W + x] = FILL_INDEX

    # 3. Icons (24x24).  Placement is measured from the frame and is the
    #    weakest part of the model: the engine maps the atlas onto 3D quads.
    if st["icons"] and assets is not None:
        icon = assets.icon(st["weapon_icon"])
        if icon is not None:
            _blit(canvas, 23, 161 - PANEL_Y, 24, 24, icon.pixels)
        icon = assets.icon(st["equipment_icon"])
        if icon is not None:
            _blit(canvas, 201, 161 - PANEL_Y, 24, 24, icon.pixels)

    # 4. Text.  Coordinates are the constants of 0x4926a0 (screen coords).
    fonts = assets.fonts if assets is not None else {}

    def font(number):
        return fonts.get(number)

    f6 = font(6)
    if f6 is not None:
        f6.draw(canvas, 23, 162 - PANEL_Y, st["weapon_name"])
        f6.draw(canvas, 52, 186 - PANEL_Y, "KILL")
        f6.draw(canvas, 141, 150 - PANEL_Y, "DAMAGE")
        f6.draw(canvas, 201, 160 - PANEL_Y, "THROTTLE")
        f6.draw(canvas, 226, 166 - PANEL_Y, st["equipment_name"])

    f10 = font(10)
    if f10 is not None:
        ammo = str(st["ammo"])
        # Right-aligned to x=85: the engine subtracts 8 px per extra digit
        # (thresholds 10/100/1000/10000 at 0x4929f4..0x492a6b).  NUMFONTS
        # copies the whole 8 px cell and adds ``extra_after = 1``, so one
        # digit advances 9 px and the last cell ends at ``start + 9*n - 1``.
        f10.draw(canvas, 86 - 9 * len(ammo), 174 - PANEL_Y, ammo)
        f10.draw(canvas, 74, 185 - PANEL_Y, str(st["kills"]))
        f10.draw(canvas, 246, 182 - PANEL_Y, str(st["equipment_value"]))

    f9 = font(9)
    if f9 is not None:
        # The engine right-aligns the number to a fixed column and prints the
        # percent sign as a separate cell at x=166 (the ``%`` cell of NUMFONTS
        # sits beyond the declared ten columns).  The x offsets of 0x492722 /
        # 0x49274f / 0x4927b3 are 140/148/156 for 3/2/1 digits — exactly
        # ``166 - 9*n``.  Values >= 70 switch to the red face (font 10).
        text = str(st["damage"])
        x = 166 - 9 * len(text)
        face = font(10) if st["damage"] >= 70 and f10 is not None else f9
        face.draw(canvas, x, 190 - PANEL_Y, text)
        f9.draw(canvas, 166, 190 - PANEL_Y, "%")

    return canvas


def compose(state=None, assets=None):
    """Public API: ``(320, 78, bytes RGB)`` panel layer.

    ``assets=None`` is legal and deterministic: the layer is then a black
    canvas with the gauge fills only (used by ``--self-test`` on a machine
    without the game).  The signature is what the port into ``gm/render.py``
    needs: ``compose(state) -> (w, h, rgb)``.
    """
    st = _normalise(state)
    canvas = compose_indices(st, assets)
    out = bytearray(PANEL_W * PANEL_H * 3)
    if assets is not None and assets.palette is not None and \
            len(assets.palette) >= 1024:
        pal = assets.palette
        for i, index in enumerate(canvas):
            out[3 * i:3 * i + 3] = pal[4 * index:4 * index + 3]
    else:
        for i, index in enumerate(canvas):
            out[3 * i] = out[3 * i + 1] = out[3 * i + 2] = index
    return PANEL_W, PANEL_H, bytes(out)


# --------------------------------------------------------------------------
# Expected layout (single source for docs/--json/cluster matching)
# --------------------------------------------------------------------------
#: ``id -> (screen rect, kind, description)``; the rects are the measured
#: bounding boxes of docs/HUD_AUDIT.md §4, used only for reporting/matching —
#: the drawing coordinates above come from the code constants.
ELEMENTS = (
    ("panel", (0, 122, 320, 78), "panel", "PANEL 320x78 at (0,122)"),
    ("led_top_left", (13, 129, 33, 8), "led", "PANEL art"),
    ("led_top_right", (195, 129, 39, 8), "led", "PANEL art"),
    ("led_top_far", (246, 131, 56, 6), "led", "PANEL art"),
    ("led_left_column", (11, 162, 6, 21), "led", "PANEL art"),
    ("led_bottom", (13, 192, 290, 8), "led", "PANEL art"),
    ("label_weapon", (22, 154, 62, 8), "text", "font 6 at (23,162)"),
    ("icon_weapon", (21, 161, 28, 27), "icon", "WICON1 at (23,161) [hyp]"),
    ("value_ammo", (49, 162, 36, 11), "digits", "font 10 right-aligned to 85"),
    ("label_kill", (51, 178, 18, 7), "text", "font 6 at (52,186)"),
    ("value_kill", (72, 178, 8, 10), "digits", "font 10 at (74,185)"),
    ("plate_damage", (136, 139, 45, 53), "frame", "PANEL hexagon"),
    ("label_damage", (139, 142, 37, 7), "text", "font 6 at (141,150)"),
    ("bar_damage_top", (146, 150, 21, 3), "bar", "fill 0xf3, charge [hyp]"),
    ("icon_damage", (149, 153, 14, 19), "icon", "PANEL art"),
    ("bar_damage_bottom", (147, 176, 20, 3), "bar", "fill 0xf3, charge [hyp]"),
    ("value_damage", (155, 180, 21, 11), "digits",
     "font 9/10 right-aligned, % at (166,190)"),
    ("label_throttle", (203, 152, 48, 8), "text", "font 6 at (201,160)"),
    ("bar_throttle", (201, 155, 55, 4), "bar", "fill 0xf3, 0..53 px"),
    ("label_headlights", (226, 160, 54, 7), "text", "font 6 at (226,166)"),
    ("icon_equipment", (199, 161, 28, 27), "icon", "WICON7 at (201,161) [hyp]"),
    ("value_equipment", (245, 172, 10, 11), "digits", "font 10 at (246,182)"),
    ("compass", (76, 0, 54, 20), "dynamic", "not drawn (yaw, docs §3.5)"),
)


def bright_mask(rgb, width=PANEL_W, height=PANEL_H, threshold=BRIGHT_THRESHOLD):
    mask = bytearray(width * height)
    for i in range(width * height):
        r, g, b = rgb[3 * i: 3 * i + 3]
        if max(r, g, b) >= threshold:
            mask[i] = 1
    return mask


def layer_clusters(rgb, threshold=INK_THRESHOLD):
    """Ink clusters of our layer, in screen coordinates."""
    import hud_audit as ha

    mask = bright_mask(rgb, threshold=threshold)
    # ``hud_audit.clusters`` works on a full-width mask; feed our 320x78 with
    # an empty band above so the returned boxes are already screen-like.
    full = bytearray(SCREEN_W * SCREEN_H)
    for y in range(PANEL_H):
        full[(PANEL_Y + y) * SCREEN_W:(PANEL_Y + y + 1) * SCREEN_W] = \
            mask[y * PANEL_W:(y + 1) * PANEL_W]
    return ha.clusters(bytes(full))


def element_bright_counts(rgb, mask=None):
    """Per-element bright/ink pixel count of our layer and oracle mask."""
    out = []
    for eid, (x, y, w, h), kind, note in ELEMENTS:
        mine = 0
        ink = 0
        for yy in range(max(y, PANEL_Y), min(y + h, SCREEN_H)):
            row = yy - PANEL_Y
            for xx in range(x, x + w):
                i = 3 * (row * PANEL_W + xx)
                value = max(rgb[i:i + 3])
                if value >= BRIGHT_THRESHOLD:
                    mine += 1
                if value >= INK_THRESHOLD:
                    ink += 1
        oracle = 0
        if mask is not None:
            for yy in range(y, y + h):
                for xx in range(x, x + w):
                    if 0 <= xx < SCREEN_W and 0 <= yy < SCREEN_H:
                        oracle += mask[yy * SCREEN_W + xx]
        out.append({"id": eid, "kind": kind, "rect": [x, y, w, h],
                    "note": note, "bright": mine, "ink": ink,
                    "oracle_bright": oracle})
    return out


# --------------------------------------------------------------------------
# Oracle helpers
# --------------------------------------------------------------------------
def find_game_dir(explicit=None):
    candidates = [
        explicit,
        os.environ.get("GUNMETAL_GAME"),
        os.path.expanduser("~/.cache/gunmetal-re/game"),
        "/mnt/d/games/Gunmetal 3dfx",
    ]
    for path in candidates:
        if not path:
            continue
        for sub in (("FRONTEND", "BMP.IDX"), ("frontend", "bmp.idx")):
            if os.path.exists(os.path.join(path, *sub)):
                return path
    return None


def _git_common_root():
    import subprocess

    try:
        out = subprocess.run(
            ["git", "rev-parse", "--git-common-dir"], cwd=ROOT,
            capture_output=True, text=True, timeout=10, check=False)
    except (OSError, subprocess.SubprocessError):
        return None
    if out.returncode != 0 or not out.stdout.strip():
        return None
    common = out.stdout.strip()
    if not os.path.isabs(common):
        common = os.path.join(ROOT, common)
    return os.path.dirname(os.path.abspath(common))


def find_frames_dir(explicit=None):
    candidates = [explicit, os.environ.get("GUNMETAL_ORACLE"),
                  os.path.join(ROOT, "out", "oracle")]
    common = _git_common_root()
    if common:
        candidates.append(os.path.join(common, "out", "oracle"))
    for path in candidates:
        if path and os.path.exists(os.path.join(path, "win1.png")):
            return path
    return None


def load_oracle_band(path, width=SCREEN_W, height=SCREEN_H):
    """Downscale an oracle frame to 320x200 and return the cockpit band.

    ``1`` px window border is cropped first (docs/ORACLE_RENDER.md), then
    ``oracle_compare.resize_area`` (area average) is applied — the exact
    inverse of dgVoodoo's 320x200 -> 1920x1080 stretch.
    """
    import oracle_compare as oc

    w, h, rgb = oc.load_rgb(path)
    if w > 2 and h > 2:
        w, h, rgb = oc.crop((w, h, rgb), (1, 1, w - 2, h - 2))
    w, h, rgb = oc.resize_area((w, h, rgb), width, height)
    band = bytearray(PANEL_W * PANEL_H * 3)
    for row in range(PANEL_H):
        src = ((PANEL_Y + row) * width) * 3
        band[row * PANEL_W * 3:(row + 1) * PANEL_W * 3] = \
            rgb[src:src + PANEL_W * 3]
    return PANEL_W, PANEL_H, bytes(band)


def oracle_clusters(frames_dir, threshold=BRIGHT_THRESHOLD):
    """Static&bright clusters of win1..win3 in the cockpit band."""
    import hud_audit as ha

    frames = []
    for name in ("win1.png", "win2.png", "win3.png"):
        path = os.path.join(frames_dir, name)
        if not os.path.exists(path):
            continue
        w, h, rgb = ha.load_rgb(path)
        frames.append(ha.downscale((w, h, rgb)))
    if not frames:
        return [], None
    static = ha.static_mask(frames)
    bright = ha.bright_mask(frames[1 if len(frames) > 1 else 0],
                            threshold=threshold)
    mask = bytes(1 if (static[i] and bright[i]) else 0
                 for i in range(SCREEN_W * SCREEN_H))
    return ha.clusters(mask), mask


def _bbox_iou(a, b):
    ax, ay, aw, ah = a
    bx, by, bw, bh = b
    ix = min(ax + aw, bx + bw) - max(ax, bx)
    iy = min(ay + ah, by + bh) - max(ay, by)
    if ix <= 0 or iy <= 0:
        return 0.0
    inter = ix * iy
    union = aw * ah + bw * bh - inter
    return inter / union if union else 0.0


# --------------------------------------------------------------------------
# Metrics
# --------------------------------------------------------------------------
def _luma(rgb):
    return [(rgb[3 * i] * 30 + rgb[3 * i + 1] * 59 + rgb[3 * i + 2] * 11) // 100
            for i in range(len(rgb) // 3)]


def _pearson(a, b):
    n = len(a)
    if not n:
        return 0.0
    ma = sum(a) / n
    mb = sum(b) / n
    cov = sum((x - ma) * (y - mb) for x, y in zip(a, b)) / n
    va = sum((x - ma) ** 2 for x in a) / n
    vb = sum((y - mb) ** 2 for y in b) / n
    if va <= 0 or vb <= 0:
        return 0.0
    return cov / math.sqrt(va * vb)


def _edges(rgb, width, height, x0, y0, x1, y1):
    lum = _luma(rgb)
    out = set()
    for y in range(max(1, y0), min(height - 1, y1)):
        for x in range(max(1, x0), min(width - 1, x1)):
            i = y * width + x
            if abs(lum[i + 1] - lum[i - 1]) + abs(lum[i + width] - lum[i - width]) > 30:
                out.add((x, y))
    return out


def block_metrics(ours, oracle):
    """Compare our 320x78 RGB with the oracle band (both panel-local)."""
    blocks = {
        "all": (0, 0, PANEL_W, PANEL_H),
        "left": (0, 0, 100, PANEL_H),
        "center": (120, 0, 80, PANEL_H),
        "right": (190, 0, 130, PANEL_H),
        "top": (0, 0, PANEL_W, 20),
        "middle": (0, 20, PANEL_W, 40),
        "bottom": (0, 60, PANEL_W, 18),
    }
    report = {}
    for name, (x0, y0, w, h) in blocks.items():
        a = []
        b = []
        bright_a = bright_b = ink_a = ink_b = 0
        for y in range(y0, y0 + h):
            for x in range(x0, x0 + w):
                i = (y * PANEL_W + x) * 3
                pa = ours[i:i + 3]
                pb = oracle[i:i + 3]
                a.append((pa[0] * 30 + pa[1] * 59 + pa[2] * 11) // 100)
                b.append((pb[0] * 30 + pb[1] * 59 + pb[2] * 11) // 100)
                if max(pa) >= BRIGHT_THRESHOLD:
                    bright_a += 1
                if max(pb) >= BRIGHT_THRESHOLD:
                    bright_b += 1
                if max(pa) >= INK_THRESHOLD:
                    ink_a += 1
                if max(pb) >= INK_THRESHOLD:
                    ink_b += 1
        n = len(a)
        mad = sum(abs(x - y) for x, y in zip(a, b)) / (255 * n) if n else 0.0
        ea = _edges(ours, PANEL_W, PANEL_H, x0, y0, x0 + w, y0 + h)
        eb = _edges(oracle, PANEL_W, PANEL_H, x0, y0, x0 + w, y0 + h)
        inter = len(ea & eb)
        union = len(ea | eb)
        report[name] = {
            "corr": round(_pearson(a, b), 4),
            "mad": round(mad, 4),
            "edges": round(inter / union, 4) if union else 0.0,
            "bright": [bright_a, bright_b],
            "ink": [ink_a, ink_b],
        }
    return report


def _ink_bbox(rgb, rect):
    """Bounding box of >=INK_THRESHOLD pixels inside a screen rect."""
    x, y, w, h = rect
    xs = []
    ys = []
    for yy in range(max(y, PANEL_Y), min(y + h, SCREEN_H)):
        row = yy - PANEL_Y
        for xx in range(x, x + w):
            if max(rgb[3 * (row * PANEL_W + xx):
                       3 * (row * PANEL_W + xx) + 3]) >= INK_THRESHOLD:
                xs.append(xx)
                ys.append(yy)
    if not xs:
        return None
    return (min(xs), min(ys), max(xs) - min(xs) + 1, max(ys) - min(ys) + 1)


def cluster_report(ours, frames_dir=None, oracle_mask=None):
    """Match measured clusters to our ink, element by element.

    A cluster is a *hit* when the element it belongs to (the same table
    ``hud_audit.match_elements`` uses) has ink in our layer at all: the oracle
    mask is ``static & bright`` and therefore merges neighbouring elements,
    so raw cluster IoU is an unfair metric.  IoU of our ink bbox inside the
    element rect is reported as a secondary number.
    """
    import hud_audit as ha

    mine = layer_clusters(ours)
    rows = [{"x": c["x"], "y": c["y"], "w": c["w"], "h": c["h"],
             "pixels": c["pixels"]} for c in mine]
    if oracle_mask is None:
        return {"ours": rows, "oracle": [], "matched": 0, "total_oracle": 0,
                "unmatched_ours": len(rows), "detail": [], "by_element": {}}

    measured = [c for c in ha.clusters(oracle_mask) if c["y"] >= PANEL_Y]
    detail = []
    by_element = {}
    matched = 0
    ink_by_id = {e["id"]: e["ink"] for e in element_bright_counts(ours)}
    for m in measured:
        names = ha.match_elements(m, include_panel=False)
        primary = names[0] if names else None
        rect = next((e[1] for e in ELEMENTS if e[0] == primary), None)
        ink = ink_by_id.get(primary, 0) if primary else 0
        iou = 0.0
        if rect is not None:
            bbox = _ink_bbox(ours, rect)
            if bbox is not None:
                iou = _bbox_iou((m["x"], m["y"], m["w"], m["h"]), bbox)
        hit = bool(ink > 0)
        if hit:
            matched += 1
        entry = by_element.setdefault(primary or "?", {"clusters": 0,
                                                       "hit": 0, "ink": ink})
        entry["clusters"] += 1
        entry["hit"] += 1 if hit else 0
        detail.append({"oracle": [m["x"], m["y"], m["w"], m["h"]],
                       "element": primary, "ours_ink": ink,
                       "bbox_iou": round(iou, 3), "match": hit})
    extra = 0
    for cl in mine:
        box = (cl["x"], cl["y"], cl["w"], cl["h"])
        if not any(_bbox_iou(box, (m["x"], m["y"], m["w"], m["h"])) > 0
                   for m in measured):
            extra += 1
    ious = sorted(row["bbox_iou"] for row in detail)
    median = ious[len(ious) // 2] if ious else 0.0
    return {
        "ours": rows,
        "oracle": [[m["x"], m["y"], m["w"], m["h"], m["pixels"]]
                   for m in measured],
        "matched": matched,
        "total_oracle": len(measured),
        "unmatched_ours": extra,
        "bbox_iou_median": round(median, 3),
        "elements_with_ink": sum(1 for e in element_bright_counts(ours)
                                 if e["ink"] > 0),
        "by_element": by_element,
        "detail": detail,
    }


def print_diff(report, clusters, path):
    print(f"oracle: {path}")
    print("block        corr      mad    edges   bright(ours/oracle)  ink")
    for name, m in report.items():
        print("%-10s %8.4f %8.4f %8.4f   %6d / %-6d %6d / %d"
              % (name, m["corr"], m["mad"], m["edges"],
                 m["bright"][0], m["bright"][1], m["ink"][0], m["ink"][1]))
    if clusters is not None:
        print("clusters: ours %d (ink>=%d), oracle %d in band, matched %d, "
              "bbox IoU median %.2f, elements with ink %d"
              % (len(clusters["ours"]), INK_THRESHOLD,
                 clusters["total_oracle"], clusters["matched"],
                 clusters["bbox_iou_median"], clusters["elements_with_ink"]))
        for row in clusters["detail"]:
            if not row["match"]:
                print("  no ink: oracle bbox %s -> element %s"
                      % (row["oracle"], row["element"]))


# --------------------------------------------------------------------------
# CLI
# --------------------------------------------------------------------------
def _cli_state(opts):
    state = dict(DEFAULT_STATE)
    if opts.ammo is not None:
        state["ammo"] = opts.ammo
    if opts.damage is not None:
        state["damage"] = opts.damage
    if opts.throttle is not None:
        state["throttle"] = opts.throttle
    if opts.kills is not None:
        state["kills"] = opts.kills
    if opts.charge is not None:
        state["charge"] = opts.charge
    if opts.weapon_name is not None:
        state["weapon_name"] = opts.weapon_name
    if opts.equipment_name is not None:
        state["equipment_name"] = opts.equipment_name
    if opts.no_icons:
        state["icons"] = False
    return state


def self_test():
    """Determinism, bounds, asset-free composition (no game needed)."""
    checks = []
    ok = True

    def check(name, condition, detail=""):
        nonlocal ok
        checks.append((name, bool(condition), detail))
        if not condition:
            ok = False

    # 1. Asset-free composition: correct size, deterministic, no exception.
    w, h, rgb = compose(None, None)
    check("size", (w, h) == (PANEL_W, PANEL_H), f"{w}x{h}")
    check("rgb length", len(rgb) == PANEL_W * PANEL_H * 3, str(len(rgb)))
    w2, h2, rgb2 = compose(None, None)
    check("deterministic", rgb == rgb2)
    check("no assets -> black", set(rgb) == {0})

    # 2. State bounds: extreme values stay inside the layer and clamp.
    for state in ({"ammo": -5, "damage": -20, "throttle": -1, "kills": -1},
                  {"ammo": 10 ** 9, "damage": 10 ** 6, "throttle": 10,
                   "kills": 10 ** 6, "charge": 5},
                  {"weapon_name": "x" * 200, "equipment_name": "y" * 200},
                  {"ammo": 0, "damage": 0, "throttle": 0.0}):
        w, h, rgb = compose(state, None)
        check("bounds %s" % list(state)[0],
              (w, h) == (PANEL_W, PANEL_H) and len(rgb) == PANEL_W * PANEL_H * 3)
    # Clamped fill never exceeds the documented 53 px.
    _, _, rgb = compose({"throttle": 1.0, "charge": 1.0}, None)
    row = (155 - PANEL_Y) * PANEL_W
    # Without assets the palette is a grey ramp, so detect any non-zero pixel.
    count = sum(1 for x in range(PANEL_W) if any(rgb[3 * (row + x):3 * (row + x) + 3]))
    check("throttle clamp 53", count == THROTTLE_MAX_PX, str(count))

    # 3. Normalisation is pure and does not mutate the input.
    source = {"ammo": 7}
    copy = dict(source)
    compose(source, None)
    check("state not mutated", source == copy)

    # 4. Palette-free RGB is a grey ramp: no crash, index -> (i,i,i).
    _, _, rgb = compose({"weapon_name": "A"}, None)
    check("grey fallback", len(rgb) == PANEL_W * PANEL_H * 3)

    # 5. Layout table is sane: rects inside 320x200, unique ids.
    ids = [e[0] for e in ELEMENTS]
    check("element ids unique", len(ids) == len(set(ids)))
    inside = all(0 <= x and 0 <= y and x + w <= SCREEN_W and y + h <= SCREEN_H
                 for _, (x, y, w, h), _, _ in ELEMENTS)
    check("element rects inside screen", inside)

    # 6. Bright-mask/cluster helpers are deterministic and bounded.
    _, _, rgb = compose({"throttle": 0.5}, None)
    m1 = bright_mask(rgb)
    m2 = bright_mask(rgb)
    check("mask deterministic", m1 == m2 and len(m1) == PANEL_W * PANEL_H)
    cl = layer_clusters(rgb)
    check("clusters bounded",
          all(0 <= c["x"] and c["x"] + c["w"] <= PANEL_W
              and PANEL_Y <= c["y"] and c["y"] + c["h"] <= SCREEN_H
              for c in cl))

    # 7. With the real game: fonts load, composition is non-trivial and the
    #    state actually changes the picture.  Skipped when the game is absent.
    game = find_game_dir()
    if game:
        assets = Assets(game)
        check("PANEL loaded", assets.panel is not None,
              ", ".join(assets.missing))
        f6 = assets.fonts.get(6)
        check("font 6 loaded", f6 is not None)
        if f6 is not None:
            check("font 6 DAMAGE width", f6.measure("DAMAGE") == 36,
                  str(f6.measure("DAMAGE")))
            g_a = f6.glyph("A")
            g_0 = f6.glyph("0")
            check("font 6 glyphs differ",
                  g_a is not None and g_0 is not None
                  and g_a.pixels != g_0.pixels)
        _, _, rgb = compose(None, assets)
        check("assets -> not black", any(rgb))
        check("assets deterministic", compose(None, assets)[2] == rgb)
        check("state changes picture", compose({"ammo": 7}, assets)[2] != rgb)
    else:
        print("skip asset checks (no game directory)")

    for name, good, detail in checks:
        print("%s %s%s" % ("ok  " if good else "FAIL", name,
                           f" ({detail})" if detail and not good else ""))
    print("self-test: %d/%d" % (sum(1 for _, g, _ in checks if g), len(checks)))
    return 0 if ok else 1


def main(argv=None):
    ap = argparse.ArgumentParser(description="Reference cockpit composition.")
    ap.add_argument("--game", help="game dir (default: mirror or install)")
    ap.add_argument("--json", action="store_true")
    ap.add_argument("--self-test", action="store_true")
    ap.add_argument("--png", help="write the composed 320x78 layer as PNG")
    ap.add_argument("--diff", metavar="FRAME.png",
                    help="compare with an oracle frame's lower 78 rows")
    ap.add_argument("--frames", help="dir with win1..win3.png (cluster mask)")
    ap.add_argument("--ammo", type=int)
    ap.add_argument("--damage", type=int)
    ap.add_argument("--throttle", type=float)
    ap.add_argument("--kills", type=int)
    ap.add_argument("--charge", type=float)
    ap.add_argument("--weapon-name")
    ap.add_argument("--equipment-name")
    ap.add_argument("--no-icons", action="store_true")
    opts = ap.parse_args(argv)

    if opts.self_test:
        return self_test()

    state = _cli_state(opts)
    game = find_game_dir(opts.game)
    assets = Assets(game) if game else None
    w, h, rgb = compose(state, assets)

    result = {
        "game": game,
        "assets_missing": list(assets.missing) if assets is not None else
                          ["no game directory"],
        "state": _normalise(state),
        "size": [w, h],
        "elements": element_bright_counts(rgb),
    }

    if opts.png:
        with open(opts.png, "wb") as fh:
            fh.write(png.rgb(w, h, rgb))
        result["png"] = opts.png

    clusters = None
    frames_dir = None
    oracle_mask = None
    if opts.diff or opts.frames:
        frames_dir = find_frames_dir(opts.frames)
        if frames_dir is not None:
            _, oracle_mask = oracle_clusters(frames_dir)
    if opts.diff:
        diff_path = opts.diff
        if not os.path.exists(diff_path):
            # The worktree has no out/oracle: resolve the bare frame name in
            # the main repository's out/oracle (find_frames_dir knows about
            # git-common-dir).
            found = find_frames_dir(opts.frames)
            candidate = os.path.join(found, os.path.basename(diff_path)) \
                if found else ""
            if candidate and os.path.exists(candidate):
                diff_path = candidate
        oracle = load_oracle_band(diff_path)
        result["oracle_frame"] = diff_path
        result["blocks"] = block_metrics(rgb, oracle[2])
        clusters = cluster_report(rgb, frames_dir, oracle_mask)
        result["clusters"] = clusters
        # Per-element comparison against the static&bright mask of win1..3.
        result["elements"] = element_bright_counts(rgb, oracle_mask)
        if not opts.json:
            print_diff(result["blocks"], clusters, diff_path)

    if opts.json:
        print(json.dumps(result, indent=2, sort_keys=False))
    elif not opts.diff:
        print("game:", game or "not found",
              "| missing:", ", ".join(result["assets_missing"]) or "-")
        for el in result["elements"]:
            if el["bright"] or el["kind"] in ("panel", "frame"):
                print("%-18s %-7s bbox=%-18s bright=%d"
                      % (el["id"], el["kind"], tuple(el["rect"]), el["bright"]))

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
