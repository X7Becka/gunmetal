#!/usr/bin/env python3
"""Semantics of the Gunmetal UI markup (``.sml`` / ``.sme``).

The tool is deliberately read-only and stdlib-only (apart from the project's
own :mod:`gm.sml` parser).  It answers the questions a future HUD/UI port
needs:

* what a document declares -- text, fonts, music, images and their alignment;
* which ``.sme`` files are encoded, which need a transport repair, and which
  ``.sml`` sibling they match (or differ from);
* which loose ``SML/*.bmp`` image a ``<I f=...>`` resolves to;
* what the front-end state machine and its menus look like (the screen table
  is static evidence from ``gundx.exe``, not guessed from filenames);
* how the SML images relate to ``FRONTEND/BMP.IDX``, ``sprites/SBM.IDX``,
  ``PICS/SBM.IDX`` and the HUD films (``Film/*.sof``).

Examples::

    python3 tools/re/ui_semantics.py --check
    python3 tools/re/ui_semantics.py --json > /tmp/ui.json
    python3 tools/re/ui_semantics.py --dump --limit 3
    python3 tools/re/ui_semantics.py --screens
    python3 tools/re/ui_semantics.py --assets
    python3 tools/re/ui_semantics.py --self-test

``GUNMETAL_GAME`` (default ``/mnt/d/games/Gunmetal 3dfx``) points at the
reference installation.  Nothing is written to it.
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
sys.path.insert(0, str(ROOT))

from gm import sml  # noqa: E402

DEFAULT_GAME = Path(os.environ.get("GUNMETAL_GAME", "/mnt/d/games/Gunmetal 3dfx"))
EXPECTED_SME = 151
EXPECTED_SML = 149
EXPECTED_PAIRS = 147

#: Shipped files that are genuinely malformed (not transport damage).  The
#: engine would reject them too; they are reported but do not make --check fail.
KNOWN_ANOMALIES = {
    "prelevel/dm4.sml": "no closing </d>; the .SME sibling is well-formed and is what the engine loads",
}

#: Engine entry points proven by disassembly (VA in gundx.exe, image base
#: 0x400000).  Kept here so JSON consumers do not have to re-reverse them.
ENGINE = {
    "sml_load": 0x497310,
    "sml_decode_and_index": 0x497650,
    "sml_layout": 0x497880,
    "sml_measure": 0x497c20,
    "sml_draw_text": 0x497b60,
    "sml_dispatch": 0x497d30,
    "sml_init_page": 0x4972f0,
    "sml_page_first": 0x498600,
    "sml_page_next": 0x4986b0,
    "sml_page_prev": 0x498750,
    "font_measure": 0x47fa50,
    "font_draw": 0x47faf0,
    "menu_item_draw": 0x485dd0,
    "screen_state": 0x4BC440,
    "screen_jump_table": 0x4883D8,
    "menu_selection": 0x68CCF4,
    "menu_plate": 0x6865F8,
    "encode_flag": 0x68F19C,
    "decode_flag": 0x68F1A0,
}

#: Screen ids handled by the jump table at 0x4883d8 (ids 5..0x25).  Ids 0..4
#: are driver/briefing states; ids with no handler are transitions.  Titles
#: are the engine's own strings (addresses in parentheses).
SCREENS = [
    {"id": 0, "handler": None, "title": "boot / title loop", "note": "not a menu"},
    {"id": 1, "handler": None, "title": "front-end main loop", "note": "briefing trigger"},
    {"id": 2, "handler": 0x487B69, "title": "SML briefing / help pager", "note": "0x4972f0 + 0x498600/6b0/750"},
    {"id": 3, "handler": 0x4882D6, "title": "briefing transition", "note": "driver state"},
    {"id": 4, "handler": 0x4882D1, "title": "briefing transition", "note": "driver state"},
    {"id": 5, "handler": 0x48ACD0, "title": "ERROR", "note": "escape returns to menu"},
    {"id": 6, "handler": 0x488850, "title": "RESTART GAME", "note": None},
    {"id": 10, "handler": 0x488BA0, "title": "CONTROL OPTIONS", "note": None},
    {"id": 11, "handler": 0x4889F0, "title": "SOUND OPTIONS", "note": None},
    {"id": 12, "handler": 0x485EE0, "title": "GRAPHICS OPTIONS", "note": "gamma / light maps"},
    {"id": 13, "handler": 0x48AA60, "title": "START GAME", "note": "single/multiplayer entries"},
    {"id": 14, "handler": 0x48ABD0, "title": "SAVE GAME", "note": None},
    {"id": 16, "handler": 0x488E90, "title": "KEY JAMMING", "note": None},
    {"id": 17, "handler": 0x48A3C0, "title": "SELECT LEVEL FOR NEW GAME", "note": None},
    {"id": 18, "handler": 0x48A8B0, "title": "SELECT VIDEO MODE", "note": None},
    {"id": 19, "handler": 0x48A580, "title": "MULTIPLAYER GAME TYPE", "note": None},
    {"id": 20, "handler": 0x48A830, "title": "SHAREWARE VERSION", "note": None},
    {"id": 21, "handler": 0x48A780, "title": "NUMBER OF LOCAL PLAYERS", "note": None},
    {"id": 22, "handler": 0x489A10, "title": "MOUSE SETUP", "note": "buttons/axes"},
    {"id": 23, "handler": 0x48A9A0, "title": "SET DIFFICULTY LEVEL", "note": None},
    {"id": 24, "handler": 0x489020, "title": "LOCAL PLAYER ONE NAME", "note": "arg 0"},
    {"id": 25, "handler": 0x489020, "title": "LOCAL PLAYER TWO NAME", "note": "arg 1"},
    {"id": 26, "handler": 0x488F70, "title": "JOYSTICK CENTERING", "note": None},
    {"id": 27, "handler": 0x4885C0, "title": "MULTIPLAY GAME OPTIONS", "note": None},
    {"id": 30, "handler": 0x489850, "title": "MOUSE SETUP", "note": "binding list"},
    {"id": 32, "handler": 0x486170, "title": "PLAY OPTIONS", "note": None},
    {"id": 33, "handler": 0x488780, "title": "QUIT OPTIONS", "note": None},
    {"id": 34, "handler": 0x4892D0, "title": "key set editor (player 1)", "note": None},
    {"id": 35, "handler": 0x4892D0, "title": "key set editor (player 2)", "note": None},
    {"id": 36, "handler": 0x489D60, "title": "PLAYER 1 JOYSTICK SETUP", "note": None},
    {"id": 37, "handler": 0x488990, "title": "NEW VIDEO MODE OK?", "note": None},
]

#: FRONTEND/BMP.IDX assets whose dimensions were pinned in 0x48fa..0x49019f.
#: The loader call's width/height arguments agree with the BMP headers.
UI_ATLAS = [
    ("f_m_titl", 232, 52, "title art"),
    ("scroll", 404, 200, "menu background"),
    ("controls", 112, 25, "menu entry"),
    ("sound", 112, 25, "menu entry"),
    ("graphics", 112, 25, "menu entry"),
    ("startgam", 112, 25, "menu entry"),
    ("quitgame", 112, 25, "menu entry"),
    ("savegame", 112, 25, "menu entry"),
    ("play", 112, 25, "menu entry"),
    ("plate", 196, 131, "menu plate, text is composed into a copy"),
    ("slide", 124, 10, "slider track"),
    ("knob", 72, 8, "slider knob strip (8 knots)"),
    ("check", 8, 16, "checkbox on"),
    ("nocheck", 8, 8, "checkbox off"),
    ("menulit", 16, 16, "selected-item marker"),
    ("menuarrw", 16, 8, "arrow"),
    ("equip%d", 24, 24, "equipment icons"),
    ("zoom", 128, 128, "zoom overlay"),
    ("detector", 16, 16, "mine detector overlay"),
    ("target", 24, 16, "target overlay"),
    ("panel", 320, 78, "HUD panel"),
    ("panelmid", 320, 8, "HUD panel centre"),
    ("paneltop", 320, 25, "HUD panel top"),
    ("radar", 64, 75, "radar"),
    ("braintile", 80, 125, "brain/RPV portrait"),
    ("kidtile", 80, 16, "kill tile"),
    ("fonts1", 512, 168, "font atlas"),
    ("fonts2", 1024, 80, "font atlas"),
    ("fonts3", 520, 56, "font atlas"),
    ("fonts4", 512, 168, "font atlas"),
    ("newfonts", 512, 168, "font atlas"),
    ("numfonts", 180, 48, "numeric font atlas"),
]


# ------------------------------------------------------------------ helpers
def _norm(text: str) -> str:
    return text.replace("\r\n", "\n").replace("\r", "\n")


def _read_bigfile(idx: Path) -> dict:
    """Parse a ``BIGFILE`` index (stdlib only); returns lower name -> record."""
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
        out[name.lower()] = {
            "name": name,
            "offset": start,
            "size": max(0, end - start),
            "dat": str(dat),
        }
    return out


def _bmp_info(blob: bytes) -> dict | None:
    if len(blob) < 30 or blob[:2] != b"BM":
        return None
    width, height = struct.unpack_from("<ii", blob, 18)
    bpp = struct.unpack_from("<H", blob, 28)[0]
    return {"width": abs(width), "height": abs(height), "bpp": bpp,
            "top_down": height < 0}


def _sbm_info(blob: bytes) -> dict | None:
    if len(blob) < 12:
        return None
    _zero, log2w, log2h, levels, _zero2, flags = struct.unpack_from(
        "<IBBBBI", blob, 0)
    if log2w > 12 or log2h > 12:
        return None
    return {"width": 1 << log2w, "height": 1 << log2h,
            "levels": levels + 1, "flags": flags}


def _with_header(idx: Path, parser) -> dict:
    """Read a BIGFILE index and parse each entry's leading header."""
    entries = _read_bigfile(idx)
    dat = idx.with_suffix(".DAT")
    if not dat.is_file():
        dat = idx.with_suffix(".dat")
    if not dat.is_file():
        return entries
    with open(dat, "rb") as fh:
        for rec in entries.values():
            fh.seek(rec["offset"])
            head = fh.read(min(64, rec["size"]))
            info = parser(head)
            if info:
                rec.update(info)
    return entries


def _read_fdx(path: Path) -> list:
    out = []
    try:
        for line in path.read_text(encoding="latin1").splitlines():
            m = re.match(r"^\s*(\d+)\s*,\s*(\d+)\s*$", line)
            if m:
                out.append({"count": int(m.group(1)), "fps": int(m.group(2))})
    except OSError:
        pass
    return out


def _asset_report(game: Path, limit: int = 0) -> dict:
    sml_dir = game / "SML"
    front = _with_header(game / "FRONTEND/BMP.IDX", _bmp_info)
    sprites = _with_header(game / "sprites/SBM.IDX", _sbm_info)
    pics = _with_header(game / "PICS/SBM.IDX", _sbm_info)

    loose = {}
    if sml_dir.is_dir():
        for p in sorted(sml_dir.glob("*")):
            if p.suffix.lower() == ".bmp":
                info = _bmp_info(p.read_bytes())
                if info:
                    loose[p.stem.lower()] = {"name": p.name, **info,
                                             "path": str(p)}

    films = []
    film_dir = game / "Film"
    if film_dir.is_dir():
        for p in sorted(film_dir.glob("*")):
            if p.suffix.lower() != ".sof":
                continue
            size = p.stat().st_size
            rec = {"name": p.name, "bytes": size,
                   "frames": size // 0x2000, "tail": size % 0x2000,
                   "fdx": []}
            base = p.stem
            for idx in (film_dir / (base + ".fdx"),
                        film_dir / (base + ".idx")):
                if idx.is_file():
                    rec["fdx"] = _read_fdx(idx)
                    rec["fdx_path"] = str(idx)
                    break
            films.append(rec)

    def names(entries):
        return [entries[k]["name"] for k in sorted(entries)]

    return {
        "frontend": {"idx": str(game / "FRONTEND/BMP.IDX"),
                     "count": len(front),
                     "entries": [front[k] for k in sorted(front)]},
        "sprites": {"idx": str(game / "sprites/SBM.IDX"),
                    "count": len(sprites),
                    "entries": [sprites[k] for k in sorted(sprites)]},
        "pics": {"idx": str(game / "PICS/SBM.IDX"),
                 "count": len(pics),
                 "names": names(pics)},
        "sml_bmps": [loose[k] for k in sorted(loose)],
        "films": films,
        "_by_name": {"frontend": front, "sprites": sprites, "pics": pics,
                     "loose": loose},
    }


# ------------------------------------------------------------------- report
def _role(rel: str) -> str:
    top = rel.split("/", 1)[0]
    if "/" not in rel:
        return "root"
    return top if top in ("brief", "prelevel", "design") else "other"


def _level(rel: str) -> int | None:
    m = re.search(r"level[_-]?(\d+)", rel, re.I)
    return int(m.group(1)) if m else None


def _scan(sml_dir: Path, assets: dict) -> dict:
    by_name = assets.get("_by_name", {})
    front = by_name.get("frontend", {})
    sprites = by_name.get("sprites", {})
    pics = by_name.get("pics", {})
    loose = by_name.get("loose", {})

    files = []
    for path in sorted(sml_dir.rglob("*")):
        if path.suffix.lower() not in (".sml", ".sme"):
            continue
        rel = path.relative_to(sml_dir).as_posix()
        blob = path.read_bytes()
        encoded = path.suffix.lower() == ".sme"
        if encoded:
            text, note = sml.decode_best(blob)
        else:
            text, note = blob.decode("latin1"), "plain"
        desc = sml.describe(text)
        warnings = []
        if note == "unrepaired":
            warnings.append("no <d>...</d> decode")
        if not desc["doc"]["ok"]:
            warnings.append("missing <d>/</d> wrapper")
        if desc["unknown_tags"]:
            warnings.append("unknown tags: " + ",".join(desc["unknown_tags"]))
        if desc["too_many_tags"]:
            warnings.append(f"too many tags ({desc['tags']} >= {sml.MAX_TAGS})")
        if not desc["brackets"]["balanced"]:
            warnings.append("unbalanced < >")

        images = []
        for img in desc["images"]:
            name = str(img["value"] or "")
            key = name.lower()
            bmp = loose.get(key)
            rec = dict(img)
            rec.update({
                "asset": bmp["name"] if bmp else None,
                "asset_size": (bmp["width"], bmp["height"]) if bmp else None,
                "in_frontend": bool(front.get(key)),
                "in_sprites": bool(sprites.get(key)),
                "in_pics": bool(pics.get(key)),
            })
            if not bmp:
                warnings.append(f"missing SML image {name}.bmp")
            if img["align"] not in ("l", "r"):
                warnings.append(f"image {name}: align {img['align']!r}")
            if img["x"] is None or img["y"] is None:
                warnings.append(f"image {name}: no x/y")
            images.append(rec)

        fonts = []
        for font in desc["fonts"]:
            value = font["value"]
            if value is None:
                warnings.append("font tag without s=")
            elif not (0 <= value <= 30):
                warnings.append(f"font out of range: {value}")
            fonts.append(font)

        music = []
        for m in desc["music"]:
            if m["track"] is None and m["offset"] is None:
                warnings.append("music tag without B/S")
            music.append(m)

        files.append({
            "path": rel,
            "bytes": len(blob),
            "encoded": encoded,
            "decode": note,
            "role": _role(rel),
            "level": _level(rel),
            "doc_ok": desc["doc"]["ok"],
            "tags": desc["tags"],
            "tag_counts": desc["tag_counts"],
            "unknown_tags": desc["unknown_tags"],
            "text_runs": desc["text_runs"],
            "images": images,
            "fonts": fonts,
            "music": music,
            "warnings": warnings,
            "preview": desc["text_preview"],
            "plain": desc["plain"],
            "markup": text,
        })

    # pair .sme/.sml by path stem (case-insensitive; dirs keep their case)
    stems: dict = {}
    for rec in files:
        stem = rec["path"].rsplit(".", 1)[0].lower()
        stems.setdefault(stem, {})[rec["path"].rsplit(".", 1)[1].lower()] = rec

    pairs = []
    for stem, recs in sorted(stems.items()):
        if "sme" not in recs:
            continue
        sme = recs["sme"]
        sml_rec = recs.get("sml")
        if not sml_rec:
            pairs.append({"stem": stem, "status": "unpaired",
                          "sme": sme["path"], "sml": None})
            continue
        expected = _norm(sml_rec["markup"])
        raw_sme = open(sml_dir / sme["path"], "rb").read()
        candidates = sml.decode_candidates(raw_sme)
        if not candidates:
            # dm4: malformed but still comparable as raw decoded text
            candidates = [(sme["decode"], sml.decode_text(raw_sme))]
        match = next((c for c in candidates if _norm(c[1]) == expected), None)
        if sme["decode"] == "as-is" and _norm(sme["markup"]) == expected:
            status, note = "identical", "as-is"
        elif match:
            note = match[0]
            if note == "crlf-strip":
                status = "line-endings"
            elif note == "newline-repair":
                status = "newline-repair"
            else:
                status = "identical"
        else:
            status, note = "revision", sme["decode"]
        first_diff = None
        a, b = _norm(sme["markup"]), expected
        for i, (ca, cb) in enumerate(zip(a, b)):
            if ca != cb:
                first_diff = i
                break
        if first_diff is None and len(a) != len(b):
            first_diff = min(len(a), len(b))
        pairs.append({
            "stem": stem,
            "status": status,
            "decode": note,
            "sme": sme["path"],
            "sml": sml_rec["path"],
            "sme_bytes": sme["bytes"],
            "sml_bytes": sml_rec["bytes"],
            "sme_chars": len(sme["markup"]),
            "sml_chars": len(sml_rec["markup"]),
            "first_diff": first_diff,
            "note": ("SME is the corrected/expanded text" if stem.endswith("help")
                     else "shipped CR/LF anomaly" if stem.endswith("brief/level17")
                     else "LF-encoded, CRLF-transported" if status == "line-endings"
                     else None),
        })

    counts = {
        "files": len(files),
        "sme": sum(1 for f in files if f["encoded"]),
        "sml": sum(1 for f in files if not f["encoded"]),
        "pairs": sum(1 for p in pairs if p["status"] != "unpaired"),
        "unpaired": sum(1 for p in pairs if p["status"] == "unpaired"),
        "identical": sum(1 for p in pairs if p["status"] == "identical"),
        "revisions": sum(1 for p in pairs if p["status"] == "revision"),
        "line_endings": sum(1 for p in pairs if p["status"] == "line-endings"),
        "newline_repair": sum(1 for f in files if f["decode"] == "newline-repair"),
        "images": sum(len(f["images"]) for f in files),
        "unique_images": len({i["value"].lower() for f in files for i in f["images"]
                              if i["value"]}),
        "font_values": sorted({i["value"] for f in files for i in f["fonts"]
                               if i["value"] is not None}),
        "music_tags": sum(len(f["music"]) for f in files),
        "warnings": sum(len(f["warnings"]) for f in files),
    }
    return {"files": files, "pairs": pairs, "counts": counts}


def _report(game: Path, sml_dir: Path, with_assets: bool = True) -> dict:
    assets = _asset_report(game) if with_assets else {"_by_name": {}}
    scan = _scan(sml_dir, assets)
    return {
        "game": str(game),
        "sml_dir": str(sml_dir),
        "engine": {k: hex(v) for k, v in ENGINE.items()},
        "screens": SCREENS,
        "counts": scan["counts"],
        "files": [{k: v for k, v in f.items() if k != "markup"}
                  for f in scan["files"]],
        "pairs": scan["pairs"],
        "assets": {k: v for k, v in assets.items() if not k.startswith("_")},
    }


# ------------------------------------------------------------------- checks
def _self_test() -> list:
    """Negative/positive parser cases; returns a list of failure strings."""
    failures = []

    def require(cond, msg):
        if not cond:
            failures.append(msg)

    require(sml.describe("<d><z>x</z></d>")["unknown_tags"] == ["z"],
            "unknown tag z not reported")
    require(not sml.describe("<p>x</p>")["doc"]["ok"],
            "missing <d> wrapper accepted")
    require(not sml.describe("<d><p>x")["doc"]["ok"],
            "missing </d> accepted")
    require(sml.describe("<d>" + "<r>" * sml.MAX_TAGS + "</d>")["too_many_tags"],
            "tag limit not enforced")
    require(sml.describe("<d><i x=\"1\" y=\"2\" f=\"x\" a=\"q\"></d>")["images"][0]["align"] == "q",
            "image align not preserved")
    require(len(sml.elements("<d><i x='1' y='2' f='a' a='l'></d>")) >= 1,
            "elements dropped an image")

    # LF-encoded, CRLF-transported file (LEVEL20/LEVEL4 shape).
    plain = "<d><p>Hello world.</p>\n<p>Second line.</p></d>\n"
    transported = sml.encode(plain.encode("latin1")).replace(b"\n", b"\r\n")
    text, note = sml.decode_best(transported)
    require(note == "crlf-strip", f"CRLF transport: got {note}")
    require(text == plain, "CRLF transport text differs")

    # One inserted CR/LF after encoding (level17 shape).
    plain2 = "<d><p>" + "A" * 60 + " " + "B" * 60 + "</p></d>"
    enc = sml.encode(plain2.encode("latin1"))
    cut = plain2.index(" ")
    mangled = enc[:cut] + b"\r\n" + enc[cut + 1:]
    text2, note2 = sml.decode_best(mangled)
    require(note2 == "newline-repair", f"newline repair: got {note2}")
    require(text2 == plain2, "newline repair text differs")

    require(sml.decode_auto(b"")[1] is False, "decode_auto on empty input")
    return failures


def _check(report: dict, expect_counts: bool = True) -> list:
    errors = []
    counts = report["counts"]
    if expect_counts:
        for key, want in (("sme", EXPECTED_SME), ("sml", EXPECTED_SML),
                          ("pairs", EXPECTED_PAIRS)):
            if counts[key] != want:
                errors.append(f"count {key}: got {counts[key]}, want {want}")
    for f in report["files"]:
        known = f["path"] in KNOWN_ANOMALIES
        if f["decode"] == "unrepaired" and not known:
            errors.append(f"{f['path']}: unrepaired decode")
        if not f["doc_ok"] and not known:
            errors.append(f"{f['path']}: missing <d>/</d>")
        if f["unknown_tags"]:
            errors.append(f"{f['path']}: unknown tags {f['unknown_tags']}")
        if f["tags"] >= sml.MAX_TAGS:
            errors.append(f"{f['path']}: {f['tags']} tags (limit {sml.MAX_TAGS})")
        for img in f["images"]:
            if not img["asset"]:
                errors.append(f"{f['path']}: image {img['value']}.bmp missing")
            if img["align"] not in ("l", "r"):
                errors.append(f"{f['path']}: image {img['value']} align {img['align']!r}")
            if img["x"] is None or img["y"] is None:
                errors.append(f"{f['path']}: image {img['value']} lacks x/y")
        for font in f["fonts"]:
            if font["value"] is None:
                errors.append(f"{f['path']}: <f> without s=")
            elif not (0 <= font["value"] <= 30):
                errors.append(f"{f['path']}: font {font['value']} out of range")
        for m in f["music"]:
            if m["track"] is None and m["offset"] is None:
                errors.append(f"{f['path']}: <M> without B/S")
    for p in report["pairs"]:
        if p["status"] == "unpaired":
            continue
        if p["decode"] == "unrepaired":
            errors.append(f"{p['stem']}: unrepaired pair decode")
    errors.extend(_self_test())
    return errors


# --------------------------------------------------------------------- dump
def _dump(report: dict, limit: int = 0) -> None:
    files = report["files"][:limit] if limit else report["files"]
    for f in files:
        print(f"=== {f['path']}  ({f['bytes']} B, {f['decode']}, "
              f"{f['tags']} tags, {f['text_runs']} text runs)")
        if f["images"]:
            for i in f["images"]:
                print(f"  image  {i['value']}  x={i['x']} y={i['y']} "
                      f"align={i['align']}  -> {i['asset']} "
                      f"{i['asset_size']}")
        if f["fonts"]:
            print("  fonts  " + ", ".join(
                str(x["value"]) for x in f["fonts"]))
        if f["music"]:
            print("  music  " + ", ".join(
                f"B={x['track']} S={x['offset']}" for x in f["music"]))
        if f["warnings"]:
            print("  !! " + "; ".join(f["warnings"]))
        preview = " ".join(f["preview"].split())
        if preview:
            print(f"  text   {preview[:180]}")


def _print_screens() -> None:
    for s in SCREENS:
        handler = "-" if s["handler"] is None else f"0x{s['handler']:06x}"
        note = f"  [{s['note']}]" if s["note"] else ""
        print(f"{s['id']:2d}  {handler:10s}  {s['title']}{note}")


def _print_assets(report: dict, limit: int = 0) -> None:
    assets = report["assets"]
    print(f"FRONTEND/BMP.IDX: {assets['frontend']['count']} entries")
    for rec in assets["frontend"]["entries"][:limit or None]:
        print(f"  {rec['name']:10s} {rec.get('width', '?'):>4}x"
              f"{rec.get('height', '?'):<4} {rec.get('bpp', '?')} bpp")
    print(f"\nsprites/SBM.IDX: {assets['sprites']['count']} entries (world FX)")
    print("  " + ", ".join(r["name"] for r in assets["sprites"]["entries"][:24])
          + (" ..." if assets["sprites"]["count"] > 24 else ""))
    print(f"\nPICS/SBM.IDX: {assets['pics']['count']} entries (world textures)")
    print(f"\nSML/*.bmp: {len(assets['sml_bmps'])} loose images")
    for rec in assets["sml_bmps"]:
        print(f"  {rec['name']:14s} {rec['width']}x{rec['height']}")
    print(f"\nFilm/*.sof: {len(assets['films'])} HUD films")
    for rec in assets["films"][:limit or None]:
        print(f"  {rec['name']:16s} {rec['frames']:4d} frames "
              f"({rec['bytes']} B)")
    print("\nUI atlas (name, w x h, role; dims verified against BMP headers):")
    for name, w, h, role in UI_ATLAS:
        print(f"  {name:10s} {w:>4}x{h:<4} {role}")


# --------------------------------------------------------------------- main
def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--game", type=Path, default=DEFAULT_GAME,
                    help="reference installation (default: %(default)s)")
    ap.add_argument("--sml", type=Path, default=None,
                    help="SML directory (default: <game>/SML)")
    ap.add_argument("--json", action="store_true", help="JSON report on stdout")
    ap.add_argument("--json-out", type=Path, help="write JSON to this file")
    ap.add_argument("--check", action="store_true",
                    help="validate the corpus and run negative self-tests")
    ap.add_argument("--dump", action="store_true", help="dump elements per file")
    ap.add_argument("--screens", action="store_true",
                    help="print the front-end screen/menu state table")
    ap.add_argument("--assets", action="store_true",
                    help="print FRONTEND/sprites/PICS/SOF/atlas inventory")
    ap.add_argument("--self-test", action="store_true",
                    help="run parser negative tests only")
    ap.add_argument("--limit", type=int, default=0,
                    help="limit dump/assets rows (0 = all)")
    args = ap.parse_args(argv)

    if args.self_test:
        fails = _self_test()
        for f in fails:
            print("FAIL:", f)
        print("self-test:", "OK" if not fails else f"{len(fails)} failure(s)")
        return 0 if not fails else 1

    sml_dir = args.sml or (args.game / "SML")
    if not sml_dir.is_dir():
        print(f"no SML directory: {sml_dir}", file=sys.stderr)
        return 2
    if not args.game.is_dir():
        print(f"warning: game directory not found: {args.game}", file=sys.stderr)

    report = _report(args.game, sml_dir, with_assets=args.game.is_dir())

    if args.screens:
        _print_screens()
        return 0
    if args.assets:
        _print_assets(report, args.limit)
        return 0
    if args.dump:
        _dump(report, args.limit)
        return 0

    if args.check:
        errors = _check(report, expect_counts=(args.sml is None))
        c = report["counts"]
        print(f"files {c['files']} (.sme {c['sme']}, .sml {c['sml']}), "
              f"pairs {c['pairs']}: identical {c['identical']}, "
              f"revisions {c['revisions']}, line-endings {c['line_endings']}; "
              f"newline-repair .sme {c['newline_repair']}")
        print(f"images {c['images']} ({c['unique_images']} unique), "
              f"fonts {c['font_values']}, music {c['music_tags']}")
        present = [f["path"] for f in report["files"]
                   if f["path"] in KNOWN_ANOMALIES]
        if present:
            print(f"known malformed: {len(present)} file(s) "
                  f"({', '.join(sorted(present))})")
        if errors:
            for e in errors[:80]:
                print("FAIL:", e)
            print(f"check: {len(errors)} failure(s)")
            return 1
        print("check: OK")
        return 0

    if args.json or args.json_out:
        text = json.dumps(report, ensure_ascii=False, indent=2, sort_keys=False)
        if args.json_out:
            args.json_out.write_text(text, encoding="utf-8")
        if args.json:
            print(text)
        return 0

    # default human summary
    c = report["counts"]
    print(f"{c['files']} files: {c['sme']} .sme, {c['sml']} .sml; "
          f"{c['pairs']} pairs ({c['identical']} identical, "
          f"{c['revisions']} revisions, {c['line_endings']} line-endings, "
          f"{c['newline_repair']} newline-repair)")
    print(f"{c['images']} images ({c['unique_images']} unique), "
          f"fonts {c['font_values']}, {c['music_tags']} music tags, "
          f"{c['warnings']} warnings")
    if c["warnings"]:
        for f in report["files"]:
            for w in f["warnings"]:
                print(f"  {f['path']}: {w}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
