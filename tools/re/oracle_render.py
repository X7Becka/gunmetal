#!/usr/bin/env python3
"""Render one frame from a captured oracle *state* and score it against the PNG.

The oracle (`docs/ORACLE.md`, `out/oracle/win*.json` + `win*.png`) keeps the
exact camera of the original game, so a frame becomes reproducible: the state
sidecar selects the level, the camera and the resolution, and this tool feeds
them into :func:`gm.render.render`.  With ``--oracle`` it also reports the
structural metrics of `tools/re/oracle_compare.py` (imported as a module, not
modified), so successive M4 improvements can be compared on equal terms.

    python3 tools/re/oracle_render.py --sidecar out/oracle/win2.json \
        --out /var/tmp/win2_ours.png --oracle out/oracle/win2.png --json

Scaling the oracle frame
------------------------
The captures are 1920x1080 window grabs of a 320x200 game image, plus a
1-pixel black window frame on all four sides (measured on win1..win3: the
outermost column/row contains only zeros, the next one has content).  The
pipeline is therefore:

1. drop the frame border: ``crop(1, 1, W-2, H-2)`` -> 1918x1078
   (``ORACLE_BORDER_PX``; ``--keep-border`` disables it);
2. area-average (box) downsample to the render size (320x200), i.e.
   ``oracle_compare.resize_area``: every destination pixel is the integer
   average of its source rectangle (6 x 5.39 texels here).  This is the exact
   inverse of the game's stretched 320x200 -> 1920x1080 blit and, unlike
   nearest, cannot drop thin geometry (light strips, rails) by sampling one
   texel out of six.

Bands
-----
``oracle_compare.metrics`` splits the frame into ``frame`` (all rows),
``world`` (top ``WORLD_END`` = 70 % — the cockpit panel starts at ~0.71 of the
height on win1..win3) and ``hud`` (the rest).  Only ``frame`` and ``world``
are printed; ``hud`` is in ``--json`` because the original draws a cockpit
there and our renderer draws nothing.

Eye position
------------
The sidecar may store either the eye or the feet in ``camera.pos``:

* ``docs/ORACLE.md`` §sidecar and ``tools/re/camera.py`` define the eye as
  ``player position + eye_height`` (``player+0x2F50``); in win1..win3
  ``camera.pos.z == camera.player_pos.z + eye_height``, so ``camera.pos`` is
  already the eye;
* a sidecar without ``player_pos`` (or with a mismatching one) is read as
  "``pos`` = feet", and the eye becomes ``pos + eye_height``.

``--eye-from auto`` (default) picks one of the two by that consistency check,
``pos`` / ``player`` force it; the choice is reported.

Nothing here writes to the game installation: the level, its lightmaps and
the texture pool are opened read-only.
"""
from __future__ import annotations

import argparse
import json
import math
import os
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, ROOT)
sys.path.insert(0, HERE)

from gm import render, sbw  # noqa: E402
import oracle_compare as oc  # noqa: E402  (reused, not modified)

DEFAULT_GAME = "/mnt/d/games/Gunmetal 3dfx"
MIRROR = os.path.expanduser("~/.cache/gunmetal-re/game")

#: black window frame around the captured picture, in source pixels
ORACLE_BORDER_PX = 1
#: share of the frame height that is world; the cockpit starts below it
WORLD_END = 0.70
#: focal lengths of the original at 320x200, in pixels: 1.0 and 5/6 of W/2
#: (``docs/CAMERA.md`` §7).  Scaled with ``--width`` when ``--focal`` is absent.
FOCAL_X_PER_HALF_WIDTH = 1.0
FOCAL_Y_RATIO = 5.0 / 6.0


# ---------------------------------------------------------------------------
# sidecar
# ---------------------------------------------------------------------------

def load_sidecar(path: str) -> dict:
    with open(path, "r", encoding="utf-8") as fh:
        data = json.load(fh)
    if "camera" not in data or "pos" not in data["camera"]:
        raise ValueError(f"{path}: no camera.pos")
    if not data.get("level"):
        raise ValueError(f"{path}: no level name")
    return data


def camera_frame(cam: dict, eye_from: str = "auto") -> dict:
    """Sidecar camera -> eye, forward, look, and the eye-mode actually used."""
    pos = tuple(float(v) for v in cam["pos"])
    # The camera pitch is the *body* pitch plus the turret/look pitch
    # (docs/CAMERA.md: `p[0x18] + p[0x2E60]`).  Sidecars written before
    # 2026-10-23 carry only the body angle, so `camera_pitch` is optional and
    # falls back to `pitch` -- old frames stay reproducible, new ones are exact.
    pitch = float(cam.get("camera_pitch", cam.get("pitch", 0.0)))
    yaw = float(cam.get("yaw", 0.0))
    roll = float(cam.get("roll", 0.0))
    eye_height = float(cam.get("eye_height", 0.0))
    player = cam.get("player_pos")
    mode = eye_from
    if mode == "auto":
        if player is not None and abs(pos[2] - float(player[2]) - eye_height) < 1e-6:
            mode = "pos"          # pos already is the eye (win1..win3)
        else:
            mode = "feet"
    if mode == "pos":
        eye = pos
    elif mode == "feet":
        eye = (pos[0], pos[1], pos[2] + eye_height)
    elif mode == "player":
        if player is None:
            raise ValueError("--eye-from player needs camera.player_pos")
        eye = (float(player[0]), float(player[1]),
               float(player[2]) + eye_height)
    else:
        raise ValueError(f"unknown eye mode {mode!r}")
    # engine basis 0x41e025 (docs/CAMERA.md): e2 is the view direction
    cp, sp = math.cos(pitch), math.sin(pitch)
    cy, sy = math.cos(yaw), math.sin(yaw)
    forward = (sy * cp, cy * cp, -sp)
    look = tuple(eye[k] + forward[k] * 100.0 for k in range(3))
    return {"eye": eye, "forward": forward, "look": look,
            "eye_mode": mode, "yaw": yaw, "pitch": pitch, "roll": roll,
            "eye_height": eye_height, "pos": pos}


def resolve_level(game: str, name: str) -> tuple[str, str]:
    """``LEVEL0`` -> ``(level0.sbw, level0.sbc)``, case-insensitively."""
    for root in (game, MIRROR):
        if not os.path.isdir(root):
            continue
        found = {}
        for entry in os.listdir(root):
            stem, ext = os.path.splitext(entry)
            if stem.lower() == name.lower():
                found[ext.lower()] = os.path.join(root, entry)
        if ".sbw" in found:
            return found[".sbw"], found.get(".sbc", "")
    raise FileNotFoundError(f"level {name!r}: no .sbw under {game} or {MIRROR}")


def parse_focal(text: str) -> tuple[float, float]:
    parts = text.replace("x", ",").replace(";", ",").split(",")
    if len(parts) != 2:
        raise ValueError(f"--focal wants 'fx,fy', got {text!r}")
    return float(parts[0]), float(parts[1])


def default_focal(width: int) -> tuple[float, float]:
    fx = width / 2.0 * FOCAL_X_PER_HALF_WIDTH
    return fx, fx * FOCAL_Y_RATIO


# ---------------------------------------------------------------------------
# oracle preprocessing
# ---------------------------------------------------------------------------

def prepare_oracle(path: str, width: int, height: int,
                   border: int = ORACLE_BORDER_PX):
    """Load the capture, drop the window frame, area-downsample to WxH."""
    img = oc.load_rgb(path)
    sw, sh, _px = img
    if border > 0:
        if sw - 2 * border < 2 or sh - 2 * border < 2:
            raise ValueError(f"{path}: {sw}x{sh} too small for a {border}px border")
        img = oc.crop(img, (border, border, sw - 2 * border, sh - 2 * border))
    return oc.resize_area(img, width, height)


def compare(oracle_path: str, ours_path: str, width: int, height: int,
            border: int = ORACLE_BORDER_PX, world_end: float = WORLD_END) -> dict:
    oracle = prepare_oracle(oracle_path, width, height, border)
    ours = oc.load_rgb(ours_path)
    if (ours[0], ours[1]) != (width, height):
        ours = oc.resize_area(ours, width, height)
    return oc.metrics(oracle, ours, world_end=world_end)


# ---------------------------------------------------------------------------
# render
# ---------------------------------------------------------------------------

def build_scene(game: str, level: str, lightmap: bool = True,
                effects: bool = False, tick: int = 0):
    sbw_path, sbc_path = resolve_level(game, level)
    world = sbw.parse_file(sbw_path)
    lms = None
    if lightmap:
        if not sbc_path or not os.path.exists(sbc_path):
            raise FileNotFoundError(f"no .sbc lightmaps next to {sbw_path}")
        lms = render.import_lightmaps(sbc_path)
    # `effects=True` draws polygons whose texture index is negative with the
    # effect table's frame at `tick` (docs/EFFECTS.md); the default keeps the
    # historical untextured faces so the golden frames do not move.
    return (sbw_path, sbc_path,
            render.level_meshes(world, lms,
                                effects=world.effects if effects else None,
                                tick=tick if effects else None,
                                transform=True))


def render_frame(meshes, frame: dict, width: int, height: int,
                 focal, game: str, flat: bool, lightmap: bool,
                 materials: bool = True, ambient: float = 0.0,
                 chromakey: bool = False, uv: str = "texel",
                 skip_untextured: bool = True,
                 pitch_offset: float = 0.0, fade: float = 0.0) -> bytes:
    pool = None if flat else render.TexturePool(game)
    look = frame["look"]
    if pitch_offset:
        # Sidecars written before 2026-10-23 carry only the *body* pitch
        # (docs/ORACLE.md): the camera looks at `p[0x18] + p[0x2E60]`, and
        # `oracle_fit.py` measures the missing term per frame (+8 deg on win2,
        # +14 on win3).  Applying it here makes the frame comparable.
        pitch = frame["pitch"] + math.radians(pitch_offset)
        cy, sy = math.cos(pitch), math.sin(pitch)
        cp, sp = math.cos(frame["yaw"]), math.sin(frame["yaw"])
        fwd = (sp * cy, cp * cy, -sy)
        look = tuple(frame["eye"][k] + fwd[k] * 100.0 for k in range(3))
    return render.render(meshes, width=width, height=height, pool=pool,
                         pos=frame["eye"], look=look,
                         focal=focal, lightmap=lightmap,
                         materials=materials, ambient=ambient,
                         chromakey=chromakey, uv=uv, fade=fade,
                         skip_untextured=skip_untextured)


# ---------------------------------------------------------------------------
# self-test (no game, no PNG on disk except temp files)
# ---------------------------------------------------------------------------

def _pattern(kind: str, w: int, h: int, cell: int = 16) -> tuple[int, int, bytes]:
    px = bytearray(w * h * 3)
    for y in range(h):
        for x in range(w):
            o = 3 * (y * w + x)
            if kind == "gradient":
                v = (x * 5 + y * 3) % 256
                px[o:o + 3] = bytes((v, (v * 2) % 256, 255 - v))
            elif kind == "shift":          # gradient moved 9 px right
                v = (((x + 9) % w) * 5 + y * 3) % 256
                px[o:o + 3] = bytes((v, (v * 2) % 256, 255 - v))
            elif kind == "noise":
                v = (x * 73 + y * 151 + (x * y) % 251) % 256
                px[o:o + 3] = bytes((v, (v * 7) % 256, (v * 13) % 256))
            elif kind == "cells":          # checker; cell*2 divides both sizes
                v = 220 if ((x // cell) + (y // cell)) % 2 else 40
                px[o:o + 3] = bytes((v, v, v))
    return w, h, bytes(px)


def selftest() -> int:
    checks = []

    def check(name: str, ok: bool, detail: str = "") -> None:
        checks.append((name, ok, detail))

    # 1. metrics: identical images, shifted image, noise
    a = _pattern("gradient", 64, 64)
    same = oc.metrics(a, a)["frame"]
    shifted = oc.metrics(a, _pattern("shift", 64, 64))["frame"]
    noise = oc.metrics(a, _pattern("noise", 64, 64))["frame"]
    check("same corr == 1", abs(same["corr"] - 1.0) < 1e-9, f"{same['corr']}")
    check("same mad == 0", same["mad"] == 0.0, f"{same['mad']}")
    check("same edges == 1", same["edges"] == 1.0, f"{same['edges']}")
    check("same hist == 0", same["hist"] == 0.0, f"{same['hist']}")
    check("shifted corr < 0.999", shifted["corr"] < 0.999, f"{shifted['corr']:.4f}")
    check("noise corr is low", abs(noise["corr"]) < 0.5, f"{noise['corr']:+.4f}")
    check("noise edges < 1", noise["edges"] < 1.0, f"{noise['edges']:.4f}")

    # 1a. haze fade (`SET_CAMERA_HAZE`, docs/SCREEN_FX.md)
    frame_rgb = bytes([10, 20, 30] * 4)
    check("fade 0 keeps the frame", render.fade_rgb(frame_rgb, 0) == frame_rgb)
    check("fade 255 is white", render.fade_rgb(frame_rgb, 255) == b"\xff" * 12,
          repr(render.fade_rgb(frame_rgb, 255)))
    half = render.fade_rgb(b"\x00\x00\x00", 128)
    check("fade 128 on black is mid-grey", half == b"\x80\x80\x80", repr(half))
    check("fade is monotonic",
          render.fade_rgb(frame_rgb, 64)[0] < render.fade_rgb(frame_rgb, 192)[0])

    # 2. oracle preprocessing: border crop + area downsample keep the mix
    with tempfile.TemporaryDirectory() as tmp:
        cells = _pattern("cells", 160, 100, 8)
        padded = bytearray(162 * 102 * 3)      # 1 px black frame
        for y in range(100):
            src = 3 * y * 160
            dst = 3 * ((y + 1) * 162 + 1)
            padded[dst:dst + 3 * 160] = cells[2][src:src + 3 * 160]
        path = os.path.join(tmp, "cells.png")
        from gm import png
        with open(path, "wb") as fh:
            fh.write(png.rgb(162, 102, bytes(padded)))
        small = prepare_oracle(path, 80, 50)
        check("crop drops the 1 px frame", (small[0], small[1]) == (80, 50),
              f"{small[0]}x{small[1]}")
        want = _pattern("cells", 80, 50, 4)[2]   # same checker, half the cells
        same_cells = oc.metrics(small, (80, 50, want))["frame"]["mad"]
        check("area downsample keeps the picture", same_cells < 0.01,
              f"mad={same_cells:.4f}")

        # 3. sidecar parsing / level name resolution
        game = os.path.join(tmp, "game")
        os.makedirs(game)
        open(os.path.join(game, "Level7.sbw"), "wb").close()
        open(os.path.join(game, "Level7.sbc"), "wb").close()
        sbw_path, sbc_path = resolve_level(game, "LEVEL7")
        check("level name resolved case-insensitively",
              sbw_path.endswith("Level7.sbw") and sbc_path.endswith("Level7.sbc"),
              os.path.basename(sbw_path))

        side = {"level": "LEVEL7",
                "camera": {"pos": [10.0, 20.0, 58.0],
                           "player_pos": [10.0, 20.0, 48.0],
                           "yaw": math.pi / 2, "pitch": 0.0, "roll": 0.0,
                           "eye_height": 10.0}}
        side_path = os.path.join(tmp, "s.json")
        with open(side_path, "w", encoding="utf-8") as fh:
            json.dump(side, fh)
        cam = load_sidecar(side_path)["camera"]
        auto = camera_frame(cam)
        check("eye auto: pos already is the eye",
              auto["eye_mode"] == "pos" and auto["eye"][2] == 58.0,
              f"{auto['eye_mode']} z={auto['eye'][2]}")
        feet = camera_frame(cam, "feet")
        check("eye feet: adds eye_height", feet["eye"][2] == 68.0,
              f"z={feet['eye'][2]}")
        fy = camera_frame(cam)["forward"]
        check("forward follows yaw (pi/2 -> +X)",
              abs(fy[0] - 1.0) < 1e-9 and abs(fy[1]) < 1e-9, str(fy))
        nose = {"pos": [0.0, 0.0, 50.0], "yaw": 0.0, "pitch": 0.0,
                "eye_height": 10.0}
        check("eye auto: no player_pos -> feet + eye_height",
              camera_frame(nose)["eye"][2] == 60.0,
              f"z={camera_frame(nose)['eye'][2]}")
        check("focal default at 320x200 is 160,133.33",
              [round(v, 2) for v in default_focal(320)] == [160.0, 133.33],
              str(default_focal(320)))
        check("--focal parses", parse_focal("160,133.3333") == (160.0, 133.3333),
              str(parse_focal("160,133.3333")))

    failed = [name for name, ok, _d in checks if not ok]
    for name, ok, detail in checks:
        print(f"  {'ok  ' if ok else 'FAIL'} {name}" + (f"  [{detail}]" if detail else ""))
    print(f"selftest: {len(checks) - len(failed)}/{len(checks)}")
    return 0 if not failed else 1


# ---------------------------------------------------------------------------
# main
# ---------------------------------------------------------------------------

def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(
        description="Render a frame from an oracle state sidecar and score it.",
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--sidecar", help="oracle state JSON (out/oracle/winN.json)")
    ap.add_argument("--out", help="output PNG (keep it out of git, e.g. /var/tmp)")
    ap.add_argument("--oracle", help="reference PNG to compute metrics against")
    ap.add_argument("--width", type=int, default=320)
    ap.add_argument("--height", type=int, default=200)
    ap.add_argument("--focal", help="focal_x,focal_y in pixels (default 160,133.333)")
    ap.add_argument("--game", default=os.environ.get("GUNMETAL_GAME", DEFAULT_GAME))
    ap.add_argument("--no-lightmap", dest="lightmap", action="store_false",
                    help="ignore the level .sbc lightmaps")
    ap.add_argument("--flat", action="store_true",
                    help="no textures (material colours only)")
    ap.add_argument("--no-materials", dest="materials", action="store_false",
                    help="skip the blended material pass (translucent/add/...)")
    ap.add_argument("--ambient", type=float, default=0.0,
                    help="constant light added to every polygon (0..1 of 127)")
    ap.add_argument("--uv", choices=("fit", "texel"), default="texel",
                    help="texture mapping: the engine texel projection (default, "
                         "docs/TEXUV.md) or the old viewer stretch (fit)")
    ap.add_argument("--keep-untextured", dest="skip_untextured",
                    action="store_false",
                    help="draw polygons with texture==0 anyway (historical "
                         "viewer behaviour; the engine skips them)")
    ap.add_argument("--chromakey", action="store_true",
                    help="chroma-key palette index 0 on textures whose flags "
                         "bit 0 is set (docs/ALPHA.md)")
    ap.add_argument("--eye-from", choices=("auto", "pos", "feet", "player"),
                    default="auto", help="how camera.pos maps to the eye")
    ap.add_argument("--pitch-offset", type=float, default=0.0,
                    help="extra camera pitch in degrees (the missing turret "
                         "pitch of old sidecars, see --fit in oracle_fit.py)")
    ap.add_argument("--effects", action="store_true",
                    help="draw negative-texture polygons with the .sbw effect "
                         "table's frame at --tick (docs/EFFECTS.md)")
    ap.add_argument("--fade", type=float, default=0.0,
                    help="white haze 0..1 (`SET_CAMERA_HAZE`, docs/SCREEN_FX.md)")
    ap.add_argument("--tick", type=int, default=0,
                    help="engine ticks for --effects frame selection (91 Hz)")
    ap.add_argument("--keep-border", action="store_true",
                    help="do not drop the 1 px window frame of the capture")
    ap.add_argument("--world-end", type=float, default=WORLD_END,
                    help="share of height that is world (default 0.70)")
    ap.add_argument("--json", action="store_true")
    ap.add_argument("--self-test", dest="selftest", action="store_true")
    opts = ap.parse_args(argv)

    if opts.selftest:
        return selftest()
    if not opts.sidecar or not opts.out:
        ap.error("--sidecar and --out are required (or --self-test)")

    focal = parse_focal(opts.focal) if opts.focal else default_focal(opts.width)
    border = 0 if opts.keep_border else ORACLE_BORDER_PX
    side = load_sidecar(opts.sidecar)
    frame = camera_frame(side["camera"], opts.eye_from)
    sbw_path, sbc_path, meshes = build_scene(opts.game, side["level"],
                                            opts.lightmap, opts.effects,
                                            opts.tick)
    data = render_frame(meshes, frame, opts.width, opts.height, focal,
                        opts.game, opts.flat, opts.lightmap,
                        opts.materials, opts.ambient, opts.chromakey,
                        opts.uv, opts.skip_untextured, opts.pitch_offset,
                        opts.fade)
    out_dir = os.path.dirname(os.path.abspath(opts.out))
    os.makedirs(out_dir, exist_ok=True)
    with open(opts.out, "wb") as fh:
        fh.write(data)

    result = {
        "sidecar": opts.sidecar,
        "out": opts.out,
        "level": side["level"],
        "sbw": sbw_path,
        "sbc": sbc_path if opts.lightmap else None,
        "size": [opts.width, opts.height],
        "focal": [round(focal[0], 4), round(focal[1], 4)],
        "eye_mode": frame["eye_mode"],
        "eye": [round(v, 4) for v in frame["eye"]],
        "forward": [round(v, 6) for v in frame["forward"]],
        "lightmap": opts.lightmap,
        "materials": opts.materials,
        "chromakey": opts.chromakey,
        "uv": opts.uv,
        "pitch_offset_deg": opts.pitch_offset,
        "skip_untextured": opts.skip_untextured,
        "ambient": opts.ambient,
        "textures": not opts.flat,
        "meshes": len(meshes),
        "oracle_border_px": border,
        "world_end": opts.world_end,
        "oracle": opts.oracle,
    }
    if opts.oracle:
        metrics = compare(opts.oracle, opts.out, opts.width, opts.height,
                          border, opts.world_end)
        result["metrics"] = metrics
        w = metrics["world"]
        result["heuristic_pass"] = w["corr"] >= 0.9 and w["edges"] >= 0.5

    if opts.json:
        print(json.dumps(result, ensure_ascii=False, indent=2))
        return 0

    print(f"sidecar {opts.sidecar}  level {side['level']}")
    print(f"sbw     {sbw_path}")
    print(f"eye     {frame['eye'][0]:.3f}, {frame['eye'][1]:.3f}, "
          f"{frame['eye'][2]:.3f} ({frame['eye_mode']})  "
          f"yaw {frame['yaw']:.5f}  pitch {frame['pitch']:.5f}")
    print(f"camera  {opts.width}x{opts.height}  focal "
          f"{focal[0]:.4f},{focal[1]:.4f}  lightmap={opts.lightmap} "
          f"textures={not opts.flat}  meshes={len(meshes)}")
    print(f"wrote   {opts.out}")
    if opts.oracle:
        print(f"oracle  {opts.oracle} (crop {border}px, area-average "
              f"-> {opts.width}x{opts.height}, world <= {opts.world_end:.2f})")
        for band_name in ("frame", "world"):
            m = result["metrics"][band_name]
            print(f"{band_name:6s} corr={m['corr']:+.4f} mad={m['mad']:.4f} "
                  f"edges={m['edges']:.4f} hist={m['hist']:.4f}")
        print("heuristic:", "match" if result["heuristic_pass"] else "differs",
              "(not a proof — materials/textures are still missing)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
