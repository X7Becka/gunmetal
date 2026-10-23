#!/usr/bin/env python3
"""Parity fixtures: Python's view of the shipped data as expected values (M3.2.7).

The point is **not** to test Python: it is to let the C++ readers be checked
without Python in the loop.  This tool parses a level corpus with `gm/*` and
writes a small text file of expected numbers; `engine/tests/test_parity.cpp`
re-parses the same files with `res/*` and compares.  A parser that drifts on
either side shows up as a concrete `key=value` mismatch.

Format (one check per line, whitespace separated, values are `key=value`):

    # gm parity fixtures v1
    sbw level0.sbw tex=11 effects=6 sectors=6 objects=42 verts=... polys=...
    sbt level0.sbt records=214 t1=8 t12=1 names=137
    sbc level0.sbc magic=SBC0 records=36 pix=0123456789abcdef
    sbm PICS/SBM.IDX#3 w=64 h=64 levels=1 flags=0 pix=...
    sob SOBS/100mmclp.sob verts=... faces=...

Names are paths relative to the game root and never contain spaces; textures
live in the ``SBM.IDX`` archives (no ``.sbm`` files on disk), so a texture is
named ``<index path>#<entry number>`` — the index order is the identity, no
name normalisation involved.  Hashes are
FNV-1a 64 (`core/hash.h::fnv1a64`) of the decoded bytes, printed as 16 hex
digits, so a decode difference cannot cancel out in a count.

Usage:
  python3 tools/re/export_parity.py --out build/parity/fixtures.txt
  python3 tools/re/export_parity.py --stats
  python3 tools/re/export_parity.py --self-test
"""

from __future__ import annotations

import argparse
import os
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))

from gm import bigfile, sbc, sbm, sbt, sbw, sml, sob, sou  # noqa: E402

FORMAT = "gm parity fixtures v1"
DEFAULT_GAME = "/mnt/d/games/Gunmetal 3dfx"

_FNV_OFFSET = 0xCBF29CE484222325
_FNV_PRIME = 0x100000001B3


def fnv1a64(data: bytes) -> int:
    """`core/hash.h::fnv1a64`: 64-bit FNV-1a, the engine's golden checksum."""
    value = _FNV_OFFSET
    for byte in data:
        value ^= byte
        value = (value * _FNV_PRIME) & 0xFFFFFFFFFFFFFFFF
    return value


def _check(kind: str, name: str, values: dict) -> str:
    parts = [kind, name]
    for key in sorted(values):
        text = str(values[key])
        if any(ch.isspace() for ch in text):
            raise ValueError("value for %s/%s/%s has whitespace: %r"
                             % (kind, name, key, values[key]))
        parts.append("%s=%s" % (key, text))
    return " ".join(parts)


def _sbw(game: Path, name: str) -> dict:
    level = sbw.parse_file(str(game / name))
    verts = sum(len(obj.vertices) for sector in level.sectors for obj in sector.objects)
    return {"tex": len(level.tex), "effects": len(level.effects),
            "sectors": len(level.sectors), "objects": level.n_objects,
            "verts": verts, "polys": level.n_polys}


def _sbt(game: Path, name: str) -> dict:
    level = sbt.parse_file(str(game / name))
    counts: dict = {}
    for record in level.records:
        counts[record.type] = counts.get(record.type, 0) + 1
    values = {"records": len(level.records)}
    for kind in sorted(counts):
        values["t%d" % kind] = counts[kind]
    tables = level.records_of(12)
    values["names"] = len([n for n in tables[0].payload.names if n]) if tables else 0
    return values


def _sbc(game: Path, name: str) -> dict:
    blob = sbc.parse((game / name).read_bytes())
    pixels = b"".join(record.pixels for record in blob.records)
    return {"magic": blob.magic.decode("latin1"), "records": len(blob.records),
            "pix": "%016x" % fnv1a64(pixels)}


def _sbm(game: Path, name: str) -> dict:
    blob = sbm.parse((game / name).read_bytes())
    return {"w": blob.width, "h": blob.height, "levels": blob.levels,
            "flags": blob.flags, "pix": "%016x" % fnv1a64(blob.levels_data[0])}


def _sml(game: Path, name: str) -> dict:
    """Decoded SML/SME markup: size, tag count and FNV of the decoded text.

    ``decode_auto`` is the **`.sme`** entry point (it applies the positional
    Caesar shift); a plain `.sml` must be read as-is, otherwise the shift turns
    ``<D>`` into ``<C>`` and the hash is wrong while the length and tag count
    still match -- exactly the kind of drift the fixtures are for.
    """
    data = (game / name).read_bytes()
    if name.lower().endswith(".sme"):
        text, _repaired = sml.decode_auto(data)
    else:
        text = data.decode("latin-1")
    raw = text.encode("latin-1")
    return {"chars": len(raw), "tags": len(sml.tags(text)),
            "pix": "%016x" % fnv1a64(raw)}


def _sou_entries(game: Path, limit: int) -> list:
    """Header parity of the shipped SOU0/1/2 entries (no codec decode)."""
    index = "sounds/SOU.IDX"
    if not (game / index).is_file():
        return []
    archive = bigfile.BigFile(str(game / index))
    data = Path(archive.dat_path).read_bytes()
    checks = []
    for number, entry in enumerate(archive.entries[:limit]):
        try:
            parsed = sou.parse(entry.data(data))
        except ValueError:
            continue
        checks.append(("sou", "%s#%d" % (index, number),
                       {"magic": parsed.magic, "rate": parsed.sample_rate,
                        "flags": parsed.flags,
                        # The declared header field, not the even-trimmed
                        # payload: an odd SOU1 length keeps its +1 in the
                        # container (the codec drops the tail halfword).
                        "len": parsed.length_field}))
    return checks


def _sob(game: Path, name: str) -> dict:
    model = sob.parse(str(game / name))
    return {"verts": len(model.vertices), "faces": len(model.faces)}


#: `kind -> (exporter, root, limit)`: levels are exactly 36 files at the root,
#: models are 672 in `SOBS/` (a sorted prefix is pinned).
EXPORTERS = {
    "sbw": (_sbw, ".", 0),
    "sbt": (_sbt, ".", 0),
    "sbc": (_sbc, ".", 0),
    "sob": (_sob, "SOBS", 60),
}

#: Texture archive: 1827 `.sbm` blobs in `PICS/SBM.IDX` + `sprites/SBM.IDX`.
SBM_INDEX = "PICS/SBM.IDX"


def _sbm_entries(game: Path, limit: int) -> list:
    archive = bigfile.BigFile(str(game / SBM_INDEX))
    data = Path(archive.dat_path).read_bytes()
    checks = []
    for index, entry in enumerate(archive.entries[:limit]):
        blob = entry.data(data)
        size = sbm.parse(blob)
        checks.append(("sbm", "%s#%d" % (SBM_INDEX, index),
                       {"w": size.width, "h": size.height, "levels": size.levels,
                        "flags": size.flags,
                        "pix": "%016x" % fnv1a64(size.levels_data[0])}))
    return checks


def build(game: Path, sob_limit: int | None = None, sbm_limit: int = 40,
          sml_limit: int = 40, sou_limit: int = 40) -> list:
    lines = ["# " + FORMAT]
    checks = []
    for kind, (exporter, root, limit) in EXPORTERS.items():
        if kind == "sob" and sob_limit is not None:
            limit = sob_limit
        base = game / root
        suffix = {"sbw": ".sbw", "sbt": ".sbt", "sbc": ".sbc", "sob": ".sob"}[kind]
        paths = sorted(p for p in base.rglob("*" + suffix) if p.is_file())
        paths = [p for p in paths if p.parent == base]
        if limit:
            paths = paths[:limit]
        for path in paths:
            name = path.relative_to(game).as_posix()
            if any(ch.isspace() for ch in name):
                continue
            checks.append((kind, name, exporter(game, name)))
    if sbm_limit and (game / SBM_INDEX).is_file():
        checks.extend(_sbm_entries(game, sbm_limit))
    if sou_limit:
        checks.extend(_sou_entries(game, sou_limit))
    sml_root = game / "SML"
    if sml_limit and sml_root.is_dir():
        paths = sorted(p for p in sml_root.rglob("*")
                       if p.suffix.lower() in (".sml", ".sme") and p.is_file())
        for path in paths[:sml_limit]:
            checks.append(("sml", path.relative_to(game).as_posix(),
                           _sml(game, path.relative_to(game).as_posix())))
    for kind, name, values in checks:
        lines.append(_check(kind, name, values))
    return lines


def parse_fixtures(lines) -> list:
    """Split fixture lines back into `(kind, name, {key: value})`."""
    checks = []
    for raw in lines:
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        tokens = line.split()
        if len(tokens) < 3:
            raise ValueError("short fixture line: %r" % raw)
        kind, name = tokens[0], tokens[1]
        values = {}
        for token in tokens[2:]:
            if "=" not in token:
                raise ValueError("bad token %r in %r" % (token, raw))
            key, text = token.split("=", 1)
            if not key or key in values:
                raise ValueError("bad key %r in %r" % (key, raw))
            values[key] = text
        checks.append((kind, name, values))
    return checks


def _self_test() -> int:
    failures = 0

    def check(cond, what):
        nonlocal failures
        if not cond:
            print("FAIL %s" % what)
            failures += 1

    check(fnv1a64(b"") == 0xCBF29CE484222325, "fnv empty")
    check(fnv1a64(b"a") == 0xAF63DC4C8601EC8C, "fnv a")
    check(fnv1a64(b"a") == fnv1a64(bytes([0x61])), "fnv byte equivalence")

    line = _check("sbw", "level0.sbw", {"tex": 11, "polys": 7})
    check(line == "sbw level0.sbw polys=7 tex=11", "line layout is sorted: %r" % line)
    parsed = parse_fixtures([line])
    check(parsed == [("sbw", "level0.sbw", {"polys": "7", "tex": "11"})],
          "round-trip: %r" % (parsed,))
    try:
        _check("sbw", "x.sbw", {"bad": "a b"})
        check(False, "whitespace in value must raise")
    except ValueError:
        pass
    for bad in ("sbw onlyname", "sbw n.sbw noequals", "sbw n.sbw k=1 k=2"):
        try:
            parse_fixtures([bad])
            check(False, "bad line accepted: %r" % bad)
        except ValueError:
            pass
    check(parse_fixtures(["# comment", ""]) == [], "comments and blanks are skipped")

    print("%d/%d self-tests passed" % (12 - failures, 12))
    return 1 if failures else 0


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--game", default=os.environ.get("GUNMETAL_DIR", DEFAULT_GAME),
                        help="installation root")
    parser.add_argument("--out", help="write the fixture file here")
    parser.add_argument("--sob-limit", type=int, default=60,
                        help="how many SOBS/*.sob to pin (default 60)")
    parser.add_argument("--sbm-limit", type=int, default=40,
                        help="how many PICS/SBM.IDX textures to pin (default 40)")
    parser.add_argument("--stats", action="store_true", help="print per-kind counts")
    parser.add_argument("--self-test", action="store_true",
                        help="format and FNV checks, no game files")
    args = parser.parse_args(argv)

    if args.self_test:
        return _self_test()
    if not args.out and not args.stats:
        parser.error("expected --out, --stats or --self-test")

    game = Path(args.game)
    if not game.is_dir():
        print("parity: game not installed at %s" % game, file=sys.stderr)
        return 2

    lines = build(game, args.sob_limit, args.sbm_limit)
    by_kind: dict = {}
    for kind, _name, _values in parse_fixtures(lines):
        by_kind[kind] = by_kind.get(kind, 0) + 1
    if args.stats:
        for kind in sorted(by_kind):
            print("%-4s %d" % (kind, by_kind[kind]))
        print("total %d checks" % sum(by_kind.values()))
    if args.out:
        out = Path(args.out)
        out.parent.mkdir(parents=True, exist_ok=True)
        out.write_text("\n".join(lines) + "\n", encoding="utf-8")
        print("parity: %d checks -> %s" % (sum(by_kind.values()), out))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
