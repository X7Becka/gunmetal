#!/usr/bin/env python3
"""Round-trip and field-map check for ``GUNMETAL.CFG`` / ``CONTROLS.CFG``.

Usage::

    python3 tools/parity_config.py [--game DIR] [--json] [--dump] [--rebuild DIR]

The script is a *test*, not a loader oracle run: it never starts the game.  It
checks, in this order:

1. ``GUNMETAL.CFG`` -> :func:`gm.config.parse` -> :meth:`Config.to_bytes`
   is byte-identical (FNV-1a over the whole file is printed for both sides).
2. ``CONTROLS.CFG`` is recognised as the same container but version 1.91 and
   kept opaque; the round-trip is byte-identical too.
3. ``KEYCFG/*.key`` round-trip through ``gm.small`` (they use a *different*
   loader, ``0x48534a``) and the two profile names stored in ``GUNMETAL.CFG``
   are checked against the names of the files on disk -- that cross-check is
   what proves the two ``char[9]`` fields at 0x1E0/0x1E4.
4. ``gunmetal.hi`` uses yet another loader (``0x45c8b3``); only its round-trip
   is checked here.
5. The field map is exercised by mutating every parsed field and asserting
   that exactly its own four bytes in the output change (writer-side proof
   that offsets in :data:`gm.config.FIELDS` are the ones actually serialised).
6. Structural coverage: 0x190..0x8AC must be covered by the model without
   holes or overlaps, and 0x025..0x18F / 0x8AC..0x2000 must be preserved
   verbatim.

What is **proven by the exe** (see ``docs/CONFIG.md`` for addresses): the
container size/magic, the offset of every field and the destination global.
What is **proven by the data**: the shipped values, the key-profile names, the
four identical control records.  What is **hypothesis** and marked as such:
the meaning of individual control-record numbers and the scale of the volume
fields.
"""

from __future__ import annotations

import argparse
import glob
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, ROOT)

from gm import config, small  # noqa: E402

DEFAULT_GAME = "/mnt/d/games/Gunmetal 3dfx"


def fnv1a(data: bytes) -> int:
    """FNV-1a 32-bit — a cheap checksum for "is this the same file"."""
    h = 0x811C9DC5
    for b in data:
        h = ((h ^ b) * 0x01000193) & 0xFFFFFFFF
    return h


def report(name: str, ok: bool, detail: str) -> bool:
    print("%-4s %-46s %s" % ("ok" if ok else "FAIL", name, detail))
    return ok


def field_offsets() -> list[tuple[int, str, int]]:
    """``(offset, name, width)`` for every modelled cell.

    *width* is 4 for a full ``u32`` and 1 for the three cells the game only
    stores as a byte (``axes[].byte``, ``extra[].lo``, ``extra[].hi``).
    """
    out = [(off, attr, 4) for off, attr, _dest, _note in config.FIELDS]
    for k in range(config.NAME_LEN):
        out.append((0x1E0 + 8 * k, "key_names[0].codes[%d]" % k, 4))
        out.append((0x1E0 + 8 * k + 4, "key_names[1].codes[%d]" % k, 4))
    for i in range(4):
        out.append((0x228 + 4 * i, "spare_keys_a[%d]" % i, 4))
        out.append((0x238 + 4 * i, "spare_keys_b[%d]" % i, 4))
    out.append((0x248, "joystick_used_by", 4))
    for off, attr, _dest in config.AXES:
        out.append((off, attr, 4))
    for i in range(3):
        out.append((0x258 + 4 * i, "sensitivity[%d]" % i, 4))
    for i in range(4):
        out.append((0x264 + 4 * i, "buttons[%d]" % i, 4))
    for i in range(2):
        out.append((0x274 + 4 * i, "assign_slot[%d]" % i, 4))
    for n in range(config.RECORD_COUNT):
        base = config.RECORD_BASE + config.RECORD_SIZE * n
        out.append((base, "records[%d].slot" % n, 4))
        for k in range(32):
            out.append((base + 0x04 + 4 * k, "records[%d].buttons[%d]" % (n, k), 4))
        for k in range(6):
            out.append((base + 0x84 + 12 * k, "records[%d].axes[%d].value" % (n, k), 4))
            out.append((base + 0x88 + 12 * k, "records[%d].axes[%d].extra" % (n, k), 4))
            out.append((base + 0x8C + 12 * k, "records[%d].axes[%d].flag" % (n, k), 4))
        for k in range(16):
            out.append((base + 0xCC + 12 * k, "records[%d].extra[%d].action" % (n, k), 4))
            out.append((base + 0xD0 + 12 * k, "records[%d].extra[%d].lo" % (n, k), 4))
            out.append((base + 0xD4 + 12 * k, "records[%d].extra[%d].hi" % (n, k), 4))
    return out


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--game", default=os.environ.get("GUNMETAL_DIR", DEFAULT_GAME))
    ap.add_argument("--json", action="store_true", help="machine-readable rows")
    ap.add_argument("--dump", action="store_true", help="print the field map")
    ap.add_argument("--rebuild", metavar="DIR", help="write the re-serialised files")
    opts = ap.parse_args(argv)

    ok = True
    rows = []

    def add(name: str, passed: bool, detail: str) -> None:
        nonlocal ok
        ok &= passed
        rows.append({"check": name, "ok": passed, "detail": detail})
        if not opts.json:
            report(name, passed, detail)

    cfg_path = os.path.join(opts.game, "GUNMETAL.CFG")
    raw = open(cfg_path, "rb").read()
    cfg = config.parse(raw)
    if not isinstance(cfg, config.Config):
        print("FAIL: GUNMETAL.CFG did not parse as the 1.93 layout", file=sys.stderr)
        return 1
    out = cfg.to_bytes()
    add("GUNMETAL.CFG round-trip", out == raw,
        "%d B, FNV %08x -> %08x, version %s, %dx%dx%d"
        % (len(raw), fnv1a(raw), fnv1a(out), cfg.version,
           cfg.width, cfg.height, cfg.colour_depth))
    if opts.dump:
        print()
        print(config.dump(cfg))
        print()

    # 1.93 magic and size are fixed by the loader (fread 0x2000, strcmp 0x25 B)
    add("magic is av1.93 + NUL", cfg.magic == config.MAGIC_1_93, repr(cfg.magic))
    add("file size is 0x2000", len(raw) == config.FILE_SIZE, "%d" % len(raw))
    add("defined area ends at 0x8ac", config.RECORD_BASE +
        config.RECORD_COUNT * config.RECORD_SIZE == config.DATA_END,
        "0x%x" % config.DATA_END)

    # 2. structural coverage of the modelled range
    seen: dict[int, int] = {}
    for off, _name, width in field_offsets():
        for k in range(width):
            seen[off + k] = seen.get(off + k, 0) + 1
    holes = [a for a in range(config.DATA_START, config.DATA_END) if a not in seen]
    overlap = [a for a, n in seen.items() if n != 1]
    add("model covers 0x190..0x8ac", not holes and not overlap,
        "%d holes, %d overlaps" % (len(holes), len(overlap)))
    add("opaque ranges preserved", out[config.HEADER_SIZE:config.DATA_START] ==
        raw[config.HEADER_SIZE:config.DATA_START] and
        out[config.DATA_END:] == raw[config.DATA_END:],
        "0x025..0x18f and 0x8ac..end")

    # 3. writer-side proof: every field owns exactly its four bytes
    bad = []
    for off, name, width in field_offsets():
        probe = config.parse(raw)
        assert isinstance(probe, config.Config)
        target = probe
        path = name.replace("]", "").replace("[", ".").split(".")
        for part in path[:-1]:
            target = getattr(target, part) if not part.isdigit() else target[int(part)]
        leaf = path[-1]
        old = target[int(leaf)] if leaf.isdigit() else getattr(target, leaf)
        # XOR every byte so a u32 field must touch all four bytes
        new = int(old) ^ 0x01010101
        if leaf.isdigit():
            target[int(leaf)] = new
        else:
            setattr(target, leaf, new)
        blob = probe.to_bytes()
        changed = [i for i in range(len(raw)) if blob[i] != raw[i]]
        if changed != list(range(off, off + width)):
            bad.append((off, name, changed[:6]))
    add("each field writes only its own bytes", not bad, "%d bad of %d"
        % (len(bad), len(field_offsets())))

    # 4. legacy CONTROLS.CFG: same container, unproven body
    cpath = os.path.join(opts.game, "CONTROLS.CFG")
    craw = open(cpath, "rb").read()
    legacy = config.parse(craw)
    if not isinstance(legacy, config.OpaqueConfig):
        print("FAIL: CONTROLS.CFG must stay opaque (older layout)", file=sys.stderr)
        ok = False
    else:
        add("CONTROLS.CFG is opaque av1.91", legacy.version == "1.91",
            "%d B, FNV %08x, body %d B" % (len(craw), fnv1a(craw), len(legacy.body)))
        add("CONTROLS.CFG round-trip", legacy.to_bytes() == craw,
            "FNV %08x -> %08x" % (fnv1a(craw), fnv1a(legacy.to_bytes())))
        try:
            config.parse(craw, kind="gunmetal")
            add("1.93 layout refuses av1.91", False, "parsed anyway")
        except ValueError as exc:
            add("1.93 layout refuses av1.91", True, str(exc))

    # 5. KEYCFG: different loader, but the config names must select these files
    key_names = []
    for path in sorted(glob.glob(os.path.join(opts.game, "KEYCFG", "*.[kK][eE][yY]"))):
        kraw = open(path, "rb").read()
        key = small.parse_key(kraw)
        key_names.append(key.name)
        add("KEYCFG/%s round-trip" % os.path.basename(path),
            key.to_bytes() == kraw,
            "%r, %d pairs, %d B, FNV %08x"
            % (key.name, len(key.entries), len(kraw), fnv1a(kraw)))
    selected = [cfg.key_names[0].text, cfg.key_names[1].text]
    add("config key names exist in KEYCFG/", all(n in key_names for n in selected),
        "%s in %s" % (selected, key_names))

    # 6. .hi uses its own loader (0x45c8b3), only round-trip is checked
    hi_path = os.path.join(opts.game, "gunmetal.hi")
    hraw = open(hi_path, "rb").read()
    hi = small.parse_hi(hraw)
    add("gunmetal.hi round-trip (other loader)", hi.to_bytes() == hraw,
        "%d records, FNV %08x" % (len(hi.records), fnv1a(hraw)))

    # 7. report the decoded values that are cross-checked against the code
    add("screen mode matches the option string table",
        (cfg.width, cfg.height, cfg.colour_depth) == (800, 600, 8),
        "%dx%dx%d, refresh %d" % (cfg.width, cfg.height, cfg.colour_depth,
                                  cfg.refresh))
    add("option indices are inside their tables",
        cfg.axis_left_right < len(config.AXIS_NAMES)
        and cfg.axis_forward_back < len(config.AXIS_NAMES)
        and cfg.axis_up_down < len(config.AXIS_NAMES)
        and all(b < len(config.BUTTON_NAMES) for b in cfg.buttons)
        and cfg.gamma <= 3 and cfg.border <= 9
        and cfg.light_maps <= 2 and cfg.dynamic_camera <= 2
        and cfg.deathmatch_type < len(config.DEATHMATCH_MODES)
        and cfg.starting_cash < len(config.STARTING_CASH),
        "axes %s, buttons %s, gamma %d, border %d"
        % ([cfg.axis_left_right, cfg.axis_forward_back, cfg.axis_up_down],
           cfg.buttons, cfg.gamma, cfg.border))

    edit = config.parse(raw)
    assert isinstance(edit, config.Config)
    edit.width = 1024
    edit.gamma = 3
    edit.records[2].buttons[7] = 0x12345678
    blob = edit.to_bytes()
    again = config.parse(blob)
    add("edit -> to_bytes -> parse is stable",
        isinstance(again, config.Config)
        and (again.width, again.gamma, again.records[2].buttons[7])
        == (1024, 3, 0x12345678)
        and again.to_bytes() == blob,
        "width 800->1024, gamma 1->3, rec2.buttons[7]")

    for bad_name, data, kind in (
            ("truncated file", raw[:0x800], "auto"),
            ("bad magic", b"NOPE" + raw[4:], "auto"),
            ("av1.91 under 1.93 handler", craw, "gunmetal")):
        try:
            config.parse(data, kind=kind)
            add("rejects %s" % bad_name, False, "parsed anyway")
        except ValueError as exc:
            add("rejects %s" % bad_name, True, str(exc)[:60])

    add("four control records are identical",
        all(r.buttons == cfg.records[0].buttons and r.axes == cfg.records[0].axes
            and r.extra == cfg.records[0].extra for r in cfg.records),
        "slot %d, %d buttons, 6 axes, 16 extra"
        % (cfg.records[0].slot, len(cfg.records[0].buttons)))

    if opts.rebuild:
        os.makedirs(opts.rebuild, exist_ok=True)
        with open(os.path.join(opts.rebuild, "GUNMETAL.CFG"), "wb") as fh:
            fh.write(out)
        if isinstance(legacy, config.OpaqueConfig):
            with open(os.path.join(opts.rebuild, "CONTROLS.CFG"), "wb") as fh:
                fh.write(legacy.to_bytes())

    if opts.json:
        for row in rows:
            print('{"check": %r, "ok": %s, "detail": %r}'
                  % (row["check"], str(row["ok"]).lower(), row["detail"]))
    else:
        print()
        print("%d checks, %s" % (len(rows), "all ok" if ok else "FAILURES"))
        print("config  FNV-1a %08x  %s" % (fnv1a(raw), config.summary(cfg)))
    return 0 if ok else 1


if __name__ == "__main__":
    raise SystemExit(main())
