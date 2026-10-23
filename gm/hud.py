"""Cockpit HUD panel composition for the reference frames (M4.4.5).

The original 320x200 frame is the world render plus the cockpit panel that
covers the bottom 78 rows: ``PANEL`` from ``FRONTEND/BMP.IDX`` (320x78, 8 bpp)
drawn at ``(0, 122)`` with palette index 0 as the transparent key
(``docs/HUD_AUDIT.md`` section 6).  ``gm/render.py`` draws the world only, so
the oracle frames miss the whole bottom band and every ``hud`` metric is
meaningless; this module adds the panel without touching the renderer.

The panel art is drawn through the engine fade LUT; the **text layer**
(labels and digits, ``0x4926a0``) is drawn *after* the blit with raw palette
indices, because the engine writes it straight to the screen
(``FUN_004926a0(puVar8, ...)`` at ``0x491a70+0x133``).  Icons and the gauge
fills are separate steps of ``docs/HUD_AUDIT.md`` section 6.

Palette: the engine draws HUD art through ``GAMEPAL`` (``gm/palette.py``,
``docs/PALETTE.md``); the BMP carries its own palette too, and the two differ
in a few slots.  ``palette="game"`` is the engine behaviour, ``palette="bmp"``
is kept for the A/B measurement in ``docs/HUD_PANEL.md``.

    python3 -m gm.hud --self-test
    python3 -m gm.hud --game /mnt/d/... --preview /var/tmp/panel.png
"""
from __future__ import annotations

import argparse
import os
import struct
import sys

#: first screen row covered by the panel (200 - 78)
PANEL_TOP = 122
#: palette index used as the chroma key (docs/HUD_AUDIT.md section 2.3)
TRANSPARENT_INDEX = 0


class Panel:
    """Indexed cockpit panel plus the palette stored in its BMP."""

    __slots__ = ("width", "height", "indices", "palette", "name")

    def __init__(self, name: str, width: int, height: int, indices: bytes,
                 palette: bytes) -> None:
        self.name = name
        self.width = width
        self.height = height
        self.indices = indices
        self.palette = palette


def load_panel(game_dir: str, name: str = "PANEL") -> Panel:
    """Read one 8-bpp panel from ``FRONTEND/BMP.IDX``/``.DAT``."""
    from . import bigfile, bmp

    idx = os.path.join(game_dir, "FRONTEND", "BMP.IDX")
    if not os.path.exists(idx):
        raise FileNotFoundError(f"no FRONTEND/BMP.IDX under {game_dir!r}")
    archive = bigfile.BigFile(idx)
    want = name.upper()
    for entry in archive.entries:
        if entry.name.upper() == want:
            image = bmp.parse(archive.read(entry))
            if image.bpp != 8:
                raise ValueError(f"{name}: {image.bpp} bpp, expected 8")
            return Panel(entry.name, image.width, image.height,
                         image.pixels, image.palette)
    raise KeyError(f"{name} not in {idx} ({len(archive.entries)} entries)")


#: Font table records of ``0x65ddd0`` (``docs/TEXT_LAYOUT.md`` 2.3):
#: ``(atlas, cell_w, cell_h, first, count, columns, col_pitch, row_pitch,
#: origin_x, origin_y, space, extra_before, extra_after, trim)``.  Origins are
#: *buffer* coordinates, i.e. ``image_row = height - 1 - y``.
FONT_SPECS = {
    6: ("FONTS3", 8, 8, 32, 64, 32, 16, 16, 8, 0, 4, 0, 1, True),
    9: ("NUMFONTS", 8, 10, 48, 10, 10, 16, 16, 8, 6, 8, 0, 1, False),
    10: ("NUMFONTS", 8, 10, 48, 10, 10, 16, 16, 0, 22, 8, 0, 1, False),
    11: ("NUMFONTS", 8, 8, 48, 10, 10, 16, 8, 8, 32, 8, 0, 1, False),
    12: ("NUMFONTS", 8, 8, 48, 10, 10, 16, 8, 0, 40, 8, 0, 1, False),
}


class Glyph:
    """One trimmed glyph: row 0 is the **top** cell row (engine draw order)."""

    __slots__ = ("advance", "width", "pixels", "cell_h")

    def __init__(self, advance: int, width: int, pixels: bytes,
                 cell_h: int) -> None:
        self.advance = advance
        self.width = width
        self.pixels = pixels
        self.cell_h = cell_h


class Font:
    """Glyphs of one font table slot, extracted from a ``FRONTEND`` atlas."""

    __slots__ = ("number", "width", "height", "pixels", "by_code")

    def __init__(self, number: int, image) -> None:
        spec = FONT_SPECS[number]
        (_, cell_w, cell_h, first, count, columns, col_pitch, row_pitch,
         origin_x, origin_y, space, extra_before, extra_after, trim) = spec
        self.number = number
        self.width = image.width
        self.height = image.height
        self.pixels = image.pixels
        self.by_code = {}
        for i in range(count):
            gx = origin_x + (i % columns) * col_pitch
            gy = origin_y + (i // columns) * row_pitch
            self.by_code[first + i] = self._extract(
                gx, gy, cell_w, cell_h, space, extra_before, extra_after,
                trim, first + i)

    # -- extraction -----------------------------------------------------
    def _pixel(self, x: int, y: int) -> int:
        if not (0 <= x < self.width and 0 <= y < self.height):
            return 0
        return self.pixels[(self.height - 1 - y) * self.width + x]

    def _extract(self, gx, gy, cw, ch, space, before, after, trim, code):
        if trim and code == 32:
            return Glyph(space + before + after, 0, b"", ch)
        if trim:
            used = [c for c in range(cw)
                    if any(self._pixel(gx + c, gy + r) for r in range(ch))]
            low, high = (used[0], used[-1]) if used else (0, -1)
            width = max(0, high - low + 1)
        else:
            low, width = 0, cw
        pixels = bytearray()
        for r in range(ch):
            for c in range(width):
                pixels.append(self._pixel(gx + low + c, gy + r))
        advance = width if trim else (space if code == 32 else width)
        return Glyph(advance + before + after, width, bytes(pixels), ch)

    def glyph(self, ch: str):
        return self.by_code.get(ord(ch))

    def measure(self, text: str) -> int:
        return sum(g.advance for g in (self.glyph(c) for c in text) if g)


def load_font(game_dir: str, number: int) -> Font:
    """Extract one font slot of the engine table (``0x65ddd0``)."""
    from . import bigfile, bmp

    if number not in FONT_SPECS:
        raise KeyError(f"font {number} is not initialised by the engine")
    atlas = FONT_SPECS[number][0]
    idx = os.path.join(game_dir, "FRONTEND", "BMP.IDX")
    archive = bigfile.BigFile(idx)
    want = atlas.upper()
    for entry in archive.entries:
        if entry.name.upper() == want:
            return Font(number, bmp.parse(archive.read(entry)))
    raise KeyError(f"{atlas} not in {idx}")


def draw_text(canvas: bytearray, font: Font, x: int, top: int, text: str,
              width: int = 320, height: int = 200, value=None) -> int:
    """Draw ``text`` into an index canvas, top-left cell row ``top``.

    ``0x47faf0`` writes glyph row 0 at the destination row and keeps going
    *down*, so the cell occupies ``top .. top+cell_h-1``; index 0 is
    transparent.  ``value`` maps a glyph index to what is stored (identity by
    default) — used to apply the fade LUT to the panel but not to the text.
    """
    pen = x
    for ch in text:
        glyph = font.glyph(ch)
        if glyph is None:
            continue
        for r in range(glyph.cell_h):
            y = top + r
            if not (0 <= y < height):
                continue
            row = r * glyph.width
            for c in range(glyph.width):
                index = glyph.pixels[row + c]
                if not index:
                    continue
                px = pen + c
                if 0 <= px < width:
                    canvas[y * width + px] = (
                        index if value is None else value[index])
        pen += glyph.advance
    return pen


#: Static labels of ``0x4926a0`` (font, x, cell top) in 320x200 screen rows.
#: The ``y`` argument of ``0x47faf0`` is the row *below* the cell, so
#: ``cell top = y - 8``; every position below matches ``gm_screen.bin`` on
#: 100% of its glyph pixels (docs/HUD_AUDIT.md 2.9).
HUD_LABELS = (
    ("weapon_name", 6, 23, 154, "MACHINE GUN"),
    ("kill_label", 6, 52, 178, "KILL"),
    ("damage_label", 6, 141, 142, "DAMAGE"),
    ("throttle_label", 6, 205, 151, "THROTTLE"),
    ("equipment_name", 6, 227, 161, "HEADLIGHTS"),
)

#: ``win2``/``win3`` HUD state (``1999`` rounds); the live dump
#: ``gm_screen.bin`` was taken at ``1985``.
HUD_STATE = {
    "weapon_name": "MACHINE GUN",
    "ammo": 1999,
    "kills": 0,
    "damage": 0,
    "equipment_name": "HEADLIGHTS",
    "equipment_value": 0,
}


def hud_text(state: dict | None = None, top: int = PANEL_TOP) -> list:
    """``(font number, x, cell top, text)`` for a HUD state.

    The digit columns are the code constants of ``0x4926a0``: the ammo row is
    right-aligned to ``x = 72`` in 8 px steps, the damage value to ``156``, the
    equipment value to ``246``; the kill counter sits at a fixed ``x = 72``.
    """
    st = dict(HUD_STATE)
    st.update({k: v for k, v in (state or {}).items() if v is not None})
    out = []
    for key, font, x, row, default in HUD_LABELS:
        text = str(st.get(key) or default)
        if text:
            out.append((font, x, row, text))
    ammo = str(max(0, int(st["ammo"])))
    out.append((10, 72 - 8 * (len(ammo) - 1), 163, ammo))
    out.append((11, 72, 177, str(max(0, int(st["kills"])))))
    damage = str(max(0, min(100, int(st["damage"]))))
    out.append((9 if int(st["damage"]) < 70 else 10,
                156 - 8 * (len(damage) - 1), 181, damage))
    equip = str(max(0, int(st["equipment_value"])))
    out.append((10, 246 - 8 * (len(equip) - 1), 173, equip))
    return out


#: Icon slots of ``0x4926a0``: weapon at ``(0x17, 0xaa)``, equipment at
#: ``(0xc9, 0xaa)`` in buffer rows, i.e. ``(23, 162)`` / ``(201, 162)`` on
#: screen after the same ``-8`` that the text uses.  ``WICON1`` is copied
#: upright, ``EQUIP0`` with its rows reversed (``EQUIP12`` is the same bitmap
#: already flipped, docs/HUD_ICONS.md section 3).
HUD_ICON_SLOTS = (
    ("weapon_icon", 23, 162),
    ("equipment_icon", 201, 162),
)

#: ``win2``/``win3`` icons: ``MACHINE GUN`` and ``HEADLIGHTS`` (slot 0).
HUD_ICONS = {"weapon_icon": "WICON1", "equipment_icon": "EQUIP12"}


def load_icon(game_dir: str, name: str):
    """One 24x24 HUD icon from ``FRONTEND/BMP.IDX`` (read-only)."""
    panel = load_panel(game_dir, name)
    if (panel.width, panel.height) != (24, 24):
        raise ValueError(f"{name}: {panel.width}x{panel.height}, "
                         "expected 24x24")
    return panel


def hud_icons(state: dict | None = None) -> list:
    """``(icon name, x, cell top)`` for the two icon slots."""
    st = dict(HUD_ICONS)
    st.update({k: v for k, v in (state or {}).items() if v})
    out = []
    for key, x, row in HUD_ICON_SLOTS:
        name = st.get(key)
        if name:
            out.append((str(name), x, row))
    return out


#: Charge bars around the damage hexagon (``0x4926a0``, the ``0x492f..``
#: block): four runs of at most 17 px, colour by length — ``0xf3`` below 6,
#: ``0x2b`` from 6 to 11, ``0xff`` from 12 (``DAT_0081c290/294/298/29c`` over
#: ``DAT_0081c28c``).  ``(key, x, screen row, dx, dy)``; the row is already
#: ``code_row - 8`` like the text, and the vertical bars grow *up*.
HUD_GAUGE_SLOTS = (
    ("bar_top", 148, 151, 1, 0),
    ("bar_bottom", 148, 177, 1, 0),
    ("bar_left", 142, 172, 0, -1),
    ("bar_right", 170, 172, 0, -1),
)

#: Longest run of one bar (``cmp ...,0x11`` in the code).
GAUGE_MAX = 17

#: ``win2``/``win3``/the live dump: all four charge bars are full.
HUD_GAUGES = {"bar_top": 17, "bar_bottom": 17, "bar_left": 17, "bar_right": 17}


def gauge_colour(length: int) -> int:
    """Palette index of a charge bar of ``length`` pixels."""
    if length < 6:
        return 0xF3
    if length < 12:
        return 0x2B
    return 0xFF


def hud_gauges(state: dict | None = None) -> list:
    """``(x, row, dx, dy, length, palette index)`` for the four charge bars."""
    st = dict(HUD_GAUGES)
    st.update({k: v for k, v in (state or {}).items() if v is not None})
    out = []
    for key, x, row, dx, dy in HUD_GAUGE_SLOTS:
        length = max(0, min(GAUGE_MAX, int(st.get(key) or 0)))
        if length:
            out.append((x, row, dx, dy, length, gauge_colour(length)))
    return out


#: Solid bars of the weapon/equipment block (``0x4926a0``): the throttle gauge
#: is four rows of ``0xf3`` from ``(202, 0xa3)`` with a maximum of ``0x35`` px
#: (``cmp edx,0x35``), the belt gauge two rows from ``(50, 0xb6)``; both grow
#: right and then move one row down.  ``(key, x, code row, rows, max)``.
HUD_BARS = (
    ("throttle_px", 202, 0xA3, 4, 0x35),
    ("belt_px", 50, 0xB6, 2, 320),
)

#: The live dump shows a 26 px throttle bar; ``win2``/``win3`` override it.
HUD_BAR_STATE = {"throttle_px": 26, "belt_px": 0}


def hud_bars(state: dict | None = None) -> list:
    """``(x, screen row, rows, length)`` for the throttle and belt gauges.

    The throttle row is cross-checked by the ``THROTTLE`` label: the code
    advances the pointer over the four fill rows (163..166) and then prints at
    ``puVar9 - 7``, i.e. row 159, which is what the frame shows to the pixel.
    """
    st = dict(HUD_BAR_STATE)
    st.update({k: v for k, v in (state or {}).items() if v is not None})
    out = []
    for key, x, row, rows, top in HUD_BARS:
        length = max(0, min(top, int(st.get(key) or 0)))
        if length:
            out.append((x, row - 8, rows, length))
    return out


#: Damage silhouette of the hexagon (`0x4926a0`, the `uVar1 & 0x40` block):
#: ``RPVICONS`` 60x29 holds three 20 px variants (intact / damaged / wrecked);
#: the code copies 19 rows from ``&DAT_006601A4 + stage*0x14`` with a 0x3c
#: stride and ``'s'`` = 0x73 as the transparency key, starting at row 0xa3
#: (163 -> screen 155) and x 0x93 (147).  The memory copy is bottom-up, so the
#: slice is flipped vertically (verified: 135/135 pixels).
HUD_DAMAGE_ICON = {"x": 147, "row": 163, "cols": 20, "rows": 19,
                   "slice_row": 5, "key": 0x73}
#: ``win2``/``win3`` show the intact variant.
HUD_DAMAGE_STAGE = 0


def load_damage_icon(game_dir: str, stage: int = 0) -> Panel:
    """One 20x19 damage silhouette out of ``RPVICONS`` (0..2)."""
    if not 0 <= stage <= 2:
        raise ValueError(f"damage stage {stage} outside 0..2")
    sheet = load_panel(game_dir, "RPVICONS")
    if (sheet.width, sheet.height) != (60, 29):
        raise ValueError(f"RPVICONS is {sheet.width}x{sheet.height}, "
                         "expected 60x29")
    base = 20 * stage
    y0 = HUD_DAMAGE_ICON["slice_row"]
    rows = [sheet.indices[(y0 + r) * sheet.width + base:
                          (y0 + r) * sheet.width + base + 20]
            for r in range(HUD_DAMAGE_ICON["rows"])]
    rows.reverse()
    return Panel(f"RPVICONS/{stage}", 20, HUD_DAMAGE_ICON["rows"],
                 b"".join(rows), sheet.palette)


def palette_bytes(game_dir: str, mode: str = "game") -> bytes:
    """256 RGBA quads for the panel: engine ``GAMEPAL`` (``"game"``).

    ``"bmp"`` is not a palette source here: the BMP's own palette travels with
    the :class:`Panel` (``panel.palette``), so a caller that wants the A/B
    simply passes it to :func:`compose`.
    """
    from . import palette

    if mode != "game":
        raise ValueError(f"unknown palette mode {mode!r} (use 'game' or "
                         "panel.palette)")
    return palette.installed(game_dir)


def compose(frame: bytes, width: int, height: int, panel: Panel,
            palette: bytes, top: int = PANEL_TOP,
            lut: bytes | None = None,
            text: tuple = (), fonts: dict | None = None,
            icons: tuple = (), icon_pixels: dict | None = None,
            gauges: tuple = (), bars: tuple = (),
            damage_icon=None) -> bytes:
    """Blit ``panel`` onto a packed RGB frame; index 0 stays transparent.

    ``lut`` is the engine's 256-byte fade table for one level: the blit maps
    every panel index through it before the palette lookup (``0x491f2d``,
    ``FUN_00a1b510(..., &DAT_00682f90 + k*0x100)``).  Without it the raw panel
    comes out unshaded, which is what made the earlier comparison fail.

    ``text`` items are ``(font number, x, cell top, string)`` in 320x200
    *screen* rows (``hud_text()`` produces them, so they do not move with
    ``top``) and are drawn **after** the blit with raw palette indices: ``0x4926a0`` runs after ``0x491a70`` and
    writes straight to the screen, so the text is not faded by ``lut``.

    Returns a new buffer.  Rows outside the frame are clipped; a panel wider
    than the frame is clipped on the right (both are trust-boundary guards,
    the shipped panel is exactly 320x78).
    """
    if len(frame) != width * height * 3:
        raise ValueError(f"frame is {len(frame)} bytes, expected "
                         f"{width * height * 3}")
    if len(palette) < 256 * 4:
        raise ValueError(f"palette is {len(palette)} bytes, expected 1024")
    if lut is not None and len(lut) < 256:
        raise ValueError(f"lut is {len(lut)} bytes, expected 256")
    out = bytearray(frame)
    rows = min(panel.height, height - top)
    cols = min(panel.width, width)
    for y in range(rows):
        dst = ((top + y) * width) * 3
        src = y * panel.width
        for x in range(cols):
            index = panel.indices[src + x]
            if index == TRANSPARENT_INDEX:
                continue
            if lut is not None:
                index = lut[index]
            po = index * 4
            do = dst + x * 3
            out[do] = palette[po]
            out[do + 1] = palette[po + 1]
            out[do + 2] = palette[po + 2]
    if icons and icon_pixels:
        for name, ix, iy in icons:
            image = icon_pixels.get(name)
            if image is None:
                continue
            _blit_index_rgb(out, width, height, palette, image, ix, iy)
    for bx, by, rows, length in bars:
        po = 0xF3 * 4
        for r in range(rows):
            y = by + r
            if not (0 <= y < height):
                continue
            for i in range(length):
                x = bx + i
                if 0 <= x < width:
                    do = (y * width + x) * 3
                    out[do] = palette[po]
                    out[do + 1] = palette[po + 1]
                    out[do + 2] = palette[po + 2]
    if damage_icon is not None:
        _blit_index_rgb(out, width, height, palette, damage_icon,
                        HUD_DAMAGE_ICON["x"],
                        HUD_DAMAGE_ICON["row"] - 8,
                        key=HUD_DAMAGE_ICON["key"])
    for gx, gy, dx, dy, length, index in gauges:
        po = index * 4
        for i in range(length):
            x = gx + dx * i
            y = gy + dy * i
            if 0 <= x < width and 0 <= y < height:
                do = (y * width + x) * 3
                out[do] = palette[po]
                out[do + 1] = palette[po + 1]
                out[do + 2] = palette[po + 2]
    if text and fonts:
        for number, tx, ty, string in text:
            font = fonts.get(number)
            if font is None:
                continue
            _draw_text_rgb(out, width, height, palette, font, tx, ty,
                           string)
    return bytes(out)


def _blit_index_rgb(out: bytearray, width: int, height: int, palette: bytes,
                    image, x: int, top: int, key: int = 0) -> None:
    """Blit an index bitmap with ``key`` transparent and no fade."""
    for r in range(image.height):
        y = top + r
        if not (0 <= y < height):
            continue
        for c in range(image.width):
            index = image.indices[r * image.width + c]
            if index == key:
                continue
            px = x + c
            if 0 <= px < width:
                po = index * 4
                do = (y * width + px) * 3
                out[do] = palette[po]
                out[do + 1] = palette[po + 1]
                out[do + 2] = palette[po + 2]


def _draw_text_rgb(out: bytearray, width: int, height: int, palette: bytes,
                   font: Font, x: int, top: int, text: str) -> None:
    """``draw_text`` straight into a packed RGB buffer (no fade)."""
    pen = x
    for ch in text:
        glyph = font.glyph(ch)
        if glyph is None:
            continue
        for r in range(glyph.cell_h):
            y = top + r
            if not (0 <= y < height):
                continue
            row = r * glyph.width
            for c in range(glyph.width):
                index = glyph.pixels[row + c]
                if not index:
                    continue
                px = pen + c
                if 0 <= px < width:
                    po = index * 4
                    do = (y * width + px) * 3
                    out[do] = palette[po]
                    out[do + 1] = palette[po + 1]
                    out[do + 2] = palette[po + 2]
        pen += glyph.advance


def engine_order(panel: Panel) -> Panel:
    """The same panel in the engine's row order.

    ``bmp.parse`` normalises rows top-down, but the engine's loader
    (``0x47efe0``) copies the BMP rows in *file* order into ``0x678188`` and
    the blit keeps that order, so the buffer is the vertical mirror of the
    parsed image (verified: ``flipV(asset)`` equals the dumped buffer on
    100% of the bytes).  Use this when reproducing the screen.
    """
    rows = [panel.indices[y * panel.width:(y + 1) * panel.width]
            for y in range(panel.height)]
    return Panel(panel.name + "/engine", panel.width, panel.height,
                 b"".join(reversed(rows)), panel.palette)


def load_lut(path: str, level: int) -> bytes:
    """One 256-byte fade table out of the engine's 16-table block."""
    data = open(path, "rb").read()
    if len(data) < 16 * 256:
        raise ValueError(f"lut file is {len(data)} bytes, expected 4096")
    if not 0 <= level < 16:
        raise ValueError(f"level {level} outside 0..15")
    return data[level * 256:(level + 1) * 256]


def screen_index_match(screen: bytes, sw: int, sh: int, panel: Panel,
                       lut: bytes, top: int, scale: float,
                       xphase: float = 2.0,
                       text: tuple = (), fonts: dict | None = None,
                       text_base: float | None = None,
                       icons: tuple = (), icon_pixels: dict | None = None,
                       gauges: tuple = (), bars: tuple = (),
                       damage_icon=None) -> dict:
    """Fraction of band pixels whose screen index equals ``lut[panel]``.

    This compares *indices* against a raw screen dump (``gm_screen.bin``), so
    neither dgVoodoo scaling nor the palette is involved: the only expected
    mismatches are the text/icons drawn over the panel afterwards.
    """
    if len(screen) != sw * sh:
        raise ValueError(f"screen is {len(screen)} bytes, expected {sw * sh}")
    if len(lut) < 256:
        raise ValueError("lut must be 256 bytes")
    expected = None
    if text and fonts:
        expected = bytearray(lut[i] for i in panel.indices)
        base = top / scale if text_base is None else text_base
        for number, tx, ty, string in text:
            font = fonts.get(number)
            if font is not None:
                draw_text(expected, font, tx, int(round(ty - base)), string,
                          width=panel.width, height=panel.height)
        for name, ix, iy in icons or ():
            image = (icon_pixels or {}).get(name)
            if image is None:
                continue
            irow = int(round(iy - base))
            for r in range(image.height):
                y = irow + r
                if not (0 <= y < panel.height):
                    continue
                for c in range(image.width):
                    index = image.indices[r * image.width + c]
                    px = ix + c
                    if index and 0 <= px < panel.width:
                        expected[y * panel.width + px] = index
        for bx, by, rows, length in bars or ():
            for r in range(rows):
                y = by - int(round(base)) + r
                if not (0 <= y < panel.height):
                    continue
                for i in range(length):
                    px = bx + i
                    if 0 <= px < panel.width:
                        expected[y * panel.width + px] = 0xF3
        if damage_icon is not None:
            ix = HUD_DAMAGE_ICON["x"]
            iy = HUD_DAMAGE_ICON["row"] - 8 - int(round(base))
            for r in range(damage_icon.height):
                y = iy + r
                if not (0 <= y < panel.height):
                    continue
                for c in range(damage_icon.width):
                    v = damage_icon.indices[r * damage_icon.width + c]
                    if v != HUD_DAMAGE_ICON["key"] and 0 <= ix + c < panel.width:
                        expected[y * panel.width + ix + c] = v
        for gx, gy, dx, dy, length, index in gauges or ():
            for i in range(length):
                px = gx + dx * i
                y = gy - int(round(base)) + dy * i
                if 0 <= px < panel.width and 0 <= y < panel.height:
                    expected[y * panel.width + px] = index
    hit = total = 0
    for py in range(panel.height):
        sy = top + int(py * scale)
        if sy >= sh:
            break
        base = sy * sw
        pbase = py * panel.width
        for px in range(panel.width):
            sx = int(px * sw / panel.width + xphase)
            if sx >= sw:
                continue
            want = (expected[pbase + px] if expected is not None
                    else lut[panel.indices[pbase + px]])
            if screen[base + sx] == want:
                hit += 1
            total += 1
    return {"match": round(hit / total, 4) if total else 0.0,
            "pixels": total, "top": top, "scale": scale}


def default_top(panel: Panel, height: int) -> int:
    """Bottom-aligned row for a panel, as the engine draws it."""
    return max(0, height - panel.height)


def panel_rgb(panel: Panel, palette: bytes, background=(0, 0, 0)) -> bytes:
    """The panel alone as packed RGB, for previews and pixel comparisons."""
    return compose(bytes(background) * (panel.width * panel.height),
                   panel.width, panel.height, panel, palette, top=0)


def load_rgb(path: str) -> tuple[int, int, bytes]:
    """Decode a PNG into packed RGB (P/RGB/RGBA/L modes, stdlib only)."""
    from . import png

    with open(path, "rb") as fh:
        image = png.decode(fh.read())
    w, h, px = image.width, image.height, image.pixels
    out = bytearray(w * h * 3)
    if image.mode == "P":
        pal = image.palette
        for i, index in enumerate(px):
            out[3 * i] = pal[4 * index]
            out[3 * i + 1] = pal[4 * index + 1]
            out[3 * i + 2] = pal[4 * index + 2]
    elif image.mode == "RGB":
        out[:] = px
    elif image.mode == "RGBA":
        for i in range(w * h):
            out[3 * i:3 * i + 3] = px[4 * i:4 * i + 3]
    elif image.mode in ("L", "LA"):
        step = 1 if image.mode == "L" else 2
        for i in range(w * h):
            v = px[step * i]
            out[3 * i] = out[3 * i + 1] = out[3 * i + 2] = v
    else:  # pragma: no cover - png.decode drops other modes
        raise ValueError(f"unsupported PNG mode {image.mode!r}")
    return w, h, bytes(out)


def frame_ncc(frame: tuple[int, int, bytes], panel: Panel, palette: bytes,
              top: int = PANEL_TOP, screen_height: int = 200,
              step: int = 3) -> dict:
    """Luma NCC of the panel against a captured frame's bottom band.

    The capture is a capture of the whole ``screen_height``-row screen scaled
    up (``win*.png`` is 1920x1080 for a 320x200 screen), so the band starts at
    ``top * frame_height / screen_height``.  ``ncc`` close to 1 means the panel
    is what the frame draws there; the measured value for the shipped frames
    is ~0.01, i.e. a cockpit band is *not* this bitmap (docs/HUD_AUDIT.md).
    """
    fw, fh, px = frame
    if screen_height <= top:
        raise ValueError("screen_height must exceed the panel top row")
    y0 = int(round(top * fh / screen_height))
    band_rows = max(1, fh - y0)
    rows = list(range(y0, fh, step))
    cols = list(range(0, fw, step))
    ours, theirs = [], []
    for y in rows:
        py = min(panel.height - 1, (y - y0) * panel.height // band_rows)
        base = y * fw
        for x in cols:
            index = panel.indices[py * panel.width
                                  + min(panel.width - 1, x * panel.width // fw)]
            o = (base + x) * 3
            theirs.append(0.299 * px[o] + 0.587 * px[o + 1] + 0.114 * px[o + 2])
            po = index * 4
            ours.append(0.299 * palette[po] + 0.587 * palette[po + 1]
                        + 0.114 * palette[po + 2])
    n = len(ours)
    mo, mt = sum(ours) / n, sum(theirs) / n
    cov = sum((a - mo) * (b - mt) for a, b in zip(ours, theirs)) / n
    vo = sum((a - mo) ** 2 for a in ours) / n
    vt = sum((b - mt) ** 2 for b in theirs) / n
    ncc = cov / ((vo ** 0.5) * (vt ** 0.5)) if vo > 1e-9 and vt > 1e-9 else 0.0
    return {"ncc": round(ncc, 4), "rows": [y0, fh], "samples": n,
            "scale": [round(fw / panel.width, 3),
                      round(band_rows / panel.height, 3)]}


# ---------------------------------------------------------------------------
# self-test
# ---------------------------------------------------------------------------

def _rgb(width: int, height: int, value: int = 10) -> bytes:
    return bytes([value, value, value]) * (width * height)


def selftest() -> int:
    checks: list[tuple[str, bool, str]] = []

    def check(name: str, ok: bool, detail: str = "") -> None:
        checks.append((name, ok, detail))

    def expect(name: str, got, want) -> None:
        check(name, got == want, f"got {got!r}, want {want!r}")

    # palette: entry i is (i, 2i, 3i)
    pal = b"".join(struct.pack("BBBB", i, (2 * i) & 0xFF, (3 * i) & 0xFF, 0)
                   for i in range(256))
    w, h = 4, 3
    frame = _rgb(w, h, 77)

    # a 2x2 panel: transparent, opaque, opaque, transparent
    indices = bytes([0, 5, 9, 0])
    panel = Panel("T", 2, 2, indices, pal)
    out = compose(frame, w, h, panel, pal, top=1)
    expect("len unchanged", len(out), len(frame))
    expect("row above untouched", out[0:4 * 3], frame[0:4 * 3])
    px = lambda x, y: tuple(out[(y * w + x) * 3:(y * w + x) * 3 + 3])
    px0 = lambda buf, x, y: tuple(buf[(y * w + x) * 3:(y * w + x) * 3 + 3])
    expect("index 0 is transparent", px(0, 1), (77, 77, 77))
    expect("index 5 -> palette[5]", px(1, 1), (5, 10, 15))
    expect("index 9 -> palette[9]", px(0, 2), (9, 18, 27))
    expect("last panel pixel transparent", px(1, 2), (77, 77, 77))

    # engine row order: rows reversed, columns kept
    flipped = engine_order(panel)
    expect("engine order reverses rows", flipped.indices, bytes([9, 0, 0, 5]))
    expect("engine order keeps width", (flipped.width, flipped.height), (2, 2))

    # fade LUT: identity changes nothing, a zero table maps every index to 0
    ident = bytes(range(256))
    out_lut = compose(frame, w, h, panel, pal, top=1, lut=ident)
    expect("identity lut == no lut", out_lut, out)
    zero = bytes(256)
    out_zero = compose(frame, w, h, panel, pal, top=1, lut=zero)
    expect("zero lut maps to palette[0]", px0(out_zero, 1, 1), (0, 0, 0))
    try:
        compose(frame, w, h, panel, pal, top=1, lut=b"\0" * 8)
        check("short lut rejected", False)
    except ValueError:
        check("short lut rejected", True)

    # bottom alignment and clipping
    expect("default_top bottom-aligns", default_top(panel, 200), 198)
    expect("default_top clamps", default_top(Panel("T", 2, 300, indices, pal),
                                             200), 0)
    wide = Panel("W", 8, 1, bytes([7] * 8), pal)
    out = compose(_rgb(4, 1, 0), 4, 1, wide, pal, top=0)
    expect("width clipped", out, bytes([7, 14, 21]) * 4)

    # a panel taller than the frame must not grow the buffer
    tall = Panel("H", 1, 4, bytes([3, 3, 3, 3]), pal)
    expect("rows clipped", len(compose(_rgb(1, 2, 0), 1, 2, tall, pal, 0)), 6)

    # error handling
    try:
        compose(b"\0" * 3, 4, 4, panel, pal)
        check("bad frame size rejected", False)
    except ValueError:
        check("bad frame size rejected", True)
    try:
        compose(frame, w, h, panel, b"\0" * 16)
        check("bad palette rejected", False)
    except ValueError:
        check("bad palette rejected", True)

    # text drawing: glyph rows go down from the cell top, index 0 is clear
    class _Stub:
        def __init__(self, glyphs):
            self._g = glyphs

        def glyph(self, ch):
            return self._g.get(ch)

    stub = _Stub({"A": Glyph(3, 2, bytes([1, 2, 0, 3, 4, 0]), 3)})
    canvas = bytearray(8 * 5)
    end = draw_text(canvas, stub, 1, 1, "A", width=8, height=5)
    expect("glyph advance returned", end, 4)
    expect("glyph pixel (1,1)", canvas[1 * 8 + 1], 1)
    expect("glyph pixel (2,1)", canvas[1 * 8 + 2], 2)
    expect("transparent glyph index stays clear", canvas[2 * 8 + 1], 0)
    expect("glyph pixel (2,2)", canvas[2 * 8 + 2], 3)
    expect("glyph pixel (1,3)", canvas[3 * 8 + 1], 4)
    expect("glyph drawn top-down", canvas[3 * 8 + 2], 0)
    mapped = bytearray(8 * 5)
    draw_text(mapped, stub, 0, 0, "A", width=8, height=5,
              value=bytes([7] * 256))
    expect("value map applied", mapped[0], 7)

    # layout constants (guard against silent drift)
    items = {f"{item[0]}:{item[3]}": (item[1], item[2])
             for item in hud_text({"ammo": 1985, "damage": 0})}
    expect("KILL label at (52,178)", items.get("6:KILL"), (52, 178))
    expect("ammo right-aligned", items.get("10:1985"), (48, 163))
    expect("kill digit at 72", items.get("11:0"), (72, 177))
    expect("damage digit at 156", items.get("9:0"), (156, 181))
    expect("equipment digit at 246", items.get("10:0"), (246, 173))
    icons = hud_icons({"weapon_icon": "WICON1", "equipment_icon": "EQUIP12"})
    expect("weapon icon slot", icons[0], ("WICON1", 23, 162))
    expect("equipment icon slot", icons[1], ("EQUIP12", 201, 162))

    # charge bars: colour by length, length clamped to 17
    expect("bar colour below 6", gauge_colour(5), 0xF3)
    expect("bar colour at 6", gauge_colour(6), 0x2B)
    expect("bar colour at 11", gauge_colour(11), 0x2B)
    expect("bar colour at 12", gauge_colour(12), 0xFF)
    expect("bar clamps to 17", hud_gauges({"bar_top": 99})[0][4], 17)
    expect("zero bar is dropped", len(hud_gauges({"bar_top": 0})), 3)
    bars = hud_gauges({"bar_top": 2, "bar_bottom": 0, "bar_left": 0,
                       "bar_right": 0})
    expect("one bar only", bars, [(148, 151, 1, 0, 2, 0xF3)])
    out_bar = compose(_rgb(320, 200, 0), 320, 200, Panel("T", 1, 1, b"\0", pal),
                      pal, top=130, gauges=bars)
    expect("bar drawn left to right", [out_bar[(151 * 320 + x) * 3]
                                       for x in (148, 149)],
           [0xF3, 0xF3])
    expect("bar stops after its length", out_bar[(151 * 320 + 150) * 3], 0)

    # throttle/belt bars: four and two rows, throttle capped at 0x35
    expect("throttle rows", hud_bars({"throttle_px": 53, "belt_px": 0}),
           [(202, 0xA3 - 8, 4, 53)])
    expect("throttle capped", hud_bars({"throttle_px": 99})[0][3], 0x35)
    expect("belt rows", hud_bars({"throttle_px": 0, "belt_px": 7}),
           [(50, 0xB6 - 8, 2, 7)])
    expect("dump default is a 26 px throttle bar",
           hud_bars({}), [(202, 0xA3 - 8, 4, 26)])

    # installed game, if present
    game = os.environ.get("GUNMETAL_GAME", "/mnt/d/games/Gunmetal 3dfx")
    if os.path.isdir(os.path.join(game, "FRONTEND")):
        try:
            real = load_panel(game)
            check("PANEL is 320x78", (real.width, real.height) == (320, 78),
                  f"{real.width}x{real.height}")
            check("PANEL has art", len(set(real.indices)) > 16,
                  f"{len(set(real.indices))} indices")
            check("PANEL palette is 1024", len(real.palette) == 1024)
            for number in (6, 9, 10, 11):
                try:
                    font = load_font(game, number)
                    check(f"font {number} loads", len(font.by_code) > 0,
                          f"{len(font.by_code)} glyphs")
                except (KeyError, FileNotFoundError, ValueError) as exc:
                    check(f"font {number} loads", False, str(exc))
            label = load_font(game, 6)
            check("label font advance is 7 at 'M'",
                  label.glyph("M").advance, 7)
            check("'MACHINE GUN' width", label.measure("MACHINE GUN"), 77)
            digits = load_font(game, 10)
            check("digit advance is 9", digits.glyph("1").advance, 9)
            icon = load_icon(game, "WICON1")
            check("WICON1 is 24x24", (icon.width, icon.height) == (24, 24))
            sil = load_damage_icon(game, 0)
            check("damage icon is 20x19", (sil.width, sil.height) == (20, 19))
            sheet = load_panel(game, "RPVICONS")
            check("damage icon bottom row is source row 23",
                  sil.indices[18 * 20:19 * 20],
                  sheet.indices[23 * 60:23 * 60 + 20])
            check("damage icon top row is source row 5",
                  sil.indices[0:20], sheet.indices[5 * 60:5 * 60 + 20])
            check("damage icon keeps the key", sil.indices.count(0x73) > 0)
            equip0 = load_icon(game, "EQUIP0")
            equip12 = load_icon(game, "EQUIP12")
            flipped = b"".join(equip0.indices[r * 24:(r + 1) * 24]
                               for r in range(23, -1, -1))
            expect("EQUIP12 is EQUIP0 flipped", equip12.indices, flipped)
        except (FileNotFoundError, KeyError, ValueError) as exc:
            check("PANEL loads", False, str(exc))
    else:
        print(f"skip: no game under {game!r} (GUNMETAL_GAME)")

    failed = [c for c in checks if not c[1]]
    for name, ok, detail in checks:
        print(f"{'ok  ' if ok else 'FAIL'} {name}" + (f"  {detail}" if detail and not ok else ""))
    print(f"{len(checks) - len(failed)}/{len(checks)} checks passed")
    return 1 if failed else 0


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--self-test", action="store_true")
    ap.add_argument("--game", default=os.environ.get(
        "GUNMETAL_GAME", "/mnt/d/games/Gunmetal 3dfx"))
    ap.add_argument("--panel", default="PANEL")
    ap.add_argument("--preview", help="write the panel as PNG here")
    ap.add_argument("--check-frame", metavar="PNG",
                    help="luma NCC of the panel against a captured frame")
    ap.add_argument("--top", type=int, default=PANEL_TOP,
                    help=f"panel top row in the 320x200 screen "
                         f"(default {PANEL_TOP})")
    ap.add_argument("--lut", metavar="FILE",
                    help="engine fade LUT dump (gm_lut.bin, 16x256 bytes)")
    ap.add_argument("--level", type=int, default=10,
                    help="fade level 0..15 used with --lut (default 10)")
    ap.add_argument("--check-screen", metavar="FILE",
                    help="raw 8bpp screen dump (gm_screen.bin) for an "
                         "index-match check")
    ap.add_argument("--engine-order", action="store_true",
                    help="flip the panel rows into the engine's order "
                         "(see engine_order()); needed for --check-screen")
    ap.add_argument("--panel-buffer", metavar="FILE",
                    help="dumped engine panel buffer (gm_panel.bin, 320x78 "
                         "indices) instead of the raw FRONTEND asset")
    ap.add_argument("--scale", type=float, default=3.0,
                    help="vertical scale of the band in --check-screen "
                         "(default 3.0 for 800x600)")
    ap.add_argument("--text", action="store_true",
                    help="draw the HUD text layer (labels + digits) and icons")
    ap.add_argument("--no-icons", action="store_true",
                    help="with --text: skip the 24x24 icons")
    ap.add_argument("--state", default="", metavar="JSON",
                    help="HUD state overrides for --text, e.g. "
                         '{"ammo": 1999}')
    opts = ap.parse_args(argv)
    if opts.self_test:
        return selftest()
    if not opts.preview and not opts.check_frame and not opts.check_screen:
        ap.error("nothing to do: pass --self-test, --preview, --check-frame "
                 "or --check-screen")
    from . import png

    panel = load_panel(opts.game, opts.panel)
    if opts.panel_buffer:
        data = open(opts.panel_buffer, "rb").read()
        if len(data) % 320:
            raise SystemExit(f"{opts.panel_buffer}: size {len(data)} is not a "
                             "320-wide buffer")
        panel = Panel("dumped", 320, len(data) // 320, data, panel.palette)
    if opts.preview:
        data = panel_rgb(panel, panel.palette)
        with open(opts.preview, "wb") as fh:
            fh.write(png.rgb(panel.width, panel.height, data))
        print(f"wrote {opts.preview} ({panel.width}x{panel.height})")
    if opts.engine_order:
        panel = engine_order(panel)
    lut = load_lut(opts.lut, opts.level) if opts.lut else None
    if opts.check_frame:
        frame = load_rgb(opts.check_frame)
        engine = palette_bytes(opts.game)
        for tag, pal in (("GAMEPAL", engine), ("bmp", panel.palette)):
            res = frame_ncc(frame, panel, pal, top=opts.top)
            print(f"{opts.check_frame}: {tag} ncc={res['ncc']:+.3f} "
                  f"rows={res['rows'][0]}..{res['rows'][1]} "
                  f"samples={res['samples']} scale={res['scale']}")
    fonts = {}
    icon_pixels = {}
    text = ()
    icons = ()
    gauges = ()
    bars = ()
    damage_icon = None
    if opts.text:
        import json
        for number in sorted({item[0] for item in hud_text()}):
            try:
                fonts[number] = load_font(opts.game, number)
            except (KeyError, FileNotFoundError) as exc:
                raise SystemExit(f"--text: {exc}")
        state = json.loads(opts.state) if opts.state else {}
        text = tuple(hud_text(state))
        print("text   " + ", ".join(f"font {n} at ({x},{y}) {t!r}"
                                    for n, x, y, t in text))
        if not opts.no_icons:
            gauges = tuple(hud_gauges(state))
            bars = tuple(hud_bars(state))
            damage_icon = load_damage_icon(
                opts.game, int(state.get("damage_stage", HUD_DAMAGE_STAGE)))
            icons = tuple(hud_icons(state))
            for name, ix, iy in icons:
                try:
                    icon_pixels[name] = load_icon(opts.game, name)
                except (KeyError, ValueError, FileNotFoundError) as exc:
                    raise SystemExit(f"--text: {exc}")
            print("icons  " + ", ".join(f"{n} at ({x},{y})"
                                        for n, x, y in icons))
    if opts.preview and text:
        data = compose(bytes(320 * 200 * 3), 320, 200, panel,
                       palette_bytes(opts.game), top=opts.top, lut=lut,
                       text=text, fonts=fonts, icons=icons,
                       icon_pixels=icon_pixels, gauges=gauges, bars=bars,
                       damage_icon=damage_icon)
        with open(opts.preview.rsplit(".", 1)[0] + "_text.png", "wb") as fh:
            fh.write(png.rgb(320, 200, data))
        print(f"wrote {opts.preview.rsplit('.', 1)[0]}_text.png")
    if opts.check_screen:
        data = open(opts.check_screen, "rb").read()
        w, h = 800, len(data) // 800
        if len(data) != w * h:
            raise SystemExit(f"{opts.check_screen}: not a 800x600 8bpp dump")
        if lut is None:
            raise SystemExit("--check-screen needs --lut")
        res = screen_index_match(data, w, h, panel, lut, opts.top, opts.scale,
                                 text=text, fonts=fonts, icons=icons,
                                 icon_pixels=icon_pixels, gauges=gauges,
                                 bars=bars, damage_icon=damage_icon)
        print(f"{opts.check_screen}: index match {100*res['match']:.1f}% "
              f"({res['pixels']} px, top={res['top']}, scale={res['scale']})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
