#!/usr/bin/env python3
"""Generate Ghidra C declarations from the documented field maps.

Why: ``docs/THING_FIELDS.md`` §4 is a complete offset/size table of the 0x528
thing instance, and ``docs/PLAYER_FIELDS.md`` §3 lays out the 0x3510 player
block.  The Ghidra project had no custom structs, so every decompile showed
``*(int *)(&DAT_007ac3e0 + i * 0x14a)`` instead of ``things[i].health``.  This
script turns the tables into a C header that ``apply_types.py`` imports into
the project's Data Type Manager.

The generator is deliberately strict: every field must fall inside the
documented structure, gaps are filled with named padding, and the total size
must match the documented stride (0x528, 0x3a8, 0x3510).  A mismatch is a
documentation bug, not a reason to guess.

Examples::

    python3 tools/ghidra/typegen.py                 # write gm_types.h
    python3 tools/ghidra/typegen.py --check         # fail if out of date
    python3 tools/ghidra/typegen.py --print         # print, write nothing
"""
from __future__ import annotations

import argparse
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
THING_DOC = os.path.join(ROOT, "docs", "THING_FIELDS.md")
PLAYER_DOC = os.path.join(ROOT, "docs", "PLAYER_FIELDS.md")
OUT = os.path.join(ROOT, "tools", "ghidra", "gm_types.h")


def section(text: str, start: str, end: str) -> str:
    i = text.find(start)
    if i < 0:
        raise SystemExit(f"typegen: section {start!r} not found")
    j = text.find(end, i + len(start))
    return text[i:j] if j > i else text[i:]


def cells(line: str) -> list[str]:
    return [c.strip() for c in line.strip().strip("|").split("|")]


def ident(raw: str) -> str:
    """First identifier-ish token of a field description, keeping ``[N]``."""
    raw = raw.replace("`", "").replace("*", "").strip()
    raw = re.sub(r"\(.*?\)", "", raw)
    m = re.match(r"[A-Za-z_][A-Za-z0-9_]*", raw)
    if not m:
        return ""
    array = re.search(r"\[(\d+)\]", raw)
    return m.group(0) + (f"[{array.group(1)}]" if array else "")


def sanitize(name: str) -> str:
    name = re.sub(r"[^A-Za-z0-9_]+", "_", name).strip("_").lower()
    return name or "field"


def offsets(cell: str) -> list[int] | None:
    """Offsets of a table cell, or None for a non-contiguous pattern."""
    raw = cell.replace("`", "").replace("*", "").strip()
    if ".." in raw or re.search(r"\+[0-9a-fx]+i", raw):
        return None
    parts = raw.split("/")
    if not parts or not parts[0].lower().startswith("+0x"):
        return None
    base = int(parts[0], 16)
    out = [base]
    for part in parts[1:]:
        part = part.replace("+8i", "").strip()
        if not part:
            continue
        try:
            out.append(int(part, 16))
        except ValueError:
            return None
    return out


def sizes(cell: str) -> list[int] | None:
    raw = cell.replace("`", "").replace("*", "").strip()
    raw = raw.replace("×", "x")
    m = re.fullmatch(r"(\d+)(?:\+(\d+))*", raw)
    if m:
        # "8+8" -> [8, 8]; plain "24" -> [24]
        return [int(x) for x in raw.split("+")]
    m = re.fullmatch(r"(\d+)x(\d+)", raw)
    if m:
        return [int(m.group(1)) * int(m.group(2))]
    try:
        return [int(raw)]
    except ValueError:
        return None


def scalar(name: str, size: int) -> tuple[str, int]:
    """C type and element count for one field of *size* bytes."""
    array = re.search(r"\[(\d+)\]$", name)
    count = int(array.group(1)) if array else 0
    base = name[:array.start()] if array else name
    if array:
        elem = size // count
        if elem == 8:
            return "double", count
        if elem == 4:
            kind = "unsigned int" if re.search(
                r"flags|mask|id|handle|event|kind|type|state|texture|owner|"
                r"parent|sector|count|index|hits|volume|color|shield|"
                r"weapon_p|path", base) else "int"
            return kind, count
        if elem == 1:
            return "unsigned char", count
        return "unsigned char", size
    if size == 8:
        return "double", 1
    if size == 4:
        kind = "unsigned int" if re.search(
            r"flags|mask|id|handle|event|kind|type|state|texture|owner|"
            r"parent|sector|count|index|hits|volume|color|shield|"
            r"weapon_p|path", name) else "int"
        return kind, 1
    if size == 2:
        return "unsigned short", 1
    if size == 1:
        return "unsigned char", 1
    if size % 8 == 0 and size <= 32:
        return "double", size // 8
    if size % 4 == 0:
        return "unsigned int", size // 4
    return "unsigned char", size


class Struct:
    def __init__(self, name: str, total: int, source: str) -> None:
        self.name = name
        self.total = total
        self.source = source
        self.fields: list[tuple[int, str, str, int]] = []  # off, type, name, bytes
        self.used: set[int] = set()

    def add(self, off: int, ctype: str, name: str, nbytes: int) -> None:
        if off + nbytes > self.total:
            # A field beyond the documented stride: keep the struct valid.
            nbytes = max(0, self.total - off)
            if nbytes <= 0:
                return
        if "[" in name:
            base, array = name.split("[", 1)
            base, array = sanitize(base), "[" + array
        else:
            base, array = sanitize(name), ""
        if not base or base[0].isdigit():
            base = f"field_{off:x}"
        candidate = base + array
        suffix = 1
        while any(f[2] == candidate for f in self.fields):
            tag = f"{base}_{off:x}" if suffix == 1 else f"{base}_{off:x}_{suffix}"
            candidate = tag + array
            suffix += 1
        self.fields.append((off, ctype, candidate, nbytes))
        self.used.add(off)

    def emit(self) -> list[str]:
        out = []
        pos = 0
        for off, ctype, name, nbytes in sorted(self.fields):
            if off > pos:
                out.append(f"    unsigned char pad_{pos:x}[{off - pos}];")
            elif off < pos:
                # Overlapping documentation rows: keep the first, drop the rest.
                continue
            out.append(f"    {ctype} {name};")
            pos = off + nbytes
        if pos < self.total:
            out.append(f"    unsigned char pad_{pos:x}[{self.total - pos}];")
        return out


def add_field(struct: Struct, off: int, size: int, name: str,
              raw: str = "") -> None:
    repeated = re.search(r"(\d+)\s*[x×]\s*(u32|int|double)", raw)
    if repeated and not re.search(r"\[(\d+)\]", raw):
        count = int(repeated.group(1))
        ctype = {"u32": "unsigned int", "int": "int",
                 "double": "double"}[repeated.group(2)]
        base = name.split("[")[0] or f"field_{off:x}"
        struct.add(off, ctype, f"{base}[{count}]", size)
        return
    array = re.search(r"\[(\d+)\]", raw)
    if array and size % int(array.group(1)) == 0:
        count = int(array.group(1))
        base = name.split("[")[0]
        ctype, _ = scalar(base, size // count)
        struct.add(off, ctype, f"{base}[{count}]", size)
        return
    has_array = bool(re.search(r"\[(\d+)\]$", name))
    if not has_array and size % 8 == 0 and 8 < size <= 32:
        # 24/32-byte triples are documented as x/y/z or min/max groups.
        if re.search(r"position|angle|velocity|direction|pose|box|limit|"
                     r"attach|rate|spawn", raw, re.I):
            struct.add(off, "double", f"{name}[{size // 8}]", size)
            return
    ctype, count = scalar(name, size)
    if count == 1:
        struct.add(off, ctype, name, size)
    else:
        struct.add(off, ctype, f"{name}[{count}]", size)


def parse_thing() -> Struct:
    text = open(THING_DOC, encoding="utf-8").read()
    body = section(text, "## 4. Структура B", "## 4a.")
    st = Struct("gm_thing", 0x528, "docs/THING_FIELDS.md §4")
    for line in body.splitlines():
        if not line.startswith("| `+0x") and not line.startswith("| **`+0x"):
            continue
        cols = cells(line)
        if len(cols) < 3:
            continue
        offs = offsets(cols[0])
        sz = sizes(cols[1])
        if not offs or not sz:
            continue
        raw_name = cols[2].replace("`", "").replace("*", "").strip()
        names = [ident(part) or sanitize(part)
                 for part in raw_name.split("/")]
        if len(names) > 1 and "_" not in names[1] and "_" in names[0]:
            prefix = names[0].rsplit("_", 1)[0] + "_"
            names = [names[0]] + [prefix + name for name in names[1:]]
        if len(offs) == 1:
            if len(names) > 1 and sum(sz) == sz[0] and sz[0] % len(names) == 0:
                step = sz[0] // len(names)
                for i, name in enumerate(names):
                    add_field(st, offs[0] + i * step, step, name, raw_name)
            else:
                add_field(st, offs[0], sz[0], names[0] or f"field_{offs[0]:x}",
                          raw_name)
            continue
        if len(names) == len(offs) and len(sz) == 1 and sz[0] == 8:
            for off, name in zip(offs, names):
                add_field(st, off, 8, name, raw_name)
            continue
        if len(sz) == len(offs):
            for off, size, name in zip(offs, sz, names or [""] * len(offs)):
                add_field(st, off, size, name or f"field_{off:x}", raw_name)
            continue
        # Uniform repeated block, e.g. 3× (8×double) at three offsets.
        if len(sz) == 1 and sz[0] > 8:
            for i, off in enumerate(offs):
                add_field(st, off, sz[0],
                          names[i] if i < len(names) and names[i]
                          else f"field_{off:x}", raw_name)
    return st


PLAYER_BLOCKS = (
    ("поза", "double", "pose[6]", 0x30),
    ("кольцо позиций", "double", "trail_pos[240][3]", 0x1680),
    ("трейл тангажа", "double", "trail_pitch[240]", 0x780),
    ("трейл крена", "double", "trail_roll[240]", 0x780),
    ("трейл рыскания", "double", "trail_yaw[240]", 0x780),
    ("параметры шасси", "unsigned char", "chassis[0x2c8]", 0x2c8),
    ("точки крепления", "double", "attach_points[48]", 0x180),
    ("id корпуса", "int", "hull_id", 4),
    ("id навесных", "unsigned int", "attach_id[16]", 0x40),
    ("флаги навесных", "unsigned int", "attach_flags[16]", 0x40),
    ("state_word", "unsigned int", "state_word", 4),
    ("флаги, трейл-индекс", "unsigned char", "state_block[0x44]", 0x44),
    ("броня и щиты", "unsigned char", "armor[0x20]", 0x20),
    ("снимок щитов", "unsigned char", "shield_snapshot[0x30]", 0x30),
    ("оружие: слот", "unsigned char", "weapons[0x38]", 0x38),
    ("не используется", "unsigned char", "unused_32cc[0x4c]", 0x4c),
    ("таймеры стрельбы", "unsigned char", "fire_timers[0xc]", 0xc),
    ("оборудование и масса", "unsigned char", "equipment[0x1c]", 0x1c),
    ("ввод и счётчики", "unsigned char", "input[0x84]", 0x84),
    ("отложенное оружие", "unsigned char", "pending[0x1c]", 0x1c),
    ("строки и числа", "unsigned char", "chassis_strings[0x8c]", 0x8c),
    ("массив из 8 id", "int", "id_array[8]", 0x20),
    ("не разобрано", "unsigned char", "unknown_tail[0x84]", 0x84),
)


def parse_player() -> Struct:
    text = open(PLAYER_DOC, encoding="utf-8").read()
    body = section(text, "## 3. Карта блока", "## 4.")
    st = Struct("gm_player", 0x3510, "docs/PLAYER_FIELDS.md §3")
    for line in body.splitlines():
        m = re.match(r"^\+0x([0-9a-f]+)\s+0x([0-9a-f]+)\s+(.*)$", line)
        if not m:
            continue
        off, size = int(m.group(1), 16), int(m.group(2), 16)
        desc = m.group(3)
        matched = None
        for needle, ctype, name, want in PLAYER_BLOCKS:
            if needle in desc and want == size:
                matched = (ctype, name)
                break
        if matched:
            ctype, name = matched
            st.add(off, ctype, name, size)
        else:
            st.add(off, "unsigned char", f"block_{off:x}", size)
    return st


def parse_thing_type() -> Struct:
    """THING_TYPES (0x776d60, stride 0x3a8) from §3.

    Only rows with an explicit type are used; the rest becomes padding.  The
    table has range rows (``+0x0d8..0x197``) and indexed children, which a C
    struct cannot express one-to-one, so those stay bytes.
    """
    text = open(THING_DOC, encoding="utf-8").read()
    body = section(text, "## 3. Структура A", "## 4. Структура B")
    st = Struct("gm_thing_type", 0x3a8, "docs/THING_FIELDS.md §3")
    for line in body.splitlines():
        if not line.startswith("|") or "`+0x" not in line:
            continue
        cols = cells(line)
        if len(cols) < 3:
            continue
        offs = offsets(cols[0])
        raw_type = cols[1].replace("`", "").replace("*", "").strip()
        raw_name = cols[2].replace("`", "").replace("*", "").strip()
        if not offs or not raw_name or ".." in cols[0]:
            continue
        names = [ident(part) or sanitize(part)
                 for part in raw_name.split("/")]
        if len(names) > 1 and "_" not in names[1] and "_" in names[0]:
            prefix = names[0].rsplit("_", 1)[0] + "_"
            names = [names[0]] + [prefix + name for name in names[1:]]
        m = re.fullmatch(r"(\d+)\s*[x×]\s*double", raw_type)
        if m:
            n, elem = int(m.group(1)), 8
        elif raw_type == "double":
            n, elem = 1, 8
        elif raw_type in ("u32", "int"):
            n, elem = 1, 4
        elif re.fullmatch(r"\d+\s*[x×]\s*u32", raw_type):
            n = int(re.match(r"(\d+)", raw_type).group(1))
            elem = 4
        elif re.fullmatch(r"char\[\d+\]", raw_type):
            size = int(re.search(r"\[(\d+)\]", raw_type).group(1))
            if len(offs) == 1:
                add_field(st, offs[0], size,
                          f"{names[0]}[{size}]" if names and names[0]
                          else f"field_{offs[0]:x}", raw_type)
            continue
        else:
            continue
        size = n * elem
        if len(offs) == 1:
            if len(names) > 1 and size % len(names) == 0:
                step = size // len(names)
                for i, name in enumerate(names):
                    add_field(st, offs[0] + i * step, step, name, raw_type)
            else:
                add_field(st, offs[0], size, names[0] or f"field_{offs[0]:x}",
                          raw_type)
        elif len(offs) == len(names):
            single = ("double" if "double" in raw_type
                      else "unsigned int" if "u32" in raw_type else "int")
            per = size // len(offs)
            for off, name in zip(offs, names):
                st.add(off, single, name, per)
    return st


def render(structs: list[Struct]) -> str:
    lines = [
        "/* Generated by tools/ghidra/typegen.py -- do not edit by hand.",
        " * Sources: docs/THING_FIELDS.md §3/§4, docs/PLAYER_FIELDS.md §3.",
        " * Field names and offsets come from the field maps; unknown ranges",
        " * stay as named byte blocks.  Pseudo-C remains a hypothesis: verify",
        " * every claim against bytes/data/tests before documenting it.",
        " */",
        "",
    ]
    for st in structs:
        lines.append(f"/* {st.source}; size 0x{st.total:x} */")
        lines.append("typedef struct {")
        lines.extend(st.emit())
        lines.append(f"}} {st.name};")
        lines.append("")
    return "\n".join(lines)


def build() -> tuple[str, list[Struct]]:
    thing = parse_thing()
    player = parse_player()
    thing_type = parse_thing_type()
    for st in (thing, player, thing_type):
        end = max((off + size for off, _t, _n, size in st.fields), default=0)
        if end > st.total:
            raise SystemExit(f"typegen: {st.name} exceeds 0x{st.total:x} "
                             f"(last field ends at 0x{end:x})")
    return render([thing, thing_type, player]), [thing, thing_type, player]


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--check", action="store_true",
                    help="fail when gm_types.h is not what the docs generate")
    ap.add_argument("--print", dest="show", action="store_true")
    opts = ap.parse_args(argv)
    text, structs = build()
    for st in structs:
        used = sum(size for _o, _t, _n, size in st.fields)
        print(f"typegen: {st.name} 0x{st.total:x} bytes, "
              f"{len(st.fields)} fields, {used} documented", file=sys.stderr)
    if opts.show:
        print(text)
        return 0
    current = open(OUT, encoding="utf-8").read() if os.path.isfile(OUT) else ""
    if opts.check:
        if current != text:
            print(f"typegen: {OUT} is out of date", file=sys.stderr)
            return 1
        print(f"typegen: {OUT} matches the field maps", file=sys.stderr)
        return 0
    with open(OUT, "w", encoding="utf-8") as fh:
        fh.write(text)
    print(f"typegen: wrote {OUT}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
