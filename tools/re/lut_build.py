#!/usr/bin/env python3
"""Rebuild the HUD fade LUT ``0x682f90`` (16 x 256) from the engine code.

The cockpit band is blitted through 16 tables of 256 bytes at ``0x682f90``
(``0x491f10``..``0x491f2d`` passes ``&DAT_00682f90 + k*0x100`` as the source
palette of ``0xa1b510``), so an index of ``PANEL`` becomes an index of the
screen palette.  The tables are **not shipped**: they are built at start-up by
``0x44b22b`` -> ``0x48f970``, which loops ``k = 0..15`` and calls
``0x483290(dst = 0x682f90 + k*0x100, double(k) * (1/11), 1)``.

This tool rebuilds the 4096 bytes in Python and compares them with the dump
taken from the live game (``tools/win/dump_lut.lua`` ->
``~/.cache/gunmetal-re/hud_lut.bin``).  The result is byte-exact:

    identical bytes=4096/4096, FNV-1a64 0x6330406254771f3d

The three pieces of the builder (all read from the objdump listing, no Ghidra):

* ``0x483290`` - one table.  For every palette slot ``i`` it either copies
  ``i`` (``0x483250`` says "leave this slot alone") or takes the slot colour
  out of the **6-bit** DAC palette at ``0x83d320``, multiplies each component
  by the factor and truncates (``0x40e0e0`` = ``__ftol`` with the x87 control
  word set to RC=11), then asks ``0x4228e0`` for the nearest palette slot.
* ``0x483250`` - the skip list: ``{173,174,175,191} U {240..255}`` = 20 slots
  (the ``NEONS.PIN`` slots, i.e. the ones outside the game ramps).  They keep
  their own index, which is why level 0 leaves exactly 20 entries untouched.
* ``0x4228e0`` - weighted nearest-colour search over slots ``1..254``
  (start=1, end=255 exclusive):

      d(i) = (pr + r + 0x100) * (pr - r)^2
           + 512             * (pg - g)^2
           + (0x17f - pr - r) * (pb - b)^2

  with 6-bit components.  Strict ``<`` keeps the **lowest** index on a tie.
  The same metric is documented for ``LOOK16.TAB`` (docs/LOOK16.md 1), which
  is the independent confirmation that the constants belong to 6-bit space.

Why 6-bit matters: the constants ``0x100``/``0x17f`` only make sense for
``0..63``.  With 8-bit inputs ``0x17f - (pr+r)`` goes negative and the
"nearest" search stops being a distance; the dump then disagrees on 2834 of
4096 bytes.  The BMP loader converts the palette on the way in
(``0x47f0b4/0x47f0bd/0x47f0c6: shr dl,2`` while copying R,G,B into
``0x83d320``), and consumers clamp to ``0x3f`` and shift back
(``0x418769: cmp edx,0x3f``, ``0x41878b: shl edx,2``), so the runtime palette
really is 6-bit.

Usage::

    python3 tools/re/lut_build.py                  # dump: size + FNV-1a64
    python3 tools/re/lut_build.py --build          # build and compare
    python3 tools/re/lut_build.py --build --json   # same, machine readable
    python3 tools/re/lut_build.py --emit OUT       # write the built 4096 bytes
    python3 tools/re/lut_build.py --explain        # formula, addresses, evidence
    python3 tools/re/lut_build.py --ablate          # what each detail of the code is worth
    python3 tools/re/lut_build.py --self-test      # properties, dump not needed

Exit codes: 0 = ok (and the build matched, if asked), 1 = the build ran but
differs from the dump / a self-test failed, 2 = refused (dump is not 4096
bytes, ``--emit`` points into the repository or the game installation, the
palette is unavailable).

Nothing is written except by ``--emit``, and the game installation is only
read.  The palette comes from ``FRONTEND/GAMEPAL`` through ``gm/palette.py``.
"""

from __future__ import annotations

import argparse
import json
import os
import struct
import sys
import tempfile

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))

#: Repository root (``tools/re/lut_build.py`` -> ``../..``), for the ``--emit`` guard.
REPO_ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

#: Reference installation; only read, and ``--emit`` refuses to write into it.
DEFAULT_GAME = "/mnt/d/games/Gunmetal 3dfx"

#: Dump of the live tables (``tools/win/dump_lut.lua``, 16*256 bytes).
DEFAULT_DUMP = os.path.expanduser("~/.cache/gunmetal-re/hud_lut.bin")

LEVELS = 16                 # k = 0..15 (0x48f9ef..0x48fa1f)
ENTRIES = 256               # one byte per palette slot
BYTES = LEVELS * ENTRIES    # 4096
IDENTITY_LEVEL = 11         # k/11 == 1
SCALE = 11.0                # 0x483290 is called with double(k) * (1/11)

#: ``0x483250`` returns "leave this slot alone" for exactly these 20 slots;
#: they are the ``NEONS.PIN`` set (docs/LOOK16.md 1).
SKIP_SLOTS = (173, 174, 175, 191) + tuple(range(240, 256))

#: Metric constants of ``0x4228e0``; 6-bit space (docs/LOOK16.md 1).
METRIC_C0 = 0x100
METRIC_G = 512
METRIC_C1 = 0x17F

#: ``0x4a2978`` = the double constant the builder multiplies ``k`` by.
FACTOR_BITS = bytes.fromhex("46175d74d145b73f")   # == 1.0/11.0 as IEEE-754 double

#: FNV-1a 64 of the reference dump (docs/HUD_LUT.md 2, engine test hud_lut).
DUMP_FNV = 0x6330406254771F3D

#: Addresses, for ``--explain`` and for the doc: (VA, what it is).
ADDRESSES = (
    (0x491F10, "band blit: pushes &0x682f90 + k*0x100 as the source palette"),
    (0x491F2D, "same blit: pushes the panel source 0x678188 (320x78)"),
    (0x44B22B, "start-up init calls the LUT builder (0x48f970)"),
    (0x48F970, "builds all 16 tables into 0x682f90..0x683f8f"),
    (0x48F9EF, "loop: fild k, fmul 1/11, call 0x483290(dst, factor, 1)"),
    (0x4A2978, "the double 1/11 (0x3FB745D1745D1746)"),
    (0x483290, "one table: 256 slots -> 256 indices"),
    (0x483250, "skip predicate: 173..175, 191, 240..255 keep their index"),
    (0x4228E0, "weighted nearest-colour search over slots 1..254"),
    (0x40E0E0, "__ftol with RC=11: truncation toward zero"),
    (0x83D320, "6-bit DAC palette (R,G,B), 256*3 bytes, from FRONTEND/GAMEPAL"),
    (0x44AF88, "palette load: 0x47efe0('frontend','gamepal',0x83d320,...)"),
    (0x47F0B4, "palette conversion to 6-bit: shr dl,2 while copying R,G,B"),
    (0x418769, "consumer: clamps a palette byte to 0x3f, then shl 2 (6-bit)"),
    (0x682F90, "the tables: 16 x 256 bytes in RAM"),
)


class LutBuildError(Exception):
    """Refusal with a message for ``[error]`` (exit code 2)."""


# --------------------------------------------------------------------------- #
# hashing / dump
# --------------------------------------------------------------------------- #


def fnv1a64(data: bytes) -> int:
    """FNV-1a 64 of *data* (offset basis 0xcbf29ce484222325, prime 0x100000001b3)."""
    h = 0xCBF29CE484222325
    for byte in data:
        h ^= byte
        h = (h * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return h


def read_dump(path: str) -> bytes:
    """Read the dump; refuse anything but exactly ``BYTES`` bytes.

    The tables are positional (``levels[k] = bytes[k*256..]``), so a short
    file would silently shift every level and a long one would hide a second
    payload.  The error text names the path and the size that was found.
    """
    try:
        with open(path, "rb") as handle:
            data = handle.read()
    except OSError as exc:
        raise LutBuildError(f"cannot read '{path}': {exc.strerror or exc}") from exc
    if len(data) != BYTES:
        raise LutBuildError(
            f"'{path}' is {len(data)} bytes, expected {BYTES} (16 x 256)")
    return data


# --------------------------------------------------------------------------- #
# palette
# --------------------------------------------------------------------------- #


def load_palette(game: str) -> bytes:
    """The engine palette as 256 RGBA quads (``FRONTEND/GAMEPAL``, 1024 bytes)."""
    try:
        from gm import palette  # noqa: PLC0415 - optional: --explain/--self-test
    except Exception as exc:  # pragma: no cover - broken checkout only
        raise LutBuildError(f"cannot import gm.palette: {exc}") from exc
    try:
        data = palette.installed(game)
    except (OSError, ValueError, KeyError) as exc:
        raise LutBuildError(f"cannot read FRONTEND/GAMEPAL from '{game}': {exc}") from exc
    if not data or len(data) != ENTRIES * 4:
        raise LutBuildError(
            f"cannot read FRONTEND/GAMEPAL from '{game}' "
            f"({len(data) if data else 0} bytes, expected {ENTRIES * 4})")
    return bytes(data)


def slots_from_palette(palette: bytes) -> list[tuple[int, int, int]]:
    """RGBA quads -> the 6-bit ``(r, g, b)`` slots the engine compares.

    The engine's buffer at ``0x83d320`` is **6-bit**: the BMP loader shifts
    every component right by 2 (``0x47f0b4``) and consumers clamp to ``0x3f``.
    This is not cosmetic: the metric constants only work for ``0..63``.
    """
    return [(palette[i * 4] >> 2, palette[i * 4 + 1] >> 2, palette[i * 4 + 2] >> 2)
            for i in range(ENTRIES)]


# --------------------------------------------------------------------------- #
# the builder (0x483290) and its helpers
# --------------------------------------------------------------------------- #


def skip_slot(index: int) -> bool:
    """``0x483250``: True for the 20 slots the builder copies unchanged."""
    return index in (173, 174, 175, 191) or 240 <= index <= 255


def metric(pr: int, pg: int, pb: int, r: int, g: int, b: int) -> int:
    """``0x4228e0`` weighted distance, 6-bit components.

    ``d = (pr+r+0x100)*(pr-r)^2 + 512*(pg-g)^2 + (0x17f-pr-r)*(pb-b)^2``

    All three coefficients stay positive for 6-bit slots and the targets the
    builder can produce (at most ``trunc(63*15/11) == 85``, so
    ``0x17f - (85+63) == 235 > 0``); with 8-bit inputs the third one goes
    negative and the "distance" is no longer a distance.
    """
    rsum = r + pr
    return ((rsum + METRIC_C0) * (pr - r) * (pr - r)
            + METRIC_G * (pg - g) * (pg - g)
            + (METRIC_C1 - rsum) * (pb - b) * (pb - b))


def nearest(slots, r: int, g: int, b: int, *,
            lo: int = 1, hi: int = ENTRIES - 1, strict: bool = True) -> int:
    """Index of the nearest slot in ``[lo, hi)`` (``0x483290`` pushes 1 and 255).

    Strict ``<`` on the running minimum, so equal distances keep the lowest
    index: the dump needs that rule (``<=`` loses 147 of 4096 bytes).
    """
    best = 0x7FFFFFFF
    best_index = 0
    for i in range(lo, hi):
        pr, pg, pb = slots[i]
        dist = metric(pr, pg, pb, r, g, b)
        if dist < best or (not strict and dist == best):
            best = dist
            best_index = i
    return best_index


def factor(level: int, scale: float = SCALE) -> float:
    """The double the builder passes: ``double(level) * (1/11)``.

    ``0x48f9ef``: ``fild k`` / ``fmul qword [0x4a2978]`` / ``fstp qword``.
    """
    return level * (1.0 / scale)


def scaled(value: int, level: int, scale: float = SCALE) -> int:
    """One component: ``trunc(double(value) * factor(level))``.

    ``0x4832d1`` calls ``0x40e0e0``, which sets the x87 rounding control to 11
    (truncate) before ``fistp``.  Values are non-negative, so this is floor.
    """
    return int(value * factor(level, scale))


def build_level(slots, level: int, *, scale: float = SCALE, rounding: str = "trunc",
                strict: bool = True, lo: int = 1, hi: int = ENTRIES - 1,
                use_skip: bool = True) -> bytes:
    """One 256-byte table: ``levels[k][i] = map(colour of slot i)``.

    The keyword arguments encode the code as it is; ``--ablate`` uses them to
    measure what breaks when one detail is changed, and nothing else does.
    """
    table = bytearray(ENTRIES)
    f = factor(level, scale)
    round_to = round if rounding == "round" else int
    for i in range(ENTRIES):
        if use_skip and skip_slot(i):
            table[i] = i           # 0x4832b4: dst[i] = i
            continue
        r, g, b = slots[i]
        table[i] = nearest(slots, round_to(r * f), round_to(g * f), round_to(b * f),
                           lo=lo, hi=hi, strict=strict)
    return bytes(table)


def build(slots) -> bytes:
    """All 16 tables, ``levels[k] = bytes[k*256..]`` - exactly 4096 bytes."""
    out = bytearray()
    for level in range(LEVELS):
        out += build_level(slots, level)
    return bytes(out)


def build_from_palette(palette: bytes) -> bytes:
    """Convenience: 1024-byte RGBA palette -> 4096-byte table set."""
    return build(slots_from_palette(palette))


# --------------------------------------------------------------------------- #
# comparison
# --------------------------------------------------------------------------- #


def compare(built: bytes, dump: bytes) -> dict:
    """Per-level byte comparison of two 4096-byte table sets."""
    if len(built) != BYTES or len(dump) != BYTES:
        raise LutBuildError(
            f"compare needs two {BYTES}-byte tables, got {len(built)} and {len(dump)}")
    levels = []
    first = None
    for k in range(LEVELS):
        mine = built[k * ENTRIES:(k + 1) * ENTRIES]
        theirs = dump[k * ENTRIES:(k + 1) * ENTRIES]
        same = sum(1 for a, b in zip(mine, theirs) if a == b)
        diff = next((i for i in range(ENTRIES) if mine[i] != theirs[i]), None)
        levels.append({"level": k, "same": same, "first_diff": diff})
        if first is None and diff is not None:
            first = {"level": k, "index": diff, "offset": k * ENTRIES + diff,
                     "built": mine[diff], "dump": theirs[diff]}
    identical = sum(item["same"] for item in levels)
    return {
        "identical": identical,
        "total": BYTES,
        "levels": levels,
        "first_diff": first,
        "fnv_built": fnv1a64(built),
        "fnv_dump": fnv1a64(dump),
    }


# --------------------------------------------------------------------------- #
# --emit guard
# --------------------------------------------------------------------------- #


def _under(path: str, root: str) -> bool:
    try:
        return os.path.commonpath([path, root]) == root
    except ValueError:              # different drives / relative mix
        return False


def _repo_roots() -> tuple[str, ...]:
    """Every repository this checkout belongs to, for the ``--emit`` guard.

    A git worktree has a ``.git`` *file* that points at
    ``<main>/.git/worktrees/<name>``, so the main checkout is guarded too:
    writing the tables into either tree would make ``git status`` dirty.
    """
    roots = [os.path.realpath(REPO_ROOT)]
    gitfile = os.path.join(REPO_ROOT, ".git")
    if os.path.isfile(gitfile):
        try:
            with open(gitfile, "r", encoding="utf-8") as handle:
                line = handle.readline().strip()
        except OSError:
            line = ""
        if line.startswith("gitdir:"):
            gitdir = os.path.realpath(os.path.join(REPO_ROOT, line[7:].strip()))
            marker = f"{os.sep}.git{os.sep}"
            if marker in gitdir:
                roots.append(gitdir.split(marker)[0])
    return tuple(dict.fromkeys(roots))


def emit_target(out: str) -> str:
    """Resolve ``--emit`` or refuse with :class:`LutBuildError`.

    Writing into the repository or into the game installation is refused: the
    tables are derived data, so an accidental ``git add`` or a file dropped
    next to ``gundx.exe`` must not be possible.  Anywhere else is allowed
    (``/tmp``, ``/var/tmp``, ``~/.cache`` are the usual places).
    """
    path = os.path.realpath(os.path.abspath(os.path.expanduser(out)))
    for repo in _repo_roots():
        if _under(path, repo):
            raise LutBuildError(f"refusing to write inside the repository: '{path}'")
    for game in (DEFAULT_GAME, os.path.expanduser("~/game")):
        if os.path.isdir(game) and _under(path, os.path.realpath(game)):
            raise LutBuildError(
                f"refusing to write inside the game installation: '{path}'")
    if os.path.isdir(path):
        raise LutBuildError(f"'{path}' is a directory, not a file")
    return path


# --------------------------------------------------------------------------- #
# --ablate: what each detail of the code is worth
# --------------------------------------------------------------------------- #

#: One detail changed at a time, everything else as in ``0x483290``.  The
#: numbers are what docs/LUT_BUILDER.md quotes, so they stay reproducible.
ABLATIONS = (
    ("code model (as in 0x483290)", {}),
    ("8-bit palette (no shr dl,2)", {"slots": "8bit"}),
    ("round() instead of truncation", {"rounding": "round"}),
    ("<= instead of < (last minimum)", {"strict": False}),
    ("candidates 0..255 (both ends)", {"lo": 0, "hi": ENTRIES}),
    ("no skip list (0x483250 ignored)", {"use_skip": False}),
    ("factor k/15 instead of k/11", {"scale": 15.0}),
    ("factor k/16 instead of k/11", {"scale": 16.0}),
)


def ablate(dump_path: str, game: str) -> int:
    """Compare the code model and seven wrong variants against the dump."""
    palette_bytes = load_palette(game)
    slots6 = slots_from_palette(palette_bytes)
    slots8 = [(palette_bytes[i * 4], palette_bytes[i * 4 + 1], palette_bytes[i * 4 + 2])
              for i in range(ENTRIES)]
    dump = read_dump(dump_path)
    print(f"dump: {dump_path} ({len(dump)} bytes)")
    print(f"{'variant':<34} {'identical':>10}  per level (min..max)")
    for label, options in ABLATIONS:
        options = dict(options)
        slots = slots8 if options.pop("slots", None) == "8bit" else slots6
        built = b"".join(build_level(slots, k, **options) for k in range(LEVELS))
        report = compare(built, dump)
        per = [item["same"] for item in report["levels"]]
        print(f"{label:<34} {report['identical']:>6}/4096  {min(per)}..{max(per)}")
    return 0


# --------------------------------------------------------------------------- #
# --explain
# --------------------------------------------------------------------------- #

FORMULA_TEXT = f"""\
Formula (6-bit components, pr/pg/pb from 0x83d320, r/g/b = trunc(slot*k/11)):

  d(i) = (pr + r + 0x{METRIC_C0:03x}) * (pr - r)^2
       + {METRIC_G:<4d}             * (pg - g)^2
       + (0x{METRIC_C1:03x} - pr - r) * (pb - b)^2

  0x483290(dst, k/11, 1):
    for i in 0..255:
      if 0x483250(i): dst[i] = i                  # 20 slots: 173..175, 191, 240..255
      else:
        r, g, b = trunc(slot6[i] * k/11)          # __ftol, RC=11 -> truncation
        dst[i]  = argmin over j in 1..254 of d(j)  # strict < : lowest index wins
        (no shift by 2: the palette buffer itself is 6-bit)
"""

PROVEN_TEXT = """\
Proven by code (disassembly) and by the dump:
  * 16 tables of 256 bytes at 0x682f90, built by 0x48f970 -> 0x483290
    (0x48f9ef loop: esi = 0x682f90..0x683f90 step 0x100, k = 0..15).
  * The factor is double(k) * (1/11), the double 0x3FB745D1745D1746 at
    0x4a2978; k = 11 therefore is the identity table.
  * Components are truncated, not rounded (0x40e0e0 writes RC=11 into the
    x87 control word, then fistp).
  * The palette read is the 6-bit DAC table at 0x83d320: the loader shifts
    every component right by 2 (0x47f0b4/0x47f0bd/0x47f0c6) and consumers
    clamp to 0x3f (0x418769) before shifting back (0x41878b).
  * 0x483250 is the skip predicate; it returns 1 for exactly
    {173,174,175,191} U {240..255} = 20 slots, so level 0 leaves exactly
    those 20 entries untouched and the black slot 1 is 236/256.
  * 0x4228e0 is a weighted nearest-colour search over 1..254 (the caller
    pushes 1 and 0xff); its metric is the LOOK16.TAB one, independently
    documented in docs/LOOK16.md 1.
  * All 4096 bytes of the shipped dump are reproduced (FNV-1a64
    0x6330406254771f3d).  Levels 12..15 (factor > 1) fall out of the same
    code without a special case, which is why they brighten instead of
    aliasing level 11.

Hypothesis (not proven by this tool):
  * Why exactly those 20 slots are skipped: they are the NEONS.PIN list,
    i.e. the slots outside the game ramps (docs/LOOK16.md 1, docs/UPSCALE.md
    3.2).  The list itself is code; the intent is inferred.
  * The x87 multiply is extended precision; Python's double multiply happens
    to give the same truncations for every (level, component) pair here, so
    the two are numerically indistinguishable on this data.
"""


def explain(dump_path: str, game: str) -> int:
    """Print addresses, formula and what is proven vs hypothesis."""
    print("lut_build: the HUD fade tables 0x682f90 (M8.5)")
    print()
    print("Addresses (VA, gundx.exe):")
    for va, what in ADDRESSES:
        print(f"  {va:#08x}  {what}")
    print()
    print(FORMULA_TEXT)
    print(PROVEN_TEXT)

    print("Levels 0/11/12..15 (the dump, or the model when the dump is missing):")
    try:
        palette_bytes = load_palette(game)
    except LutBuildError as exc:
        print(f"  palette n/a: {exc}")
        return 1
    slots = slots_from_palette(palette_bytes)
    data = build(slots)
    source = "model"
    try:
        data = read_dump(dump_path)
        source = f"dump {os.path.basename(dump_path)}"
    except LutBuildError as exc:
        print(f"  [warn] {exc}; showing the model")
    print(f"  source: {source}, FNV-1a64 {fnv1a64(data):#018x}")
    print("  level  identity  colour-id  white  min  max  distinct")
    for k in range(LEVELS):
        table = data[k * ENTRIES:(k + 1) * ENTRIES]
        ident = sum(1 for i in range(ENTRIES) if table[i] == i)
        colour = sum(1 for i in range(ENTRIES) if slots[table[i]] == slots[i])
        print(f"  {k:5d}  {ident:8d}  {colour:9d}  {table.count(255):5d}"
              f"  {min(table):3d}  {max(table):3d}  {len(set(table)):8d}")
    print()
    print("  level 0  -> 236/256 entries are the black slot 1, 20 skipped slots kept")
    print("  level 11 -> 254/256 identical indices (0->1 and 65->48 are colour")
    print("              duplicates), 256/256 identical colours")
    print("  level 12..15 -> factor k/11 > 1: 130/71/46/43 identical indices")
    return 0


# --------------------------------------------------------------------------- #
# --self-test (no dump: properties of the builder itself)
# --------------------------------------------------------------------------- #


def _synthetic_slots() -> list[tuple[int, int, int]]:
    """256 unique 6-bit slots; slots 0 and 1 are both black, the rest are not.

    ``(r, g)`` determines the index (``r = i % 32 + 1``, ``g = i // 32 + 1``),
    so the colours are unique and no entry can shadow another - which is what
    the identity and tie checks need.  Slot 1 is black so level 0 has a known
    target.
    """
    out = [(0, 0, 0), (0, 0, 0)]
    for i in range(2, ENTRIES):
        out.append((i % 32 + 1, i // 32 + 1, (i * 37) % 63 + 1))
    return out


def _tie_slots() -> list[tuple[int, int, int]]:
    """Synthetic slots with slot 20 repeating slot 10's colour (tie check)."""
    slots = list(_synthetic_slots())
    slots[20] = slots[10]
    return slots


def self_test(game: str = DEFAULT_GAME) -> int:
    """Properties of the model that do not need the dump.

    Returns 0 when every check passed (``--self-test`` exit code).
    """
    checks: list[tuple[bool, str]] = []

    def check(ok: bool, text: str) -> None:
        checks.append((bool(ok), text))

    slots = _synthetic_slots()
    tie_slots = _tie_slots()
    table = build(slots)

    # --- shape ------------------------------------------------------------
    check(len(slots) == ENTRIES, "synthetic palette has 256 slots")
    check(all(0 <= c <= 63 for slot in slots for c in slot),
          "synthetic slots are 6-bit (0..63)")
    check(len(set(slots[2:])) == ENTRIES - 2, "synthetic slots 2..255 are unique")
    check(len(table) == BYTES, f"build returns {BYTES} bytes (got {len(table)})")
    check(len(table) % ENTRIES == 0 and len(table) // ENTRIES == LEVELS,
          f"build is {LEVELS} levels of {ENTRIES} entries")
    check(table == build(slots), "build is deterministic")
    check(LEVELS == 16 and ENTRIES == 256 and IDENTITY_LEVEL == 11,
          "shape constants: 16 levels, 256 entries, identity level 11")

    # --- the skip list (0x483250) ----------------------------------------
    check(len(SKIP_SLOTS) == 20, "skip list has exactly 20 slots")
    check(set(SKIP_SLOTS) == {173, 174, 175, 191} | set(range(240, 256)),
          "skip list is {173,174,175,191} U {240..255}")
    check(all(skip_slot(i) for i in (173, 174, 175, 191, 240, 255)),
          "skip_slot() true for 173/174/175/191/240/255")
    check(not any(skip_slot(i) for i in (0, 1, 172, 176, 190, 192, 239)),
          "skip_slot() false for 0/1/172/176/190/192/239")

    # --- invariants of every level ---------------------------------------
    white_ok = all(table[k * ENTRIES + 255] == 255 for k in range(LEVELS))
    check(white_ok, "every level maps slot 255 to 255 (all 16 tables)")
    skip_ok = all(table[k * ENTRIES + i] == i
                  for k in range(LEVELS) for i in SKIP_SLOTS)
    check(skip_ok, "every level leaves all 20 skipped slots unchanged")
    check(all(min(table[k * ENTRIES:(k + 1) * ENTRIES]) >= 1
              for k in range(LEVELS)),
          "no level ever produces index 0 (search starts at 1)")

    # --- level 0 / level 11 / 12..15 -------------------------------------
    zero = table[0:ENTRIES]
    check(all(zero[i] == 1 for i in range(ENTRIES) if not skip_slot(i)),
          "level 0 sends every non-skipped slot to black slot 1")
    eleven = table[IDENTITY_LEVEL * ENTRIES:(IDENTITY_LEVEL + 1) * ENTRIES]
    check(all(eleven[i] == i for i in range(1, ENTRIES - 1)),
          "level 11 is the identity on slots 1..254")
    check(eleven[0] != 0, "level 11 does not map slot 0 to 0 (0 is not a candidate)")
    check(all(table[11 * ENTRIES + i] == i for i in SKIP_SLOTS),
          "level 11 keeps the skipped slots too")
    for k in (12, 13, 14, 15):
        other = table[k * ENTRIES:(k + 1) * ENTRIES]
        check(sum(1 for i in range(ENTRIES) if other[i] != eleven[i]) > 0,
              f"level {k} is not an alias of level 11")
    check(all(eleven[i] == i for i in range(1, 255)) and
          table[15 * ENTRIES:16 * ENTRIES] != eleven,
          "levels 12..15 brighten (factor k/11 > 1), they do not clamp to 11")

    # --- ties: strict < keeps the lowest index ---------------------------
    tie = build_level(tie_slots, IDENTITY_LEVEL)
    check(tie[10] == 10 and tie[20] == 10,
          "equal colours resolve to the lowest index (strict <)")

    # --- the metric -------------------------------------------------------
    check(metric(10, 20, 30, 10, 20, 30) == 0, "metric(x, x) == 0")
    check(metric(10, 20, 30, 5, 6, 7) == metric(5, 6, 7, 10, 20, 30),
          "metric is symmetric")
    expected = ((10 + 5 + METRIC_C0) * 25 + METRIC_G * 196
                + (METRIC_C1 - 15) * 529)
    check(metric(10, 20, 30, 5, 6, 7) == expected,
          "metric matches the documented 0x4228e0 formula on a sample")
    check(METRIC_C0 == 0x100 and METRIC_G == 512 and METRIC_C1 == 0x17F,
          "metric constants are 0x100 / 512 / 0x17f (6-bit space)")
    check(0x17F - (scaled(63, LEVELS - 1) + 63) == 235,
          "third coefficient stays positive: 0x17f - (85 + 63) == 235")

    # --- factor and truncation -------------------------------------------
    check(struct.pack("<d", 1.0 / SCALE) == FACTOR_BITS,
          "1/11 is the double at 0x4a2978 (0x3FB745D1745D1746)")
    check(scaled(63, 11) == 63, "level 11: trunc(63 * 1.0000000000000002) == 63")
    check(scaled(63, 15) == 85, "level 15: trunc(63 * 15/11) == 85 (not round 86)")
    check(scaled(0, 15) == 0, "level 15: trunc(0 * 15/11) == 0")
    check(all(scaled(63, k) <= scaled(63, k + 1) for k in range(LEVELS - 1)),
          "the component target grows with the level")

    # --- dump reader refusals (temp files, no real dump) -----------------
    tmp = tempfile.mkdtemp(prefix="gm_lut_build_")
    try:
        for size, label in ((0, "an empty file"), (BYTES - 1, "a 4095-byte file"),
                            (BYTES + 1, "a 4097-byte file")):
            path = os.path.join(tmp, f"bad_{size}.bin")
            with open(path, "wb") as handle:
                handle.write(b"\0" * size)
            try:
                read_dump(path)
                ok = False
            except LutBuildError:
                ok = True
            check(ok, f"read_dump refuses {label}")
        good = os.path.join(tmp, "good.bin")
        with open(good, "wb") as handle:
            handle.write(b"\0" * BYTES)
        check(read_dump(good) == b"\0" * BYTES, "read_dump accepts exactly 4096 bytes")
    finally:
        for name in os.listdir(tmp):
            os.unlink(os.path.join(tmp, name))
        os.rmdir(tmp)

    # --- --emit guard -----------------------------------------------------
    for target, label in (
            (os.path.join(REPO_ROOT, "hud_lut.bin"), "repository"),
            (os.path.join(DEFAULT_GAME, "hud_lut.bin"), "game installation")):
        try:
            emit_target(target)
            ok = False
        except LutBuildError:
            ok = True
        check(ok, f"--emit refuses the {label}")
    check(emit_target(os.path.join(tempfile.gettempdir(), "hud_lut.bin")).endswith(
        "hud_lut.bin"), "--emit accepts /tmp")
    check(emit_target(os.path.expanduser("~/.cache/gunmetal-re/hud_lut.bin"))
          .startswith(os.path.expanduser("~/.cache")), "--emit accepts ~/.cache")

    # --- hashing ----------------------------------------------------------
    check(fnv1a64(b"") == 0xCBF29CE484222325, "FNV-1a64 of empty input")
    check(fnv1a64(b"a") == 0xAF63DC4C8601EC8C, "FNV-1a64 of 'a'")
    check(DUMP_FNV == 0x6330406254771F3D, "reference dump FNV constant is pinned")

    # --- the installed palette (optional: skipped without the game) ------
    try:
        palette_bytes = load_palette(game)
    except LutBuildError as exc:
        print(f"self-test: real palette skipped ({exc})")
    else:
        real = slots_from_palette(palette_bytes)
        check(len(palette_bytes) == 1024, "installed palette is 1024 bytes (256 RGBA)")
        check(all(0 <= c <= 63 for slot in real for c in slot),
              "installed palette is 6-bit after the >>2 the loader does")
        real_table = build(real)
        check(len(real_table) == BYTES, "real build returns 4096 bytes")
        level0 = real_table[0:ENTRIES]
        check(sum(1 for b in level0 if b == 1) == 236,
              "real level 0: 236/256 entries are the black slot 1")
        check(sum(1 for i in SKIP_SLOTS if level0[i] == i) == 20,
              "real level 0: all 20 skipped slots kept")
        level11 = real_table[11 * ENTRIES:12 * ENTRIES]
        check(sum(1 for i in range(ENTRIES) if level11[i] == i) == 254,
              "real level 11: 254/256 identical indices (0->1, 65->48)")
        check(level11[0] == 1 and real[0] == real[1],
              "real level 11: slot 0 -> 1 (both black)")
        check(level11[65] == 48 and real[65] == real[48],
              "real level 11: slot 65 -> 48 (same 6-bit colour)")
        check(sum(1 for i in range(ENTRIES) if real[level11[i]] == real[i]) == 256,
              "real level 11: 256/256 identical colours")
        for k, want in ((12, 130), (13, 71), (14, 46), (15, 43)):
            table_k = real_table[k * ENTRIES:(k + 1) * ENTRIES]
            got = sum(1 for i in range(ENTRIES) if table_k[i] == i)
            check(got == want, f"real level {k}: {want}/256 identical indices (got {got})")
        check(len(set(real)) < ENTRIES,
              "installed palette has colour duplicates (0/1, 65/48, 255/96)")

    failed = [text for ok, text in checks if not ok]
    for ok, text in checks:
        print(f"{'ok  ' if ok else 'FAIL'} {text}")
    print(f"self-test: {len(checks) - len(failed)}/{len(checks)} checks passed")
    return 0 if not failed else 1


# --------------------------------------------------------------------------- #
# CLI
# --------------------------------------------------------------------------- #


def _print_dump(path: str) -> int:
    data = read_dump(path)
    print(f"dump: {path}")
    print(f"      {len(data)} bytes, FNV-1a64 {fnv1a64(data):#018x}")
    if fnv1a64(data) == DUMP_FNV:
        print(f"      FNV matches the pinned reference {DUMP_FNV:#018x}")
    else:
        print(f"      [warn] FNV differs from the pinned reference {DUMP_FNV:#018x}")
        print("      (levels/entries are still positional; run --build to see where)")
    return 0


def _print_build(dump_path: str, game: str, as_json: bool) -> int:
    palette_bytes = load_palette(game)
    built = build_from_palette(palette_bytes)
    dump = read_dump(dump_path)
    report = compare(built, dump)
    if as_json:
        print(json.dumps({"dump": dump_path, "game": game, **report}, indent=2))
    else:
        print(f"palette: FRONTEND/GAMEPAL from {game} (256 slots, 6-bit)")
        print("model  : 0x483290(dst, k/11, 1) + 0x4228e0 metric + 0x483250 skip list")
        print(f"dump   : {dump_path} {len(dump)} bytes, "
              f"FNV-1a64 {report['fnv_dump']:#018x}")
        print(f"built  : {len(built)} bytes, FNV-1a64 {report['fnv_built']:#018x}")
        print(f"identical bytes={report['identical']}/{report['total']}")
        for item in report["levels"]:
            tail = "" if item["first_diff"] is None else f"  first diff at index {item['first_diff']}"
            print(f"level {item['level']:2d}: {item['same']:3d}/256{tail}")
        first = report["first_diff"]
        if first is None:
            print("first divergent byte: none")
        else:
            print(f"first divergent byte: level {first['level']} index {first['index']} "
                  f"(offset {first['offset']}): model {first['built']}, dump {first['dump']}")
    return 0 if report["identical"] == report["total"] else 1


def _emit(out: str, dump_path: str, game: str) -> int:
    target = emit_target(out)
    built = build_from_palette(load_palette(game))
    try:
        with open(target, "wb") as handle:
            handle.write(built)
    except OSError as exc:
        raise LutBuildError(f"cannot write '{target}': {exc.strerror or exc}") from exc
    print(f"built {len(built)} bytes -> {target} (FNV-1a64 {fnv1a64(built):#018x})")
    try:
        dump = read_dump(dump_path)
    except LutBuildError as exc:
        print(f"[warn] {exc}")
        return 0
    report = compare(built, dump)
    print(f"matches dump: {report['identical']}/{report['total']} bytes")
    return 0 if report["identical"] == report["total"] else 1


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--dump", nargs="?", const=DEFAULT_DUMP, default=DEFAULT_DUMP,
                        metavar="PATH",
                        help=f"dump of 0x682f90 (default {DEFAULT_DUMP})")
    parser.add_argument("--game", default=DEFAULT_GAME,
                        help=f"installation to read the palette from (default {DEFAULT_GAME})")
    parser.add_argument("--build", action="store_true",
                        help="rebuild all 16 tables and compare with the dump")
    parser.add_argument("--emit", metavar="OUT",
                        help="write the built 4096 bytes to OUT (not the repo/game)")
    parser.add_argument("--explain", action="store_true",
                        help="addresses, formula, proven vs hypothesis")
    parser.add_argument("--ablate", action="store_true",
                        help="compare the code model and wrong variants with the dump")
    parser.add_argument("--self-test", action="store_true",
                        help="check the builder's properties without the dump")
    parser.add_argument("--json", action="store_true", help="machine-readable output")
    args = parser.parse_args(argv)

    try:
        if args.self_test:
            return self_test(args.game)
        if args.explain:
            return explain(args.dump, args.game)
        if args.ablate:
            return ablate(args.dump, args.game)
        if args.emit:
            return _emit(args.emit, args.dump, args.game)
        if args.build:
            return _print_build(args.dump, args.game, args.json)
        return _print_dump(args.dump)
    except LutBuildError as exc:
        print(f"[error] lut_build: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
