#!/usr/bin/env python3
"""``.sub`` substitution tables (and the ``.sni`` / ``.skk`` / ``.skb`` neighbours).

The shipped corpus is tiny — three ``.sub`` files — but the engine meaning of
the leading number is not obvious from the text.  ``gundx.exe`` proves it:

* ``0x422f75`` ``sub_load(dir, name, kind, ram_class)`` opens
  ``<dir>/<name>.SUB`` through ``0x40d1a0``, reads the count with
  ``fscanf("%ld")`` (``0x422fb1``) and then exactly that many
  ``fscanf("%ld %s %s")`` records (``0x422fff``);
* a record is skipped when ``ram_class > weight`` (``0x42302c``..``0x423035``);
  ``ram_class`` is filled once from ``GlobalMemoryStatus`` (``0x404640``) as
  ``(dwTotalPhys - 4_000_000) / 1_048_576`` (``0x40465c``);
* ``0x42307a`` maps the sprite named *second* (``right``) to the image named
  *first* (``left``); ``0x4232b7`` then loads that image into the sprite table
  ``0x55a0c0`` and aggregates reference counts in ``0x5330c0``.

So ``20 alumnm4 alumnm5`` means "on a machine with at most ~20 MiB above the
4 MiB baseline, draw ``alumnm5`` with the image of ``alumnm4``".  It is a
low-memory fallback, not a random/animation variant selector; `.soa` frames
just reference sprite names and inherit whatever substitution was installed at
load time.  Full write-up: ``docs/SUB.md``.

``textures.sub`` is loaded with kind 0 from ``pics\\``, ``sprites.sub`` with
kind 1 from ``sprites\\`` (``0x4426c3``..``0x4426ee`` and
``0x4432fa``..``0x443319``).  ``sounds.sub`` is different: tab-separated, no
count, no weight.

Usage::

    python3 tools/re/sub.py                       # inventory table
    python3 tools/re/sub.py --dump                # every record
    python3 tools/re/sub.py --check               # parse -> rebuild (corpus)
    python3 tools/re/sub.py --json
    python3 tools/re/sub.py --exe-check           # verify the VAs above
    python3 tools/re/sub.py --self-test           # no game data needed
    python3 tools/re/sub.py --game DIR --check

Read-only: nothing in the game directory is written.
"""

from __future__ import annotations

import argparse
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
if ROOT not in sys.path:
    sys.path.insert(0, ROOT)

from gm import small  # noqa: E402

DEFAULT_GAME = "/mnt/d/games/Gunmetal 3dfx"
SUFFIXES = (".sub", ".sni", ".skk", ".skb")

#: Proof anchors in ``gundx.exe`` (see the module docstring / docs/SUB.md).
VA_SUB_EXT = 0x4A8CCC         # "SUB"
VA_FMT_COUNT = 0x4A8CD0       # "%ld"
VA_FMT_ENTRY = 0x4A8CD4       # "%ld %s %s"
VA_LOADER = 0x422F75
VA_REGISTER = 0x42307A
VA_BUILD = 0x4232B7
VA_RAM_CLASS = 0x404630
VA_RAM_GLOBAL = 0x4BF290


# --- corpus ----------------------------------------------------------------
def find_files(game: str, paths: list[str]) -> list[str]:
    out: list[str] = []
    for p in paths or [game]:
        if os.path.isdir(p):
            for base, dirs, files in os.walk(p):
                dirs.sort()
                for name in sorted(files):
                    if os.path.splitext(name)[1].lower() in SUFFIXES:
                        out.append(os.path.join(base, name))
        elif os.path.splitext(p)[1].lower() in SUFFIXES:
            out.append(p)
    return out


def _rel(path: str, game: str) -> str:
    try:
        return os.path.relpath(path, game).replace(os.sep, "/")
    except ValueError:
        return path


def sub_kind(rel: str, s: small.SubFile) -> str:
    low = rel.lower()
    if "texture" in low:
        return "textures"
    if "sprite" in low:
        return "sprites"
    if "sound" in low:
        return "sounds"
    return "counted" if s.is_counted else "pairs"


def analyse_sub(path: str, rel: str) -> dict:
    raw = open(path, "rb").read()
    s = small.parse_sub(raw)
    dup: dict[str, int] = {}
    for e in s.entries:
        dup[e.right] = dup.get(e.right, 0) + 1
    info = {
        "file": rel,
        "format": "sub",
        "kind": sub_kind(rel, s),
        "bytes": len(raw),
        "count": s.count,
        "entries": len(s.entries),
        "weights": {str(k): v for k, v in sorted(s.weights().items())},
        "weighted": s.is_counted,
        "tab": s.tab,
        "eol": "CRLF" if s.eol == "\r\n" else "LF",
        "trailing_eol": s.trailing,
        "trailing_blanks": s.blanks,
        "unique_replacement": len({e.left for e in s.entries}),
        "unique_replaced": len({e.right for e in s.entries}),
        "duplicate_replaced": {k: v for k, v in sorted(dup.items()) if v > 1},
        "rebuild_exact": s.to_bytes() == raw,
        "validate": s.validate(),
        "warnings": sorted(set(s.warnings())),
    }
    return info


def analyse_other(path: str, rel: str) -> dict:
    raw = open(path, "rb").read()
    ext = os.path.splitext(path)[1].lower()
    info: dict = {"file": rel, "format": ext[1:], "bytes": len(raw)}
    if ext == ".sni":
        text = small.parse_sni(raw)
        info.update({"path": text, "rebuild_exact":
                     small.sni_to_bytes(text) == raw})
        return info
    k = small.parse_kill(raw)
    info.update({
        "lines": len(k.lines),
        "messages": len(k.messages),
        "trailing_eol": k.trailing,
        "max_message": max((len(m) for m in k.messages), default=0),
        # 0x485a7d: strlen(fgets line incl. "\n") must be < 0x20; text mode
        # turns CRLF into LF, so a message may be at most 30 chars.
        "engine_loaded": sum(1 for m in k.messages if len(m) <= 30),
        "rebuild_exact": k.to_bytes() == raw,
        "validate": ([] if len(k.messages) <= 0x80
                     else ["more than 128 messages"]),
    })
    return info


def analyse(path: str, game: str) -> dict:
    rel = _rel(path, game)
    if os.path.splitext(path)[1].lower() == ".sub":
        return analyse_sub(path, rel)
    return analyse_other(path, rel)


# --- exe proof -------------------------------------------------------------
def exe_check() -> list[tuple[str, bool]]:
    sys.path.insert(0, HERE)
    import exelib  # local import: only needed by --exe-check

    checks: list[tuple[str, bool]] = []

    checks.append(("string 0x4a8ccc == 'SUB'",
                   exelib.cstr(VA_SUB_EXT) == "SUB"))
    checks.append(("string 0x4a8cd0 == '%ld'",
                   exelib.cstr(VA_FMT_COUNT) == "%ld"))
    checks.append(("string 0x4a8cd4 == '%ld %s %s'",
                   exelib.cstr(VA_FMT_ENTRY) == "%ld %s %s"))

    def at(va: int, expected: bytes) -> bool:
        off = exelib.va2off(va)
        return off is not None and exelib.data()[off:off + len(expected)] == expected

    checks.append(("loader prologue 0x422f75",
                   at(VA_LOADER, bytes.fromhex("558bec81ec0c020000"))))
    checks.append(("loader pushes 'SUB' (0x422f7e)",
                   at(0x422F7E, bytes.fromhex("68cc8c4a00"))))
    checks.append(("loader pushes '%ld' (0x422fb1)",
                   at(0x422FB1, bytes.fromhex("68d08c4a00"))))
    checks.append(("loader pushes '%ld %s %s' (0x422fff)",
                   at(0x422FFF, bytes.fromhex("68d48c4a00"))))
    checks.append(("loader compares ram_class vs weight (0x42302c)",
                   at(0x42302C, bytes.fromhex("8b4d143b8df4fdffff7f1a"))))
    checks.append(("GlobalMemoryStatus call (0x404640)",
                   at(0x404640, bytes.fromhex("ff1524138400"))))
    checks.append(("RAM class stored at 0x4bf290 (0x40465c)",
                   at(0x40465C, bytes.fromhex("a390f24b00"))))

    def call_target(va: int) -> int | None:
        off = exelib.va2off(va)
        if off is None or exelib.data()[off] != 0xE8:
            return None
        rel = int.from_bytes(exelib.data()[off + 1:off + 5], "little",
                             signed=True)
        return va + 5 + rel

    checks.append(("0x4426d3 calls sub_load",
                   call_target(0x4426D3) == VA_LOADER))
    checks.append(("0x4432fa calls sub_load",
                   call_target(0x4432FA) == VA_LOADER))
    checks.append(("'pics' / 'textures' strings",
                   exelib.cstr(0x4AB54C) == "pics"
                   and exelib.cstr(0x4AB554) == "textures"))
    checks.append(("'sprites' string",
                   exelib.cstr(0x4AB544) == "sprites"))
    return checks


# --- self test -------------------------------------------------------------
def self_test() -> tuple[int, list[str]]:
    results: list[tuple[str, bool]] = []

    def check(name: str, cond: bool) -> None:
        results.append((name, bool(cond)))

    counted = b"3\r\n20 aaa bbb\r\n20 ccc ddd\r\n16 eee fff\r\n"
    s = small.parse_sub(counted)
    check("counted: rebuild", s.to_bytes() == counted)
    check("counted: count", s.count == 3 and len(s.entries) == 3)
    check("counted: weights", s.weights() == {20: 2, 16: 1})
    check("counted: variants", s.variants() == {"bbb": ["aaa"],
                                                "ddd": ["ccc"],
                                                "fff": ["eee"]})
    check("counted: weight filter", s.variants(20) == {"bbb": ["aaa"],
                                                       "ddd": ["ccc"]})
    check("counted: active(12)", len(s.active(12)) == 3)
    check("counted: active(20)", len(s.active(20)) == 2)
    check("counted: active(28)", len(s.active(28)) == 0)
    check("counted: resolve", s.resolve(12) == {"bbb": "aaa", "ddd": "ccc",
                                                "fff": "eee"})
    check("counted: validate", s.validate() == [])
    check("counted: eol", s.eol == "\r\n" and s.trailing)

    no_final = b"1\r\n20 aaa bbb"
    s2 = small.parse_sub(no_final)
    check("no final EOL: rebuild", s2.to_bytes() == no_final)
    check("no final EOL: trailing", s2.trailing is False)

    lf = b"1\n20 aaa bbb\n"
    s3 = small.parse_sub(lf)
    check("LF: rebuild", s3.to_bytes() == lf and s3.eol == "\n")

    blank = b"1\r\n\r\n20 aaa bbb\r\n\r\n"
    s4 = small.parse_sub(blank)
    check("interior/trailing blanks: rebuild", s4.to_bytes() == blank)

    sounds = b"aaa\tbbb\r\nccc\tbbb\r\n"
    s5 = small.parse_sub(sounds)
    check("sounds: rebuild", s5.to_bytes() == sounds)
    check("sounds: layout", s5.count is None and s5.tab
          and s5.weights() == {} and s5.entries[0].weight is None)
    check("sounds: variants",
          s5.variants() == {"bbb": ["aaa", "ccc"]})
    check("sounds: validate", s5.validate() == [])

    # negatives ------------------------------------------------------------
    truncated = b"3\r\n20 aaa bbb\r\n"
    st = small.parse_sub(truncated)
    check("negative: truncated parsed but invalid",
          any("count" in p for p in st.validate()))
    check("negative: truncated still rebuilds",
          st.to_bytes() == truncated)

    short = b"1\r\n20 aaa\r\n"
    try:
        ss = small.parse_sub(short)
        ok = any("missing weight" in p for p in ss.validate())
    except ValueError:
        ok = True
    check("negative: 2-field counted record rejected/flagged", ok)

    missing_weight = b"1\r\naaa bbb\r\n"
    sm = small.parse_sub(missing_weight)
    check("negative: missing weight flagged",
          any("missing weight" in p for p in sm.validate()))

    negative_weight = b"1\r\n-5 aaa bbb\r\n"
    sn = small.parse_sub(negative_weight)
    check("negative: negative weight flagged",
          any("negative weight" in p for p in sn.validate()))

    oversized = b"1\r\n20 " + b"x" * (small.SUB_NAME_MAX + 1) + b" bbb\r\n"
    so = small.parse_sub(oversized)
    check("negative: oversized name flagged",
          any("longer than" in p for p in so.validate()))

    long_key = b"1\r\n20 abcdefghi bbb\r\n"
    sk = small.parse_sub(long_key)
    check("negative: >8-char key warns",
          sk.validate() == [] and sk.warnings())

    bad_weight = b"1\r\nzzz aaa bbb\r\n"
    try:
        small.parse_sub(bad_weight)
        ok = False
    except ValueError:
        ok = True
    check("negative: non-numeric weight rejected", ok)

    # neighbours -----------------------------------------------------------
    sni = b".\r\n"
    check("sni: rebuild", small.sni_to_bytes(small.parse_sni(sni)) == sni)
    check("sni: parse", small.parse_sni(sni) == ".")

    kill = b"%S DIED\r\n%S WAS %S\r\n\r\n"
    k = small.parse_kill(kill)
    check("kill: rebuild", k.to_bytes() == kill)
    check("kill: messages", k.messages == ["%S DIED", "%S WAS %S"])

    return len(results), [name for name, ok in results if not ok]


# --- reporting -------------------------------------------------------------
def dump_rows(info: dict, game: str) -> None:
    path = os.path.join(game, info["file"]) if not os.path.isabs(info["file"]) \
        else info["file"]
    s = small.parse_sub(open(path, "rb").read())
    print("# %s  %s  %d entries" % (info["file"], info["kind"],
                                    len(s.entries)))
    for i, e in enumerate(s.entries):
        w = "-" if e.weight is None else str(e.weight)
        print("%4d  w=%-3s  %-8s -> %s" % (i, w, e.left, e.right))


def print_table(rows: list[dict]) -> None:
    print("%-28s %-8s %6s %5s %-14s %s"
          % ("file", "kind", "bytes", "n", "weights", "rebuild"))
    for r in rows:
        if r["format"] != "sub":
            extra = ("%d msgs" % r["messages"] if "messages" in r
                     else repr(r.get("path", "")))
            print("%-28s %-8s %6d %5s %-14s %s"
                  % (r["file"], r["format"], r["bytes"], "-", extra,
                     "exact" if r["rebuild_exact"] else "DIFF"))
            continue
        weights = ",".join("%s x%d" % (k, v) for k, v in r["weights"].items())
        print("%-28s %-8s %6d %5d %-14s %s"
              % (r["file"], r["kind"], r["bytes"], r["entries"], weights,
                 "exact" if r["rebuild_exact"] else "DIFF"))


def print_check(rows: list[dict]) -> bool:
    ok = True
    for r in rows:
        bad = list(r.get("validate") or [])
        if not r["rebuild_exact"]:
            bad.append("parse -> rebuild differs")
        if r["format"] == "sub":
            n = "%d entries" % r["entries"]
        elif r["format"] == "sni":
            n = repr(r.get("path"))
        else:
            n = "%d messages" % r.get("messages", 0)
        status = "ok" if not bad else "FAIL"
        print("%-28s %-6s %-6s %s" % (r["file"], r["format"], status, n))
        for msg in bad:
            print("    ! %s" % msg)
            ok = False
        for msg in r.get("warnings") or []:
            print("    ~ %s" % msg)
    exact = sum(1 for r in rows if r["rebuild_exact"])
    valid = sum(1 for r in rows if not r.get("validate"))
    print("sub: %d/%d files round-trip, %d/%d valid"
          % (exact, len(rows), valid, len(rows)))
    return ok and exact == len(rows) and valid == len(rows)


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(
        description="inspect/verify Gunmetal .sub tables and .sni/.skk/.skb")
    ap.add_argument("paths", nargs="*",
                    help="files or directories (default: --game)")
    ap.add_argument("--game", default=DEFAULT_GAME,
                    help="installation directory (default: %(default)s)")
    ap.add_argument("--json", action="store_true", help="machine-readable output")
    ap.add_argument("--dump", action="store_true",
                    help="print every .sub record")
    ap.add_argument("--check", action="store_true",
                    help="parse -> rebuild + validate the whole corpus")
    ap.add_argument("--exe-check", action="store_true",
                    help="verify the gundx.exe anchors quoted in docs/SUB.md")
    ap.add_argument("--self-test", action="store_true",
                    help="run built-in positive/negative tests (no game data)")
    opts = ap.parse_args(argv)

    if opts.self_test:
        total, failures = self_test()
        for name in failures:
            print("FAIL %s" % name)
        print("sub self-test: %d checks, %d failed"
              % (total, len(failures)))
        return 0 if not failures else 1

    if opts.exe_check:
        results = exe_check()
        for name, ok in results:
            print("%-6s %s" % ("ok" if ok else "FAIL", name))
        print("sub exe-check: %d/%d"
              % (sum(1 for _, ok in results if ok), len(results)))
        return 0 if all(ok for _, ok in results) else 1

    files = find_files(opts.game, opts.paths)
    if not files:
        print("sub: no .sub/.sni/.skk/.skb under %s" % opts.game,
              file=sys.stderr)
        return 2
    try:
        rows = [analyse(p, opts.game) for p in files]
    except (OSError, ValueError) as exc:
        print("sub: %s" % exc, file=sys.stderr)
        return 1

    if opts.json:
        print(json.dumps({
            "game": opts.game,
            "files": rows,
            "summary": {
                "files": len(rows),
                "sub_files": sum(1 for r in rows if r["format"] == "sub"),
                "entries": sum(r.get("entries", 0) for r in rows),
                "rebuild_exact": sum(1 for r in rows if r["rebuild_exact"]),
                "valid": sum(1 for r in rows if not r.get("validate")),
            },
        }, indent=2, sort_keys=True))
        return 0 if all(r["rebuild_exact"] for r in rows) else 1

    if opts.check:
        return 0 if print_check(rows) else 1

    print_table(rows)
    if opts.dump:
        for r in rows:
            if r["format"] == "sub":
                print()
                dump_rows(r, opts.game)
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
