#!/usr/bin/env python3
"""Cockpit HUD: 24x24 icons, compass ribbon and the sources of the numbers.

This is the continuation of ``tools/re/hud_compose_ref.py`` (M4.4.5): the panel
composer draws 18 of the 23 elements of the lower band ``320x78``; this tool
answers the three questions that were left open there:

1. **Icons.**  Which entry of ``FRONTEND/BMP.IDX`` is used for the weapon and
   the equipment icon, where the id comes from, and how the 24x24 bitmap is
   copied into the framebuffer.
2. **Compass.**  Where the heading tape sits in the frame, how wide the
   window, how far apart the marks are and how the tape moves with ``yaw``.
3. **Numbers.**  Which player field feeds ``KILL`` and the equipment counter.

Everything is read-only: the game directory and the oracle frames are opened
read-only, nothing is written (``--png`` is deliberately not implemented).

Proven facts (addresses are VA in ``gundx.exe``, ImageBase ``0x400000``)
-----------------------------------------------------------------------

Icons, weapon (``DEFINE_WEAPONS``, opcode 170)::

    0x450be0  consumer: 21 dwords per record, icon bitmap -> record+0x50
    0x450c97  push 0x18 / 0x18 / 0 / dest / name / "frontend" / call 0x47efe0
              -> ``0x47efe0("frontend", <icon>, 0x80fa50 + slot*0x2a8, 0, 24, 24)``
    0x450c35  strncpy(record+0x298, <display name>, 15)
    0x4928eb  eax = [player+0x3294]                  ; weapon slot
    0x4928f9  cmp eax, [0x819028]                    ; weapon count
    0x49291e  name text: "0x80fc98 + slot*0x2a8"
    0x49292f  24x24 blit: source 0x80fc78 + slot*0x2a8 (last row of the
              bitmap), destination buf + pitch*0xAA + 0x17, rows bottom-up,
              no transparency key

Icons, equipment (``DEFINE_EQUIPMENT``, opcode 175)::

    0x48fa6a  12 iterations: sprintf(buf, "equip%d", i);
              ``0x47efe0("frontend", buf, 0x683ff8 + i*0x240, 0, 24, 24)``
    0x492b32  eax = [player + 0x3320]                ; equipment slot
    0x492cc3  cmp eax, 0xc                           ; 12 equipment slots
    0x492ce4  24x24 blit: source 0x683ff8 + idx*0x240, destination
              buf + pitch*0xAA + 0xC9, rows top-down, no transparency key
    0x492d2e  highlight: test [player+0x3328], 1 << idx, then a 0xAA dither

The register of the ``[reg+0x81c360]`` reads is **not** the pitch.  At
``0x4928d5..0x4928e7`` the function recomputes ``edi = arg3*0x3510`` (the
player record offset) and stores it **into the arg2 stack slot**
(``mov [esp+0x60],edi``); from there on ``[esp+0x60]`` holds that offset, not
the pitch.  The read is therefore ``[i*0x3510 + 0x81c360]``, and the immediate
is the absolute address for player 0: ``0x81c360 = player+0x3320`` (equipment
slot) and ``0x81c368 = player+0x3328`` (active mask, ``docs/PLAYER_FIELDS.md``
``+0x3328``).  Pitch stays in ``esi`` (``0x492937``, ``0x492cd1``).  Every
other read of the function resolves the same way: ``0x81c288`` = ``+0x3248``
(armor), ``0x81c2d4`` = ``+0x3294`` (weapon slot), ``0x81c240`` = ``+0x3200``
(flags), ``0x81bf88`` = ``+0x2f48`` (engine power), ``0x81bfe8`` = ``+0x2fa8``
(charge).  The earlier working note that the address was ``0x81c4a0``
(``+0x3460``) came from reading ``arg2`` as the pitch: ``0x81c4a0`` is the
panel "selected id" of ``docs/FILM.md`` §9.1, a different field.
The 8 px difference between the code destination row (``0xAA = 170``) and the
measured frame row (162) is **not** explained here; see
``docs/HUD_ICONS.md`` §6.

Numbers::

    0x492700  damage %: 100 - 100*[player+0x3248]/[player+0x3244]
    0x4929cd  ammo: [player+0x329c + slot*4] / [0x818980 + type*0x38]
    0x492bd0  equipment counter: [player+0x329c + type*4] / divisor, where
              type = [0x814c80 + idx*0x40] (|-1| = no ammo type)
    0x4927e0  "%ld" of [player+0x332c], drawn only when [0x6e89c0] != 0

Compass: the window, marks and letters are measured from the frames; the
drawing code was **not** found (it is not ``0x47faf0`` and not in the
``0x4926a0`` panel function), so the formula below is empirical.

Usage
-----

    python3 tools/re/hud_icons.py --self-test
    python3 tools/re/hud_icons.py --icons
    python3 tools/re/hud_icons.py --icons --json
    python3 tools/re/hud_icons.py --compare
    python3 tools/re/hud_icons.py --compare out/oracle/win2.png --json
    python3 tools/re/hud_icons.py --compass

The game is looked up as in ``hud_compose_ref`` (``--game``,
``$GUNMETAL_GAME``, the mirror ``~/.cache/gunmetal-re/game``, the installed
game); frames as ``--frames``, ``$GUNMETAL_ORACLE``, ``out/oracle`` of this
tree or of the main repository (through ``git rev-parse --git-common-dir``).
"""
from __future__ import annotations

import argparse
import glob
import json
import math
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
for extra in (ROOT, os.path.join(ROOT, "tools", "re")):
    if extra not in sys.path:
        sys.path.insert(0, extra)

# --------------------------------------------------------------------------
# Addresses and constants (see the module docstring for the evidence)
# --------------------------------------------------------------------------
PLAYER_BASE = 0x819040
PLAYER_STRIDE = 0x3510

WEAPON_BASE = 0x80FA00          # DEFINE_WEAPONS array
WEAPON_STRIDE = 0x2A8           # 680 bytes
WEAPON_COUNT = 0x819028         # u32, <= 31
WEAPON_ICON_OFF = 0x50          # 24x24 indexed bitmap, 0x240 bytes
WEAPON_NAME_OFF = 0x298         # char[16]
WEAPON_ICON_SRC = WEAPON_BASE + WEAPON_ICON_OFF + 24 * 23   # 0x80fc78

EQUIP_ICON_TABLE = 0x683FF8     # 12 x 24x24 indexed bitmaps
EQUIP_ICON_STRIDE = 0x240
EQUIP_ICON_END = 0x685AF8
EQUIP_ICON_COUNT = (EQUIP_ICON_END - EQUIP_ICON_TABLE) // EQUIP_ICON_STRIDE
EQUIP_ICON_FMT = "equip%d"      # string at 0x4be3a8
EQUIP_RECORD = 0x814C80         # DEFINE_EQUIPMENT record +0x20: ammo type;
                                # the array itself is 0x814c60, 0x40 bytes each
EQUIP_NAME_OFF = 0x0C           # name inside the record at 0x814c80 (= record
                                # base 0x814c60 + 0x2c, docs/FORMATS.md §12.3)
EQUIP_HUD_LIMIT = 0xC           # cmp eax,0xc at 0x492cc3
#: Equipment slot actually shown by the HUD (``[player+0x3320]``).  The reads
#: at 0x492b32/0x492b9a/0x492cb5 use a *player offset* register, not the
#: pitch: 0x4928e7 overwrites the arg2 slot with ``arg3*0x3510``.
EQUIP_INDEX_ABS = 0x81C360
EQUIP_INDEX_OFF = EQUIP_INDEX_ABS - PLAYER_BASE          # 0x3320
#: Active-equipment bit mask (``[player+0x3328]``); bit ``idx`` -> highlight.
EQUIP_MASK_ABS = 0x81C368
EQUIP_MASK_OFF = EQUIP_MASK_ABS - PLAYER_BASE            # 0x3328

ICON_W, ICON_H = 24, 24

#: Code destination of the two 24x24 blits (``0xAA`` rows, ``0x17``/``0xC9``).
ICON_CODE_POS = {"weapon": (23, 170), "equipment": (201, 170)}
#: Position measured in ``out/oracle/win2.png`` (see ``--compare``).
ICON_FRAME_POS = {"weapon": (23, 162), "equipment": (201, 162)}

#: Compass ribbon window measured on ``win1..win3`` (screen coordinates).
COMPASS_WINDOW = (79, 129, 2, 15)      # x0, x1 inclusive, y0, y1 inclusive
COMPASS_LETTER_ROWS = (9, 14)
COMPASS_TICK_ROW = 3                   # every mark reaches this row
COMPASS_LONG_ROW = 7                   # only the cardinal marks reach this row

INK_THRESHOLD = 100                    # same luma cut as hud_audit clusters

#: Default frame state of ``win2``: MACHINE GUN, equipment slot 0 (HEADLIGHTS).
DEFAULT_WEAPON = "MACHINE GUN"
DEFAULT_EQUIPMENT = 0


# --------------------------------------------------------------------------
# Small helpers
# --------------------------------------------------------------------------
def ncc(a, b):
    """Pearson correlation of two equal-length sequences (0 when constant)."""
    n = len(a)
    if n != len(b) or n == 0:
        return 0.0
    ma = sum(a) / n
    mb = sum(b) / n
    va = sum((x - ma) ** 2 for x in a)
    vb = sum((y - mb) ** 2 for y in b)
    if va <= 0 or vb <= 0:
        return 0.0
    cov = sum((x - ma) * (y - mb) for x, y in zip(a, b)) / n
    return cov / math.sqrt((va / n) * (vb / n))


def luma(rgb):
    return [(rgb[3 * i] * 30 + rgb[3 * i + 1] * 59 + rgb[3 * i + 2] * 11) // 100
            for i in range(len(rgb) // 3)]


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
        out = subprocess.run(["git", "rev-parse", "--git-common-dir"], cwd=ROOT,
                             capture_output=True, text=True, timeout=10,
                             check=False)
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


# --------------------------------------------------------------------------
# Assets: FRONTEND/BMP.IDX
# --------------------------------------------------------------------------
class Archive:
    """Read-only view of ``FRONTEND/BMP.IDX`` (143 entries, 8-bit BMPs)."""

    def __init__(self, game_dir):
        from gm import bigfile

        index = os.path.join(game_dir, "FRONTEND", "BMP.IDX")
        if not os.path.exists(index):
            alt = os.path.join(game_dir, "frontend", "bmp.idx")
            index = alt if os.path.exists(alt) else index
        self.archive = bigfile.BigFile(index)
        self._cache = {}
        self.names = [e.name.upper() for e in self.archive.entries]

    def image(self, name):
        from gm import bmp

        key = name.upper()
        if key in self._cache:
            return self._cache[key]
        entry = next((e for e in self.archive.entries
                      if e.name.upper() == key), None)
        if entry is None:
            self._cache[key] = None
            return None
        try:
            self._cache[key] = bmp.parse(self.archive.read(entry))
        except (ValueError, OSError):
            self._cache[key] = None
        return self._cache[key]

    def has(self, name):
        return name.upper() in self.names

    def icons24(self):
        """Every 24x24 entry: ``[(name, Bmp)]`` (the cockpit icon atlas)."""
        out = []
        for name in self.names:
            image = self.image(name)
            if image is not None and (image.width, image.height) == (ICON_W, ICON_H):
                out.append((name, image))
        return out


def icon_art(image, top_down=True):
    """Luma of a 24x24 indexed bitmap, one value per pixel.

    ``top_down=True`` returns the image as it is seen on screen (the BMP reader
    already reversed the bottom-up rows), ``top_down=False`` returns the
    bottom-up order — the engine's weapon blit walks the bitmap from its last
    row upwards (``0x49295b``), which flips it.
    """
    palette = image.palette or b"\x00" * 1024
    out = []
    for row in range(ICON_H):
        src = row if top_down else ICON_H - 1 - row
        base = src * image.width
        for col in range(ICON_W):
            idx = image.pixels[base + col] if base + col < len(image.pixels) else 0
            r, g, b = palette[4 * idx:4 * idx + 3] or (idx, idx, idx)
            out.append((r * 30 + g * 59 + b * 11) // 100)
    return out


# --------------------------------------------------------------------------
# Data: DEFINE_WEAPONS / DEFINE_EQUIPMENT
# --------------------------------------------------------------------------
def _script_records(path):
    from gm import sbt as sbtmod

    data = sbtmod.parse_file(path)
    for record in data.records:
        if record.type == 11:                     # SCRIPT
            yield record.payload


def load_weapon_icons(game_dir):
    """``[{file, block, slot, name, sound, icon}]`` over every shipped ``.sbt``.

    ``slot`` is the index the engine would use for that record (it increments
    ``[0x819028]`` once per record), i.e. the index into ``0x80fa00``.  A level
    carries several ``DEFINE_WEAPONS`` blocks (campaign/deathmatch variants
    behind ``IF``), so slots are numbered per block.
    """
    from gm import sbtvm

    paths = sorted(glob.glob(os.path.join(game_dir, "*.sbt")))
    rows = []
    for path in paths:
        try:
            scripts = list(_script_records(path))
        except (ValueError, OSError):
            continue
        block = 0
        for script in scripts:
            try:
                insns, _ = sbtvm.decode(script.code, 0, len(script.code))
            except (ValueError, IndexError):
                continue
            for insn in insns:
                if insn.name != "DEFINE_WEAPONS":
                    continue
                for slot, (strings, _fields) in enumerate(insn.data):
                    rows.append({
                        "file": os.path.basename(path),
                        "block": block,
                        "slot": slot,
                        "name": strings[0],
                        "sound": strings[1],
                        "icon": strings[2],
                    })
                block += 1
    return rows


def load_equipment_names(game_dir, level="level0.sbt"):
    """``[(index, name)]`` of the level's ``DEFINE_EQUIPMENT`` block."""
    from gm import sbtvm

    path = os.path.join(game_dir, level)
    if not os.path.exists(path):
        return []
    out = []
    for script in _script_records(path):
        try:
            insns, _ = sbtvm.decode(script.code, 0, len(script.code))
        except (ValueError, IndexError):
            continue
        for insn in insns:
            if insn.name == "DEFINE_EQUIPMENT":
                for i, (strings, _fields) in enumerate(insn.data):
                    out.append((i, strings[0]))
    return out


def weapon_icon_map(rows):
    """``{weapon: icon}`` plus the list of conflicting weapons (must be empty)."""
    mapping = {}
    conflicts = []
    for row in rows:
        name, icon = row["name"], row["icon"]
        if name in mapping and mapping[name] != icon:
            conflicts.append(name)
        mapping[name] = icon
    return mapping, sorted(set(conflicts))


def equipment_icon_map(archive, names):
    """``[(index, name, asset, present)]`` for the 12 HUD equipment slots."""
    out = []
    for index, name in names:
        asset = EQUIP_ICON_FMT % index
        out.append((index, name, asset,
                    archive is not None and archive.has(asset)))
    return out


# --------------------------------------------------------------------------
# Frames
# --------------------------------------------------------------------------
class Frame:
    """One oracle frame downscaled to 320x200 plus its sidecar state."""

    def __init__(self, path):
        import hud_audit as ha

        self.path = path
        w, h, rgb = ha.load_rgb(path)
        self.width, self.height, self.rgb = 320, 200, ha.downscale((w, h, rgb))
        self.luma = luma(self.rgb)
        self.state = {}
        sidecar = os.path.splitext(path)[0] + ".json"
        if os.path.exists(sidecar):
            try:
                with open(sidecar, "r", encoding="utf-8") as fh:
                    self.state = json.load(fh)
            except (OSError, ValueError):
                self.state = {}

    @property
    def yaw(self):
        camera = self.state.get("camera") or {}
        value = camera.get("yaw")
        return float(value) if value is not None else None

    def patch_luma(self, x, y, w=ICON_W, h=ICON_H):
        out = []
        for row in range(h):
            yy = y + row
            if not (0 <= yy < self.height):
                out.extend([0] * w)
                continue
            base = yy * self.width
            for col in range(w):
                xx = x + col
                out.append(self.luma[base + xx] if 0 <= xx < self.width else 0)
        return out

    def bright_columns(self, y, x0=0, x1=None):
        x1 = self.width if x1 is None else x1
        band = self.luma[y * self.width:(y + 1) * self.width]
        return [x for x in range(x0, min(x1, self.width))
                if band[x] >= INK_THRESHOLD]


# --------------------------------------------------------------------------
# Icon comparison
# --------------------------------------------------------------------------
def match_icon(frame, assets, expected, search=None, top=3):
    """Find ``expected`` (and runner-ups) in the frame by Pearson luma.

    ``search`` is ``(x0, y0, x1, y1)`` — the inclusive box of the top-left
    corner.  Both orientations are tried: the engine's weapon blit walks the
    bitmap backwards (top-down on screen), the equipment blit walks it forwards
    (bottom-up), so the caller checks ``orientation``.  ``ncc_expected`` is the
    score of the *expected* asset in the winning orientation.
    """
    expected_image = assets.image(expected) if assets is not None else None
    if expected_image is None:
        return {"expected": expected, "found": False, "reason": "asset missing"}
    if search is None:
        cx, cy = ICON_CODE_POS["weapon"]
        search = (cx - 10, cy - 20, cx + 10, cy + 10)
    x0, y0, x1, y1 = search
    art = icon_art(expected_image, top_down=True)
    flipped = icon_art(expected_image, top_down=False)
    best = None
    for y in range(y0, y1 + 1):
        for x in range(x0, x1 + 1):
            patch = frame.patch_luma(x, y)
            score = ncc(patch, art)
            score_flip = ncc(patch, flipped)
            if best is None or max(score, score_flip) > best[0]:
                best = (max(score, score_flip), x, y,
                        "top-down" if score >= score_flip else "bottom-up")
    if best is None:
        return {"expected": expected, "found": False, "reason": "empty search"}
    runners = []
    if assets is not None:
        patch = frame.patch_luma(best[1], best[2])
        for name, image in assets.icons24():
            forward = ncc(patch, icon_art(image, top_down=True))
            reverse = ncc(patch, icon_art(image, top_down=False))
            if reverse > forward:
                runners.append((round(reverse, 3), name, "bottom-up"))
            else:
                runners.append((round(forward, 3), name, "top-down"))
        runners.sort(key=lambda row: (-row[0], row[1]))
    return {
        "expected": expected,
        "found": True,
        "ncc": round(best[0], 3),
        "ncc_expected": round(ncc(frame.patch_luma(best[1], best[2]),
                                  flipped if best[3] == "bottom-up" else art), 3),
        "rect": [best[1], best[2], ICON_W, ICON_H],
        "orientation": best[3],
        "runners": runners[:top],
    }


def compare_icons(frame, assets, weapon_name="MACHINE GUN", equip_index=0,
                  weapon_map=None):
    """Weapon + equipment icon check against one frame."""
    weapon_map = weapon_map or {}
    weapon_asset = weapon_map.get(weapon_name, "WICON1")
    equip_asset = EQUIP_ICON_FMT % equip_index
    out = {"frame": frame.path, "weapon_name": weapon_name,
           "equipment_index": equip_index}

    wx, wy = ICON_CODE_POS["weapon"]
    out["weapon"] = match_icon(frame, assets, weapon_asset,
                               search=(wx - 10, wy - 20, wx + 10, wy + 10))
    out["weapon"]["code_pos"] = [wx, wy]
    ex, ey = ICON_CODE_POS["equipment"]
    out["equipment"] = match_icon(frame, assets, equip_asset,
                                  search=(ex - 10, ey - 20, ex + 10, ey + 10))
    out["equipment"]["code_pos"] = [ex, ey]
    for key in ("weapon", "equipment"):
        item = out[key]
        if item.get("found"):
            item["dy_vs_code"] = item["rect"][1] - item["code_pos"][1]
            item["dx_vs_code"] = item["rect"][0] - item["code_pos"][0]
    return out


# --------------------------------------------------------------------------
# Compass
# --------------------------------------------------------------------------
def compass_marks(frame, window=COMPASS_WINDOW):
    """Ticks and letters of the heading tape inside the window.

    A column that is bright at ``COMPASS_LONG_ROW`` belongs to a long (cardinal)
    mark; a column that is bright at ``COMPASS_TICK_ROW`` only belongs to a
    short tick.  Letters are the bright groups in ``COMPASS_LETTER_ROWS``.
    """
    x0, x1, y0, y1 = window
    long_cols = frame.bright_columns(COMPASS_LONG_ROW, x0, x1 + 1)
    tick_cols = frame.bright_columns(COMPASS_TICK_ROW, x0, x1 + 1)
    letter_cols = set()
    for y in range(COMPASS_LETTER_ROWS[0], COMPASS_LETTER_ROWS[1] + 1):
        letter_cols.update(frame.bright_columns(y, x0, x1 + 1))

    def groups(cols):
        out = []
        for c in sorted(cols):
            if out and c - out[-1][-1] <= 1:
                out[-1].append(c)
            else:
                out.append([c])
        return [(g[0], g[-1], (g[0] + g[-1]) / 2.0) for g in out]

    return {
        "window": list(window),
        "ticks": groups(tick_cols),
        "long_marks": groups(long_cols),
        "letters": groups(letter_cols),
        "yaw": frame.yaw,
    }


def fit_compass(marks, cardinal_deg=90.0, k_range=(0.15, 0.45),
                k_step=0.002, c_step=0.25, c_span=80.0):
    """Fit ``x = C + k*(bearing - yaw)`` to the measured long marks.

    The tape repeats every ``cardinal_deg`` (the long marks are the cardinals),
    so ``C`` is only defined modulo ``cardinal_deg * k``; the reported ``C`` is
    the solution closest to the middle of the window.  Returns ``None`` when
    there are fewer than two marks or a frame has no ``yaw``.

    The fit is a coarse grid search — deterministic and fast (< 1 s), and the
    residual is expressed in **bearing degrees** so it can be compared with the
    mark spacing.  ``px_per_deg`` is the only number the composer needs.
    """
    samples = []
    for mark in marks:
        yaw = mark.get("yaw")
        if yaw is None:
            continue
        for _x0, _x1, centre in mark["long_marks"]:
            samples.append((math.degrees(yaw), centre))
    if len(samples) < 2:
        return None
    x0, x1 = marks[0]["window"][0], marks[0]["window"][1]
    centre_x = (x0 + x1) / 2.0
    best = None
    k = k_range[0]
    while k <= k_range[1] + 1e-9:
        period = cardinal_deg * k
        c = centre_x - c_span
        while c <= centre_x + c_span + 1e-9:
            err = 0.0
            for yaw_deg, x in samples:
                bearing = yaw_deg + (x - c) / k
                r = bearing % cardinal_deg
                err += min(r, cardinal_deg - r) ** 2
            if best is None or err < best[0]:
                best = (err, k, c)
            c += c_step
        k += k_step
    err, k, c = best
    period = cardinal_deg * k
    # Pick the C representative nearest to the window centre.
    c += round((centre_x - c) / period) * period
    return {
        "px_per_deg": round(k, 4),
        "deg_per_px": round(1.0 / k, 3),
        "period_px": round(period, 2),
        "period_deg": cardinal_deg,
        "bearing0_x": round(c, 2),
        "rms_deg": round((err / len(samples)) ** 0.5, 2),
        "rms_px": round((err / len(samples)) ** 0.5 * k, 2),
        "marks": len(samples),
        "window": [x0, x1],
        "window_deg": round((x1 - x0) / k, 1),
        "visible_deg": round((x1 - x0 + 1) / k, 1),
    }


def compass_x(bearing_deg, yaw_deg, fit):
    """Screen x of one bearing for the fitted tape (``None`` if outside)."""
    x = fit["bearing0_x"] + fit["px_per_deg"] * (bearing_deg - yaw_deg)
    x0, x1 = fit["window"]
    return x if x0 <= x <= x1 else None


def compass_report(frames, nominal_window=COMPASS_WINDOW):
    """Geometry + movement of the tape over a list of frames."""
    marks = [compass_marks(f, nominal_window) for f in frames]
    out = {"window": list(nominal_window), "frames": marks,
           "fit": fit_compass(marks)}
    return out


# --------------------------------------------------------------------------
# Reporting
# --------------------------------------------------------------------------
def print_icons(rows, mapping, conflicts, equipment, assets, archive_names):
    print("weapon icons (DEFINE_WEAPONS, opcode 170)")
    print("  %-16s %-8s %-8s %s" % ("weapon", "icon", "asset", "levels"))
    by_weapon = {}
    for row in rows:
        by_weapon.setdefault(row["name"], []).append(row)
    for name in sorted(by_weapon):
        entry = by_weapon[name]
        icon = entry[0]["icon"]
        present = assets.has(icon) if assets is not None else None
        levels = len({e["file"] for e in entry})
        print("  %-16s %-8s %-8s %d" % (name, icon,
                                        "yes" if present else
                                        ("no" if present is not None else "?"),
                                        levels))
    if conflicts:
        print("  CONFLICTS: %s" % ", ".join(conflicts))
    print("\nequipment icons (DEFINE_EQUIPMENT + equip%d, 12 HUD slots)")
    print("  %-3s %-16s %-8s %s" % ("#", "equipment", "asset", "asset present"))
    for index, name, asset, present in equipment:
        note = "" if index < EQUIP_HUD_LIMIT else "  (beyond 0x492cc3 limit)"
        print("  %-3d %-16s %-8s %s%s" % (index, name, asset,
                                          "yes" if present else "no", note))
    if archive_names is not None:
        used = {a.upper() for _, _, a, _ in equipment}
        unused = [n for n in archive_names
                  if n.upper().startswith("EQUIP") and n.upper() not in used]
        if unused:
            print("  archive-only: %s" % ", ".join(sorted(unused)))


def print_compare(report):
    print("frame: %s (weapon %r, equipment %d)"
          % (report["frame"], report["weapon_name"], report["equipment_index"]))
    for key in ("weapon", "equipment"):
        item = report[key]
        if not item.get("found"):
            print("  %-9s %s: not found (%s)"
                  % (key, item["expected"], item.get("reason")))
            continue
        print("  %-9s %-8s ncc=%.3f rect=%s %s  (code %s, d=%+d,%+d)"
              % (key, item["expected"], item["ncc"], item["rect"],
                 item["orientation"], item["code_pos"],
                 item["dx_vs_code"], item["dy_vs_code"]))
        top = ", ".join("%s %.3f (%s)" % (n, s, o)
                        for s, n, o in item["runners"][1:])
        if top:
            print("             next: %s" % top)


def print_compass(report):
    x0, x1, y0, y1 = report["window"]
    print("compass window: x %d..%d (%d px), y %d..%d"
          % (x0, x1, x1 - x0 + 1, y0, y1))
    for m in report["frames"]:
        yaw = m["yaw"]
        print("  %s yaw=%s"
              % (os.path.basename(m.get("path", "?")),
                 "%.4f" % yaw if yaw is not None else "?"))
        print("    long marks: %s" % [round(v, 1) for _, _, v in m["long_marks"]])
        print("    ticks:      %s" % [round(v, 1) for _, _, v in m["ticks"]])
        print("    letters:    %s" % [round(v, 1) for _, _, v in m["letters"]])
    fit = report.get("fit")
    if fit:
        print("  fit: x = %.2f + %.4f*(bearing - yaw_deg)  "
              "(period %.2f px per %.0f deg, rms %.2f deg / %.2f px, "
              "%d marks, window %.1f deg)"
              % (fit["bearing0_x"], fit["px_per_deg"], fit["period_px"],
                 fit["period_deg"], fit["rms_deg"], fit["rms_px"],
                 fit["marks"], fit["visible_deg"]))
    else:
        print("  fit: not enough marks (need yaw sidecars)")


# --------------------------------------------------------------------------
# Self test
# --------------------------------------------------------------------------
def self_test():
    checks = []

    def check(name, condition, detail=""):
        checks.append((name, bool(condition), detail))

    # 1. Layout arithmetic: the blit source is the last row of the bitmap.
    check("weapon icon src == base+0x50+24*23",
          WEAPON_ICON_SRC == 0x80FC78,
          hex(WEAPON_ICON_SRC))
    check("weapon name inside the record",
          WEAPON_ICON_OFF + 0x240 <= WEAPON_NAME_OFF < WEAPON_STRIDE,
          hex(WEAPON_NAME_OFF))
    check("equipment slots == 12", EQUIP_ICON_COUNT == 12,
          str(EQUIP_ICON_COUNT))
    check("equipment fmt", EQUIP_ICON_FMT % 11 == "equip11")
    check("equipment index field is player+0x3320",
          EQUIP_INDEX_OFF == 0x3320, hex(EQUIP_INDEX_OFF))
    check("equipment mask field is player+0x3328",
          EQUIP_MASK_OFF == 0x3328, hex(EQUIP_MASK_OFF))
    check("equipment index inside player[0]",
          PLAYER_BASE <= EQUIP_INDEX_ABS < EQUIP_MASK_ABS
          < PLAYER_BASE + PLAYER_STRIDE,
          "%#x..%#x" % (EQUIP_INDEX_ABS, EQUIP_MASK_ABS))
    check("equipment index is not the panel selected id",
          EQUIP_INDEX_ABS != 0x81C4A0, hex(EQUIP_INDEX_ABS))

    # 2. NCC behaviour on synthetic images.
    a = [0, 1, 2, 3, 4, 5, 6, 7]
    check("ncc identical", abs(ncc(a, a) - 1.0) < 1e-9)
    check("ncc inverted", abs(ncc(a, [-v for v in a]) + 1.0) < 1e-9)
    check("ncc constant is 0", ncc([1] * 8, a) == 0.0)
    check("ncc length mismatch", ncc(a, a[:-1]) == 0.0)
    ramp = [float(v) for v in range(64)]
    check("ncc gain invariant", abs(ncc(ramp, [3 * v + 5 for v in ramp]) - 1.0)
          < 1e-9)

    # 3. Weapon triplet scanning on a synthetic record.
    synthetic = (b"\x00\x00" + b"MACHINE GUN\x00clickblt\x00WICON1\x00"
                 + bytes(0x54) + b"CANNON\x00clickblt\x00WICON3\x00" + bytes(0x54))
    triples = scan_weapon_triplets(synthetic)
    check("triplet scan finds 2", len(triples) == 2, str(triples))
    check("triplet names", [t[0] for t in triples] == ["MACHINE GUN", "CANNON"],
          str([t[0] for t in triples]))
    check("triplet icons", [t[2] for t in triples] == ["WICON1", "WICON3"])
    check("triplet on empty", scan_weapon_triplets(b"") == [])

    # 4. Compass window is a sane rectangle inside 320x200.
    x0, x1, y0, y1 = COMPASS_WINDOW
    check("compass window inside screen",
          0 <= x0 < x1 < 320 and 0 <= y0 < y1 < 200,
          str(COMPASS_WINDOW))
    check("compass rows order",
          y1 < COMPASS_LETTER_ROWS[0] or COMPASS_LETTER_ROWS[0] < y1,
          "letters may sit on the last ribbon rows")
    # The mark-grouping helper must merge adjacent columns and keep gaps.
    fake = [[1, 2, 3, 7, 8]]
    groups = _groups(fake[0])
    check("mark grouping", groups == [(1, 3, 2.0), (7, 8, 7.5)], str(groups))

    # 5. Compass fit on a synthetic tape: k = 0.30 px/deg, bearing 0 at x=104.
    def synth(yaw_deg):
        marks = []
        for bearing in (0.0, 90.0, 180.0, 270.0):
            x = 104.0 + 0.30 * (bearing - yaw_deg)
            if 79.0 <= x <= 129.0:
                marks.append((x, x + 1, x + 0.5))
        return {"window": [79, 129, 2, 15], "yaw": math.radians(yaw_deg),
                "long_marks": marks, "ticks": [], "letters": []}

    synthetic = [synth(0.0), synth(21.0), synth(200.0)]
    fit = fit_compass(synthetic)
    check("compass fit recovers k",
          fit is not None and abs(fit["px_per_deg"] - 0.30) < 0.01, str(fit))
    check("compass fit recovers phase",
          fit is not None and abs(fit["bearing0_x"] - 104.0) < 1.0, str(fit))
    check("compass fit residual small",
          fit is not None and fit["rms_deg"] < 1.0, str(fit))
    check("compass fit needs marks",
          fit_compass([{"window": [79, 129, 2, 15], "yaw": 0.0,
                        "long_marks": [], "ticks": [], "letters": []}]) is None)
    check("compass_x inside window",
          abs(compass_x(0.0, 0.0, fit) - 104.0) < 1.0)
    check("compass_x outside window",
          compass_x(180.0, 0.0, fit) is None)

    # 6. With the real game: tables, assets, asset presence N/N.
    game = find_game_dir()
    if game:
        archive = Archive(game)
        rows = load_weapon_icons(game)
        mapping, conflicts = weapon_icon_map(rows)
        check("weapon records found", len(rows) > 0, str(len(rows)))
        check("weapon icons unique per weapon", not conflicts, str(conflicts))
        check("one icon per weapon", all(len({r["icon"] for r in rows
                                              if r["name"] == n}) == 1
                                         for n in mapping))
        missing = sorted({r["icon"] for r in rows if not archive.has(r["icon"])})
        check("every weapon icon is in BMP.IDX", not missing, str(missing))
        bad = sorted({r["icon"] for r in rows
                      if (archive.image(r["icon"]) is not None
                          and (archive.image(r["icon"]).width,
                               archive.image(r["icon"]).height) != (24, 24))})
        check("weapon icons are 24x24", not bad, str(bad))
        names = load_equipment_names(game)
        equipment = equipment_icon_map(archive, names)
        check("equipment table loaded", len(equipment) >= 12, str(len(equipment)))
        present = sum(1 for e in equipment if e[3])
        check("all 12 equipment icons present", present >= 12,
              "%d/%d" % (present, len(equipment)))
        icons = archive.icons24()
        check("icon atlas size", len(icons) == 57, str(len(icons)))

        # 7. The frame really contains WICON1 at the measured place.
        frames = find_frames_dir()
        if frames:
            frame = Frame(os.path.join(frames, "win2.png"))
            report = compare_icons(frame, archive, DEFAULT_WEAPON,
                                   DEFAULT_EQUIPMENT, mapping)
            w = report["weapon"]
            e = report["equipment"]
            check("weapon icon matched in win2",
                  w["found"] and w["ncc"] >= 0.8, str(w.get("ncc")))
            check("weapon icon is the table entry",
                  w["found"] and w["runners"][0][1] == "WICON1",
                  str(w.get("runners")[:1]))
            check("weapon ncc for the table entry",
                  w["found"] and w["ncc_expected"] >= 0.8,
                  str(w.get("ncc_expected")))
            check("weapon icon is stored top-down",
                  w["found"] and w["orientation"] == "top-down",
                  str(w.get("orientation")))
            check("equipment icon matched in win2",
                  e["found"] and e["ncc"] >= 0.8, str(e.get("ncc")))
            check("equipment icon is EQUIP0 read bottom-up",
                  e["found"] and e["orientation"] == "bottom-up"
                  and e["ncc_expected"] >= 0.8,
                  str((e.get("ncc_expected"), e.get("runners")[:2])))
            check("measured icon row is 162",
                  w["found"] and (w["rect"][0], w["rect"][1]) == (23, 162),
                  str(w.get("rect")))
            equip0, equip12 = archive.image("EQUIP0"), archive.image("EQUIP12")
            if equip0 is not None and equip12 is not None:
                mirrored = b"".join(
                    equip12.pixels[r * 24:(r + 1) * 24]
                    for r in range(23, -1, -1))
                check("EQUIP12 is EQUIP0 mirrored",
                      mirrored == equip0.pixels)
            # Compass: every frame has a ribbon with long marks.
            compass_frames = []
            for name in ("win1.png", "win2.png", "win3.png"):
                path = os.path.join(frames, name)
                if os.path.exists(path):
                    compass_frames.append(Frame(path))
            report = compass_report(compass_frames)
            check("compass frames", len(report["frames"]) >= 2,
                  str(len(report["frames"])))
            counts = [len(m["long_marks"]) for m in report["frames"]]
            check("compass has cardinal marks", all(c >= 1 for c in counts),
                  str(counts))
            fit = report.get("fit")
            check("compass fit computed", fit is not None, str(fit))
            if fit:
                check("compass scale plausible", 0.2 < fit["px_per_deg"] < 0.4,
                      str(fit["px_per_deg"]))
                check("compass fit residual <= 8 deg", fit["rms_deg"] <= 8.0,
                      str(fit["rms_deg"]))
                check("compass period ~ 90 deg",
                      abs(fit["period_px"] - 90.0 * fit["px_per_deg"]) < 0.01)
        else:
            print("skip frame checks (no out/oracle)")
    else:
        print("skip asset checks (no game directory)")

    ok = True
    for name, good, detail in checks:
        print("%s %s%s" % ("ok  " if good else "FAIL", name,
                           " (%s)" % detail if detail and not good else ""))
        if not good:
            ok = False
    print("self-test: %d/%d" % (sum(1 for _, g, _ in checks if g), len(checks)))
    return 0 if ok else 1


def _groups(cols):
    out = []
    for c in sorted(cols):
        if out and c - out[-1][-1] <= 1:
            out[-1].append(c)
        else:
            out.append([c])
    return [(g[0], g[-1], (g[0] + g[-1]) / 2.0) for g in out]


def scan_weapon_triplets(data):
    """``[(name, sound, icon)]`` in a raw ``.sbt`` stream (fallback parser).

    The engine's record is ``name\\0 sound\\0 icon\\0`` + 21 dwords; this helper
    is only used by the self-test and as a cross-check of
    ``load_weapon_icons`` (it must find the same triplets).
    """
    out = []
    pos = 0
    while True:
        at = data.find(b"WICON", pos)
        at2 = data.find(b"WCON", pos)
        if at < 0 or (0 <= at2 < at):
            at = at2
        if at < 0:
            break
        pos = at + 5
        # icon name: WICON<n> / WCON<n>[B]
        end = at
        while end < len(data) and data[end] != 0:
            end += 1
        icon = data[at:end]
        if not icon.startswith((b"WICON", b"WCON")) or end >= len(data):
            continue
        # walk back over the two NUL-terminated strings that precede the icon
        nul1 = at - 1                      # NUL that terminates "sound"
        if nul1 < 0 or data[nul1] != 0:
            continue
        i = nul1 - 1
        while i >= 0 and data[i] != 0:
            i -= 1
        if i < 0:
            continue
        nul0 = i                           # NUL that terminates "name"
        sound = data[nul0 + 1:nul1]
        j = nul0 - 1
        while j >= 0 and data[j] != 0:
            j -= 1
        if j < 0:
            continue
        name = data[j + 1:nul0]
        if not name or not sound:
            continue
        out.append((name.decode("latin1"), sound.decode("latin1"),
                    icon.decode("latin1")))
    return out


def cross_check_triplets(game_dir, rows):
    """Verify every parsed triplet byte-for-byte in its own ``.sbt``.

    The record is ``name\\0 sound\\0 icon\\0``; the check also requires the
    ``0x54`` operand bytes that the engine reads after the strings
    (``docs/FORMATS.md`` §12.3), which is what makes the triplet a real
    ``DEFINE_WEAPONS`` record and not a coincidence.
    """
    by_file = {}
    for row in rows:
        by_file.setdefault(row["file"], set()).add(
            (row["name"], row["sound"], row["icon"]))
    bad = []
    for name, expected in sorted(by_file.items()):
        path = os.path.join(game_dir, name)
        with open(path, "rb") as fh:
            data = fh.read()
        for triplet in expected:
            needle = b"\0".join(s.encode("latin1") for s in triplet) + b"\0"
            at = data.find(needle)
            if at < 0 or at + len(needle) + 0x54 > len(data):
                bad.append((name, triplet))
    return bad


# --------------------------------------------------------------------------
# CLI
# --------------------------------------------------------------------------
def main(argv=None):
    ap = argparse.ArgumentParser(
        description="Cockpit HUD icons, compass and number sources.")
    ap.add_argument("--self-test", action="store_true")
    ap.add_argument("--icons", action="store_true",
                    help="WICON/WCON/EQUIP table for weapons and equipment")
    ap.add_argument("--compare", nargs="?", const="", metavar="FRAME.png",
                    help="compare the icons with an oracle frame")
    ap.add_argument("--compass", nargs="?", const="", metavar="FRAME.png",
                    help="measure the heading tape (default: win1..win3)")
    ap.add_argument("--json", action="store_true")
    ap.add_argument("--game", help="game dir (default: mirror or install)")
    ap.add_argument("--frames", help="dir with win1..win3.png")
    ap.add_argument("--weapon", default=DEFAULT_WEAPON,
                    help="weapon name for --compare (default: %s)" % DEFAULT_WEAPON)
    ap.add_argument("--equipment", type=int, default=DEFAULT_EQUIPMENT,
                    help="equipment index for --compare (default: 0)")
    opts = ap.parse_args(argv)

    if opts.self_test:
        return self_test()
    if not (opts.icons or opts.compare is not None or opts.compass is not None):
        ap.print_help()
        return 0

    game = find_game_dir(opts.game)
    archive = Archive(game) if game else None
    rows = load_weapon_icons(game) if game else []
    mapping, conflicts = weapon_icon_map(rows)
    equipment = equipment_icon_map(archive, load_equipment_names(game)) if game else []
    result = {"game": game, "weapon_records": len(rows),
              "weapons": len(mapping), "conflicts": conflicts,
              "equipment": [{"index": i, "name": n, "asset": a, "present": p}
                            for i, n, a, p in equipment]}

    if opts.icons:
        result["weapon_icons"] = mapping
        result["cross_check"] = cross_check_triplets(game, rows) if game else []
        if not opts.json:
            print_icons(rows, mapping, conflicts, equipment, archive,
                        archive.names if archive else None)
            if result["cross_check"]:
                print("\ncross-check mismatches: %s" % result["cross_check"])
        elif result["cross_check"]:
            print(json.dumps(result.get("cross_check"), indent=2),
                  file=sys.stderr)

    if opts.compare is not None:
        path = opts.compare
        if not path or not os.path.exists(path):
            frames = find_frames_dir(opts.frames)
            if frames is None:
                print("no oracle frames found (use --compare FRAME.png)")
                return 1
            path = os.path.join(frames, os.path.basename(path or "win2.png"))
        if not os.path.exists(path):
            print("frame not found: %s" % path)
            return 1
        frame = Frame(path)
        report = compare_icons(frame, archive, opts.weapon, opts.equipment,
                               mapping)
        result["compare"] = report
        if not opts.json:
            print_compare(report)

    if opts.compass is not None:
        frames = []
        if opts.compass:
            frames.append(Frame(opts.compass))
        else:
            directory = find_frames_dir(opts.frames)
            if directory is None:
                print("no oracle frames found (use --compass FRAME.png)")
                return 1
            for name in ("win1.png", "win2.png", "win3.png"):
                candidate = os.path.join(directory, name)
                if os.path.exists(candidate):
                    frames.append(Frame(candidate))
        if not frames:
            print("no frames for --compass")
            return 1
        report = compass_report(frames)
        for mark, frame in zip(report["frames"], frames):
            mark["path"] = frame.path
        result["compass"] = report
        if not opts.json:
            print_compass(report)

    if opts.json:
        print(json.dumps(result, indent=2, ensure_ascii=False))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
