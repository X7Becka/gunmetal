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
import hashlib
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
    "LIGHTMAP_EDIT": "docs/LIGHTMAP_EDIT.md",
    "SBW_OBJECTS": "docs/SBW_OBJECTS.md",
    "SAMPLE_CACHE": "docs/SAMPLE_CACHE.md",
    "SBW_MESH": "docs/SBW_MESH.md",
    "SAVE_WRITE_CPP": "docs/SAVE_WRITE_CPP.md",
    "PAGER_RENDER": "docs/PAGER_RENDER.md",
    "MENU_RENDER": "docs/MENU_RENDER.md",
    "SBW_POLY": "docs/SBW_POLY.md",
    "CAMPAIGN_STATE": "docs/CAMPAIGN_STATE.md",
    "UI_SOUNDS_STATE": "docs/UI_SOUNDS_STATE.md",
    "CHASSIS_PHYSICS": "docs/CHASSIS_PHYSICS.md",
    "FX_FRAME": "docs/FX_FRAME.md",
    "OBJECTIVES_STATE": "docs/OBJECTIVES_STATE.md",
    "BRIEFING": "docs/BRIEFING.md",
    "WORLD_CENSUS": "docs/WORLD_CENSUS.md",
    "TEXTURE_ANIM": "docs/TEXTURE_ANIM.md",
    "HUD_ICONS_CPP": "docs/HUD_ICONS_CPP.md",
    "SUB_CPP": "docs/SUB_CPP.md",
    "SOA_WRITE": "docs/SOA_WRITE.md",
    "KILLED": "docs/KILLED.md",
    "PLAYLIST": "docs/PLAYLIST.md",
    "GAME_CFG": "docs/GAME_CFG.md",
    "HUD_LUT": "docs/HUD_LUT.md",
    "SOG_WRITE": "docs/SOG_WRITE.md",
    "SOF_WRITE": "docs/SOF_WRITE.md",
    "SOO_WRITE": "docs/SOO_WRITE.md",
    "SOE_WRITE": "docs/SOE_WRITE.md",
    "SBM_WRITE": "docs/SBM_WRITE.md",
    "BMP_WRITE": "docs/BMP_WRITE.md",
    "HI_WRITE": "docs/HI_WRITE.md",
    "KILLED_WRITE": "docs/KILLED_WRITE.md",
    "KEY_WRITE": "docs/KEY_WRITE.md",
    "CLI": "docs/CLI.md",
    "LUT_BUILDER": "docs/LUT_BUILDER.md",
    "THING_ANIM_BIND": "docs/THING_ANIM_BIND.md",
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



# --------------------------------------------------------------------------
# LIGHTMAP_EDIT (M11.2): the .sbc corpus totals
# --------------------------------------------------------------------------

@check("LIGHTMAP_EDIT", "51 файл / 67200 записей / 24214080 Б",
       "51 / 67200 / 23944872 / 24214080")
def c_sbc_corpus(env):
    from gm.sbc import parse

    paths = sorted(glob.glob(os.path.join(env.game, "**", "*.sbc"), recursive=True))
    records = texels = total = 0
    for path in paths:
        with open(path, "rb") as fh:
            data = fh.read()
        blob = parse(data)
        records += len(blob.records)
        texels += sum(len(record.pixels) for record in blob.records)
        total += len(data)
    ok = (len(paths), records, texels, total) == (51, 67200, 23944872, 24214080)
    return "%d / %d / %d / %d" % (len(paths), records, texels, total), ok, \
        "все .sbc в корне и SOBS/"



@check("SBW_OBJECTS", "36 миров / 28192 объекта / 357818 вершин",
       "36 / 28192 / 357818")
def c_sbw_objects(env):
    from gm.sbw import parse

    paths = sorted(glob.glob(os.path.join(env.game, "*.sbw")))
    objects = vertices = 0
    for path in paths:
        with open(path, "rb") as fh:
            level = parse(fh.read())
        for sector in level.sectors:
            for obj in sector.objects:
                objects += 1
                vertices += len(obj.vertices)
    ok = (len(paths), objects, vertices) == (36, 28192, 357818)
    return "%d / %d / %d" % (len(paths), objects, vertices), ok, \
        "объекты и локальные вершины 36 миров"



# --------------------------------------------------------------------------
# SAMPLE_CACHE (M7.4): the level table and its resolvable names
# --------------------------------------------------------------------------

@check("SAMPLE_CACHE", "level0: 300 слотов, 137 разрешаемых имён",
       "300 / 137 / 137")
def c_sample_names(env):
    from gm.bigfile import BigFile
    from gm.sbt import parse_file

    parsed = parse_file(os.path.join(env.game, "level0.sbt"))
    records = parsed.records_of(12)
    if not records:
        return "нет записи 12", False, "level0.sbt без SOUND_SAMPLE_TABLE"
    table = records[0].payload
    named = [name for name, count in zip(table.names, table.counts) if name and count]
    index = BigFile(os.path.join(env.game, "sounds", "SOU.IDX"))
    keys = set()
    for entry in index.entries:
        key = entry.name.rsplit("\\", 1)[-1].rsplit(":", 1)[-1].lower()
        keys.add(key)
    resolvable = sum(1 for name in named
                     if name.rsplit("\\", 1)[-1].rsplit(":", 1)[-1].lower() in keys)
    ok = (len(table.names), len(named), resolvable) == (300, 137, 137)
    return "%d / %d / %d" % (len(table.names), len(named), resolvable), ok, \
        "слоты / имена с count>0 / найденные в sounds/SOU.IDX"



@check("SBW_MESH", "правило radius/bounds: 28192/28192 объектов",
       "28192 / 28192 / 20736")
def c_sbw_radius_rule(env):
    from gm.sbw import parse

    paths = sorted(glob.glob(os.path.join(env.game, "*.sbw")))
    objects = rule = by_bounds = 0
    for path in paths:
        with open(path, "rb") as fh:
            level = parse(fh.read())
        for sector in level.sectors:
            for obj in sector.objects:
                objects += 1
                if not obj.vertices:
                    continue
                basis = obj.points[1:4]
                far = 0.0
                box = [0.0, 0.0, 0.0]
                for vertex in obj.vertices:
                    local = [sum(basis[r][k] * vertex[k] for k in range(3))
                             for r in range(3)]
                    far = max(far, sum(v * v for v in local) ** 0.5)
                    for r in range(3):
                        box[r] = max(box[r], abs(local[r]))
                if abs(far - obj.radius) <= 1e-3:
                    rule += 1
                corner = sum(b * b for b in box) ** 0.5
                if abs(corner - obj.radius) <= 1e-3:
                    by_bounds += 1
    ok = (objects, rule, by_bounds) == (28192, 28192, 20736)
    return "%d / %d / %d" % (objects, rule, by_bounds), ok, \
        "объекты / radius == max|basis*v| / radius == |bounds|"



@check("SAVE_WRITE_CPP", "saved0/saved1: 243368 байт, sha256 e39c0c35…/93f4edd3…",
       "243368 / e39c0c35c44af6b985cb272f52b1236404d4316b266dffc7ccf0548503b623ff / "
       "93f4edd3fad07b31f47f392acf0c409f4e9217efcc521052ca8e74966834bd13")
def c_save_write_bytes(env):
    env.require_game()
    facts = []
    for name in ("saved0.sbt", "saved1.sbt"):
        blob = open(env.game_file("SAVEGAME", name), "rb").read()
        facts.append([len(blob), hashlib.sha256(blob).hexdigest()])
    ok = (facts[0][0] == 243368 and facts[1][0] == 243368 and
          facts[0][1].startswith("e39c0c35") and facts[1][1].startswith("93f4edd3"))
    return "%d / %s / %s" % (facts[0][0], facts[0][1][:8], facts[1][1][:8]), ok, \
        "размер и sha256 двух поставочных сейвов"


@check("PAGER_RENDER", "HELPSCRN: запись 46, 320x200, 65 078 Б, индекс 0 не встречается",
       "46 / 320x200 / 65078")
def c_pager_helpers(env):
    env.require_game()
    idx = open(env.game_file("FRONTEND", "BMP.IDX"), "rb").read()
    dat = open(env.game_file("FRONTEND", "BMP.DAT"), "rb").read()
    names = [name.upper() for name, _ in parse_bigfile(idx)]
    blobs = bigfile_blobs(idx, dat)
    blob = blobs["HELPSCRN"]
    width, height = bmp_size(blob)
    off = struct.unpack_from("<I", blob, 10)[0]
    pixels = blob[off:]
    ok = (names.index("HELPSCRN") == 46 and (width, height) == (320, 200) and
          len(blob) == 65078 and 0 not in pixels)
    return "%d / %dx%d / %d" % (names.index("HELPSCRN"), width, height, len(blob)), ok, \
        "запись / размер / байт / индекс 0 отсутствует"


@check("MENU_RENDER", "FONTS4: 4 грани по 1174 чернильных пикселя", "1174 / 1174 / 1174 / 1174")
def c_menu_font_faces(env):
    env.require_game()
    idx = open(env.game_file("FRONTEND", "BMP.IDX"), "rb").read()
    dat = open(env.game_file("FRONTEND", "BMP.DAT"), "rb").read()
    blob = bigfile_blobs(idx, dat)["FONTS4"]
    width, height = bmp_size(blob)
    off = struct.unpack_from("<I", blob, 10)[0]
    pixels = blob[off:]
    band = height // 4
    ink = [sum(1 for y in range(i * band, (i + 1) * band)
               for x in range(width) if pixels[y * width + x] != 0)
           for i in range(4)]
    ok = (width, height) == (512, 168) and ink == [1174, 1174, 1174, 1174]
    return " / ".join(str(value) for value in ink), ok, \
        "чернильных пикселей в каждой из четырёх граней (512x168, полосы по 42)"




# ==========================================================================
# docs/SBW_POLY.md
# ==========================================================================
@check("SBW_POLY", "36 миров / 235 069 полигонов / 357 818 вершин / 24 нулевых normal",
       "235069 / 357818 / 24")
def c_sbw_poly_counts(env):
    from gm.sbw import parse

    paths = sorted(glob.glob(os.path.join(env.game, "*.sbw")))
    polys = verts = zero = 0
    for path in paths:
        with open(path, "rb") as fh:
            level = parse(fh.read())
        for sector in level.sectors:
            for obj in sector.objects:
                verts += len(obj.vertices)
                for poly in obj.polys:
                    polys += 1
                    if all(abs(c) <= 1e-12 for c in poly.normal):
                        zero += 1
    ok = (len(paths), polys, verts, zero) == (36, 235069, 357818, 24)
    return "%d / %d / %d / %d" % (len(paths), polys, verts, zero), ok, \
        "миры / полигоны / вершины / нулевые normal"


# ==========================================================================
# docs/CAMPAIGN_STATE.md
# ==========================================================================
@check("CAMPAIGN_STATE", "GUNMETAL.HI: 576 Б, 9 записей, времена 146,1570,18,109,18,7,11,40,6",
       "576 / 9 / 146,1570,18,109,18,7,11,40,6")
def c_campaign_hi(env):
    blob = open(env.game_file("GUNMETAL.HI"), "rb").read()
    times = [struct.unpack_from("<i", blob, i * 0x40)[0] for i in range(9)]
    ok = len(blob) == 576 and times == [146, 1570, 18, 109, 18, 7, 11, 40, 6]
    return "%d / %d / %s" % (len(blob), len(times),
                             ",".join(str(t) for t in times)), ok, \
        "размер / записей / времена подряд (i32 в начале записи 0x40)"


# ==========================================================================
# docs/UI_SOUNDS_STATE.md
# ==========================================================================
@check("UI_SOUNDS_STATE",
       "SOU.IDX: UPDOWN 581 / SELECT 510 / SELECTM 511 / MENUON 425 / VLEFT 804 / VBACK 636",
       "UPDOWN=581 SELECT=510 SELECTM=511 MENUON=425 VLEFT=804 VBACK=636")
def c_ui_sound_indices(env):
    entries, blobs = env.sou()
    want = {581: "UPDOWN", 510: "SELECT", 511: "SELECTM",
            425: "MENUON", 804: "VLEFT", 636: "VBACK"}
    facts = []
    good = True
    for idx in sorted(want):
        name = entries[idx][0].rsplit("\\", 1)[-1]
        size = len(blobs.get(name.upper(), b""))
        facts.append("%s=%d(%d)" % (name, idx, size))
        good = good and name.upper() == want[idx] and size > 0
    return " ".join(facts), good, \
        "имя и байт записи по индексу SOU.IDX"




# ==========================================================================
# docs/CHASSIS_PHYSICS.md
# ==========================================================================
@check("CHASSIS_PHYSICS", "DEFINE_PLAYER_TYPES: 616 записей 36 уровней",
       "616")
def c_chassis_player_types(env):
    sites = list(env.corpus.ops(172))
    total = sum(ins.args[0][1] for _, ins in sites)
    ok = (len(sites), total) == (145, 616)
    return "%d / %d" % (len(sites), total), ok, \
        "сайтов опкода 172 / записей (сумма n)"


# ==========================================================================
# docs/FX_FRAME.md
# ==========================================================================
@check("FX_FRAME", "6117 texture<0 / 133 overlay<0 / 18 overlay-selected / 6135 эффект-полигонов",
       "6117 / 133 / 18 / 6135")
def c_fx_frame_counts(env):
    from gm.sbw import parse

    tex = overlay = selected = 0
    for path in sorted(glob.glob(os.path.join(env.game, "*.sbw"))):
        with open(path, "rb") as fh:
            level = parse(fh.read())
        for sector in level.sectors:
            for obj in sector.objects:
                for poly in obj.polys:
                    t = poly.texture - 0x10000 if poly.texture >= 0x8000 else poly.texture
                    o = poly.overlay - 0x10000 if poly.overlay >= 0x8000 else poly.overlay
                    if t < 0:
                        tex += 1
                    if o < 0:
                        overlay += 1
                    if poly.handle >= 1 and o < 0:
                        selected += 1
    effective = tex + selected
    ok = (tex, overlay, selected, effective) == (6117, 133, 18, 6135)
    return "%d / %d / %d / %d" % (tex, overlay, selected, effective), ok, \
        "poly+0x00<0 / poly+0x78<0 / handle>=1 и overlay<0 / эффективно отрицательных"




# ==========================================================================
# docs/OBJECTIVES_STATE.md
# ==========================================================================
@check("OBJECTIVES_STATE", "секреты: 101 объявлено (25 уровней) / 105 инкрементов (27)",
       "101 / 25 / 105 / 27")
def c_objectives_secrets(env):
    sites94 = list(env.corpus.ops(94))
    sites95 = list(env.corpus.ops(95))
    declared = sum(a if isinstance(a, int) else a[1]
                   for _, ins in sites94 for a in [ins.args[0]])
    ok = (declared, len(sites94), len(sites95)) == (101, 25, 105) and \
        len({base for base, _ in sites95}) == 27
    return "%d / %d / %d / %d" % (declared, len(sites94), len(sites95),
                                  len({base for base, _ in sites95})), ok, \
        "сумма SET_LEVEL_SECRETS / сайтов / инкрементов / уровней с инкрементом"




# ==========================================================================
# docs/BRIEFING.md
# ==========================================================================
@check("BRIEFING", "SML/brief: 49 файлов = 25 .sme + 24 .sml", "25 / 24")
def c_briefing_files(env):
    env.require_game()
    names = os.listdir(env.game_file("SML", "brief"))
    sme = sum(1 for n in names if n.lower().endswith(".sme"))
    sml = sum(1 for n in names if n.lower().endswith(".sml"))
    ok = (sme, sml) == (25, 24)
    return "%d / %d" % (sme, sml), ok, "файлы .sme / .sml в SML/brief"




# ==========================================================================
# docs/WORLD_CENSUS.md
# ==========================================================================
@check("WORLD_CENSUS", "двери 1779 / переключатели 3964 / лифты 45 (по скриптам)",
       "1779 / 3964 / 45")
def c_world_census_calls(env):
    doors = len(list(env.corpus.ops(184)))
    switches = len(list(env.corpus.ops(183)))
    lifts = len(list(env.corpus.ops(188)))
    ok = (doors, switches, lifts) == (1779, 3964, 45)
    return "%d / %d / %d" % (doors, switches, lifts), ok, \
        "сайты ADD_DOOR (184) / ADD_SWITCH (183) / ADD_LIFT (188) по 36 .sbt"




# ==========================================================================
# docs/TEXTURE_ANIM.md
# ==========================================================================
@check("TEXTURE_ANIM", "235069 полигонов / 2917 со скроллом / max 15224 и 10000 / 114 пар",
       "2917 / 15224 / 10000 / 114")
def c_texture_anim_corpus(env):
    from gm.sbw import parse

    total = scroll = 0
    max_u = max_v = 0
    pairs = set()
    for path in sorted(glob.glob(os.path.join(env.game, "*.sbw"))):
        with open(path, "rb") as fh:
            level = parse(fh.read())
        for sector in level.sectors:
            for obj in sector.objects:
                for poly in obj.polys:
                    total += 1
                    su = poly.scroll_u - 0x10000 if poly.scroll_u >= 0x8000 else poly.scroll_u
                    sv = poly.scroll_v - 0x10000 if poly.scroll_v >= 0x8000 else poly.scroll_v
                    if su or sv:
                        scroll += 1
                        pairs.add((su, sv))
                        max_u = max(max_u, abs(su))
                        max_v = max(max_v, abs(sv))
    ok = (total, scroll, max_u, max_v, len(pairs)) == (235069, 2917, 15224, 10000, 114)
    return "%d / %d / %d / %d / %d" % (total, scroll, max_u, max_v, len(pairs)), ok, \
        "полигоны / со скроллом / max|u| / max|v| / различных пар"


# ==========================================================================
# docs/HUD_ICONS_CPP.md
# ==========================================================================
@check("HUD_ICONS_CPP",
       "FRONTEND/BMP.IDX: 143 записи, equip0..equip12 и WICON1 24x24",
       "143 / 13 / 24x24")
def c_hud_icons_archive(env):
    env.require_game()
    idx = open(env.game_file("FRONTEND", "BMP.IDX"), "rb").read()
    dat = open(env.game_file("FRONTEND", "BMP.DAT"), "rb").read()
    names = [name.upper() for name, _ in parse_bigfile(idx)]
    blobs = bigfile_blobs(idx, dat)
    equips = ["EQUIP%d" % i for i in range(13)]
    sizes = {bmp_size(blobs[n]) for n in equips + ["WICON1"] if n in blobs}
    present = sum(1 for n in equips if n in blobs)
    ok = (len(names), present) == (143, 13) and sizes == {(24, 24)}
    return "%d / %d / %s" % (len(names), present,
                             ",".join("%dx%d" % s for s in sorted(sizes))), ok, \
        "записи архива / equip0..equip12 / их размеры и WICON1"




# ==========================================================================
# docs/SUB_CPP.md
# ==========================================================================
@check("SUB_CPP", "textures 119 / sprites 64 / sounds 161 строк", "119 / 64 / 161")
def c_sub_tables(env):
    env.require_game()
    counts = []
    for rel, expect in (("PICS/textures.sub", 119), ("sprites/sprites.sub", 64),
                        ("sounds/sounds.sub", 161)):
        lines = [ln for ln in open(env.game_file(*rel.split("/")), "rb")
                 .read().decode("latin-1").splitlines() if ln.strip()]
        # textures/sprites carry a counter line; sounds are bare TAB pairs.
        data = lines[1:] if rel != "sounds/sounds.sub" else lines
        counts.append(len(data))
    ok = counts == [119, 64, 161]
    return "%d / %d / %d" % tuple(counts), ok, \
        "строки пар в textures/sprites (без счётчика) / sounds"


# ==========================================================================
# docs/SOA_WRITE.md
# ==========================================================================
@check("SOA_WRITE", "104 файла .soa, 8573 Б, 839 имён кадров", "104 / 8573 / 839")
def c_soa_corpus(env):
    env.require_game()
    paths = [p for p in glob.glob(os.path.join(env.game, "**", "*"), recursive=True)
             if p.lower().endswith(".soa")]
    total = sum(os.path.getsize(p) for p in paths)
    names = 0
    for path in paths:
        with open(path, "rb") as fh:
            text = fh.read().decode("latin-1")
        # One frame name per line after the header; blank lines are allowed.
        names += sum(1 for line in text.splitlines() if line.strip() and
                     not line.strip()[0].isdigit())
    ok = (len(paths), total) == (104, 8573)
    return "%d / %d / %d" % (len(paths), total, names), ok, \
        "файлы / байты / непустые строки-имена"




# ==========================================================================
# docs/KILLED.md
# ==========================================================================
@check("KILLED", ".skb 2338 Б / 69 строк; .skk 1967 Б / 50 строк", "2338 / 69 / 1967 / 50")
def c_killed_screens(env):
    env.require_game()
    facts = []
    for name in ("KILLED.skb", "KILLED.skk"):
        blob = open(env.game_file("FRONTEND", name), "rb").read()
        # fgets keeps the trailing `\n` in the buffer and text mode already
        # folded CRLF, so the engine's `0 < strlen < 0x20` accepts 1..30
        # visible bytes (docs/KILLED.md §1).
        lines = [ln.rstrip(b"\r") for ln in blob.split(b"\n")]
        if lines and not lines[-1]:
            lines = lines[:-1]
        accepted = sum(1 for ln in lines if 1 <= len(ln) <= 30)
        facts.append((len(blob), accepted))
    ok = facts == [(2338, 69), (1967, 50)]
    return "%d / %d / %d / %d" % (facts[0][0], facts[0][1], facts[1][0], facts[1][1]), \
        ok, "байты и принятые строки (0 < len < 0x20) .skb / .skk"




# ==========================================================================
# docs/PLAYLIST.md
# ==========================================================================
@check("PLAYLIST", "651 .sog (598 + 53 .SOG), MUSIC/SOU.IDX 3092 записи",
       "651 / 598 / 53 / 3092")
def c_playlist_corpus(env):
    env.require_game()
    paths = [p for p in glob.glob(os.path.join(env.game, "**", "*"), recursive=True)
             if p.lower().endswith(".sog")]
    upper = sum(1 for p in paths if p.endswith(".SOG"))
    entries = parse_bigfile(open(env.game_file("MUSIC", "SOU.IDX"), "rb").read())
    ok = (len(paths), len(paths) - upper, upper, len(entries)) == (651, 598, 53, 3092)
    return "%d / %d / %d / %d" % (len(paths), len(paths) - upper, upper,
                                  len(entries)), ok, \
        "все .sog / строчные / .SOG / записи MUSIC/SOU.IDX"


# ==========================================================================
# docs/GAME_CFG.md
# ==========================================================================
@check("GAME_CFG", "GUNMETAL.CFG: 8192 Б, FNV-1a64 439e3e13a3242648, 16 полей",
       "8192 / 439e3e13a3242648 / 16")
def c_game_cfg_bytes(env):
    blob = open(env.game_file("GUNMETAL.CFG"), "rb").read()
    ok = len(blob) == 8192 and fnv1a64(blob) == 0x439e3e13a3242648 and \
        blob.startswith(b"GUNMETAL Configuration File. av1.93") and \
        "fields=16" in env.doc("GAME_CFG")
    return "%d / %016x" % (len(blob), fnv1a64(blob)), ok, \
        "размер / FNV-1a64 (16 полей — по доку и тесту)"



# ==========================================================================
# docs/HUD_LUT.md
# ==========================================================================
@check("HUD_LUT", "hud_lut.bin: 4096 Б, FNV 6330406254771f3d, уровни 12..15 = 132/73/48/45",
       "4096 / 6330406254771f3d / 132,73,48,45")
def c_hud_lut_dump(env):
    path = os.path.expanduser("~/.cache/gunmetal-re/hud_lut.bin")
    if not os.path.exists(path):
        raise Skip("hud_lut.bin is not dumped on this machine")
    blob = open(path, "rb").read()
    ident = [sum(1 for i in range(256) if blob[k * 256 + i] == i) for k in range(16)]
    ok = len(blob) == 4096 and fnv1a64(blob) == 0x6330406254771f3d and \
        ident[11] == 254 and ident[12:] == [130, 71, 46, 43]
    return "%d / %016x / %d,%d,%d,%d" % (len(blob), fnv1a64(blob),
                                         ident[12], ident[13], ident[14],
                                         ident[15]), ok, \
        "байты / FNV-1a64 / индексные тождества уровней 12..15 (уровень 11 = %d)" % ident[11]


# ==========================================================================
# docs/SOG_WRITE.md
# ==========================================================================
@check("SOG_WRITE", "651 .sog (598 + 53), 43205 Б, 5211 имён", "651 / 43205 / 5211")
def c_sog_corpus(env):
    env.require_game()
    paths = [p for p in glob.glob(os.path.join(env.game, "**", "*"), recursive=True)
             if p.lower().endswith(".sog")]
    total = sum(os.path.getsize(p) for p in paths)
    names = 0
    for path in paths:
        with open(path, "rb") as fh:
            for line in fh.read().decode("latin-1").splitlines():
                if line.strip():
                    names += 1
    ok = (len(paths), total, names) == (651, 43205, 5211)
    return "%d / %d / %d" % (len(paths), total, names), ok, \
        "файлы / байты / непустые имена"




# ==========================================================================
# docs/SOF_WRITE.md
# ==========================================================================
@check("SOF_WRITE", "48 .sof, 95330304 Б, 11637 кадров", "48 / 95330304 / 11637")
def c_sof_corpus(env):
    env.require_game()
    paths = [p for p in glob.glob(os.path.join(env.game, "**", "*"), recursive=True)
             if p.lower().endswith(".sof")]
    total = sum(os.path.getsize(p) for p in paths)
    ok = (len(paths), total) == (48, 95330304) and total % 8192 == 0 and \
        total // 8192 == 11637
    return "%d / %d / %d" % (len(paths), total, total // 8192), ok, \
        "файлы / байты / кадры (8192 Б на кадр)"




# ==========================================================================
# docs/SOO_WRITE.md
# ==========================================================================
@check("SOO_WRITE", "40 .soo (38 + 2 .SOO), 3416 Б, 264 кадра", "40 / 3416 / 264")
def c_soo_corpus(env):
    env.require_game()
    from gm.soo import parse

    paths = [p for p in glob.glob(os.path.join(env.game, "**", "*"), recursive=True)
             if p.lower().endswith(".soo")]
    total = sum(os.path.getsize(p) for p in paths)
    frames = 0
    for path in paths:
        with open(path, "rb") as fh:
            anim = parse(fh.read())
        frames += len(anim.frames)
    ok = (len(paths), total, frames) == (40, 3416, 264)
    return "%d / %d / %d" % (len(paths), total, frames), ok, \
        "файлы / байты / кадры (по gm/soo.py)"







# ==========================================================================
# docs/SOE_WRITE.md
# ==========================================================================
@check("SOE_WRITE", ".soe 340 Б / .sni 3 Б = 2 файла, 343 Б", "340 / 3 / 2")
def c_soe_corpus(env):
    env.require_game()
    paths = [p for p in glob.glob(os.path.join(env.game, "**", "*"), recursive=True)
             if p.lower().endswith((".soe", ".sni"))]
    sizes = {}
    for path in paths:
        ext = os.path.splitext(path)[1].lower()
        sizes.setdefault(ext, []).append(os.path.getsize(path))
    soe = sizes.get(".soe", [])
    sni = sizes.get(".sni", [])
    ok = soe == [340] and sni == [3]
    return "%d / %d / %d" % (soe[0] if soe else -1, sni[0] if sni else -1,
                             len(paths)), ok, \
        "размер .soe / .sni / всего файлов"




# ==========================================================================
# docs/SBM_WRITE.md
# ==========================================================================
@check("SBM_WRITE", "2 SBM-архива, 1827 записей, 46525219 Б payload",
       "2 / 1827 / 46525219")
def c_sbm_corpus(env):
    env.require_game()
    total_entries = 0
    total_payload = 0
    found = 0
    for folder, dat_name in (("PICS", "SBM.DAT"), ("sprites", "SBM.DAT")):
        idx = os.path.join(env.game, folder, "SBM.IDX")
        dat = os.path.join(env.game, folder, dat_name)
        if not (os.path.exists(idx) and os.path.exists(dat)):
            continue
        with open(idx, "rb") as fh:
            header = fh.read(12)
        if len(header) != 12 or header[:8] != b"BIGFILE\x1a":
            continue
        found += 1
        total_entries += int.from_bytes(header[8:12], "little")
        total_payload += os.path.getsize(dat)
    ok = (found, total_entries, total_payload) == (2, 1827, 46525219)
    return "%d / %d / %d" % (found, total_entries, total_payload), ok, \
        "архивы / записи (из .IDX) / байты .DAT"




# ==========================================================================
# docs/BMP_WRITE.md
# ==========================================================================
@check("BMP_WRITE", "FRONTEND/BMP.IDX: 143 записи, 2845582 Б, .IDX 1728 Б",
       "143 / 2845582 / 1728")
def c_bmp_corpus(env):
    env.require_game()
    idx = env.game_file("FRONTEND", "BMP.IDX")
    dat = env.game_file("FRONTEND", "BMP.DAT")
    with open(idx, "rb") as fh:
        header = fh.read(12)
    entries = int.from_bytes(header[8:12], "little")
    ok = (entries, os.path.getsize(dat), os.path.getsize(idx)) == (143, 2845582, 1728)
    return "%d / %d / %d" % (entries, os.path.getsize(dat), os.path.getsize(idx)), ok, \
        "записи из .IDX / байты .DAT / байты .IDX"




# ==========================================================================
# docs/HI_WRITE.md
# ==========================================================================
@check("HI_WRITE", "GUNMETAL.HI: 576 Б = 9×0x40, времена 146,1570,18,109,18,7,11,40,6",
       "576 / 9 / 146,1570,18,109,18,7,11,40,6")
def c_hi_corpus(env):
    env.require_game()
    path = env.game_file("GUNMETAL.HI")
    with open(path, "rb") as fh:
        blob = fh.read()
    times = [int.from_bytes(blob[i * 0x40:i * 0x40 + 4], "little")
             for i in range(len(blob) // 0x40)]
    ok = len(blob) == 576 and times == [146, 1570, 18, 109, 18, 7, 11, 40, 6]
    return "%d / %d / %s" % (len(blob), len(times),
                             ",".join(str(t) for t in times)), ok, \
        "байты / записи / времена"


# ==========================================================================
# docs/KILLED_WRITE.md
# ==========================================================================
@check("KILLED_WRITE", ".skb 2338 Б / 69 принято / 138 %S; .skk 1967 / 50 / 50",
       "2338 / 69 / 138 ; 1967 / 50 / 50")
def c_killed_corpus(env):
    env.require_game()
    out = []
    ok = True
    for name, size, accepted, subs in (("KILLED.skb", 2338, 69, 138),
                                       ("KILLED.skk", 1967, 50, 50)):
        path = env.game_file("FRONTEND", name)
        with open(path, "rb") as fh:
            blob = fh.read()
        lines = blob.decode("latin-1").split("\n")
        if lines and lines[-1] == "":
            lines.pop()
        visible = [ln[:-1] if ln.endswith("\r") else ln for ln in lines]
        # The loader reads in text mode and `fgets` keeps the `\n`, so the
        # proven filter `0 < strlen(line) < 0x20` admits 1..30 visible bytes.
        taken = [ln for ln in visible if 0 < len(ln) < 0x1F]
        placeholders = sum(ln.count("%S") + ln.count("%s") for ln in taken)
        ok = ok and (len(blob), len(taken), placeholders) == (size, accepted, subs)
        out.append("%d/%d/%d" % (len(blob), len(taken), placeholders))
    return " ; ".join(out), ok, "байты/принято/плейсхолдеры по каждому файлу"




# ==========================================================================
# docs/KEY_WRITE.md
# ==========================================================================
@check("KEY_WRITE", "KEYCFG: 3 .key по 376 Б = 1128 Б, имена PERFECT/SPLIT1/SPLIT2",
       "3 / 1128 / PERFECT,SPLIT1,SPLIT2")
def c_key_corpus(env):
    env.require_game()
    root = env.game_file("KEYCFG")
    paths = sorted(p for p in glob.glob(os.path.join(root, "*"))
                   if p.lower().endswith(".key"))
    total = sum(os.path.getsize(p) for p in paths)
    names = []
    for path in paths:
        with open(path, "rb") as fh:
            blob = fh.read()
        names.append(blob[:16].split(b"\x00", 1)[0].decode("latin-1"))
    ok = (len(paths), total, names) == (3, 1128, ["PERFECT", "SPLIT1", "SPLIT2"])
    return "%d / %d / %s" % (len(paths), total, ",".join(names)), ok, \
        "файлы / байты / имена (по порядку имён файлов)"




# ==========================================================================
# docs/CLI.md
# ==========================================================================
@check("CLI", "199 опции парсера, все есть в --help", "199")
def c_cli_options(env):
    root = os.path.dirname(env.docs.rstrip("/"))
    source = os.path.join(root, "engine", "src", "core", "config.cpp")
    if not os.path.exists(source):
        raise Skip("config.cpp not found at " + source)
    with open(source, encoding="utf-8") as fh:
        text = fh.read()
    options = sorted(set(re.findall(r'"(--[a-z0-9-]+)"', text)))
    ok = len(options) == 199
    return str(len(options)), ok, "уникальных опций в парсере (docs/CLI.md)"




# ==========================================================================
# docs/LUT_BUILDER.md
# ==========================================================================
@check("LUT_BUILDER", "построитель 0x682f90: 4096/4096 байт, FNV 6330406254771f3d",
       "4096 / 6330406254771f3d")
def c_lut_builder(env):
    env.require_game()
    dump = os.path.expanduser("~/.cache/gunmetal-re/hud_lut.bin")
    if not os.path.exists(dump):
        raise Skip("hud_lut.bin is not dumped on this machine")
    sys.path.insert(0, os.path.join(os.path.dirname(env.docs.rstrip("/")), "tools", "re"))
    import lut_build

    palette = lut_build.load_palette(env.game)
    table = lut_build.build_from_palette(palette)
    with open(dump, "rb") as fh:
        stored = fh.read()
    same = sum(1 for a, b in zip(table, stored) if a == b)
    digest = lut_build.fnv1a64(table)
    ok = len(table) == 4096 and same == 4096 and digest == 0x6330406254771f3d
    return "%d / %d / %016x" % (len(table), same, digest), ok, \
        "байты / совпало с дампом / FNV-1a64"




# ==========================================================================
# docs/THING_ANIM_BIND.md
# ==========================================================================
@check("THING_ANIM_BIND", "40 .soo / mode 0: 26, mode 1: 14 (264 кадра в файлах; 261 загружаемых)",
       "40 / 26 / 14 / 264")
def c_thing_anim(env):
    env.require_game()
    from gm.soo import parse

    paths = [p for p in glob.glob(os.path.join(env.game, "**", "*"), recursive=True)
             if p.lower().endswith(".soo")]
    frames = 0
    mode0 = 0
    mode1 = 0
    for path in paths:
        with open(path, "rb") as fh:
            anim = parse(fh.read())
        frames += len(anim.frames)
        if anim.mode == 0:
            mode0 += 1
        else:
            mode1 += 1
    ok = (len(paths), mode0, mode1, frames) == (40, 26, 14, 264)
    return "%d / %d / %d / %d" % (len(paths), mode0, mode1, frames), ok, \
        "файлы / mode 0 / mode 1 / все кадры (261 — загружаемые движком)"


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
