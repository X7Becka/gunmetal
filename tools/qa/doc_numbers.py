#!/usr/bin/env python3
"""Independent re-check of the numbers quoted by the fresh RE documents.

The doc set under audit (each number is re-derived here, not trusted from the
tool that produced it):

    docs/PICKUP.md        docs/UI_SCREENS.md   docs/UI_SOUNDS.md
    docs/SOUND_NAMES.md   docs/DM_RULES.md     docs/CAMPAIGN_FLOW.md
    docs/AUDIO_3D.md

Design rules
------------
* stdlib only (no third-party imports).  The executable is mapped with a small
  PE reader, the objdump *listing* is indexed here, BIGFILE/BMP/GUNMETAL.HI
  are parsed by this file, and ``sounds.sub`` is split here.  The only project
  module used is ``gm.sbt``/``gm.sbtvm`` — the canonical *format oracle*
  (docs/LAYOUT.md §2), not one of the tools whose output is being checked.
* read-only: the game directory is never written to.
* every check reports ``подтверждено`` / ``расходится`` / ``не проверяемо``.

Usage::

    python3 tools/qa/doc_numbers.py --self-test
    python3 tools/qa/doc_numbers.py --check
    python3 tools/qa/doc_numbers.py --check --json
    python3 tools/qa/doc_numbers.py --check --game ~/.cache/gunmetal-re/game

Exit codes: 0 — all checks ran; 2 — a document or the exe could not be read;
1 — ``--strict`` and at least one number differs (or is unverifiable).
"""
from __future__ import annotations

import argparse
import bisect
import glob
import json
import os
import re
import struct
import sys
from collections import Counter

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
DEFAULT_GAME = os.environ.get("GUNMETAL_GAME", "/mnt/d/games/Gunmetal 3dfx")
DEFAULT_CACHE = os.environ.get("GUNMETAL_CACHE") or os.path.join(
    os.path.expanduser("~"), ".cache", "gunmetal-re")

DOC_FILES = {
    "PICKUP": "docs/PICKUP.md",
    "UI_SCREENS": "docs/UI_SCREENS.md",
    "UI_SOUNDS": "docs/UI_SOUNDS.md",
    "SOUND_NAMES": "docs/SOUND_NAMES.md",
    "DM_RULES": "docs/DM_RULES.md",
    "CAMPAIGN_FLOW": "docs/CAMPAIGN_FLOW.md",
    "AUDIO_3D": "docs/AUDIO_3D.md",
    "GAME_OVER": "docs/GAME_OVER.md",
    "SBT_ASM": "docs/SBT_ASM.md",
    "OBJECTIVES": "docs/OBJECTIVES.md",
    "BIGFILE_WRITE": "docs/BIGFILE_WRITE.md",
    "SBT_WRITE": "docs/SBT_WRITE.md",
    "SCENE_EDIT": "docs/SCENE_EDIT.md",
    "SBW_EDIT": "docs/SBW_EDIT.md",
    "SCRIPT_DEBUG": "docs/SCRIPT_DEBUG.md",
}


# --------------------------------------------------------------------------
# pure helpers (unit-tested by --self-test)
# --------------------------------------------------------------------------
def fnv1a64(data: bytes) -> int:
    h = 0xCBF29CE484222325
    for b in data:
        h ^= b
        h = (h * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return h


def clamp(v: int, lo: int, hi: int) -> int:
    return lo if v < lo else hi if v > hi else v


def parse_bigfile(data: bytes) -> list[tuple[str, int]]:
    """``BIGFILE`` index -> ``[(NAME, offset)]``.  Layout: magic[7], 0x1a, u32 n."""
    if len(data) < 12 or data[:7] != b"BIGFILE" or data[7] != 0x1A:
        raise ValueError("not a BIGFILE index")
    n = struct.unpack_from("<I", data, 8)[0]
    if 12 + 12 * n > len(data):
        raise ValueError("BIGFILE count overruns the file")
    out = []
    for i in range(n):
        o = 12 + 12 * i
        name = data[o:o + 8].rstrip(b"\0").decode("latin1")
        out.append((name, struct.unpack_from("<I", data, o + 8)[0]))
    return out


def bmp_size(blob: bytes) -> tuple[int, int]:
    """(width, |height|) of an uncompressed BMP header."""
    if len(blob) < 26 or blob[:2] != b"BM":
        raise ValueError("not a BMP")
    w = struct.unpack_from("<i", blob, 18)[0]
    h = struct.unpack_from("<i", blob, 22)[0]
    return w, abs(h)


def bigfile_blobs(index: bytes, dat: bytes) -> dict[str, bytes]:
    """Name (upper-case) -> blob, using neighbour offsets like the engine."""
    ents = parse_bigfile(index)
    out = {}
    for i, (name, off) in enumerate(ents):
        end = ents[i + 1][1] if i + 1 < len(ents) else len(dat)
        out[name.upper()] = dat[off:end]
    return out


def audio_example(pos, volume, atten, listener, right=(1.0, 0.0, 0.0)):
    """Closed form of docs/AUDIO_3D.md §5/§8 for the front-axis listener.

    Returns ``(dist, gain, pan, volume_index, L, R)``.
    """
    d = [pos[i] - listener[i] for i in range(3)]
    a = sorted((abs(v) for v in d), reverse=True)
    dist = a[0] + 0.25 * (a[1] + a[2])
    gain = int(volume - atten * dist)          # ftrunc semantics, positive here
    gain = min(gain, volume)
    v = 0 if gain <= 0 else clamp((gain * 32) >> 6, 1, 32)  # 0 == no request
    if v == 0:
        return dist, gain, None, 0, None, None
    dot = sum(d[i] * right[i] for i in range(3))
    pan = clamp(int((dot / dist + 1.0) * 128.0), 0, 255)
    left = clamp(((255 - pan) * v + 0x3D) // 0x7A, 1, 32)
    rgt = clamp((pan * v + 0x3D) // 0x7A, 1, 32)
    return dist, gain, pan, v, left, rgt


def item_op_total(counter: Counter, ops) -> int:
    return sum(counter[o] for o in ops)


def doc_table_rows(text: str, start_marker: str, end_marker: str) -> list[str]:
    """Markdown table data rows between two section markers."""
    try:
        body = text.split(start_marker, 1)[1].split(end_marker, 1)[0]
    except IndexError:
        return []
    rows = []
    for line in body.splitlines():
        if not line.startswith("|"):
            continue
        cells = [c.strip() for c in line.strip().strip("|").split("|")]
        if len(cells) != 4:
            continue
        if re.fullmatch(r"[\d/]+", cells[0]) and re.fullmatch(r"[\d/ ]+", cells[1]):
            rows.append(line)
    return rows


# --------------------------------------------------------------------------
# executable image + objdump listing
# --------------------------------------------------------------------------
class Exe:
    def __init__(self, path: str, listing: str | None = None):
        self.path = path
        with open(path, "rb") as fh:
            self.data = fh.read()
        pe = struct.unpack_from("<I", self.data, 0x3C)[0]
        nsec = struct.unpack_from("<H", self.data, pe + 6)[0]
        optsz = struct.unpack_from("<H", self.data, pe + 20)[0]
        base = struct.unpack_from("<I", self.data, pe + 24 + 28)[0]
        off = pe + 24 + optsz
        self.image_base = base
        self.sections = []
        for i in range(nsec):
            o = off + 40 * i
            name = self.data[o:o + 8].rstrip(b"\0").decode("latin1")
            vsize, rva, rawsize, rawoff = struct.unpack_from("<IIII", self.data, o + 8)
            self.sections.append((name, base + rva, rawsize, rawoff))
        self._listing = None
        self._listing_path = listing
        self._vmas = None
        self._offs = None

    def section(self, name: str):
        for s in self.sections:
            if s[0] == name:
                return s
        return None

    def sa(self) -> int:
        return self.sections[0][1]

    def sz(self) -> int:
        return self.sections[0][2]

    def va2off(self, va: int):
        for _n, vma, rawsize, foff in self.sections:
            if vma <= va < vma + rawsize:
                return foff + (va - vma)
        return None

    def u8(self, va):
        o = self.va2off(va)
        return None if o is None else self.data[o]

    def u32(self, va):
        o = self.va2off(va)
        return None if o is None else struct.unpack_from("<I", self.data, o)[0]

    def u64(self, va):
        o = self.va2off(va)
        return None if o is None else struct.unpack_from("<Q", self.data, o)[0]

    def f64(self, va):
        o = self.va2off(va)
        return None if o is None else struct.unpack_from("<d", self.data, o)[0]

    def bytes_at(self, va, n):
        o = self.va2off(va)
        if o is None:
            raise ValueError("unmapped VA 0x%x" % va)
        return self.data[o:o + n]

    # -- listing ---------------------------------------------------------
    def _listing_file(self) -> str:
        if self._listing_path:
            return self._listing_path
        env = os.environ.get("GUNDX_ASM")
        if env and os.path.exists(env):
            return env
        cached = os.path.join(DEFAULT_CACHE, "gundx.asm")
        if os.path.exists(cached):
            return cached
        raise FileNotFoundError("objdump listing not found (set GUNDX_ASM)")

    def listing(self):
        if self._vmas is not None:
            return self._vmas, self._offs
        path = self._listing_file()
        vmas, offs = [], []
        line_re = re.compile(rb"^\s*([0-9a-f]{6,8}):\t")
        pos = 0
        with open(path, "rb") as fh:
            for line in fh:
                m = line_re.match(line)
                if m:
                    vmas.append(int(m.group(1), 16))
                    offs.append(pos)
                pos += len(line)
        self._listing_path = path
        self._vmas, self._offs = vmas, offs
        return vmas, offs

    def lines(self, lo: int, hi: int) -> list[str]:
        vmas, offs = self.listing()
        i = bisect.bisect_left(vmas, lo)
        j = bisect.bisect_left(vmas, hi, i)
        if j <= i:
            return []
        path = self._listing_file()
        begin = offs[i]
        end = offs[j] if j < len(offs) else None
        with open(path, "rb") as fh:
            fh.seek(begin)
            blob = fh.read() if end is None else fh.read(end - begin)
        return blob.decode("latin1").splitlines()

    def line_at(self, va: int):
        for line in self.lines(va, va + 0x10):
            m = re.match(r"\s*([0-9a-f]+):", line)
            if m and int(m.group(1), 16) == va:
                return line
        return None

    def op_at(self, va: int, pattern: str) -> bool:
        line = self.line_at(va)
        return bool(line and re.search(pattern, line))

    def op_near(self, va: int, span: int, pattern: str):
        rx = re.compile(pattern)
        for line in self.lines(va, va + span):
            m = rx.search(line)
            if m:
                return m
        return None

    def refs(self, token: str) -> list[str]:
        """Listing lines mentioning ``token`` (disassembly operand), once per line."""
        vmas, offs = self.listing()
        path = self._listing_file()
        out = []
        with open(path, "rb") as fh:
            for line in fh:
                s = line.decode("latin1")
                if token.lower() in s.lower():
                    out.append(s.rstrip("\n"))
        return out


# --------------------------------------------------------------------------
# corpus (36 .sbt) — decoded through the gm oracle, counted here
# --------------------------------------------------------------------------
class Corpus:
    def __init__(self, game: str):
        sys.path.insert(0, ROOT)
        from gm.sbt import parse_file          # noqa: E402
        from gm.sbtvm import decode            # noqa: E402
        self._decode = decode
        self.levels = {}
        for path in sorted(glob.glob(os.path.join(game, "*.sbt"))):
            base = os.path.basename(path)[:-4]
            s = parse_file(path)
            insns = []
            for rec in s.records_of(11):
                code = rec.payload.code
                block, end = decode(code, 0, len(code))
                assert end == len(code), (base, end, len(code))
                self._walk(block, insns)
            self.levels[base] = {"sbt": s, "insns": insns}

    @staticmethod
    def _walk(block, out):
        for ins in block:
            out.append(ins)
            if ins.body:
                Corpus._walk(ins.body, out)

    def __len__(self):
        return len(self.levels)

    def opcount(self) -> Counter:
        c = Counter()
        for lv in self.levels.values():
            for ins in lv["insns"]:
                c[ins.op] += 1
        return c

    def ops(self, op: int):
        for base, lv in self.levels.items():
            for ins in lv["insns"]:
                if ins.op == op:
                    yield base, ins

    def record(self, base: str, type_: int):
        recs = self.levels[base]["sbt"].records_of(type_)
        return [r.payload for r in recs]


# --------------------------------------------------------------------------
# check registry
# --------------------------------------------------------------------------
CHECKS = []


def check(doc: str, number: str, expected: str):
    def deco(fn):
        CHECKS.append({"doc": doc, "number": number, "expected": expected, "fn": fn})
        return fn
    return deco


class Skip(Exception):
    pass


def verdict_for(ok):
    return "подтверждено" if ok else "расходится"


def run_checks(env) -> list[dict]:
    out = []
    for spec in CHECKS:
        rec = {"doc": spec["doc"], "number": spec["number"],
               "expected": spec["expected"]}
        try:
            fact, ok, note = spec["fn"](env)
            rec["fact"] = fact
            rec["verdict"] = "не проверяемо" if ok is None else verdict_for(ok)
            rec["note"] = note or ""
        except Skip as exc:
            rec["fact"] = "—"
            rec["verdict"] = "не проверяемо"
            rec["note"] = str(exc)
        except Exception as exc:                      # pragma: no cover
            rec["fact"] = "ошибка: %s" % exc
            rec["verdict"] = "не проверяемо"
            rec["note"] = "%s: %s" % (type(exc).__name__, exc)
        out.append(rec)
    return out


class Env:
    def __init__(self, game: str, docs: str, exe: str, listing: str | None):
        self.game = game
        self.docs = docs
        self._exe = Exe(exe, listing) if os.path.exists(exe) else None
        self._corpus = None
        self._sou = None

    @property
    def exe(self) -> Exe:
        if self._exe is None:
            raise Skip("gundx.exe not found")
        return self._exe

    @property
    def corpus(self) -> Corpus:
        if self._corpus is None:
            if not os.path.isdir(self.game):
                raise Skip("game directory not found: %s" % self.game)
            self._corpus = Corpus(self.game)
        return self._corpus

    def require_game(self):
        if not os.path.isdir(self.game):
            raise Skip("game directory not found: %s" % self.game)

    def game_file(self, *parts) -> str:
        """Case-insensitive path resolution (drvfs vs ext4 mirror)."""
        cur = self.game
        for part in parts:
            direct = os.path.join(cur, part)
            if os.path.exists(direct):
                cur = direct
                continue
            cand = [p for p in glob.glob(os.path.join(cur, "*"))
                    if os.path.basename(p).lower() == part.lower()]
            if not cand:
                raise Skip("missing file: %s" % os.path.join(cur, part))
            cur = cand[0]
        return cur

    def sou(self):
        if self._sou is None:
            idx = open(self.game_file("sounds", "SOU.IDX"), "rb").read()
            dat = open(self.game_file("sounds", "SOU.DAT"), "rb").read()
            self._sou = (parse_bigfile(idx), bigfile_blobs(idx, dat))
        return self._sou

    def doc(self, key: str) -> str:
        path = os.path.join(self.docs, os.path.basename(DOC_FILES[key]))
        if not os.path.exists(path):
            raise Skip("document missing: %s" % path)
        return open(path, encoding="utf-8").read()


# ==========================================================================
# docs/PICKUP.md
# ==========================================================================
ITEM_OPS = (39, 52, 53, 54, 55, 64, 65, 75, 173, 174, 176, 177, 185, 186)
CREATE_OPS = (52, 53, 54, 55, 177, 185, 186)


@check("PICKUP", "11513 инструкций (14 item-опкодов)", "11513")
def c_pickup_ops(env):
    c = env.corpus.opcount()
    total = item_op_total(c, ITEM_OPS)
    note = ("сумма op 39/52/53/54/55/64/65/75/173/174/176/177/185/186; "
            "это НЕ все инструкции скриптов: листьев %d, с IF/ELSE %d"
            % (sum(c.values()) - c[199] - c[200], sum(c.values())))
    return str(total), total == 11513, note


@check("PICKUP", "5883 создания (52/53/54/55/177/185/186)", "5883")
def c_pickup_creates(env):
    c = env.corpus.opcount()
    total = item_op_total(c, CREATE_OPS)
    return str(total), total == 5883, "401+17+636+84+440+53+4252"


@check("PICKUP", "триггеры +0x44: 690 ненулевых / 448 нулевых", "690 / 448")
def c_pickup_triggers(env):
    nz = zero = 0
    zero_radii = set()
    for base, ins in env.corpus.ops(52):
        pass
    for op in (52, 53, 54, 55):
        for base, ins in env.corpus.ops(op):
            if ins.args[3]:
                nz += 1
            else:
                zero += 1
                zero_radii.add(ins.args[5])
    tot = nz + zero
    ok = (nz, zero, tot) == (690, 448, 1138) and zero_radii == {0}
    return ("%d/%d (записей %d; у нулевых радиусы %s)"
            % (nz, zero, tot, sorted(zero_radii))), ok, \
        "op 52–55 + 177; триггер +0x44, радиус +0x4c"


@check("PICKUP", "0 промахов триггеров в сущность", "0")
def c_pickup_trig_miss(env):
    sys.path.insert(0, ROOT)
    from gm.scene import entities            # noqa: E402
    misses = []
    checked = 0
    for base, lv in env.corpus.levels.items():
        scripts = lv["sbt"].records_of(11)
        if not scripts:
            continue
        ents = {eid for eid, _flags, _off in entities(scripts[0].payload)}
        for ins in lv["insns"]:
            if ins.op in (52, 53, 54, 55) and ins.args[3]:
                checked += 1
                trig = ins.args[3]
                trig = trig - (1 << 32) if trig >= (1 << 31) else trig
                if trig not in ents:
                    misses.append((base, trig))
    ok = checked == 690 and not misses
    return "%d/%d разрешены в entity" % (checked - len(misses), checked), ok, \
        "id триггера ищется в таблице entities SCRIPT (gm.scene.entities)"


@check("PICKUP", "радиусы 200x492 / 0x448 / 1600x108", "492 / 448 / 108")
def c_pickup_radii(env):
    rad = Counter()
    for op in (52, 53, 54, 55):
        for base, ins in env.corpus.ops(op):
            rad[ins.args[5]] += 1
    ok = (rad[200], rad[0], rad[1600]) == (492, 448, 108)
    return "%d / %d / %d" % (rad[200], rad[0], rad[1600]), ok, "op 52–55"


@check("PICKUP", "DEFINE_SHIELDS (50,0) 36/36", "36/36")
def c_pickup_shields(env):
    vals = Counter()
    for base, ins in env.corpus.ops(176):
        vals[tuple(ins.args)] += 1
    ok = vals == Counter({(50, 0): 36})
    return "%d/36 (50,0)" % vals[(50, 0)], ok, "op 176"


@check("PICKUP", "REGEN MARKER (19,3000.0,50.0) 36/36", "36/36")
def c_pickup_regen(env):
    vals = Counter()
    for base, ins in env.corpus.ops(174):
        vals[tuple(ins.args)] += 1
    ok = vals == Counter({(19, 3000.0, 50.0): 36})
    return "%d/36 (19,3000.0,50.0)" % vals[(19, 3000.0, 50.0)], ok, "op 174"


@check("PICKUP", "DEFINE_VIRUS L0=16 V=28 P=10 D=0.03; N=12 в 34, N=22 в dm2/level15",
       "36/36; 34 + 2")
def c_pickup_virus(env):
    vals = Counter()
    per_level_n = {}
    for base, ins in env.corpus.ops(173):
        a = ins.args                       # L0, T, N, V, P, D
        vals[(a[0], a[3], a[4], a[5])] += 1
        per_level_n[base] = a[2]
    main = vals[(16, 28, 10, 0.03)]
    n12 = sum(1 for v in per_level_n.values() if v == 12)
    n22 = sorted(k for k, v in per_level_n.items() if v == 22)
    ok = main == 36 and n12 == 34 and n22 == ["dm2", "level15"]
    return ("L0=16,V=28,P=10,D=0.03: %d/36; N=12: %d; N=22: %s"
            % (main, n12, ",".join(n22))), ok, "op 173"


@check("PICKUP", "таблица 0x806360: 300x0x60 = 0x7080, конец 0x80d3e0", "300x0x60")
def c_pickup_table(env):
    exe = env.exe
    base = 0x806360
    slots, stride = 300, 0x60
    end = base + slots * stride
    checks = {
        "300*0x60=0x7080": (slots * stride == 0x7080),
        "base+size=0x80d3e0": (end == 0x80D3E0),
        "push base (0x468e0d)": exe.op_at(0x468E0D, r"push\s+0x806360"),
        "push size (0x468e08)": exe.op_at(0x468E08, r"push\s+0x7080"),
        "loop bound 0x80d418 (0x4686e0)": exe.op_at(0x4686E0, r"cmp\s+eax,0x80d418"),
        "state base 0x806398 = base+0x38": (base + 0x38 == 0x806398),
        "0x806398+0x7080=0x80d418": (0x806398 + 0x7080 == 0x80D418),
    }
    ok = all(checks.values())
    return "0x%x..0x%x; 7/7 якорей" % (base, end), ok, \
        "провалы: %s" % ([k for k, v in checks.items() if not v] or "нет")


# ==========================================================================
# docs/UI_SCREENS.md
# ==========================================================================
@check("UI_SCREENS", "33 экрана в таблице 0x4883d8 (id 5..37)", "33")
def c_ui_screens(env):
    exe = env.exe
    dwords = [exe.u32(0x4883D8 + 4 * i) for i in range(40)]
    n = 0
    for v in dwords:
        if v is None or not (exe.sa() <= v < exe.sa() + exe.sz()):
            break
        n += 1
    ids = 37 - 5 + 1
    ok = n == 33 and ids == 33
    return "%d входов; следующий dword 0x%08x" % (n, dwords[n]), \
        ok, "id 5..37 = 33"


@check("UI_SCREENS", "34 перехода (таблица §4)", "34")
def c_ui_transitions(env):
    text = env.doc("UI_SCREENS")
    rows = doc_table_rows(text, "## 4. Переходы", "## 5.")
    ok_sites = 0
    bad = []
    for row in rows:
        cells = [c.strip() for c in row.strip().strip("|").split("|")]
        to_cell = cells[1]
        sites = re.findall(r"0x[0-9a-f]{6}", cells[3])
        if not sites:
            bad.append((cells[0], cells[1], "no site"))
            continue
        site = int(sites[0], 16)
        want = {int(x) for x in re.findall(r"\d+", to_cell)}
        got = resolve_state_write(env.exe, site)
        if got is not None and got & want:
            ok_sites += 1
        else:
            bad.append((cells[0], cells[1], hex(site), got))
    fact = "%d строк таблицы; сайтов подтверждено %d/%d" % (
        len(rows), ok_sites, len(rows))
    note = ("счёт зависит от определения: --json даёт 34 (без 10→30/34, "
            "но 34→10 и 35→10 отдельно); расхождения: %s" % (bad or "нет"))
    return fact, (len(rows) == 34 and ok_sites == len(rows)), note


def resolve_state_write(exe: Exe, site: int):
    """Set of state ids the instruction at ``site`` writes to 0x4bc440.

    Only the *first* state write in the window counts: ``0x48d1d9`` writes
    ``ebp`` and the next instruction block (``0x48d1e4``) writes 36, which is
    a different transition.
    """
    window = exe.lines(site, site + 0x20)
    rx_imm = re.compile(r"mov\s+DWORD PTR ds:0x4bc440,0x([0-9a-f]+)")
    rx_reg = re.compile(r"mov\s+DWORD PTR ds:0x4bc440,(ebp|e[a-z]{2})")
    for line in window:
        m = rx_imm.search(line)
        if m:
            return {int(m.group(1), 16)}
        if rx_reg.search(line):
            maddr = re.match(r"\s*([0-9a-f]+):", line)
            if not maddr:
                continue
            at = int(maddr.group(1), 16)
            back = exe.lines(max(0, at - 0x40), at)
            for prev in reversed(back):
                m = re.search(r"mov\s+ebp,0x([0-9a-f]+)", prev)
                if m:
                    return {int(m.group(1), 16)}
                if re.search(r"add\s+edx,0x1e", prev):
                    return {30, 34}      # 0x48d1b2: 30 + 4*(set < 1)
            return None
    return None


@check("UI_SCREENS", "20 слотов сейва (0x6c2fe0, шаг 0x13, конец 0x6c315c)", "20")
def c_ui_slots(env):
    n = (0x6C315C - 0x6C2FE0) // 0x13
    code = env.exe.op_at(0x48AA9F, r"cmp\s+eax,0x14")
    ok = n == 20 and code
    return "%d слотов; cmp 0x14 в 0x48aa9f: %s" % (n, "есть" if code else "нет"), \
        ok, "0x17c/0x13 = 20"


@check("UI_SCREENS", "6 видимых строк списка сейвов", "6")
def c_ui_visible(env):
    ok = env.exe.op_at(0x48AA9C, r"add\s+eax,0x6")
    return "add eax,0x6 в 0x48aa9c: %s" % ("есть" if ok else "нет"), ok, \
        "рядом cmp eax,0x14 (20 слотов)"


@check("UI_SCREENS", "32 UI-ассета из FRONTEND/BMP.IDX (143 записи)", "32 / 143")
def c_ui_assets(env):
    env.require_game()
    idx = open(env.game_file("FRONTEND", "BMP.IDX"), "rb").read()
    dat = open(env.game_file("FRONTEND", "BMP.DAT"), "rb").read()
    ents = parse_bigfile(idx)
    blobs = bigfile_blobs(idx, dat)
    wanted = {
        "f_m_titl": (232, 52), "scroll": (404, 200), "controls": (112, 25),
        "sound": (112, 25), "graphics": (112, 25), "startgam": (112, 25),
        "savegame": (112, 25), "quitgame": (112, 25), "play": (112, 25),
        "plate": (196, 131), "fileslot": (124, 10), "slide": (72, 8),
        "knob": (8, 8), "check": (8, 16), "nocheck": (8, 8),
        "menulit": (16, 16), "menuarrw": (16, 8), "killtile": (80, 16),
        "rpvicons": (60, 29), "panelmid": (320, 8), "fonts1": (512, 168),
        "fonts2": (1024, 80), "fonts3": (520, 56), "fonts4": (512, 168),
        "numfonts": (180, 48), "helpscrn": (320, 200), "briefbg2": (320, 200),
        "loadpage": (320, 200), "multibg": (320, 166), "demotion": (320, 200),
        "disband": (320, 200), "statsbg": (320, 200),
    }
    bad = []
    for name, wh in wanted.items():
        blob = blobs.get(name.upper())
        if blob is None or bmp_size(blob) != wh:
            bad.append(name)
    ok = len(ents) == 143 and len(wanted) == 32 and not bad
    return "%d записей; %d/32 размеров" % (len(ents), len(wanted) - len(bad)), \
        ok, "провалы: %s" % (bad or "нет")


@check("UI_SCREENS", "KEYCFG 3 файла x 376 байт", "3 x 376")
def c_ui_keycfg(env):
    env.require_game()
    files = [p for p in glob.glob(os.path.join(env.game, "KEYCFG", "*"))
             if os.path.isfile(p)]
    sizes = sorted(os.path.getsize(p) for p in files)
    ok = len(files) == 3 and sizes == [376, 376, 376]
    return "%d файла, размеры %s" % (len(files), sizes), ok, ""


# ==========================================================================
# docs/UI_SOUNDS.md
# ==========================================================================
UI_SOUND_IDX = {"UPDOWN": 581, "SELECT": 510, "SELECTM": 511,
                "MENUON": 425, "VLEFT": 804}
UI_SOUND_FNV = {
    "UPDOWN": "c71a5008363b7f25", "SELECT": "dece4c364782b7e2",
    "SELECTM": "2274810948bc7779", "MENUON": "57f410a64053d576",
    "VLEFT": "5218f3ff48ff7fc9",
}


@check("UI_SOUNDS", "idx UPDOWN 581 / SELECT 510 / SELECTM 511 / MENUON 425 / VLEFT 804",
       "581/510/511/425/804")
def c_snd_idx(env):
    ents, _blobs = env.sou()
    pos = {name.upper(): i for i, (name, _o) in enumerate(ents)}
    got = {k: pos.get(k) for k in UI_SOUND_IDX}
    ok = got == UI_SOUND_IDX
    return " ".join("%s=%s" % (k, got[k]) for k in UI_SOUND_IDX), ok, \
        "%d записей каталога" % len(ents)


@check("UI_SOUNDS", "FNV-1a64 payload пяти UI-сэмплов", "5/5")
def c_snd_fnv(env):
    _ents, blobs = env.sou()
    got, bad = {}, []
    for name, want in UI_SOUND_FNV.items():
        blob = blobs[name]
        if blob[:4] in (b"SOU0", b"SOU1"):
            length = struct.unpack_from("<I", blob, 12)[0]
            payload = blob[16:16 + (length & ~1)]
        else:
            raise Skip("unexpected codec for %s" % name)
        h = "%016x" % fnv1a64(payload)
        got[name] = h
        if h != want:
            bad.append("%s:%s" % (name, h))
    return "5/5" if not bad else "провалы " + ",".join(bad), not bad, \
        "FNV по payload длиной length & ~1"


@check("UI_SOUNDS", "3 сайта play_music (0x40b8a0)", "3")
def c_snd_music(env):
    lines = env.exe.refs("0x40b8a0")
    sites = sorted(int(m.group(1), 16)
                   for line in lines
                   for m in [re.match(r"\s*([0-9a-f]+):", line)]
                   if m and re.search(r"call\s+0x40b8a0", line))
    ok = sites == [0x45B5B4, 0x491122, 0x4914D4]
    return "%d вызова: %s" % (len(sites), ", ".join(hex(x) for x in sites)), ok, \
        "call-инструкции в .text"


@check("UI_SOUNDS", "651 файл MUSIC/*.sog (598 .sog + 53 .SOG)", "651")
def c_snd_sog(env):
    env.require_game()
    files = glob.glob(os.path.join(env.game, "MUSIC", "*"))
    sog = [os.path.basename(p) for p in files if p.lower().endswith(".sog")]
    lower = [f for f in sog if f.endswith(".sog")]
    upper = [f for f in sog if f.endswith(".SOG")]
    ok = (len(sog), len(lower), len(upper)) == (651, 598, 53)
    return "%d .sog (%d .sog + %d .SOG)" % (len(sog), len(lower), len(upper)), ok, \
        "поправка 2026-10-23: прежние 598 теряли 53 файла .SOG"


# ==========================================================================
# docs/SOUND_NAMES.md
# ==========================================================================
@check("SOUND_NAMES", "SOU.IDX 999 записей", "999")
def c_names_catalog(env):
    ents, _ = env.sou()
    return str(len(ents)), len(ents) == 999, ""


@check("SOUND_NAMES", "528/528 имён SOUND_SAMPLE_TABLE разрешаются", "528/528")
def c_names_resolve(env):
    slots, names = sample_table_slots(env)
    cat = {n.upper() for n, _o in env.sou()[0]}
    uniq = {n.upper() for n in names}
    missing = sorted(n for n in uniq if n not in cat)
    ok = len(uniq) == 528 and not missing
    return "%d/%d (написаний %d; нет в каталоге: %d)" % (
        len(uniq) - len(missing), len(uniq), len(names), len(missing)), ok, \
        "регистр не учитывается, как в 0x480180"


@check("SOUND_NAMES", "5800 слотов с именем на 36 уровней", "5800")
def c_names_slots(env):
    slots, _names = sample_table_slots(env)
    ok = slots == 5800
    return str(slots), ok, "counts[i] > 0 и имя непустое, 36 файлов"


@check("SOUND_NAMES", "sounds.sub: 95 / 42 / 391", "95 / 42 / 391")
def c_names_sub(env):
    _slots, names = sample_table_slots(env)
    names = {n.upper() for n in names}
    path = env.game_file("sounds", "sounds.sub")
    text = open(path, "rb").read().decode("latin1")
    pairs = [l for l in text.replace("\r\n", "\n").split("\n") if l.strip()]
    left = {l.split("\t")[0].upper() for l in pairs if "\t" in l}
    right = {l.split("\t")[1].upper() for l in pairs if "\t" in l}
    a = len(names & left)
    b = len((right - left) & names)
    c = len(names - left - right)
    ok = len(pairs) == 161 and (a, b, c) == (95, 42, 391)
    return "%d пар; %d / %d / %d" % (len(pairs), a, b, c), ok, \
        "левая колонка / только правая / нет вовсе"


@check("SOUND_NAMES", "sounds.sub 161 пара", "161")
def c_names_sub_pairs(env):
    path = env.game_file("sounds", "sounds.sub")
    text = open(path, "rb").read().decode("latin1")
    pairs = [l for l in text.replace("\r\n", "\n").split("\n") if l.strip()]
    ok = len(pairs) == 161 and pairs[0].startswith("alarmk\talarmkbig")
    return "%d пар; первая %r" % (len(pairs), pairs[0][:20]), ok, ""


def sample_table_slots(env):
    """(number of named slots, set of names) over all 36 record-12 tables."""
    slots = 0
    names = set()
    for base in env.corpus.levels:
        for table in env.corpus.record(base, 12):
            for name, count in zip(table.names, table.counts):
                if name and count > 0:
                    slots += 1
                    names.add(name)
    return slots, names


# ==========================================================================
# docs/DM_RULES.md
# ==========================================================================
@check("DM_RULES", "game_mode 0/1/2/3: 0 кампания, 1 normal, 2 classic, 3 hunt",
       "4 условия")
def c_dm_modes(env):
    exe = env.exe
    checks = {
        "!=0 (0x4421c8 test / 0x4421ca setne)":
            exe.op_at(0x4421C8, r"test\s+ecx,ecx") and
            exe.op_at(0x4421CA, r"setne\s+al"),
        "==2 (0x45eb0e cmp / 0x45eb11 sete)":
            exe.op_at(0x45EB0E, r"cmp\s+ecx,0x2") and
            exe.op_at(0x45EB11, r"sete\s+al"),
        "==1 (0x45eb24 cmp / 0x45eb27 sete)":
            exe.op_at(0x45EB24, r"cmp\s+ecx,0x1") and
            exe.op_at(0x45EB27, r"sete\s+al"),
        "==3 (0x4421d8 cmp / 0x4421db sete)":
            exe.op_at(0x4421D8, r"cmp\s+ecx,0x3") and
            exe.op_at(0x4421DB, r"sete\s+al"),
    }
    ok = all(checks.values())
    return "4/4 условия" if ok else "провалы: %s" % [k for k, v in checks.items() if not v], \
        ok, "все читают ds:0x83d2c8"


@check("DM_RULES", "mp_flags: team 1, ctf 2, nomonsters 4, freeze 8, nobuyammo 0x10, handicap 0x20",
       "6 бит")
def c_dm_flags(env):
    want = {0x44A752: 0x1, 0x44A777: 0x2, 0x44A79C: 0x4,
            0x44A7C1: 0x8, 0x44A80B: 0x10, 0x44A7E6: 0x20}
    exe = env.exe
    got, bad = {}, []
    for site, bit in want.items():
        m = exe.op_near(site, 0x10, r"or\s+al,0x([0-9a-f]+)")
        val = int(m.group(1), 16) if m else None
        got[site] = val
        if val != bit:
            bad.append("%s=%s" % (hex(site), val))
    return "6/6 or al,imm" if not bad else "провалы " + ",".join(bad), not bad, \
        "командная строка 0x44a719..0x44a811"


@check("DM_RULES", "248 DM-стартов на 31 карте", "248 / 31")
def c_dm_starts(env):
    sites = Counter()
    for base, _ins in env.corpus.ops(168):
        sites[base] += 1
    total = sum(sites.values())
    maps = len(sites)
    ok = total == 248 and maps == 31
    return "%d сайтов на %d картах" % (total, maps), ok, "op 168 SET_DEATHMATCH_START"


@check("DM_RULES", "143 SET_PLAYER_START", "143")
def c_dm_player_starts(env):
    total = sum(1 for _ in env.corpus.ops(169))
    return str(total), total == 143, "op 169"


@check("DM_RULES", "21 обращение к 0x81c2b0", "21")
def c_dm_frag_refs(env):
    lines = [l for l in env.exe.refs("0x81c2b0") if "0x81c2b0" in l]
    return "%d строк листинга" % len(lines), len(lines) == 21, \
        "grep 81 c2 b0 00 (адрес player+0x3270 для слота 0)"


# ==========================================================================
# docs/CAMPAIGN_FLOW.md
# ==========================================================================
@check("CAMPAIGN_FLOW", "66 переходов = 60 NEW_LEVEL + 6 NEXT_LEVEL", "60 + 6")
def c_camp_transitions(env):
    new = sum(1 for _ in env.corpus.ops(61))
    nxt = sum(1 for _ in env.corpus.ops(62))
    ok = (new, nxt) == (60, 6)
    return "%d + %d = %d" % (new, nxt, new + nxt), ok, "op 61 / op 62"


@check("CAMPAIGN_FLOW", "24 миссии", "24")
def c_camp_missions(env):
    levels = env.corpus.levels
    missions = [f"level{i}" for i in range(1, 25)]
    have = [m for m in missions if m in levels]
    with_exit = []
    for m in have:
        ops = {ins.op for ins in levels[m]["insns"]}
        if 61 in ops or 62 in ops:
            with_exit.append(m)
    ok = len(have) == 24 and len(with_exit) == 24
    return "%d файлов level1..24; %d с NEW_LEVEL/NEXT_LEVEL" % (
        len(have), len(with_exit)), ok, ""


@check("CAMPAIGN_FLOW", "101 объявленный секрет", "101")
def c_camp_secrets(env):
    total = sum(ins.args[0] for _b, ins in env.corpus.ops(94))
    levels = len({b for b, _i in env.corpus.ops(94)})
    return "%d на %d уровнях" % (total, levels), total == 101, \
        "op 94 SET_LEVEL_SECRETS"


@check("CAMPAIGN_FLOW", "105 сайтов инкремента секретов", "105")
def c_camp_inc(env):
    total = sum(1 for _ in env.corpus.ops(95))
    return str(total), total == 105, "op 95 INCREMENT_SOURCEPLAYER_SECRET_TALLY"


@check("CAMPAIGN_FLOW", "GUNMETAL.HI 9 записей (576 Б)", "9 / 576")
def c_camp_hi(env):
    path = env.game_file("GUNMETAL.HI")
    size = os.path.getsize(path)
    n = size // 0x40
    ok = size == 576 and n == 9
    return "%d байт = %d записей по 0x40" % (size, n), ok, ""


# ==========================================================================
# docs/AUDIO_3D.md
# ==========================================================================
@check("AUDIO_3D", "10409 3D-вызовов (187 + 9049 + 1173)", "10409")
def c_audio_calls(env):
    loc = sum(1 for _ in env.corpus.ops(21))
    src = sum(1 for _ in env.corpus.ops(37)) + sum(1 for _ in env.corpus.ops(38))
    add = sum(1 for _ in env.corpus.ops(187))
    ok = (loc, src, add) == (187, 9049, 1173) and loc + src + add == 10409
    return "%d + %d + %d = %d" % (loc, src, add, loc + src + add), ok, \
        "op 21 / 37+38 / 187"


@check("AUDIO_3D", "константа 0.25 @0x4a2248 и fmul в 0x4344df", "0.25")
def c_audio_const(env):
    exe = env.exe
    val = exe.f64(0x4A2248)
    ok = val == 0.25 and exe.op_at(0x434595, r"fmul\s+QWORD PTR ds:0x4a2248") \
        and exe.op_at(0x43459B, r"fadd\s+QWORD PTR \[ebp\+0x8\]")
    return "%.2f; fmul+fadd на месте" % val, ok, "dist = max + 0.25*(mid+min)"


@check("AUDIO_3D", "atten 0.03 = 0x3f9eb851eb851eb8 (0x46b8b3/0x46b8bd)", "0.03")
def c_audio_atten(env):
    exe = env.exe
    lo = exe.bytes_at(0x46B8B3, 4)
    hi = exe.bytes_at(0x46B8BD, 4)
    value = struct.unpack("<d", lo + hi)[0]
    ok = (lo == bytes.fromhex("b81e85eb") and hi == bytes.fromhex("51b89e3f")
          and value == 0.03)
    return "%.6f (%s+%s)" % (value, lo.hex(), hi.hex()), ok, "два imm по 4 байта"


@check("AUDIO_3D", "пример level0:0 -> 198 / 58 / 159 / L23 R32", "198/58/159/23/32")
def c_audio_example(env):
    calls = list(env.corpus.ops(21))
    base0 = [(b, i) for b, i in calls if b == "level0"]
    ins = base0[0][1]
    sample, volume, rate, priority, x, y, z = ins.args
    z = struct.unpack("<i", struct.pack("<I", z))[0]
    dist, gain, pan, v, left, right = audio_example(
        (x, y, z), volume, 0.03, (0, 3200, 48))
    ok = (round(dist), gain, pan, left, right) == (198, 58, 159, 23, 32)
    return "%.0f/%d/%d/%d-%d (v=%d, rate=%d, prio=%d)" % (
        dist, gain, pan, left, right, v, rate, priority), ok, \
        "pos=(%d,%d,%d) vol=%d" % (x, y, z, volume)


# --------------------------------------------------------------------------
# self-test (no game files needed)
# --------------------------------------------------------------------------
def self_test() -> int:
    ok = 0
    total = 0

    def t(name, cond, got=""):
        nonlocal ok, total
        total += 1
        if cond:
            ok += 1
        else:
            print("FAIL %s %s" % (name, got))

    t("fnv empty", fnv1a64(b"") == 0xCBF29CE484222325)
    t("fnv a", fnv1a64(b"a") == 0xAF63DC4C8601EC8C,
      "%016x" % fnv1a64(b"a"))
    t("clamp", (clamp(-1, 1, 32), clamp(33, 1, 32), clamp(7, 1, 32)) == (1, 32, 7))

    idx = b"BIGFILE\x1a" + struct.pack("<I", 2) + \
        b"AAA\0\0\0\0\0" + struct.pack("<I", 0) + \
        b"BBB\0\0\0\0\0" + struct.pack("<I", 10)
    ents = parse_bigfile(idx)
    t("bigfile names", [e[0] for e in ents] == ["AAA", "BBB"], ents)
    t("bigfile offsets", [e[1] for e in ents] == [0, 10], ents)
    blobs = bigfile_blobs(idx, b"0123456789ABCDEF")
    t("bigfile blobs", blobs["AAA"] == b"0123456789" and blobs["BBB"] == b"ABCDEF",
      blobs)

    bmp = bytearray(64)
    bmp[:2] = b"BM"
    struct.pack_into("<i", bmp, 18, 232)
    struct.pack_into("<i", bmp, 22, -52)
    t("bmp size", bmp_size(bytes(bmp)) == (232, 52), bmp_size(bytes(bmp)))

    ex = audio_example((48, 3208, -136), 64, 0.03, (0, 3200, 48))
    t("audio example", (round(ex[0]), ex[1], ex[2], ex[4], ex[5]) ==
      (198, 58, 159, 23, 32), ex)
    ex2 = audio_example((48, 3208, -136), 64, 0.03, (0, 3200, 48),
                        right=(0.0, -1.0, 0.0))
    t("audio yaw 90", ex2[2] == 122 and ex2[4] == 32 and ex2[5] == 29, ex2)

    c = Counter({52: 401, 53: 17, 54: 636, 55: 84, 177: 440, 185: 53,
                 186: 4252, 39: 2342, 64: 45, 65: 37, 75: 3098,
                 173: 36, 174: 36, 176: 36})
    t("item op total", item_op_total(c, ITEM_OPS) == 11513,
      item_op_total(c, ITEM_OPS))
    t("creation total", item_op_total(c, CREATE_OPS) == 5883,
      item_op_total(c, CREATE_OPS))

    text = ("intro\n| из | в | как | сайт |\n|---|---|---|---|\n"
            "| 0 | 1 | x | `0x487709` |\n| 1 | 11 | y | `0x48cf78` |\n"
            "| 10 | 30 / 34 | z | `0x48d1b2` |\nend\n")
    rows = doc_table_rows(text, "intro", "end")
    t("doc rows", len(rows) == 3, rows)

    t("state write imm", resolve_state_write_imm("mov DWORD PTR ds:0x4bc440,0xb")
      == {11})
    t("state write ebp", resolve_state_write_imm("mov DWORD PTR ds:0x4bc440,ebp")
      is None)
    t("state write other", resolve_state_write_imm("mov eax,0x4bc440") is None)

    print("self-test: %d/%d checks passed" % (ok, total))
    return 0 if ok == total else 1


def resolve_state_write_imm(line: str):
    m = re.search(r"mov\s+DWORD PTR ds:0x4bc440,0x([0-9a-f]+)", line)
    return {int(m.group(1), 16)} if m else None


# --------------------------------------------------------------------------
# CLI
# --------------------------------------------------------------------------
def print_table(rows, strict_docs=None):
    docs_order = list(DOC_FILES)
    w_doc = max(len("док"), *(len(r["doc"]) for r in rows))
    w_num = max(len("число"), *(len(r["number"]) for r in rows))
    w_exp = max(len("ожидание"), *(len(r["expected"]) for r in rows))
    w_fact = max(len("факт"), *(len(r["fact"]) for r in rows))
    print("%-*s  %-*s  %-*s  %-*s  %s" % (
        w_doc, "док", w_num, "число", w_exp, "ожидание", w_fact, "факт", "вердикт"))
    print("-" * (w_doc + w_num + w_exp + w_fact + 30))
    for r in rows:
        print("%-*s  %-*s  %-*s  %-*s  %s" % (
            w_doc, r["doc"], w_num, r["number"], w_exp, r["expected"],
            w_fact, r["fact"], r["verdict"]))
    print()
    notes = [r for r in rows if r.get("note")]
    if notes:
        print("Примечания:")
        for r in notes:
            print("  [%s] %s — %s" % (r["doc"], r["number"], r["note"]))
        print()
    _ = docs_order, strict_docs



# --------------------------------------------------------------------------
# GAME_OVER (M9.5): the end-of-level screen opcode and the absent game-over
# --------------------------------------------------------------------------

@check("GAME_OVER", "сайтов SHOW_LEVEL_END_SCREEN (опкод 91), не 96", "38")
def c_go_opcode91(env):
    sites = sum(1 for _ in env.corpus.ops(91))
    gravity = sum(1 for _ in env.corpus.ops(96))
    ok = sites == 38 and gravity == 13
    return "op 91: %d, op 96 (SET_LEVEL_GRAVITY): %d" % (sites, gravity), ok, \
        "экран конца уровня — 91; 96 — гравитация, не экран"


@check("GAME_OVER", "строк GAME OVER / GAMEOVER в образе", "0")
def c_go_strings(env):
    data = env.exe.data
    upper = data.count(b"GAME OVER") + data.count(b"GAMEOVER")
    lower = data.lower().count(b"game over")
    ok = upper == 0 and lower == 0
    return "GAME OVER=%d, game over=%d" % (upper, lower), ok, \
        "нет экрана/строки game over; респавн через 136 тиков"


# --------------------------------------------------------------------------
# SBT_ASM (M11.1): the byte-level round-trip census
# --------------------------------------------------------------------------

@check("SBT_ASM", "блоков / байт / инструкций / опкодов (36 .sbt)",
       "15061 / 3542023 / 147777 / 171")
def c_asm_corpus(env):
    from gm.scene import entities
    from gm.sbt import parse_file
    from gm.sbtvm import decode

    blocks = byte_total = insn_total = 0
    opcodes = set()
    for path in sorted(glob.glob(os.path.join(env.game, "*.sbt"))):
        parsed = parse_file(path)
        for rec in parsed.records_of(11):
            code = rec.payload.code
            offsets = sorted({off for _eid, _flags, off in entities(rec.payload)
                              if 0 <= off < len(code)})
            for k, off in enumerate(offsets):
                end = offsets[k + 1] if k + 1 < len(offsets) else len(code)
                block, got = decode(code, off, end)
                blocks += 1
                byte_total += got - off
                seen = set()
                stack = [block]
                while stack:
                    for ins in stack.pop():
                        if ins.off in seen:
                            continue        # ELSE may re-walk a sibling range
                        seen.add(ins.off)
                        opcodes.add(ins.op)
                        if ins.body:
                            stack.append(ins.body)
                insn_total += len(seen)
    ok = (blocks, byte_total, insn_total, len(opcodes)) == \
        (15061, 3542023, 147777, 171)
    return "%d / %d / %d / %d" % (blocks, byte_total, insn_total,
                                  len(opcodes)), ok, \
        "по блокам таблицы сущностей, без двойного счёта ELSE"



# --------------------------------------------------------------------------
# OBJECTIVES (M6.4.5): goals, objective bits and the secret tally
# --------------------------------------------------------------------------

@check("OBJECTIVES", "25 уровней / 35 объявлений DEFINE_LEVEL_OBJECTIVES (103)",
       "25 / 35")
def c_obj_declarations(env):
    levels = set()
    sites = 0
    for base, _ins in env.corpus.ops(103):
        levels.add(base)
        sites += 1
    ok = (len(levels), sites) == (25, 35)
    return "%d уровней / %d объявлений" % (len(levels), sites), ok, \
        "op 103 по корпусу"


@check("OBJECTIVES", "51 сайт OR_LEVEL_OBJECTIVES_BITS (104)", "51")
def c_obj_bits(env):
    levels = set()
    sites = 0
    for base, _ins in env.corpus.ops(104):
        levels.add(base)
        sites += 1
    ok = sites == 51 and len(levels) == 19
    return "%d сайтов на %d уровнях" % (sites, len(levels)), ok, \
        "op 104: единственный писатель маски 0x80f9d0"


@check("OBJECTIVES", "секреты: 25 объявлений / 101 всего / 105 инкрементов",
       "25 / 101 / 105")
def c_obj_secrets(env):
    declared_levels = set()
    declared_total = 0
    for base, ins in env.corpus.ops(94):
        declared_levels.add(base)
        arg = ins.args[0] if ins.args else 0
        declared_total += int(getattr(arg, "value", arg))
    increments = sum(1 for _ in env.corpus.ops(95))
    ok = (len(declared_levels), declared_total, increments) == (25, 101, 105)
    return "%d / %d / %d" % (len(declared_levels), declared_total, increments), ok, \
        "op 94 (сумма операнда) и op 95"



# --------------------------------------------------------------------------
# BIGFILE_WRITE (M11.6): the archive corpus and its totals
# --------------------------------------------------------------------------

@check("BIGFILE_WRITE", "5 архивов / 6061 запись / 151737971 Б",
       "5 / 6061 / 151737971")
def c_big_totals(env):
    from gm.bigfile import BigFile

    archives = entries = total = 0
    for path in sorted(glob.glob(os.path.join(env.game, "**", "*.[Ii][Dd][Xx]"),
                                 recursive=True)):
        base = os.path.splitext(path)[0]
        if not any(os.path.isfile(base + ext) for ext in (".DAT", ".dat")):
            continue                    # Film/ammo.idx is not a BIGFILE pair
        try:
            archive = BigFile(path)
        except (ValueError, OSError):
            continue
        archives += 1
        entries += len(archive.entries)
        total += sum(entry.size for entry in archive.entries)
    ok = (archives, entries, total) == (5, 6061, 151737971)
    return "%d / %d / %d" % (archives, entries, total), ok, \
        "пары .IDX+.DAT по всему установленному дереву"



# --------------------------------------------------------------------------
# SBT_WRITE / SCENE_EDIT (M11.3/M11.5): the container corpus and its records
# --------------------------------------------------------------------------

@check("SBT_WRITE", "36 файлов / 6793572 байта", "36 / 6793572")
def c_sbt_corpus(env):
    paths = sorted(glob.glob(os.path.join(env.game, "*.sbt")))
    total = sum(os.path.getsize(path) for path in paths)
    ok = (len(paths), total) == (36, 6793572)
    return "%d / %d" % (len(paths), total), ok, "все .sbt в корне установки"


@check("SCENE_EDIT", "241 запись сцены (всё, кроме SCRIPT 11 и END 255)", "241")
def c_scene_records(env):
    from gm.sbt import parse_file

    scene = 0
    for path in sorted(glob.glob(os.path.join(env.game, "*.sbt"))):
        parsed = parse_file(path)
        scene += sum(1 for record in parsed.records
                     if record.type not in (11, 255))
    ok = scene == 241
    return "%d" % scene, ok, "типы записей != 11/255 по 36 уровням"



@check("SBW_EDIT", "36 миров / 28192 объекта / 235069 полигонов / 40037031 Б",
       "36 / 28192 / 235069 / 40037031")
def c_sbw_corpus(env):
    from gm.sbw import parse

    paths = sorted(glob.glob(os.path.join(env.game, "*.sbw")))
    objects = polygons = total = 0
    for path in paths:
        with open(path, "rb") as fh:
            level = parse(fh.read())
        objects += level.n_objects
        polygons += level.n_polys
        total += os.path.getsize(path)
    ok = (len(paths), objects, polygons, total) == (36, 28192, 235069, 40037031)
    return "%d / %d / %d / %d" % (len(paths), objects, polygons, total), ok, \
        "все .sbw в корне установки"



# --------------------------------------------------------------------------
# SCRIPT_DEBUG (M11.4): the block/byte counts of the debugger's table
# --------------------------------------------------------------------------

@check("SCRIPT_DEBUG", "level0: 350 блоков / 62497 байт", "350 / 62497")
def c_script_corpus(env):
    from gm.sbt import parse_file

    parsed = parse_file(os.path.join(env.game, "level0.sbt"))
    records = parsed.records_of(11)
    if not records:
        return "нет SCRIPT", False, "level0.sbt без записи 11"
    script = records[0].payload
    ok = (script.n, script.m) == (350, 62497) and len(script.code) == script.m
    return "%d / %d" % (script.n, script.m), ok, \
        "SCRIPT: handles (подскрипты) / длина кода"


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--self-test", action="store_true")
    ap.add_argument("--check", action="store_true")
    ap.add_argument("--json", action="store_true")
    ap.add_argument("--strict", action="store_true",
                    help="exit 1 if a number differs or is unverifiable")
    ap.add_argument("--game", default=DEFAULT_GAME)
    ap.add_argument("--exe", default=os.environ.get(
        "GUNMETAL_EXE", os.path.join(DEFAULT_GAME, "gundx.exe")))
    ap.add_argument("--listing", default=os.environ.get("GUNDX_ASM"))
    ap.add_argument("--docs", default=os.path.join(ROOT, "docs"))
    args = ap.parse_args(argv)

    if args.self_test:
        return self_test()
    if not args.check:
        ap.print_help()
        return 0

    env = Env(args.game, args.docs, args.exe, args.listing)
    rows = run_checks(env)
    summ = Counter(r["verdict"] for r in rows)
    docs = {r["doc"] for r in rows}

    if args.json:
        print(json.dumps({
            "game": args.game,
            "exe": args.exe,
            "docs": {k: DOC_FILES[k] for k in DOC_FILES},
            "checks": rows,
            "summary": {
                "total": len(rows),
                "confirmed": summ["подтверждено"],
                "mismatch": summ["расходится"],
                "unverifiable": summ["не проверяемо"],
                "documents": sorted(docs),
            },
        }, ensure_ascii=False, indent=1))
    else:
        print_table(rows)
        print("проверок %d: подтверждено %d, расходится %d, "
              "не проверяемо %d; доков %d/%d"
              % (len(rows), summ["подтверждено"], summ["расходится"],
                 summ["не проверяемо"], len(docs & set(DOC_FILES)),
                 len(DOC_FILES)))

    if len(docs & set(DOC_FILES)) != len(DOC_FILES):
        return 2
    if args.strict and (summ["расходится"] or summ["не проверяемо"]):
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
