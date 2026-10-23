#!/usr/bin/env python3
"""Which fade level (``player+0x81c24c``) does a capture show?

The cockpit band is ``PANEL`` blitted through ``LUT[k]`` (``0x491f2d``,
``docs/HUD_AUDIT.md`` 2.8), so a capture's band is the panel art in the
palette of table ``k``.  ``k`` is a runtime field that ``tools/win/state.ps1``
does not dump yet, which is why the oracle renders have to be told
``--hud-level``; this tool recovers it from the picture alone.

For every level 0..15 the panel index of each band pixel is mapped through
that table and turned into RGB with the installed palette; the level whose
colours are closest to the capture's wins (mean absolute channel difference).

Pixels the text/icon pass paints over are skipped: they are drawn *after* the
blit with raw indices (no fade), so they say nothing about ``k``.  The top of
the band is skipped as well -- rows 130..145 are 3D cockpit, not ``PANEL``.

    python3 tools/re/hud_level.py out/oracle/win2.png [--state JSON]
"""

from __future__ import annotations

import argparse
import json
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", ".."))

from gm import hud, palette  # noqa: E402

DEFAULT_GAME = "/mnt/d/games/Gunmetal 3dfx"
#: band rows where the panel art is visible (see docs/HUD_AUDIT.md 2.9)
DEFAULT_ROWS = (146, 200)


def band_pixels(capture: str, size=(320, 200)):
    """Capture -> ``(width, height, rgb)`` on the comparison grid."""
    sys.path.insert(0, os.path.dirname(__file__))
    import frame_metric

    return frame_metric.prepare_oracle(capture, size)


def overlay_mask(width: int, height: int, panel, palette_bytes: bytes,
                 lut: bytes, top: int, state: dict, game: str) -> set:
    """Band pixels the text/icon pass repaints (they are not faded)."""
    text = tuple(hud.hud_text(state))
    fonts = {number: hud.load_font(game, number) for number, _, _, _ in text}
    icons = tuple(hud.hud_icons(state))
    icon_pixels = {name: hud.load_icon(game, name) for name, _, _ in icons}
    damage = hud.load_damage_icon(game, int(state.get(
        "damage_stage", hud.HUD_DAMAGE_STAGE)))
    common = dict(width=width, height=height, panel=panel,
                  palette=palette_bytes, top=top, lut=lut)
    art = hud.compose(bytes(width * height * 3), **common)
    full = hud.compose(bytes(width * height * 3),
                       text=text, fonts=fonts,
                       icons=icons, icon_pixels=icon_pixels,
                       gauges=tuple(hud.hud_gauges(state)),
                       bars=tuple(hud.hud_bars(state)),
                       damage_icon=damage, **common)
    return {i for i in range(width * height)
            if art[i * 3:i * 3 + 3] != full[i * 3:i * 3 + 3]}


def score_level(art: bytes, capture: tuple, rows, mask) -> float:
    """Mean absolute channel difference over the visible band pixels."""
    width, height, rgb = capture
    total = 0
    count = 0
    for y in range(rows[0], min(rows[1], height)):
        for x in range(width):
            index = y * width + x
            if index in mask:
                continue
            expected = art[index * 3:index * 3 + 3]
            observed = rgb[index * 3:index * 3 + 3]
            total += (abs(expected[0] - observed[0])
                      + abs(expected[1] - observed[1])
                      + abs(expected[2] - observed[2]))
            count += 1
    return total / count if count else float("inf")


def measure_image(capture_img, game: str = DEFAULT_GAME,
                  lut_path: str | None = None, top: int = hud.PANEL_TOP,
                  rows=DEFAULT_ROWS, state: dict | None = None) -> dict:
    """Same as :func:`measure` for an already prepared ``(w, h, rgb)``."""
    lut_path = lut_path or os.path.join(
        os.path.expanduser("~"), ".cache", "gunmetal-re", "hud_lut.bin")
    st = dict(hud.HUD_STATE)
    st.update(hud.HUD_GAUGES)
    st.update(hud.HUD_BAR_STATE)
    st.update(state or {})
    panel = hud.engine_order(hud.load_panel(game))
    palette_bytes = hud.palette_bytes(game)
    width, height = capture_img[0], capture_img[1]
    scores = []
    for level in range(16):
        lut = hud.load_lut(lut_path, level)
        mask = overlay_mask(width, height, panel, palette_bytes, lut, top, st,
                            game)
        art = hud.compose(bytes(width * height * 3), width=width,
                          height=height, panel=panel,
                          palette=palette_bytes, top=top, lut=lut)
        scores.append(score_level(art, capture_img, rows, mask))
    best = min(range(16), key=lambda k: scores[k])
    ordered = sorted(scores)
    return {"scores": scores, "best": best,
            "margin": ordered[1] - ordered[0] if len(ordered) > 1 else 0.0}


def measure(capture: str, game: str = DEFAULT_GAME,
            lut_path: str | None = None, top: int = hud.PANEL_TOP,
            rows=DEFAULT_ROWS, state: dict | None = None) -> dict:
    """``{"scores": [...16], "best": k, "margin": ...}`` for one capture."""
    return measure_image(band_pixels(capture), game, lut_path, top, rows,
                         state)


def self_test(game: str = DEFAULT_GAME, lut_path: str | None = None) -> int:
    """Round trip: a band composed at level k must be measured as k."""
    lut_path = lut_path or os.path.join(
        os.path.expanduser("~"), ".cache", "gunmetal-re", "hud_lut.bin")
    panel = hud.engine_order(hud.load_panel(game))
    palette_bytes = hud.palette_bytes(game)
    st = dict(hud.HUD_STATE)
    st.update(hud.HUD_GAUGES)
    st.update(hud.HUD_BAR_STATE)
    text = tuple(hud.hud_text(st))
    fonts = {number: hud.load_font(game, number) for number, _, _, _ in text}
    icons = tuple(hud.hud_icons(st))
    icon_pixels = {name: hud.load_icon(game, name) for name, _, _ in icons}
    damage = hud.load_damage_icon(game, hud.HUD_DAMAGE_STAGE)
    failed = 0
    for level in (0, 4, 7, 12):
        lut = hud.load_lut(lut_path, level)
        frame = hud.compose(bytes(320 * 200 * 3), width=320, height=200,
                            panel=panel, palette=palette_bytes,
                            top=hud.PANEL_TOP, lut=lut, text=text, fonts=fonts,
                            icons=icons, icon_pixels=icon_pixels,
                            gauges=tuple(hud.hud_gauges(st)),
                            bars=tuple(hud.hud_bars(st)), damage_icon=damage)
        got = measure_image((320, 200, frame), game, lut_path, hud.PANEL_TOP,
                            DEFAULT_ROWS, st)
        ok = got["best"] == level
        failed += 0 if ok else 1
        print(f"  {'ok  ' if ok else 'FAIL'} band composed at level {level} "
              f"is measured as {got['best']}")
    return failed


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("capture", nargs="?")
    ap.add_argument("--game", default=DEFAULT_GAME)
    ap.add_argument("--lut", default=None)
    ap.add_argument("--top", type=int, default=hud.PANEL_TOP)
    ap.add_argument("--rows", default=f"{DEFAULT_ROWS[0]}:{DEFAULT_ROWS[1]}",
                    help="band rows to score (default 146:200)")
    ap.add_argument("--state", default=None, help="HUD state JSON")
    ap.add_argument("--self-test", action="store_true",
                    help="round-trip synthetic bands instead of a capture")
    opts = ap.parse_args(argv)
    if opts.self_test:
        failed = self_test(opts.game, opts.lut)
        print(f"{4 - failed}/4 checks passed" if not failed
              else f"{4 - failed}/4 checks passed, {failed} failed")
        return 1 if failed else 0
    lo, _, hi = opts.rows.partition(":")
    rows = (int(lo), int(hi or 200))
    state = json.loads(opts.state) if opts.state else None
    res = measure(opts.capture, opts.game, opts.lut, opts.top, rows, state)
    for level, score in enumerate(res["scores"]):
        mark = "  <-- best" if level == res["best"] else ""
        print(f"  level {level:2d}: mean |d| {score:6.1f}{mark}")
    print(f"{opts.capture}: level {res['best']} "
          f"(margin {res['margin']:.1f})")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
