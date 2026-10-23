#!/usr/bin/env python3
"""UI sounds of the Gunmetal front end (M8.6): samples, triggers, music.

The tool is read-only and stdlib-only (plus the project's ``exelib`` for the
pre-generated listing).  It answers three questions:

* **which** samples the front end loads and plays (``--sounds``): the seven
  handle globals ``0x4bc44c..0x4bc464``, their names, the ``sounds/SOU.IDX``
  record each name resolves to (codec, rate, channels, frames, seconds,
  FNV-1a64 of the payload) and whether the handle is ever consumed;
* **when** each one plays (``--map``): the event code the front-end input
  dispatcher ``0x48b4d0`` writes (1 = ``updown``, 2 = ``select``), the
  ``vleft`` sites, the ``selectm``/``menuon`` callbacks, and the music path;
* **what music** the front end starts (``--music``): ``MUSIC/<name>.sog`` is
  opened for a *film* name only; there is no menu music bank.

No Ghidra and no decompiler: every claim is re-checked against the bytes by
``--self-test`` (strings, store/read sites, the ``dword`` reference sets,
call arguments, the ``sounds.sub`` grammar, the ``.sou`` headers and the
``docs/manifests/sou1-fnv.json`` decode).

Examples::

    python3 tools/re/ui_sounds.py --sounds
    python3 tools/re/ui_sounds.py --map
    python3 tools/re/ui_sounds.py --music
    python3 tools/re/ui_sounds.py --json
    python3 tools/re/ui_sounds.py --self-test

Addresses are VA in ``gundx.exe`` (ImageBase 0x400000); ``GUNMETAL_GAME``
(default ``/mnt/d/games/Gunmetal 3dfx``) points at the installation and is
only read.
"""
from __future__ import annotations

import argparse
import json
import os
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools" / "re"))
sys.path.insert(0, str(ROOT))

try:
    import exelib  # noqa: E402
except Exception as exc:  # pragma: no cover - broken checkout
    exelib = None
    _EXELIB_ERROR = exc

DEFAULT_GAME = Path(os.environ.get("GUNMETAL_GAME", "/mnt/d/games/Gunmetal 3dfx"))
MANIFEST = ROOT / "docs" / "manifests" / "sou1-fnv.json"

# ---------------------------------------------------------------------------
# Static code facts (VA in gundx.exe).  Every one is re-checked by --self-test.
# ---------------------------------------------------------------------------

#: The 2-D one-shot request queue.  ``0x40be20(handle, volume, pan, rate,
#: priority)``; the request record at ``0x4cabd8`` has ``+0x10 = pan``
#: (docs/SOUND_FIELDS.md 4), so the third argument is the pan byte.
SOUND_QUEUE = 0x40BE20
#: ``name -> handle``: basename up to 8 chars, 300-slot cache at ``0x4c9c90``,
#: handles in ``0x4ca720``.  Applies ``sounds.sub`` (see SUB_TABLE_LOAD).
SAMPLE_LOAD = 0x40B9C0
#: ``sounds.sub`` parser (``%ld`` count, then ``%ld %s %s``), table ``0x4c8778``.
SUB_TABLE_LOAD = 0x40B510
SUB_COUNT = 0x4CF6B8
SUB_TABLE = 0x4C8778
SUB_STRIDE = 0x24
SUB_MAX = 0x96  # 150, the "maximum sample substitutions" warning
#: Loads all seven UI samples (called from 0x44b0ad right after the sound
#: system comes up, and from four cache-flush sites).
UI_SOUND_LOADER = 0x48F770
#: Front-end input dispatcher (docs/UI_SCREENS.md 8): the sound gate and the
#: two event-code branches live at its tail.
FRONTEND_INPUT = 0x48B4D0
SOUND_GATE = 0x4AA384        # 0 = sound disabled (/n or no DirectSound)
EFF_VOLUME = 0x4A68BC        # 0..32, scales arg2 of 0x40be20 (>>6)
MUSIC_VOLUME = 0x4AAC74      # 0..20, CFG 0x190
#: Volume setters: 0..100 in, clamped and scaled (docs/UI_SCREENS.md 3.2).
MUSIC_VOL_SET = 0x40B860
EFF_VOL_SET = 0x40B900
#: Music: playlist loader / bank start / bank free / stop.
MUSIC_LOAD = 0x40AB20        # (bank, name, report) -> 0; 0x40b8a0 is a wrapper
PLAY_MUSIC = 0x40B8A0
MUSIC_START = 0x40B8D0
MUSIC_FREE = 0x40B8C0
MUSIC_STOP = 0x40AA10
#: Film player with paired music, and the SBV decoder it drives.
FILM_PLAYER = 0x4913A0
SBV_DECODER = 0x49E1E8
#: Main-menu entry-art slide-in: starter, updater and the callback field.
ANIM_START = 0x498AE0        # 0x498ae0(9, 0xc4, 0, tick, ms, callback)
ANIM_START_SPRITE = 0x498820 # draw with a 16th "completion callback" argument
ANIM_UPDATE = 0x4988E0       # calls [record+0x44]([record+0x40]) on completion

#: name, name string VA, handle global, handle store site, handle read sites,
#: play call site(s), volume argument, pan argument, status.
UI_SOUNDS = [
    dict(name="updown", name_va=0x4BDF7C, handle=0x4BC454,
         store=0x48F780, reads=[0x48E60C], calls=[0x48E624],
         vol=0x30, pan=0x80, push_va=0x48E612, status="plays"),
    dict(name="select", name_va=0x4BDF4C, handle=0x4BC458,
         store=0x48F7A8, reads=[0x48E639], calls=[0x48E651],
         vol=0x30, pan=0x80, push_va=0x48E63F, status="plays"),
    dict(name="vleft", name_va=0x4BDF1C, handle=0x4BC45C,
         store=0x48F7CC, reads=[0x48BE65, 0x48C6AB, 0x48D6EB],
         calls=[0x48C6C0, 0x48D700],
         vol=0x30, pan=0x00, push_va=0x48C6B1, status="plays (pan = left)"),
    dict(name="vfront", name_va=0x4BDEEC, handle=0x4BC460,
         store=0x48F7F0, reads=[], calls=[],
         vol=None, pan=None, push_va=None, status="loaded, never played"),
    dict(name="vback", name_va=0x4BDEBC, handle=0x4BC464,
         store=0x48F814, reads=[], calls=[],
         vol=None, pan=None, push_va=None, status="loaded, never played"),
    dict(name="menuon", name_va=0x4BDE8C, handle=0x4BC44C,
         store=0x48F835, reads=[0x486720], calls=[0x486737],
         vol=0x30, pan=0x80, push_va=0x486705, status="plays (title entry)"),
    dict(name="selectm", name_va=0x4BDE84, handle=0x4BC450,
         store=0x48F85E, reads=[0x486700], calls=[0x486717],
         vol=0x30, pan=0x80, push_va=0x486705, status="plays (every Enter)"),
]

#: The seven vleft sites share the argument block but three different read
#: instructions (a1 / 8b 15 / 8b 0d); ``push_va`` above is one of them.
VLEFT_READS = [0x48BE65, 0x48C6AB, 0x48D6EB]
VLEFT_PUSHES = [0x48BE6A, 0x48C6B1, 0x48D6F1]

#: The handle globals are seven consecutive dwords; nothing indexes them.
HANDLE_BLOCK = (0x4BC44C, 0x4BC468)  # [lo, hi)

#: dword reference sets (operand VA, i.e. instruction + 1 for ``a1``/``a3``).
EXPECTED_REFS = {
    "updown":  [0x48F781, 0x48E60E],
    "select":  [0x48F7A9, 0x48E63B],
    "vleft":   [0x48F7CD, 0x48BE66, 0x48C6AD, 0x48D6ED],
    "vfront":  [0x48F7F1],
    "vback":   [0x48F815, 0x48F83B, 0x48F864],
    "menuon":  [0x48F836, 0x486721],
    "selectm": [0x48F85F, 0x486701],
}

#: Loader facts: the seven ``push name`` + ``mov [handle], eax`` pairs, and the
#: copy-paste bug — the two last error checks read vback (0x4bc464) instead of
#: menuon/selectm, and both report "VBACK".
LOADER_PUSHES = [0x48F771, 0x48F79B, 0x48F7BF, 0x48F7E3, 0x48F807,
                 0x48F82B, 0x48F854]
LOADER_BUG_READS = [0x48F83A, 0x48F863]
LOADER_CALLS = [0x44B0AD, 0x4423F8, 0x442D1E, 0x442FB8, 0x491446]

#: ``sounds.sub``: tab-separated ``from<TAB>to`` pairs, CRLF, 161 lines.  The
#: parser at SUB_TABLE_LOAD expects a decimal count line first, so the shipped
#: file yields zero records — the substitution table stays empty.
SUB_PAIRS = 161
SUB_GREEDY = ("selectm", "select")
SUB_COUNT_FMT = 0x4A6910   # "%ld"
SUB_LINE_FMT = 0x4A68C0    # "%ld %s %s"

#: Music: only three PLAY_MUSIC call sites exist in the whole executable.
PLAY_MUSIC_SITES = {
    0x45B5B4: "level script opcode: 0x40b8a0(bank, name, 1); 0x45b56a is 0x40b8d0(bank)",
    0x491122: "title: intro film, MUSIC/Intro.sog",
    0x4914D4: "film: MUSIC/<film name>.sog (winner)",
}
MUSIC_FORMAT_OGG = 0x4A6878   # "music\%s.sog"
MUSIC_FORMAT_BARE = 0x4A688C  # "%s.sog"
MUSIC_BANKS = 0x20            # 32
MUSIC_SAMPLES = 0x64          # 100 samples per playlist

#: The title sequence (state 0) plays a plain SOU1 sample called "intro"
#: before the film starts; 32000 Hz stereo, 18.0 s.
TITLE_SAMPLE = dict(name="INTRO", name_va=0x4BE444, handle=0x68DE20,
                    load=0x490AA4, play=0x490D70, vol=0x50, pan=0x80)

#: Expected sample facts (regression anchors; measured, see docs/UI_SOUNDS.md).
SAMPLES = {
    "MENUON":  dict(idx=425, rate=16000, ch=1, frames=28500, payload=14250,
                    fnv="57f410a64053d576"),
    "SELECT":  dict(idx=510, rate=16000, ch=1, frames=6272, payload=3136,
                    fnv="dece4c364782b7e2"),
    "SELECTM": dict(idx=511, rate=16000, ch=1, frames=6608, payload=3304,
                    fnv="2274810948bc7779"),
    "UPDOWN":  dict(idx=581, rate=16000, ch=1, frames=6272, payload=3136,
                    fnv="c71a5008363b7f25"),
    "VBACK":   dict(idx=636, rate=8000, ch=1, frames=4168, payload=2084,
                    fnv="a1040cb0a2c890b8"),
    "VFRONT":  dict(idx=735, rate=8000, ch=1, frames=3976, payload=1988,
                    fnv="e95bda7dcf69791c"),
    "VLEFT":   dict(idx=804, rate=8000, ch=1, frames=4608, payload=2304,
                    fnv="5218f3ff48ff7fc9"),
    "INTRO":   dict(idx=392, rate=32000, ch=2, frames=576000, payload=576000,
                    fnv="ef20d35a946f4b5f"),
}

#: (screen, action, sound, anchor VA, expected bytes hex, proof)
MAP_ROWS = [
    ("1..37 (except 16)", "Enter 0xd / 0xa4", "selectm", 0x48CED5,
     "6800674800", "push 0x486700 -> 0x498ae0(9,0xc4,0,tick,27,cb) at 0x48cef1"),
    ("1..37 (except 16)", "Enter 0xd / 0xa4", "select", 0x48CF0D,
     "c744241002000000", "event=2; gated by 0x48cef6 (0x4aa384) and 0x48cf08 (!=0x10)"),
    ("1..37 (except 16)", "up 0x98 / 0xa9", "updown", 0x48B9F2,
     "390d84a34a00", "event=1 at 0x48b9ff; gates 0x48b9f2 (sound), 0x48b9fa (!=0x10)"),
    ("1..37 (except 16)", "down 0xa0 / 0xae", "updown", 0x48BB97,
     "390d84a34a00", "event=1 at 0x48bba4; gates 0x48bb97 (sound), 0x48bb9f (!=0x10)"),
    ("lists", "left/right (default)", "select", 0x48BD79,
     "c744241002000000", "event=2 chosen before the per-screen switch"),
    ("11 SOUND OPTIONS", "Switch Left Right: Enter", "vleft", 0x48D6EB,
     "8b0d5cc44b00", "toggle 0x5a494c -> 0x40b600, then pan=0 (left)"),
    ("11 SOUND OPTIONS", "Switch Left Right: left", "vleft", 0x48BE65,
     "a15cc44b00", "same handler, pan=0"),
    ("11 SOUND OPTIONS", "Switch Left Right: right", "vleft", 0x48C6AB,
     "8b155cc44b00", "same handler, pan=0"),
    ("11 SOUND OPTIONS", "Switch Left Right: select suppressed", "-", 0x48C6C8,
     "c744241000000000", "event forced to 0 after vleft (0x48d708 for Enter)"),
    ("11 SOUND OPTIONS", "volume changed", "updown", 0x48BE88,
     "895c2410", "event=1 (ebx=bVar19) when the volume really moved"),
    ("11 SOUND OPTIONS", "volume at 0/20 limit", "-", 0x48BE93,
     "894c2410", "event=0 (ecx=0) at the limit; mirrored at 0x48c717/0x48c722"),
    ("12 GRAPHICS OPTIONS", "Border", "updown", 0x48C7A8,
     "c744241001000000", "border 0x4bc1c8 +/-1, event=1"),
    ("12 GRAPHICS OPTIONS", "Reflections", "select", 0x48C7D3,
     "2b1d84ac4a00", "0x4aac84 = 1-x, event stays 2"),
    ("12 GRAPHICS OPTIONS", "Gamma / Light Maps", "select", 0x48C7E4,
     "a188ac4a00", "clamped at 3 / 2 -> 0x48ce70 (event=0), else event stays 2"),
    ("any", "value clamped", "-", 0x48CE70,
     "894c2410", "shared clamp tail: event=0 -> silence"),
    ("any", "Esc 0x1b", "-", 0x48B4E1,
     "897c2410", "event initialised 0; 0x48e4ca..0x48e5ad writes nothing"),
    ("28/29 flag select", "up/down", "updown", 0x48CE85,
     "c744241001000000", "flag list 0x6e93f0/0x68ddc0, event=1"),
    ("34/35 key editor", "key rebind", "-", 0x48B909,
     "890495601f6c00", "writes 0x6c1f60 and returns before the sound block"),
    ("1 main menu", "entry art slide-in", "menuon", 0x486756,
     "6820674800", "callback of the 0x498820 slide-in, fired at 0x498a6c"),
    ("-", "vfront / vback", "never", 0x48F7F0,
     "a360c44b00", "handle stored, no reader (see EXPECTED_REFS)"),
]


# ---------------------------------------------------------------------------
# Executable helpers
# ---------------------------------------------------------------------------

def _need_exelib() -> None:
    if exelib is None:
        raise SystemExit(f"ui_sounds: cannot import exelib: {_EXELIB_ERROR}")


def blob() -> bytes:
    _need_exelib()
    return exelib.data()


def bytes_at(va: int, size: int) -> bytes:
    off = exelib.va2off(va)
    return blob()[off:off + size]


def cstr_at(va: int) -> str:
    _need_exelib()
    return exelib.cstr(va, 160)


def dword_refs(value: int) -> list[int]:
    """Every dword operand equal to ``value`` (as VA), sorted."""
    d = blob()
    needle = struct.pack("<I", value)
    out, i = [], d.find(needle)
    while i >= 0:
        va = exelib.off2va(i)
        if va is not None:
            out.append(va)
        i = d.find(needle, i + 1)
    return sorted(out)


def call_target(site: int) -> int | None:
    """``call rel32`` at ``site`` -> target VA."""
    raw = bytes_at(site, 5)
    if raw[:1] != b"\xe8":
        return None
    rel = struct.unpack_from("<i", raw, 1)[0]
    return site + 5 + rel


def call_sites(target: int) -> list[int]:
    """Every ``call rel32`` in ``.text`` whose target is ``target``."""
    d = blob()
    text = [s for s in exelib.sections() if s[0] == ".text"][0]
    _name, va, size, off = text
    out = []
    chunk = d[off:off + size]
    i = chunk.find(b"\xe8")
    while i >= 0:
        if i + 5 <= len(chunk):
            rel = struct.unpack_from("<i", chunk, i + 1)[0]
            site = va + i
            if site + 5 + rel == target:
                out.append(site)
        i = chunk.find(b"\xe8", i + 1)
    return out


def writes_local8(lo: int, hi: int) -> list[int]:
    """Sites in ``[lo, hi)`` that store into ``[esp+0x10]`` (``local_8``)."""
    d = bytes_at(lo, hi - lo)
    out = []
    for i in range(len(d) - 3):
        if d[i + 1:i + 4] != b"\x44\x24\x10":
            continue
        if d[i] in (0x89, 0x88, 0xC7, 0xC6):  # mov [esp+0x10], reg/imm
            out.append(lo + i)
    return out


def fnv1a64(data: bytes) -> int:
    value = 0xCBF29CE484222325
    for byte in data:
        value ^= byte
        value = (value * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return value


# ---------------------------------------------------------------------------
# Installation helpers (read only)
# ---------------------------------------------------------------------------

def bigfile_index(idx: Path) -> list[tuple[str, int]]:
    """``BIGFILE`` index -> [(name, offset)], in file order."""
    if not idx.is_file():
        return []
    raw = idx.read_bytes()
    if len(raw) < 12 or not raw.startswith(b"BIGFILE"):
        raise ValueError(f"{idx}: not a BIGFILE index")
    count = struct.unpack_from("<I", raw, 8)[0]
    if 12 + count * 12 > len(raw):
        raise ValueError(f"{idx}: truncated index")
    out = []
    for i in range(count):
        off = 12 + 12 * i
        name = raw[off:off + 8].rstrip(b"\0").decode("latin1")
        out.append((name, struct.unpack_from("<I", raw, off + 8)[0]))
    return out


def sou_record(game: Path, name: str) -> tuple[dict, bytes] | None:
    """Parse ``sounds/SOU.IDX`` entry ``name`` -> (facts, raw blob)."""
    idx = game / "sounds" / "SOU.IDX"
    dat = game / "sounds" / "SOU.DAT"
    recs = bigfile_index(idx)
    if not recs or not dat.is_file():
        return None
    dat_size = dat.stat().st_size
    want = name.upper()
    for i, (rec_name, start) in enumerate(recs):
        if rec_name.upper() != want:
            continue
        end = recs[i + 1][1] if i + 1 < len(recs) else dat_size
        with dat.open("rb") as fh:
            fh.seek(start)
            raw = fh.read(end - start)
        return parse_sou(raw), raw
    return None


def parse_sou(raw: bytes) -> dict:
    """Minimal ``.sou`` reader (container layout: docs/SOU_CONTAINER.md)."""
    magic = raw[:4].decode("latin1")
    length = 0
    if magic in ("SOU0", "SOU1"):
        rate, flags, length = struct.unpack_from("<III", raw, 4)
        payload = raw[16:16 + (length & ~1)]
        channels = 2 if flags & 1 else 1
        frames = (2 * len(payload) // channels if magic == "SOU1"
                  else len(payload) // channels)
    elif magic == "SOU2":
        rate, frames, _meta2, units = struct.unpack_from("<IIII", raw, 4)
        payload = raw[20:20 + units * 4]
        channels = 2
    else:
        raise ValueError(f"not a .sou blob: {raw[:4]!r}")
    return dict(magic=magic, rate=rate, channels=channels, frames=frames,
                payload_bytes=len(payload), payload=payload,
                length_field=(length if magic != "SOU2" else len(payload)),
                dropped=(1 if magic != "SOU2" and length & 1 else 0))


def sub_pairs(game: Path) -> list[tuple[str, str]]:
    path = game / "sounds" / "sounds.sub"
    if not path.is_file():
        return []
    text = path.read_bytes().decode("latin1").replace("\r\n", "\n")
    out = []
    for line in text.split("\n"):
        if not line.strip():
            continue
        parts = line.split("\t")
        if len(parts) != 2:
            continue
        out.append((parts[0].strip(), parts[1].strip()))
    return out


def sub_pairs_by_code_grammar(game: Path) -> int:
    """Records the shipped ``sounds.sub`` yields under the exe's grammar.

    ``0x40b510`` reads ``fscanf(f, "%ld", &count)`` and then
    ``fscanf(f, "%ld %s %s", ...)`` per pair; ``r`` mode is ``"rt"``.  The
    shipped file starts with a ``from<TAB>to`` line, so ``%ld`` fails and the
    count stays the one written before the first read (0).
    """
    path = game / "sounds" / "sounds.sub"
    if not path.is_file():
        return 0
    count = 0
    for line in path.read_bytes().decode("latin1").splitlines():
        head = line.split("\t")[0].strip()
        if not head or not (head.isdigit() or (head[0] in "+-" and head[1:].isdigit())):
            break  # fscanf("%ld") failed: the loop stops, nothing was read
        count += 1
        if count >= SUB_MAX:
            break
    return count


def music_banks(game: Path) -> list[str]:
    mdir = game / "MUSIC"
    if not mdir.is_dir():
        return []
    return sorted(p.stem for p in mdir.glob("*.sog"))


def sog_lines(game: Path, name: str) -> list[str]:
    path = game / "MUSIC" / f"{name}.sog"
    if not path.is_file():
        return []
    text = path.read_bytes().decode("latin1").replace("\r\n", "\n")
    return [line.strip() for line in text.split("\n") if line.strip()]


# ---------------------------------------------------------------------------
# Reports
# ---------------------------------------------------------------------------

def sound_rows(game: Path | None) -> list[dict]:
    rows = []
    for spec in UI_SOUNDS:
        row = dict(name=spec["name"], name_va=f"{spec['name_va']:#x}",
                   handle=f"{spec['handle']:#x}", store=f"{spec['store']:#x}",
                   reads=[f"{va:#x}" for va in spec["reads"]],
                   calls=[f"{va:#x}" for va in spec["calls"]],
                   volume=spec["vol"], pan=spec["pan"],
                   status=spec["status"])
        if game is not None:
            rec = sou_record(game, spec["name"])
            if rec:
                facts, raw = rec
                row.update(
                    idx=SAMPLES.get(spec["name"].upper(), {}).get("idx"),
                    codec=facts["magic"], rate=facts["rate"],
                    channels=facts["channels"], frames=facts["frames"],
                    seconds=round(facts["frames"] / facts["rate"], 3),
                    payload=facts["payload_bytes"],
                    dropped=facts["dropped"],
                    fnv="%016x" % fnv1a64(facts["payload"]),
                )
        rows.append(row)
    if game is not None:
        rec = sou_record(game, TITLE_SAMPLE["name"])
        if rec:
            facts, _raw = rec
            rows.append(dict(
                name=TITLE_SAMPLE["name"], name_va=f"{TITLE_SAMPLE['name_va']:#x}",
                handle=f"{TITLE_SAMPLE['handle']:#x}",
                store=f"{TITLE_SAMPLE['load']:#x}",
                reads=[], calls=[f"{TITLE_SAMPLE['play']:#x}"],
                volume=TITLE_SAMPLE["vol"], pan=TITLE_SAMPLE["pan"],
                status="title fade-in sample (18 s)",
                idx=SAMPLES["INTRO"]["idx"], codec=facts["magic"],
                rate=facts["rate"], channels=facts["channels"],
                frames=facts["frames"],
                seconds=round(facts["frames"] / facts["rate"], 3),
                payload=facts["payload_bytes"], dropped=facts["dropped"],
                fnv="%016x" % fnv1a64(facts["payload"])))
    return rows


def music_report(game: Path | None) -> dict:
    banks = music_banks(game) if game is not None else []
    level = [b for b in banks if b[:1] in "LD" and len(b) >= 4]
    other = [b for b in banks if b not in level]
    return dict(
        play_sites={f"{va:#x}": note for va, note in PLAY_MUSIC_SITES.items()},
        loader=f"{MUSIC_LOAD:#x}", wrapper=f"{PLAY_MUSIC:#x}",
        formats=[cstr_at(MUSIC_FORMAT_OGG), cstr_at(MUSIC_FORMAT_BARE)],
        banks=banks, bank_count=len(banks), level_banks=len(level),
        other_banks=other,
        intro_playlist=sog_lines(game, "Intro") if game else [],
        winner_playlist=sog_lines(game, "winner") if game else [],
        title_sample=TITLE_SAMPLE["name"],
        film_player=f"{FILM_PLAYER:#x}", sbv=f"{SBV_DECODER:#x}",
        music_volume=f"{MUSIC_VOLUME:#x}", music_vol_set=f"{MUSIC_VOL_SET:#x}",
        eff_volume=f"{EFF_VOLUME:#x}", eff_vol_set=f"{EFF_VOL_SET:#x}",
        menu_music_bank=None,
    )


def print_sounds(rows: list[dict]) -> None:
    print("UI sounds: handle globals 0x4bc44c..0x4bc464, loader 0x48f770")
    head = (f"{'name':8s} {'idx':>4s} {'codec':5s} {'rate':>5s} {'ch':>2s} "
            f"{'frames':>6s} {'sec':>5s} {'bytes':>6s} {'fnv1a64':16s} "
            f"{'handle':8s} {'store':8s} status")
    print(head)
    print("-" * len(head))
    for r in rows:
        print(f"{r['name']:8s} {str(r.get('idx','-')):>4s} "
              f"{r.get('codec','-'):5s} {str(r.get('rate','-')):>5s} "
              f"{str(r.get('channels','-')):>2s} {str(r.get('frames','-')):>6s} "
              f"{str(r.get('seconds','-')):>5s} {str(r.get('payload','-')):>6s} "
              f"{r.get('fnv','-'):16s} {r['handle']:8s} {r['store']:8s} "
              f"{r['status']}")
    print()
    print("idx = sounds/SOU.IDX record (0-based); bytes = payload the loader")
    print("0x40af30 sees (length & ~1); fnv1a64 is over exactly those bytes.")


def print_map() -> None:
    print("UI sound triggers (input 0x48b4d0, play 0x40be20):")
    print("event 1 -> updown (0x48e60c), event 2 -> select (0x48e639),"
          " gate 0x4aa384 (0x48e5fa)")
    head = f"{'screen':20s} {'action':34s} {'sound':8s} {'anchor':8s} proof"
    print(head)
    print("-" * len(head))
    for screen, action, sound, anchor, _pat, proof in MAP_ROWS:
        print(f"{screen:20s} {action:34s} {sound:8s} {anchor:#08x} {proof}")
    print()
    print("selectm: 0x498ae0 call sites 0x48ced5, 0x48f3f9, 0x48f65a, 0x48f692")
    print("menuon : animation callback 0x486756 -> 0x498820, fired at 0x498a6c")
    print("Esc (0x48e4ca..0x48e5ad) writes no event -> silence")


def print_music(rep: dict) -> None:
    print(f"music: loader {rep['loader']}, wrapper {rep['wrapper']}, "
          f"formats {rep['formats']}")
    others = rep["other_banks"]
    shown = ", ".join(others[:12]) + (" …" if len(others) > 12 else "")
    print(f".sog banks: {rep['bank_count']} (levels: {rep['level_banks']}, "
          f"other {len(others)}: {shown})")
    for va, note in rep["play_sites"].items():
        print(f"  play_music @ {va}: {note}")
    print(f"Intro.sog  -> {rep['intro_playlist']}")
    print(f"winner.sog -> {rep['winner_playlist']}")
    print(f"menu music: {'none' if rep['menu_music_bank'] is None else rep['menu_music_bank']}")
    print(f"music volume: {rep['music_volume']} (CFG 0x190) -> {rep['music_vol_set']}")
    print(f"effects volume: {rep['eff_volume']} -> {rep['eff_vol_set']}")


# ---------------------------------------------------------------------------
# Self-test
# ---------------------------------------------------------------------------

class Checks:
    def __init__(self) -> None:
        self.items: list[dict] = []

    def add(self, ok: bool, name: str, detail: str = "") -> bool:
        self.items.append(dict(ok=bool(ok), name=name, detail=detail))
        return bool(ok)

    @property
    def failed(self) -> list[dict]:
        return [c for c in self.items if not c["ok"]]


def check_samples_exe(chk: Checks) -> None:
    for spec in UI_SOUNDS:
        name = spec["name"]
        chk.add(cstr_at(spec["name_va"]) == name, f"name string {name}",
                f"{spec['name_va']:#x} = {cstr_at(spec['name_va'])!r}")
        want = b"\xa3" + struct.pack("<I", spec["handle"])
        got = bytes_at(spec["store"], 5)
        chk.add(got == want, f"store {name}", f"{spec['store']:#x}: {got.hex()}")
        refs = dword_refs(spec["handle"])
        chk.add(refs == sorted(EXPECTED_REFS[name]), f"refs {name}",
                ", ".join(f"{va:#x}" for va in refs))
        for read in spec["reads"]:
            body = bytes_at(read, 7)
            chk.add(body[:1] in (b"\xa1", b"\x8b") and
                    struct.pack("<I", spec["handle"]) in body,
                    f"read {name} {read:#x}", body.hex())
    # the seven handles sit in one block and nothing indexes it
    lo, hi = HANDLE_BLOCK
    handles = [struct.unpack_from("<I", bytes_at(va, 4))[0]
               for va in range(lo, hi, 4)]
    chk.add(len(handles) == 7, "handle block",
            " ".join(f"{h:#x}" for h in handles))
    # the neighbouring dword 0x4bc468 is a joystick table, not a handle
    mouse = dword_refs(0x4BC468)
    chk.add(bool(mouse) and all(0x489000 <= va < 0x48A500 for va in mouse),
            "0x4bc468 is not a handle", ", ".join(f"{va:#x}" for va in mouse))


def check_play_sites(chk: Checks) -> None:
    for spec in UI_SOUNDS:
        if not spec["calls"]:
            continue
        for call in spec["calls"]:
            tgt = call_target(call)
            chk.add(tgt == SOUND_QUEUE, f"call {spec['name']} {call:#x}",
                    f"-> {tgt if tgt is None else hex(tgt)}")
        push = spec["push_va"]
        if push is None:
            continue
        head = bytes_at(push, 10)
        chk.add(head == bytes.fromhex("68e803000068112b0000"),
                f"args {spec['name']}", head.hex())
        small = spec["pan"] < 0x80
        plen = 2 if small else 5
        pan = bytes_at(push + 10, plen)
        want = (b"\x6a" + bytes([spec["pan"]]) if small
                else b"\x68" + struct.pack("<I", spec["pan"]))
        chk.add(pan == want, f"pan {spec['name']}", pan.hex())
        vol = bytes_at(push + 10 + plen, 2)
        chk.add(vol == b"\x6a\x30", f"vol {spec['name']}", vol.hex())
    for push in VLEFT_PUSHES:
        chk.add(bytes_at(push + 10, 2) == b"\x6a\x00", f"vleft pan {push:#x}",
                bytes_at(push + 10, 2).hex())


def check_callbacks(chk: Checks) -> None:
    chk.add(bytes_at(0x486756, 5) == bytes.fromhex("6820674800"),
            "menuon callback push", bytes_at(0x486756, 5).hex())
    chk.add(bytes_at(0x486737, 5) == bytes.fromhex("e8e456f8ff") and
            call_target(0x486737) == SOUND_QUEUE, "menuon play call",
            bytes_at(0x486737, 5).hex())
    chk.add(bytes_at(0x486717, 5) == bytes.fromhex("e80457f8ff") and
            call_target(0x486717) == SOUND_QUEUE, "selectm play call",
            bytes_at(0x486717, 5).hex())
    for site in (0x48CED5, 0x48F3F9, 0x48F65A, 0x48F692):
        chk.add(bytes_at(site, 5) == bytes.fromhex("6800674800"),
                f"selectm push {site:#x}", bytes_at(site, 5).hex())
    # selectm is dispatched from the timer callback slot 0x68f1a8
    chk.add(bytes_at(0x498BB9, 6) == bytes.fromhex("ff15a8f16800"),
            "selectm dispatch", bytes_at(0x498BB9, 6).hex())
    chk.add(bytes_at(0x498B42, 5) == bytes.fromhex("a3a8f16800"),
            "selectm store", bytes_at(0x498B42, 5).hex())
    # menuon is called through animation record +0x44
    chk.add(bytes_at(0x498A6C, 3) == bytes.fromhex("ff5644"),
            "menuon dispatch", bytes_at(0x498A6C, 3).hex())
    chk.add(bytes_at(0x4988C0, 3) == bytes.fromhex("895144"),
            "callback field", bytes_at(0x4988C0, 3).hex())


def check_loader(chk: Checks) -> None:
    for site in LOADER_PUSHES:
        chk.add(bytes_at(site, 1) == b"\x68", f"loader push {site:#x}",
                bytes_at(site, 5).hex())
    for site in LOADER_BUG_READS:
        got = bytes_at(site, 5)
        chk.add(got == b"\xa1" + struct.pack("<I", 0x4BC464),
                f"loader bug {site:#x}", got.hex())
    # both of those branches report VBACK
    chk.add(bytes_at(0x48F846, 5) == bytes.fromhex("6894de4b00"),
            "VBACK warning #1", bytes_at(0x48F846, 5).hex())
    chk.add(bytes_at(0x48F86F, 5) == bytes.fromhex("6894de4b00"),
            "VBACK warning #2", bytes_at(0x48F86F, 5).hex())
    for site in LOADER_CALLS:
        chk.add(call_target(site) == UI_SOUND_LOADER, f"loader call {site:#x}",
                bytes_at(site, 5).hex())
    # sound init chain: sounds.sub, then the UI samples
    chk.add(call_target(0x44B0A5) == SUB_TABLE_LOAD, "sub load call",
            bytes_at(0x44B0A5, 5).hex())
    chk.add(cstr_at(0x4ABB24) == "sounds\\sounds.sub", "sub path",
            cstr_at(0x4ABB24))
    chk.add(cstr_at(SUB_COUNT_FMT) == "%ld", "sub count format",
            cstr_at(SUB_COUNT_FMT))
    chk.add(cstr_at(SUB_LINE_FMT) == "%ld %s %s", "sub line format",
            cstr_at(SUB_LINE_FMT))
    chk.add(bytes_at(0x40B517, 2) == b"\x33\xff" and
            bytes_at(0x40B51B, 6) == b"\x89\x3d" + struct.pack("<I", SUB_COUNT),
            "sub count init", bytes_at(0x40B51B, 6).hex())


def check_input(chk: Checks) -> None:
    # sound gate and the two event branches
    chk.add(bytes_at(0x48E5FA, 9) == bytes.fromhex("a184a34a0085c07456"),
            "sound gate", bytes_at(0x48E5FA, 9).hex())
    chk.add(bytes_at(0x48E607, 3) == bytes.fromhex("83f801"),
            "event 1 branch", bytes_at(0x48E607, 3).hex())
    chk.add(bytes_at(0x48E634, 3) == bytes.fromhex("83f802"),
            "event 2 branch", bytes_at(0x48E634, 3).hex())
    chk.add(bytes_at(0x48B4D7, 2) == bytes.fromhex("33ff") and
            bytes_at(0x48B4E1, 3) == bytes.fromhex("897c24"),
            "event initialised to 0", bytes_at(0x48B4D7, 2).hex())
    # Esc leaves the event alone -> silence
    esc = writes_local8(0x48E4CA, 0x48E5AD)
    chk.add(not esc, "Esc writes no event", ", ".join(f"{va:#x}" for va in esc))
    # each map row: the anchor bytes are really there
    for screen, action, sound, anchor, pat, _proof in MAP_ROWS:
        want = bytes.fromhex(pat)
        got = bytes_at(anchor, len(want))
        chk.add(got == want, f"map {sound} {anchor:#x}",
                f"{screen}/{action}: {got.hex()}")


def check_sub_table(chk: Checks, game: Path) -> None:
    pairs = sub_pairs(game)
    chk.add(len(pairs) == SUB_PAIRS, "sounds.sub pairs", f"{len(pairs)} пар")
    raw = (game / "sounds" / "sounds.sub").read_bytes()
    chk.add(raw.startswith(b"alarmk\talarmkbig\r\n"), "sounds.sub grammar",
            repr(raw[:20]))
    chk.add(SUB_GREEDY in pairs, "selectm -> select", str(SUB_GREEDY))
    got = sub_pairs_by_code_grammar(game)
    chk.add(got == 0, "code grammar reads 0 records",
            f"{got} записей (формат exe: %ld, затем %ld %s %s)")
    chk.add(len(pairs) + 11 > SUB_MAX, "161 > 150",
            f"{len(pairs)} пар при лимите {SUB_MAX}")


def check_samples_data(chk: Checks, game: Path) -> None:
    idx = game / "sounds" / "SOU.IDX"
    recs = bigfile_index(idx)
    if not recs:
        chk.add(False, "sounds/SOU.IDX present", str(idx))
        return
    chk.add(len(recs) == 999, "SOU.IDX records", f"{len(recs)}")
    names = [n.upper() for n, _ in recs]
    for name, want in SAMPLES.items():
        rec = sou_record(game, name)
        if rec is None:
            chk.add(False, f"sample {name}", "нет в SOU.IDX")
            continue
        facts, _ = rec
        if want["idx"] is not None:
            chk.add(names.index(name) == want["idx"], f"idx {name}",
                    f"{names.index(name)}")
        chk.add(facts["magic"] == "SOU1", f"codec {name}", facts["magic"])
        chk.add(facts["rate"] == want["rate"], f"rate {name}",
                f"{facts['rate']}")
        chk.add(facts["channels"] == want["ch"], f"channels {name}",
                f"{facts['channels']}")
        chk.add(facts["frames"] == want["frames"], f"frames {name}",
                f"{facts['frames']}")
        chk.add(facts["payload_bytes"] == want["payload"], f"payload {name}",
                f"{facts['payload_bytes']} (+{facts['dropped']} dropped)")
        if want["fnv"]:
            got = "%016x" % fnv1a64(facts["payload"])
            chk.add(got == want["fnv"], f"fnv {name}", got)


def check_manifest(chk: Checks, game: Path) -> None:
    """Cross-check the decode against docs/manifests/sou1-fnv.json."""
    if not MANIFEST.is_file():
        chk.add(False, "manifest present", str(MANIFEST))
        return
    try:
        import gm.sou1 as sou1  # noqa: WPS433 - optional, needs the repo
    except Exception as exc:  # pragma: no cover
        chk.add(False, "gm.sou1 import", repr(exc))
        return
    try:
        entries = {e["name"].upper(): e
                   for e in json.loads(MANIFEST.read_text())["entries"]}
    except Exception as exc:  # pragma: no cover
        chk.add(False, "manifest parse", repr(exc))
        return
    import array
    for name in ("MENUON", "SELECT", "SELECTM", "UPDOWN", "VBACK", "VFRONT",
                 "VLEFT", "INTRO"):
        rec = sou_record(game, name)
        if rec is None:
            chk.add(False, f"manifest {name}", "нет записи")
            continue
        _facts, raw = rec
        try:
            rate, channels, streams = sou1.decode_entry(raw)
        except Exception as exc:
            chk.add(False, f"decode {name}", repr(exc))
            continue
        if channels == 2:
            inter = array.array("I")
            for i in range(len(streams[0])):
                inter.append(streams[0][i])
                inter.append(streams[1][i])
        else:
            inter = streams[0]
        got = "%016x" % fnv1a64(inter.tobytes())
        want = entries.get(name, {}).get("fnv1a64")
        chk.add(got == want, f"manifest fnv {name}", f"{got} vs {want}")
        chk.add(entries.get(name, {}).get("rate") == rate,
                f"manifest rate {name}", f"{rate}")


def check_music(chk: Checks, game: Path) -> None:
    sites = call_sites(PLAY_MUSIC)
    chk.add(sorted(sites) == sorted(PLAY_MUSIC_SITES), "play_music sites",
            ", ".join(f"{va:#x}" for va in sites))
    chk.add(cstr_at(MUSIC_FORMAT_OGG).endswith("%s.sog"), "sog format",
            cstr_at(MUSIC_FORMAT_OGG))
    chk.add(cstr_at(0x4BE444) == "intro", "title sample name",
            cstr_at(0x4BE444))
    chk.add(bytes_at(0x490D59, 5) == b"\xa1" + struct.pack("<I", 0x68DE20),
            "title sample read", bytes_at(0x490D59, 5).hex())
    chk.add(call_target(0x490D70) == SOUND_QUEUE, "title sample play",
            bytes_at(0x490D70, 5).hex())
    chk.add(bytes_at(0x490D5E, 5) == bytes.fromhex("68e8030000"),
            "title sample args", bytes_at(0x490D5E, 5).hex())
    # film player: volume 100, then MUSIC/<film name>, then stop
    chk.add(bytes_at(0x4914C2, 2) == bytes.fromhex("6a64"), "film vol 100",
            bytes_at(0x4914C2, 2).hex())
    chk.add(call_target(0x4914C8) == MUSIC_VOL_SET, "film vol call",
            bytes_at(0x4914C8, 5).hex())
    chk.add(call_target(0x4914D4) == PLAY_MUSIC, "film music call",
            bytes_at(0x4914D4, 5).hex())
    chk.add(call_target(0x491463) == SBV_DECODER, "film sbv call",
            bytes_at(0x491463, 5).hex())
    # credits: winner.sbv via the film player
    chk.add(cstr_at(0x4BCB08) == "winner", "credits film name",
            cstr_at(0x4BCB08))
    chk.add(call_target(0x483476) == FILM_PLAYER, "credits film call",
            bytes_at(0x483476, 5).hex())
    # volume plumbing
    chk.add(call_target(0x40B88B) == 0x40A9E0, "music vol setter",
            bytes_at(0x40B88B, 5).hex())
    chk.add(bytes_at(0x40B92A, 6) == b"\x89\x15" + struct.pack("<I", EFF_VOLUME),
            "effects volume store", bytes_at(0x40B92A, 6).hex())
    # installation: no menu bank, but Intro/winner playlists exist
    banks = music_banks(game)
    if banks:
        chk.add(not any(b.lower() in ("menu", "title", "frontend", "main")
                        for b in banks), "no menu music bank",
                f"{len(banks)} банков: {', '.join(banks[:3])} …")
        chk.add(sog_lines(game, "Intro") == ["Opening"], "Intro.sog",
                str(sog_lines(game, "Intro")))
        chk.add(sog_lines(game, "winner") == ["winner"], "winner.sog",
                str(sog_lines(game, "winner")))
        chk.add(len([b for b in banks if b.upper().startswith(("L", "DM"))]) >= 500,
                "level/dm banks", f"{len(banks)} банков")


def self_test(game: Path | None) -> int:
    chk = Checks()
    check_samples_exe(chk)
    check_play_sites(chk)
    check_callbacks(chk)
    check_loader(chk)
    check_input(chk)
    check_music(chk, game if game is not None else Path("/nonexistent"))
    if game is not None and (game / "sounds" / "SOU.IDX").is_file():
        check_sub_table(chk, game)
        check_samples_data(chk, game)
        check_manifest(chk, game)
    else:
        chk.add(True, "game data skipped", "установка не найдена — только exe")
    bad = chk.failed
    for item in bad:
        print(f"FAIL {item['name']}: {item['detail']}")
    print(f"self-test: {len(chk.items) - len(bad)}/{len(chk.items)} checks passed")
    return 0 if not bad else 1


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------

def build_parser() -> argparse.ArgumentParser:
    ap = argparse.ArgumentParser(
        description="UI sounds of the Gunmetal front end (samples, triggers, music)")
    ap.add_argument("--sounds", action="store_true", help="таблица UI-сэмплов")
    ap.add_argument("--map", action="store_true", help="экран/действие -> звук")
    ap.add_argument("--music", action="store_true", help="музыка фронтенда")
    ap.add_argument("--json", action="store_true", help="полный отчёт в JSON")
    ap.add_argument("--self-test", action="store_true", help="проверки по бинарю и файлам")
    ap.add_argument("--game", type=Path, default=DEFAULT_GAME,
                    help=f"каталог установки (по умолчанию {DEFAULT_GAME})")
    return ap


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    game = args.game if args.game.is_dir() else None
    if args.self_test:
        return self_test(game)
    if args.json:
        rep = dict(
            sounds=sound_rows(game),
            map=[dict(screen=s, action=a, sound=snd, anchor=f"{va:#x}",
                      evidence=p) for s, a, snd, va, _pat, p in MAP_ROWS],
            music=music_report(game),
            vleft_reads=[f"{va:#x}" for va in VLEFT_READS],
            handle_block=[f"{HANDLE_BLOCK[0]:#x}", f"{HANDLE_BLOCK[1]:#x}"],
            sub=dict(pairs=len(sub_pairs(game)) if game else None,
                     code_records=sub_pairs_by_code_grammar(game) if game else None,
                     greedy=list(SUB_GREEDY)),
            gate=f"{SOUND_GATE:#x}", queue=f"{SOUND_QUEUE:#x}",
            loader=f"{UI_SOUND_LOADER:#x}", input=f"{FRONTEND_INPUT:#x}",
        )
        print(json.dumps(rep, ensure_ascii=False, indent=1))
        return 0
    explicit = args.sounds or args.map or args.music
    if args.sounds or not explicit:
        rows = sound_rows(game)
        if rows:
            print_sounds(rows)
            print()
    if args.map or not explicit:
        print_map()
        print()
    if args.music:
        print_music(music_report(game))
        print()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
