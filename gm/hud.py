"""Cockpit HUD panel composition for the reference frames (M4.4.5).

The original 320x200 frame is the world render plus the cockpit panel that
covers the bottom 78 rows: ``PANEL`` from ``FRONTEND/BMP.IDX`` (320x78, 8 bpp)
drawn at ``(0, 122)`` with palette index 0 as the transparent key
(``docs/HUD_AUDIT.md`` section 6).  ``gm/render.py`` draws the world only, so
the oracle frames miss the whole bottom band and every ``hud`` metric is
meaningless; this module adds the panel without touching the renderer.

Only the panel is covered here.  Text, icons and gauges are separate steps of
``docs/HUD_AUDIT.md`` section 6 and are not implemented yet.

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
            lut: bytes | None = None) -> bytes:
    """Blit ``panel`` onto a packed RGB frame; index 0 stays transparent.

    ``lut`` is the engine's 256-byte fade table for one level: the blit maps
    every panel index through it before the palette lookup (``0x491f2d``,
    ``FUN_00a1b510(..., &DAT_00682f90 + k*0x100)``).  Without it the raw panel
    comes out unshaded, which is what made the earlier comparison fail.

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
    return bytes(out)


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
                       xphase: float = 2.0) -> dict:
    """Fraction of band pixels whose screen index equals ``lut[panel]``.

    This compares *indices* against a raw screen dump (``gm_screen.bin``), so
    neither dgVoodoo scaling nor the palette is involved: the only expected
    mismatches are the text/icons drawn over the panel afterwards.
    """
    if len(screen) != sw * sh:
        raise ValueError(f"screen is {len(screen)} bytes, expected {sw * sh}")
    if len(lut) < 256:
        raise ValueError("lut must be 256 bytes")
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
            if screen[base + sx] == lut[panel.indices[pbase + px]]:
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
    ap.add_argument("--panel-buffer", metavar="FILE",
                    help="dumped engine panel buffer (gm_panel.bin, 320x78 "
                         "indices) instead of the raw FRONTEND asset")
    ap.add_argument("--scale", type=float, default=3.0,
                    help="vertical scale of the band in --check-screen "
                         "(default 3.0 for 800x600)")
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
    lut = load_lut(opts.lut, opts.level) if opts.lut else None
    if opts.check_frame:
        frame = load_rgb(opts.check_frame)
        engine = palette_bytes(opts.game)
        for tag, pal in (("GAMEPAL", engine), ("bmp", panel.palette)):
            res = frame_ncc(frame, panel, pal, top=opts.top)
            print(f"{opts.check_frame}: {tag} ncc={res['ncc']:+.3f} "
                  f"rows={res['rows'][0]}..{res['rows'][1]} "
                  f"samples={res['samples']} scale={res['scale']}")
    if opts.check_screen:
        data = open(opts.check_screen, "rb").read()
        w, h = 800, len(data) // 800
        if len(data) != w * h:
            raise SystemExit(f"{opts.check_screen}: not a 800x600 8bpp dump")
        if lut is None:
            raise SystemExit("--check-screen needs --lut")
        res = screen_index_match(data, w, h, panel, lut, opts.top, opts.scale)
        print(f"{opts.check_screen}: index match {100*res['match']:.1f}% "
              f"({res['pixels']} px, top={res['top']}, scale={res['scale']})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
