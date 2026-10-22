#!/usr/bin/env python3
"""Screen-space effects of Gunmetal (``gundx.exe`` v1.36): haze flash and
palette modulation -- model, map and frame probe (M4.2.8).

Stdlib only.  The installation is only read; nothing is written.  Every
constant below carries a virtual address (image base ``0x400000``); the
reasoning and the disproved hypotheses are in ``docs/SCREEN_FX.md``.

Two independent things are modelled, and they must not be confused:

1. **Camera haze / screen flash** -- a full-screen white quad whose alpha is
   ``ftol(255 * haze)``.  ``haze`` is the double global ``0x80f9a8``, set by
   the script command ``SET_CAMERA_HAZE(level, decay)`` (op 80, handler
   ``0x45b3bf``) and decayed linearly once per frame by
   ``haze -= dt_ms * 0.91 * decay`` (``0x4572d6``, ``dt`` capped at 45 ms,
   ``0x44d518``).  The frame copies it into the camera record
   (``0x44672c`` -> ``0x446764`` -> ``f32 [0x83d288]`` = ``ctx+0xe8``) and the
   2D pass reads it at ``0x418c7f``.

2. **Damage / haze flash of the player's own model** -- *not* a screen tint.
   ``SOURCEPLAYER_HAZE`` (op 120, ``0x45a281``) and ``damage_player``
   (``0x453b50``) add to the per-player timer ``p+0x3224`` and set flag
   ``0x20``; ``0x4538d0`` then sets object flag ``0x10`` (``0x4283ca``) and
   writes the model light byte ``p+0x3238`` into ``[obj->0x6c]+0x70``
   (``0x428421``), which the renderer reads as the 0..127 LOOK16 light
   (``0x41ab8f``, ``0x41be9f``).  ``SOURCEPLAYER_HAZE_BRIGHTNESS`` (op 121,
   ``0x45a455``) is that byte.

The palette side: the in-memory table is **6-bit RGB** (the BMP loader shifts
every channel right by 2, ``0x47f0ae``..``0x47f0cd``), the display is fed by
``0x407220`` (``<<2`` + ``IDirectDrawPalette::SetEntries``), and the only
global palette modulation is the user brightness level ``[0x5a4950]`` 0..3
blended toward 63 by ``1/16`` per step (``0x4847b0`` -> ``0x40c7d0``).
Underwater switches the table to ``WATERPAL`` (``0x83d640``); when the file is
missing it is generated as ``R>>2, G/2+16, B/2+10`` (``0x41edd3``).

Usage::

    python3 tools/re/screen_fx.py --self-test     # byte anchors + synthetic curves
    python3 tools/re/screen_fx.py --map           # field/function map with VAs
    python3 tools/re/screen_fx.py --scripts       # SET_CAMERA_HAZE census in *.sbt
    python3 tools/re/screen_fx.py --frames        # oracle frames: brightness/bands
    python3 tools/re/screen_fx.py --json          # everything, one JSON object
    python3 tools/re/screen_fx.py --trace --level 1.0 --decay 0.005

Paths honour ``GUNMETAL_EXE`` (via ``exelib``), ``GUNMETAL_GAME`` and
``GUNMETAL_ORACLE`` (default ``out/oracle``).
"""

import argparse
import glob
import json
import math
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, ROOT)

import exelib  # noqa: E402

DEFAULT_GAME = os.environ.get("GUNMETAL_GAME", "/mnt/d/games/Gunmetal 3dfx")


def _default_oracle():
    """``$GUNMETAL_ORACLE``, else ``<repo>/out/oracle``.

    ``out/`` is not in git, so a worktree usually has none; in that case the
    main checkout (the parent of the common git dir) is tried before giving
    up.  Nothing is written.
    """
    env = os.environ.get("GUNMETAL_ORACLE")
    if env:
        return env
    local = os.path.join(ROOT, "out", "oracle")
    if os.path.isdir(local):
        return local
    try:
        import subprocess
        common = subprocess.run(
            ["git", "-C", ROOT, "rev-parse", "--path-format=absolute",
             "--git-common-dir"],
            capture_output=True, text=True, timeout=10, check=False)
        if common.returncode == 0:
            main = os.path.dirname(common.stdout.strip())
            cand = os.path.join(main, "out", "oracle")
            if os.path.isdir(cand):
                return cand
    except (OSError, subprocess.SubprocessError):
        pass
    return local


DEFAULT_ORACLE = _default_oracle()

# --------------------------------------------------------------------------
# Provenance
# --------------------------------------------------------------------------

#: camera/scene record; ``0x446590`` fills it, the 2D pass reads it
CTX = 0x83D1A0
CTX_X = 0x83D248            # ctx+0xa8, u16
CTX_Y = 0x83D24A            # ctx+0xaa, u16
CTX_W = 0x83D24C            # ctx+0xac, u16
CTX_H = 0x83D24E            # ctx+0xae, u16
CTX_HAZE = 0x83D288         # ctx+0xe8, f32 -- copied from 0x80f9a8

HAZE_LEVEL = 0x80F9A8       # f64, SET_CAMERA_HAZE level; 0x80f9ac = high dword
HAZE_RATE = 0x5E3DD8        # f64, decay per millisecond
GAMEPAL = 0x83D320          # 768 B, 6-bit RGB
WATERPAL = 0x83D640         # 768 B, 6-bit RGB
CUR_PAL = 0x83D2C4          # pointer to GAMEPAL or WATERPAL
PAL_BRIGHT = 0x5A4950       # i32, 0..3, config "brightness"
PAL_SCRATCH = 0x4CF6C8      # 768 B, post-brightness palette

C_WHITE_F32 = 0x4A20D4      # 255.0f
C_HAZE_K = 0x4A2578         # 0.91 (f64)
C_ZERO = 0x4A2570           # 0.0
C_ONE = 0x4A1878            # 1.0 (palette blend)
C_SIXBIT = 0x4A1880         # 63.0 (palette blend pivot)

PLAYER_BASE = 0x819040
PLAYER_STRIDE = 0x3510
P_FLAGS = 0x3200            # 0x81c240, bits 0x10 invuln, 0x20 haze, 0x40/0x80 infect
P_INVULN = 0x3220           # 0x81c260, timer ms
P_HAZE = 0x3224             # 0x81c264, timer ms
P_HAZE_BRIGHT = 0x3238      # 0x81c278, model light 0..127
P_BRIGHT = 0x3234           # 0x81c274, same, when no haze active

#: byte anchors: (VA, expected bytes, what)
ANCHORS = [
    (0x44672C, "dd 05 a8 f9 80 00", "fld qword [0x80f9a8] -- frame reads haze"),
    (0x4469B7, "68 a0 d1 83 00", "push 0x83d1a0 -- ctx passed to 0x41928e"),
    (0x446764, "d9 1d 88 d2 83 00", "fstp dword [0x83d288] -- ctx+0xe8"),
    (0x418C7F, "d9 81 e8 00 00 00", "fld dword [ecx+0xe8] -- quad alpha"),
    (0x418C85, "d8 0d d4 20 4a 00", "fmul dword [0x4a20d4] -- *255.0f"),
    (0x418C93, "83 7d f4 00", "cmp alpha,0 -- skip when <=0"),
    (0x418CA6, "c7 45 f4 ff 00 00 00", "clamp alpha to 0xff"),
    (0x418CB3, "68 ff ff ff 00", "grConstantColorValue(0xffffff)"),
    (0x4189DA, "68 00 00 ff 00", "grConstantColorValue(0x00ff0000) -- red mode"),
    (0x41A2CC, "e8 e1 e5 ff ff", "call 0x4188b2 from the Glide pass"),
    (0x45B3C2, "dd 1d a8 f9 80 00", "SET_CAMERA_HAZE: fstp qword [0x80f9a8]"),
    (0x45B3FA, "dc 0d 78 25 4a 00", "fmul qword [0x4a2578] -- decay*0.91"),
    (0x4572D6, "dc 2d a8 f9 80 00", "fsubr qword [0x80f9a8] -- haze -= dt*rate"),
    (0x4572DC, "dd 15 a8 f9 80 00", "fst qword [0x80f9a8]"),
    (0x45712B, "c7 05 a8 f9 80 00 00 00 00 00", "init: haze = 0"),
    (0x457315, "2b c7", "sleeper -= dt -- SLEEP operand is in ms"),
    (0x44D518, "83 fb 2d", "cmp dt,0x2d -- 45 ms cap"),
    (0x453F76, "8b 8e 64 c2 81 00", "damage: read p+0x3224"),
    (0x453F9E, "83 cf 20", "damage: flags |= 0x20"),
    (0x45A2B0, "89 b9 64 c2 81 00", "SOURCEPLAYER_HAZE: write p+0x3224"),
    (0x45A2C5, "83 ca 20", "SOURCEPLAYER_HAZE: flags |= 0x20"),
    (0x45A47B, "89 8a 78 c2 81 00", "SOURCEPLAYER_HAZE_BRIGHTNESS: p+0x3238"),
    (0x443C0B, "f6 83 40 c2 81 00 20", "tick: test flags & 0x20"),
    (0x443C1E, "89 8b 64 c2 81 00", "tick: p+0x3224 -= dt"),
    (0x452762, "c7 83 78 c2 81 00 40 00 00 00", "reset brightness = 0x40"),
    (0x453B0C, "8b 8e 78 c2 81 00", "apply: read p+0x3238"),
    (0x42845A, "88 42 70", "obj instance light <- brightness"),
    (0x428401, "83 ca 10", "obj+0x70 |= 0x10 when haze active"),
    (0x428412, "83 e2 ef", "obj+0x70 &= ~0x10 when haze is over"),
    (0x41AB8F, "0f be 42 70", "frame: movsx light, [inst+0x70]"),
    (0x41A9FE, "0f be 48 70", "frame: skip draw when light <= 0"),
    (0x47F0B4, "c0 ea 02", "palette loader: channel >> 2 (6-bit)"),
    (0x47F0C2, "8a 51 fb", "palette loader: read B of the BMP entry"),
    (0x418769, "83 fa 3f", "texture decode: clamp palette index to 0x3f (6-bit)"),
    (0x41878B, "c1 e2 02", "texture decode: 6-bit -> RGB565 (<<2)"),
    (0x407268, "c0 e2 02", "display palette: channel << 2"),
    (0x4072B0, "6a 44", "SetEntries(flags 0x44)"),
    (0x447962, "f6 86 40 c2 81 00 08", "waterpal switch: test flags & 0x8"),
    (0x447971, "b8 40 d6 83 00", "waterpal address 0x83d640"),
    (0x449EC0, "e8 eb a8 03 00", "frame end: re-apply palette brightness"),
    (0x4847B0, "db 05 50 49 5a 00", "fild [0x5a4950] -- brightness level"),
    (0x41EE1E, "d1 fa", "waterpal auto-gen: G >>= 1"),
    (0x41EE20, "83 c2 10", "waterpal auto-gen: G += 0x10"),
    (0x41EE3F, "d1 f9", "waterpal auto-gen: B >>= 1"),
    (0x41EE41, "83 c1 0a", "waterpal auto-gen: B += 0x0a"),
    (0x457AF3, "03 de 6a 4f", "NEW_LEVEL: no haze/palette write"),
    (0x457B11, "a1 cc f9 80 00", "NEXT_LEVEL: level counter only"),
]

#: the map printed by ``--map``: (VA, kind, name, role)
MAP = [
    (HAZE_LEVEL, "f64", "camera_haze", "haze level written by op 80; read by 0x44672c"),
    (HAZE_RATE, "f64", "haze_decay", "per-ms decay = 0.91*arg2 (0x45b3fa)"),
    (0x4572B9, "code", "haze_tick", "haze -= dt*rate, clamp 0 (0x4572d6..0x457300)"),
    (0x457230, "code", "scene_timers", "per-frame timer tick, arg = dt ms"),
    (0x44D4C0, "code", "frame_dt", "dt from 0x5a4a74, fixed-rate path 0x5b/fps"),
    (0x44D518, "const", "dt_cap", "min(dt, 45) ms"),
    (0x45B3BF, "code", "SET_CAMERA_HAZE", "op 80, sig DD"),
    (0x446590, "code", "frame_setup", "fills ctx 0x83d1a0; +0xe8 from 0x80f9a8"),
    (0x44672C, "code", "haze_read", "fld qword [0x80f9a8]"),
    (0x446764, "code", "haze_store", "fstp dword [0x83d288] (ctx+0xe8)"),
    (CTX, "struct", "camera_ctx", "x/y/w/h at +0xa8..+0xae, haze at +0xe8"),
    (0x4469B7, "code", "ctx_push", "push 0x83d1a0 -> 0x41928e -> 0x4188b2"),
    (0x41A2CC, "code", "2d_pass_call", "call 0x4188b2 when [0x530968]!=0"),
    (0x4188B2, "code", "screen_prim_draw", "draws the 2D list and the haze quad"),
    (0x418C7C, "code", "haze_quad_alpha", "alpha = ftol(255*ctx+0xe8), clamp 0..255"),
    (0x418CB3, "color", "haze_color", "0xffffff"),
    (0x418D42, "code", "haze_vertex_alpha", "writes alpha into the 4 GrVertex"),
    (0x418EBF, "code", "haze_quad", "guDrawPolygonVertexListWithClip(4)"),
    (0x4189DA, "color", "red_mode_color", "0x00ff0000 for list items with mode 3"),
    (0x4170E0, "code", "red_mode_select", "item+0x14 = 3 if obj+0x75 != 0, else 2"),
    (0x45A281, "code", "SOURCEPLAYER_HAZE", "op 120, sig P; p+0x3224 += arg"),
    (0x45A455, "code", "SOURCEPLAYER_HAZE_BRIGHTNESS", "op 121, sig L; p+0x3238"),
    (0x45AD04, "code", "SOURCEPLAYER_INVINCIBLE", "op 122; p+0x3220, flag 0x10"),
    (0x453B50, "code", "damage_player", "p+0x3224 += 91*pct/100, flag 0x20"),
    (0x453F76, "code", "damage_haze_add", "x91 via lea, /100 via 0x51eb851f"),
    (0x443C0B, "code", "haze_timer_tick", "p+0x3224 -= dt; clear 0x20; 0x4538d0"),
    (0x4538D0, "code", "apply_player_visuals", "0x4283ca(obj,flag20); 0x428421(obj,light)"),
    (0x4283CA, "code", "obj_flag10", "obj+0x70 |= 0x10"),
    (0x428421, "code", "set_instance_light", "[obj->0x6c]+0x70 = byte"),
    (0x41AB8F, "code", "light_to_float", "inst+0x70 -> 0x9e8d24"),
    (0x41BE9F, "code", "light_index", "inst+0x70 + obj+0x74 + tex+0x16 + 0x80"),
    (0x41A9FE, "code", "light_cull", "flag 0x10 + light<=0 -> skip"),
    (0x452762, "const", "brightness_default", "0x40 written at spawn"),
    (GAMEPAL, "data", "gamepal", "768 B, 6-bit RGB (FRONTEND/GAMEPAL.BMP)"),
    (WATERPAL, "data", "waterpal", "768 B, 6-bit RGB (FRONTEND/WATERPAL.BMP)"),
    (CUR_PAL, "ptr", "current_palette", "0x83d320 or 0x83d640"),
    (0x41878B, "code", "palette_to_rgb565", "6-bit palette -> 16-bit texel (<<2)"),
    (0x44AF88, "code", "gamepal_load", "0x47efe0(0x83d320,'frontend','gamepal')"),
    (0x44AFA3, "code", "waterpal_load", "0x47efe0(0x83d640,'frontend','waterpal')"),
    (0x41EDD3, "code", "waterpal_generate", "R>>2, G/2+16, B/2+10"),
    (0x447962, "code", "waterpal_select", "player flags & 0x8 -> waterpal"),
    (0x449F4D, "code", "waterpal_select_world", "same test inside the 3D draw"),
    (0x449EC0, "code", "palette_reapply", "0x4847b0 at frame end when changed"),
    (0x4847B0, "code", "palette_brightness", "0x40c7d0(cur, [0x5a4950]*0.25*0.25)"),
    (0x40C7D0, "code", "palette_blend", "out = 63 - (63-b)*(1-p), p=level/16"),
    (0x407220, "code", "ddraw_set_entries", "<<2 to 8-bit + SetEntries(0x44)"),
    (PAL_BRIGHT, "i32", "brightness_level", "0..3, keyconfig_loader 0x484f52"),
    (0x48BF20, "code", "brightness_dec", "level-- (menu)"),
    (0x48C7B5, "code", "brightness_inc", "level++ if <3 (menu)"),
    (0x457AF3, "code", "NEW_LEVEL", "op 61: no screen effect"),
    (0x457B11, "code", "NEXT_LEVEL", "op 62: no screen effect"),
    (0x456F50, "code", "reset_camera_defaults", "0x45712b: haze = 0 on (re)start"),
]

#: script census (measured on the shipped 36 ``.sbt``); kept here so that
#: ``--json`` documents the data even when the game is not mounted
EXPECTED_SCRIPTS = {
    "files": 36,
    "SET_CAMERA_HAZE": 247,
    "SOURCEPLAYER_HAZE": 108,
    "SOURCEPLAYER_HAZE_BRIGHTNESS": 684,
    "NEW_LEVEL": 60,
    "NEXT_LEVEL": 6,
}


# --------------------------------------------------------------------------
# Model
# --------------------------------------------------------------------------

def ftol(x):
    """Truncation toward zero -- ``__ftol`` at 0x40e0e0."""
    return int(x)


def haze_alpha(level):
    """Alpha the quad gets: ``ftol(255*level)`` clamped to 0..255 (0x418c85)."""
    return max(0, min(255, ftol(255.0 * level)))


def haze_decay_rate(arg):
    """``0x5e3dd8 = arg*0.91`` when ``arg > 0``, else 0 (0x45b3d7..0x45b400).

    A non-positive second operand stores zero, so the level never decays --
    it stays until the next ``SET_CAMERA_HAZE``.
    """
    return arg * 0.91 if arg > 0.0 else 0.0


def haze_step(level, rate, dt_ms):
    """One tick of 0x4572b9..0x457300 (returns the new level)."""
    if level <= 0.0:
        return 0.0
    v = level - dt_ms * rate
    return v if v > 0.0 else 0.0


def haze_trace(level, arg2, dt_ms=1000.0 / 60.0, max_frames=10000):
    """Per-frame levels/alphas until the haze is gone.

    Returns ``[(t_ms, level, alpha), ...]``; the first entry is the moment the
    script set the level.  ``dt`` is capped at 45 ms like the engine
    (0x44d518), because one long frame must not swallow the whole flash.
    """
    dt_ms = min(dt_ms, 45.0)
    rate = haze_decay_rate(arg2)
    out = [(0.0, level, haze_alpha(level))]
    t = 0.0
    cur = level
    for _ in range(max_frames):
        if cur <= 0.0:
            break
        t += dt_ms
        cur = haze_step(cur, rate, dt_ms)
        out.append((t, cur, haze_alpha(cur)))
    return out


def haze_duration_ms(level, arg2):
    """Time until the level reaches 0; ``inf`` when the rate is 0."""
    rate = haze_decay_rate(arg2)
    if rate <= 0.0:
        return math.inf
    return level / rate


def haze_full_ms(level, arg2):
    """How long the quad stays fully opaque (level >= 1.0), in ms."""
    rate = haze_decay_rate(arg2)
    if level <= 1.0:
        return 0.0
    if rate <= 0.0:
        return math.inf
    return (level - 1.0) / rate


def palette_blend6(value, p):
    """6-bit palette blend of 0x40c7d0 with ``p = level/16``.

    ``out = 63 - (63 - b) * (1 - p)`` truncated by ``ftol`` -- i.e. every
    channel is blended by ``p`` toward 63 (white in 6-bit space).
    """
    v = 63.0 - (63.0 - float(value)) * (1.0 - p)
    return max(0, min(63, ftol(v)))


def underwater_palette(rgb):
    """Auto-generated WATERPAL entry (0x41edd3): ``R>>2, G/2+16, B/2+10``."""
    r, g, b = rgb
    return (r >> 2, (g >> 1) + 0x10, (b >> 1) + 0x0A)


# --------------------------------------------------------------------------
# exe probe
# --------------------------------------------------------------------------

def _exe_bytes(va, n):
    off = exelib.va2off(va)
    if off is None:
        raise SystemExit("0x%06x is not backed by the exe file" % va)
    return exelib.data()[off:off + n]


def probe_anchors():
    """Verify every code/constant anchor against the shipped exe."""
    rows = []
    for va, want_hex, what in ANCHORS:
        want = bytes.fromhex(want_hex)
        try:
            got = _exe_bytes(va, len(want))
        except SystemExit as exc:
            rows.append({"va": va, "ok": False, "what": what, "error": str(exc)})
            continue
        rows.append({"va": va, "ok": got == want, "what": what,
                     "want": want.hex(" "), "got": got.hex(" ")})
    return rows


def probe_constants():
    out = {}
    for name, va, fmt in (
        ("white_f32", C_WHITE_F32, "<f"),
        ("haze_k", C_HAZE_K, "<d"),
        ("zero", C_ZERO, "<d"),
        ("one", C_ONE, "<d"),
        ("sixbit", C_SIXBIT, "<d"),
    ):
        raw = _exe_bytes(va, struct.calcsize(fmt))
        out[name] = {"va": "0x%06x" % va, "value": struct.unpack(fmt, raw)[0]}
    return out


def probe_strings():
    """The two palette entry names the loader asks for (0x44af88/0x44afa3)."""
    return {
        "gamepal": exelib.cstr(0x4ABBF8),
        "waterpal": exelib.cstr(0x4ABBAC),
        "gamepal_error": exelib.cstr(0x4ABBB8),
        "waterpal_error": exelib.cstr(0x4ABB5C),
    }


# --------------------------------------------------------------------------
# .sbt script census
# --------------------------------------------------------------------------

def _load_sbtvm():
    from gm import sbt, sbtvm
    return sbt, sbtvm


def _walk(insns, path=()):
    for ins in insns:
        yield ins, path
        if ins.body:
            yield from _walk(ins.body, path + (ins.name, ))


def scan_scripts(game=DEFAULT_GAME):
    """Count the haze commands in every shipped ``.sbt``.

    Returns ``{"files": n, "counts": {...}, "values": {...},
    "SET_CAMERA_HAZE": [{file, off, args, conditions}]}``.  Missing game
    directory is reported as ``{"available": False}``, not an exception.
    """
    files = sorted(glob.glob(os.path.join(game, "*.sbt")))
    if not files:
        return {"available": False, "game": game}
    sbt, sbtvm = _load_sbtvm()
    counts = {}
    values = {}
    hits = []
    decoded = 0
    for path in files:
        stem = os.path.splitext(os.path.basename(path))[0]
        try:
            rec = sbt.parse_file(path).records_of(11)[0].payload.code
            top, _end = sbtvm.decode(rec)
        except Exception as exc:                     # noqa: BLE001 - report it
            hits.append({"file": stem, "decode_error": str(exc)})
            continue
        decoded += 1
        for ins, conds in _walk(top):
            counts[ins.name] = counts.get(ins.name, 0) + 1
            if ins.name == "SET_CAMERA_HAZE":
                key = "x".join("%g" % v for v in ins.args)
                values[key] = values.get(key, 0) + 1
                hits.append({"file": stem, "off": ins.off, "args": list(ins.args),
                             "conditions": list(conds)})
    return {
        "available": True,
        "game": game,
        "files": len(files),
        "decoded": decoded,
        "counts": {k: counts.get(k, 0) for k in EXPECTED_SCRIPTS if k != "files"},
        "values": values,
        "SET_CAMERA_HAZE": [h for h in hits if "conditions" in h],
        "errors": [h for h in hits if "decode_error" in h],
    }


# --------------------------------------------------------------------------
# Oracle frames
# --------------------------------------------------------------------------

def _load_rgb(path):
    from gm import png
    with open(path, "rb") as fh:
        img = png.decode(fh.read())
    px = img.pixels
    w, h = img.width, img.height
    if img.mode == "RGB":
        return w, h, px
    out = bytearray(w * h * 3)
    if img.mode == "P":
        pal = img.palette
        for i, idx in enumerate(px):
            out[3 * i] = pal[4 * idx]
            out[3 * i + 1] = pal[4 * idx + 1]
            out[3 * i + 2] = pal[4 * idx + 2]
    elif img.mode == "RGBA":
        for i in range(w * h):
            out[3 * i:3 * i + 3] = px[4 * i:4 * i + 3]
    elif img.mode in ("L", "LA"):
        step = 2 if img.mode == "LA" else 1
        for i in range(w * h):
            v = px[step * i]
            out[3 * i] = out[3 * i + 1] = out[3 * i + 2] = v
    else:
        raise ValueError("unsupported PNG mode %r" % img.mode)
    return w, h, bytes(out)


def frame_metric(path, bands=8, hist_bins=16):
    """Brightness bands + luma histogram + a white/red wash verdict.

    A haze flash would raise the mean luma of *every* band towards 255 and
    make the frame nearly achromatic; a red full-screen fill would raise R
    over G/B.  Both are reported as raw numbers so the verdict is checkable.
    """
    w, h, px = _load_rgb(path)
    n = w * h
    lum = bytearray(n)
    rs = gs = bs = 0
    hist = [0] * hist_bins
    for i in range(n):
        o = 3 * i
        r, g, b = px[o], px[o + 1], px[o + 2]
        rs += r
        gs += g
        bs += b
        v = (r * 299 + g * 587 + b * 114) // 1000
        lum[i] = v
        hist[min(hist_bins - 1, v * hist_bins // 256)] += 1
    total = sum(lum)
    band_rows = []
    for k in range(bands):
        y0 = h * k // bands
        y1 = h * (k + 1) // bands
        seg = lum[y0 * w:y1 * w]
        band_rows.append({
            "band": k,
            "y0": y0,
            "y1": y1,
            "mean": round(sum(seg) / len(seg), 2),
            "white_frac": round(sum(1 for v in seg if v >= 240) / len(seg), 5),
        })
    means = [b["mean"] for b in band_rows]
    mean = total / n
    var = sum((v - mean) ** 2 for v in lum) / n
    white_frac = sum(1 for v in lum if v >= 240) / n
    return {
        "file": path,
        "width": w,
        "height": h,
        "mean_luma": round(mean, 2),
        "std_luma": round(math.sqrt(var), 2),
        "p99_luma": sorted(lum)[int(0.99 * (n - 1))],
        "min_luma": min(lum),
        "max_luma": max(lum),
        "white_frac": round(white_frac, 5),
        "mean_rgb": [round(rs / n, 2), round(gs / n, 2), round(bs / n, 2)],
        "red_bias": round((rs - (gs + bs) / 2.0) / n, 2),
        "bands": band_rows,
        "band_spread": round(max(means) - min(means), 2),
        "histogram": [round(c / n, 4) for c in hist],
        "haze_trace": _haze_verdict(mean, white_frac),
    }


def _haze_verdict(mean, white_frac):
    # thresholds are deliberately loose: any alpha above ~0.6 washes the frame
    if mean >= 200.0 or white_frac >= 0.60:
        return "white-wash (haze/flash visible)"
    if mean >= 120.0 and white_frac >= 0.20:
        return "partial white-wash (weak flash?)"
    return "none (normal frame: haze alpha ~0)"


def probe_frames(oracle=DEFAULT_ORACLE):
    out = {"oracle": oracle, "frames": [], "missing": []}
    for stem in ("win2", "win3"):
        path = os.path.join(oracle, stem + ".png")
        if not os.path.exists(path):
            out["missing"].append(path)
            continue
        out["frames"].append(frame_metric(path))
    return out


# --------------------------------------------------------------------------
# Rendering
# --------------------------------------------------------------------------

def _fmt_row(va, kind, name, role):
    return "  0x%06x  %-8s %-30s %s" % (va, kind, name, role)


def print_map():
    print("screen effects map (image base 0x400000, gundx.exe v1.36)")
    print("  CTX = 0x%06x (camera/scene record)" % CTX)
    print("  player fields are offsets from 0x%06x (stride 0x%x)"
          % (PLAYER_BASE, PLAYER_STRIDE))
    for va, kind, name, role in MAP:
        print(_fmt_row(va, kind, name, role))
    print("  player offsets: +0x%04x invuln timer, +0x%04x haze timer,"
          " +0x%04x haze brightness, +0x%04x default brightness"
          % (P_INVULN, P_HAZE, P_HAZE_BRIGHT, P_BRIGHT))
    print("  player flag bits: 0x10 invulnerable, 0x20 haze/damage flash,"
          " 0x40|0x80 infected")


def print_scripts(res):
    if not res.get("available"):
        print("scripts: no .sbt under %s" % res.get("game"))
        return
    print("scripts: %d files, %d decoded" % (res["files"], res["decoded"]))
    for name, want in EXPECTED_SCRIPTS.items():
        if name == "files":
            continue
        got = res["counts"].get(name, 0)
        flag = "ok" if got == want else "MISMATCH (expected %d)" % want
        print("  %-32s %5d  %s" % (name, got, flag))
    print("  SET_CAMERA_HAZE values (level x decay):")
    for key, n in sorted(res["values"].items(), key=lambda kv: -kv[1]):
        lvl, arg = (float(x) for x in key.split("x"))
        dur = haze_duration_ms(lvl, arg)
        full = haze_full_ms(lvl, arg)
        print("    %-14s x %3d   fade %8.1f ms   full-white %7.1f ms"
              % (key, n, dur, full))


def print_trace(level, arg2, dt, frames):
    print("SET_CAMERA_HAZE(level=%g, decay=%g); dt=%.3f ms (cap 45)"
          % (level, arg2, min(dt, 45.0)))
    print("  rate = 0.91*decay = %.6f / ms" % haze_decay_rate(arg2))
    print("  fade to 0: %.1f ms; full white (alpha 255): %.1f ms"
          % (haze_duration_ms(level, arg2), haze_full_ms(level, arg2)))
    print("    t_ms      level   alpha")
    for t, v, a in haze_trace(level, arg2, dt)[:frames]:
        print("  %7.2f  %8.4f  %5d" % (t, v, a))


def print_frames(res):
    if not res["frames"]:
        print("frames: nothing under %s" % res["oracle"])
        return
    for f in res["frames"]:
        print("%s  %dx%d  mean %.1f  std %.1f  white>=240 %.3f%%"
              % (os.path.basename(f["file"]), f["width"], f["height"],
                 f["mean_luma"], f["std_luma"], 100.0 * f["white_frac"]))
        print("  mean rgb %.1f/%.1f/%.1f  red_bias %+.2f  band_spread %.1f"
              % (f["mean_rgb"][0], f["mean_rgb"][1], f["mean_rgb"][2],
                 f["red_bias"], f["band_spread"]))
        print("  bands: " + " ".join("%.1f" % b["mean"] for b in f["bands"]))
        print("  histogram(16): " + " ".join("%.3f" % v for v in f["histogram"]))
        print("  verdict: " + f["haze_trace"])


# --------------------------------------------------------------------------
# Self-test
# --------------------------------------------------------------------------

def self_test(verbose=True):
    fails = []
    stats = {"n": 0}

    def check(cond, what):
        stats["n"] += 1
        if not cond:
            fails.append(what)
        if verbose:
            print("  [%s] %s" % ("ok" if cond else "FAIL", what))

    if verbose:
        print("byte anchors")
    rows = probe_anchors()
    for row in rows:
        check(row["ok"], "0x%06x %s" % (row["va"], row["what"]))
    if verbose:
        print("constants")
    c = probe_constants()
    check(abs(c["white_f32"]["value"] - 255.0) < 1e-6, "0x4a20d4 == 255.0f")
    check(abs(c["haze_k"]["value"] - 0.91) < 1e-12, "0x4a2578 == 0.91")
    check(c["zero"]["value"] == 0.0, "0x4a2570 == 0.0")
    check(c["one"]["value"] == 1.0, "0x4a1878 == 1.0")
    check(c["sixbit"]["value"] == 63.0, "0x4a1880 == 63.0")
    if verbose:
        print("palette entry names")
    st = probe_strings()
    check(st["gamepal"] == "gamepal", "0x4abbf8 == 'gamepal'")
    check(st["waterpal"] == "waterpal", "0x4abbac == 'waterpal'")

    if verbose:
        print("synthetic fade curves")
    check(haze_alpha(0.0) == 0, "alpha(0) == 0")
    check(haze_alpha(1.0) == 255, "alpha(1.0) == 255")
    check(haze_alpha(1.3) == 255, "alpha(1.3) saturates at 255")
    check(haze_alpha(-0.5) == 0, "alpha(-0.5) clamps to 0")
    check(abs(haze_decay_rate(0.005) - 0.00455) < 1e-15, "rate(0.005) == 0.00455")
    check(haze_decay_rate(0.0) == 0.0 and haze_decay_rate(-1.0) == 0.0,
          "non-positive decay -> rate 0")
    check(abs(haze_duration_ms(1.0, 0.005) - 219.78021978021977) < 1e-9,
          "1.0 @ 0.005 fades in 219.78 ms")
    check(abs(haze_duration_ms(0.35, 0.005) - 76.92307692307692) < 1e-9,
          "0.35 @ 0.005 fades in 76.92 ms")
    check(abs(haze_duration_ms(1.3, 0.002) - 714.2857142857142) < 1e-9,
          "1.3 @ 0.002 fades in 714.29 ms")
    check(abs(haze_full_ms(1.3, 0.002) - 164.83516483516485) < 1e-9,
          "1.3 @ 0.002 is fully white for 164.8 ms")
    tr = haze_trace(1.0, 0.005, 16.666666666666668, max_frames=200)
    levels = [v for _t, v, _a in tr]
    alphas = [a for _t, _v, a in tr]
    check(all(a >= b for a, b in zip(levels, levels[1:])), "trace is monotone")
    check(levels[-1] == 0.0 and alphas[-1] == 0, "trace ends at 0")
    check(tr[-1][0] - 45.0 <= 219.78 <= tr[-1][0],
          "zero crossing within one dt of 219.78 ms")
    check(all(tr[i][1] <= tr[i - 1][1] for i in range(1, len(tr))),
          "each dt step subtracts dt*rate")
    hold = haze_trace(0.7, 0.0, 16.666666666666668, max_frames=8)
    check(len(hold) == 9 and all(v == 0.7 for _t, v, _a in hold),
          "rate 0 holds the level (never fades)")
    # 45 ms cap: one 100 ms step must behave like one 45 ms step
    check(haze_step(1.0, 0.00455, 45.0) == haze_trace(1.0, 0.005, 100.0, 2)[1][1],
          "dt is capped at 45 ms")

    if verbose:
        print("synthetic palette")
    check(palette_blend6(0, 0.0) == 0 and palette_blend6(63, 0.0) == 63,
          "brightness 0 is identity on 6-bit")
    check(palette_blend6(0, 3 / 16.0) == 11, "b=0, level 3 -> 11 (63*3/16)")
    check(palette_blend6(63, 3 / 16.0) == 63, "white stays white")
    check(palette_blend6(32, 1 / 16.0) == 33, "b=32, level 1 -> 33")
    check(underwater_palette((63, 63, 63)) == (15, 47, 41),
          "waterpal(63,63,63) == (15,47,41)")
    check(underwater_palette((0, 0, 0)) == (0, 16, 10),
          "waterpal(0,0,0) == (0,16,10)")

    if verbose:
        print("shipped script census")
    sc = scan_scripts()
    if sc.get("available"):
        for name, want in EXPECTED_SCRIPTS.items():
            if name == "files":
                continue
            check(sc["counts"].get(name, 0) == want,
                  "%s x%d" % (name, want))
        vals = sc["values"]
        check(vals.get("1x0.005", 0) > 0, "SET_CAMERA_HAZE(1,0.005) present")
        check(vals.get("0.35x0.005", 0) > 0, "SET_CAMERA_HAZE(0.35,0.005) present")
    else:
        if verbose:
            print("  [skip] no .sbt corpus under %r" % sc.get("game"))

    if verbose:
        print("oracle frames")
    fr = probe_frames()
    if fr["frames"]:
        for f in fr["frames"]:
            check(f["mean_luma"] < 200.0 and f["white_frac"] < 0.2,
                  "%s has no white wash (mean %.1f)"
                  % (os.path.basename(f["file"]), f["mean_luma"]))
    else:
        if verbose:
            print("  [skip] no oracle frames under %r" % fr["oracle"])

    if verbose:
        print("self-test: %d checks, %d failed" % (stats["n"], len(fails)))
        for f in fails:
            print("  FAILED:", f)
    return fails


# --------------------------------------------------------------------------
# CLI
# --------------------------------------------------------------------------

def main(argv=None):
    ap = argparse.ArgumentParser(
        description="Gunmetal screen effects: haze flash, damage flash, palette")
    ap.add_argument("--self-test", action="store_true",
                    help="byte anchors + synthetic curves (exit 1 on failure)")
    ap.add_argument("--map", action="store_true",
                    help="field/function map with virtual addresses")
    ap.add_argument("--scripts", action="store_true",
                    help="SET_CAMERA_HAZE / SOURCEPLAYER_HAZE census in *.sbt")
    ap.add_argument("--frames", action="store_true",
                    help="oracle frames: brightness bands, histogram, verdict")
    ap.add_argument("--json", action="store_true", help="everything as one JSON")
    ap.add_argument("--trace", action="store_true", help="print a fade trace")
    ap.add_argument("--level", type=float, default=1.0,
                    help="SET_CAMERA_HAZE first operand (default 1.0)")
    ap.add_argument("--decay", type=float, default=0.005,
                    help="SET_CAMERA_HAZE second operand (default 0.005)")
    ap.add_argument("--dt", type=float, default=1000.0 / 60.0,
                    help="frame time in ms (default 16.667)")
    ap.add_argument("--frames-n", type=int, default=20,
                    help="rows printed by --trace (default 20)")
    ap.add_argument("--game", default=DEFAULT_GAME, help="game directory")
    ap.add_argument("--oracle", default=DEFAULT_ORACLE, help="oracle frame dir")
    args = ap.parse_args(argv)

    if args.self_test:
        fails = self_test()
        return 1 if fails else 0

    if args.trace:
        print_trace(args.level, args.decay, args.dt, args.frames_n)
        return 0

    if args.json:
        doc = {
            "tool": "screen_fx",
            "image_base": "0x400000",
            "ctx": "0x%06x" % CTX,
            "anchors": probe_anchors(),
            "constants": probe_constants(),
            "strings": probe_strings(),
            "map": [{"va": "0x%06x" % va, "kind": k, "name": n, "role": r}
                    for va, k, n, r in MAP],
            "player_fields": {
                "flags": "0x%04x" % P_FLAGS,
                "invuln_timer": "0x%04x" % P_INVULN,
                "haze_timer": "0x%04x" % P_HAZE,
                "haze_brightness": "0x%04x" % P_HAZE_BRIGHT,
                "default_brightness": "0x%04x" % P_BRIGHT,
            },
            "formulas": {
                "alpha": "clamp(ftol(255*haze), 0, 255)",
                "decay": "haze -= min(dt_ms,45) * 0.91 * decay_arg",
                "fade_ms": "level / (0.91*decay_arg)",
                "palette_blend6": "out = ftol(63 - (63-b)*(1-level/16))",
                "waterpal": "R>>2, G/2+16, B/2+10",
            },
            "expected_scripts": EXPECTED_SCRIPTS,
            "scripts": scan_scripts(args.game),
            "frames": probe_frames(args.oracle),
            "unproven": [
                "no shipped .sbt writes the player haze timer with a negative arg"
                " other than damage -1 / infect -2",
                "which sprites set obj+0x75 (red mode 3) is not traced to a"
                " gameplay event",
                "the exact rounding of the x87->ftol chain was verified for"
                " alpha and palette, not for every other user of 0x83d288",
                "a screen-wide red damage flash was not found; the red"
                " constant 0xff0000 belongs to the 2D list item mode 3",
            ],
        }
        print(json.dumps(doc, ensure_ascii=False, indent=2))
        return 0

    if args.map:
        print_map()
        rows = probe_anchors()
        bad = [r for r in rows if not r["ok"]]
        print("  anchor check: %d/%d ok" % (len(rows) - len(bad), len(rows)))
        for r in bad:
            print("    FAIL 0x%06x %s" % (r["va"], r["what"]))
        return 1 if bad else 0

    if args.scripts:
        print_scripts(scan_scripts(args.game))
        return 0

    if args.frames:
        print_frames(probe_frames(args.oracle))
        return 0

    print(__doc__)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
