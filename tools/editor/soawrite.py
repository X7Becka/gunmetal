#!/usr/bin/env python3
"""Layout-preserving writer/editor for Gunmetal ``.soa`` sprite/texture
animation descriptors (``M11.5``, slice 4).

``gm/soa.py`` (``M1.6``, ``docs/SOA.md``) already reads every shipped ``.soa``
and ``Soa.to_bytes()`` re-joins the stored text lines: the byte-exact
round-trip of the 104 shipped files is 104/104 (``docs/SOA_WRITE.md``).  What
that writer cannot do is *edit* a file without rebuilding the text: it
re-serialises from the fields, and the canonical ``render_lines()`` layout
matches only 1 of 104 shipped files (the others carry trailing blank lines and
one name line is padded with spaces).

This tool adds the missing half: an edit **rewrites only the bytes of the
edited header token** (``mode``, ``period`` or ``loop``).  ``.soa`` is a DOS
text file with a four-number header and one sprite name per line, so the field
lives in one token; replacing that token in place leaves every other byte --
the CRLF endings, the blank lines, the padded name line, the trailing newline
(or its absence) -- untouched.  Only the width of the new number can change the
file length (``20`` -> ``30`` keeps it, ``20`` -> ``150`` adds a byte).

What is proven here
-------------------
* ``--roundtrip`` reads every shipped ``.soa``, writes it back through the
  oracle into a scratch directory, reads the bytes back and compares them with
  the original: **104/104 byte-identical** (8 573 bytes).  It also re-joins the
  file from the line spans of this tool's own reader, re-parses the oracle text
  semantically and checks the engine timeline.
* ``--audit`` measures the corpus (modes, frames, periods, zero-delay frames,
  sizes) -- numbers, not guesses.
* ``--set`` edits the cycle period (or a per-frame delay, see below) and
  proves that only the target token changed.

The format has **no per-frame delay field**: ``period`` is the duration of the
whole cycle in centiseconds and the frame selector ``0x41de7d`` derives each
frame's share as ``t % period * declared / period`` (``docs/SOA.md`` §4).
``--delay MS`` therefore computes the cycle period whose i-th frame interval is
exactly ``MS`` milliseconds on the 91 Hz engine clock and refuses when no
period produces that duration (the quantisation is real, not rounded away).

Command surface
---------------
``--list FILE``            header + frame timeline (ticks, ms, local id, name).
``--get FILE --field NAME`` one header field with its byte span.
``--set FILE --field NAME=VALUE --out OUT``
                           token-level edit; refuses the game/mirror/repo.
``--set FILE --frame N --delay MS --out OUT``
                           the cycle period that gives frame N that delay.
``--check FILE [--against FILE] [--field NAME=VALUE]``
                           structure + byte comparison.
``--roundtrip [DIR]``      read -> write -> compare every ``.soa`` under DIR
                           (default: the game installation).
``--audit [DIR]``          corpus statistics.
``--self-test``            synthetic tests, no game data.
``--json``                 machine-readable report for every mode.

Exit codes: ``0`` success, ``1`` verification failed, ``2`` bad input or a
refused edit (message on stderr), ``3`` internal error.  No traceback escapes
:func:`main`.
"""
from __future__ import annotations

import argparse
import contextlib
import io
import json
import os
import shutil
import sys
import tempfile
import time
from dataclasses import dataclass

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
if ROOT not in sys.path:
    sys.path.insert(0, ROOT)

from gm import soa as oracle                      # noqa: E402

DEFAULT_GAME = "/mnt/d/games/Gunmetal 3dfx"
DEFAULT_OUT_DIR = "/tmp/opencode/soawrite"
MIRROR = os.path.expanduser("~/.cache/gunmetal-re/game")

#: The loader refuses more than 32 names (``0x42baf3``: ``frames <= 0x20``).
MAX_FRAMES = oracle.MAX_FRAMES                    # 32

#: ``period`` is read with ``fscanf("%ld")`` into a 32-bit field and multiplied
#: by 91; the shipped range is 1..600, the cap here only stops silly input.
PERIOD_MIN, PERIOD_MAX = 1, 32767

#: Shipped modes: 0 (one shot) x23, 1 (loop) x77, 100/101 (clocks) x3,
#: 102 (film) x1.  Anything else still returns frame 0 (``0x44dc50``), but the
#: tool stays inside the measured range.
MODE_MIN, MODE_MAX = 0, 102

CRLF = b"\r\n"


class EditError(Exception):
    """Bad input or a refused edit; the message is meant for the user."""


class ScanError(Exception):
    """The bytes do not walk as a ``.soa`` (own reader)."""


# ---------------------------------------------------------------------------
# line and token spans
# ---------------------------------------------------------------------------

@dataclass(frozen=True)
class Line:
    """One physical line with its byte span and its terminator.

    ``start``/``end`` delimit the text *without* the terminator; ``eol`` keeps
    the exact bytes that followed (``b"\\r\\n"``, ``b"\\n"``, ``b"\\r"`` or
    ``b""`` for an unterminated last line), so ``text + eol`` re-joins the file.
    """

    index: int
    start: int
    end: int
    eol: bytes
    text: str

    @property
    def blank(self) -> bool:
        return not self.text.strip()

    @property
    def padded(self) -> bool:
        return self.text != self.text.rstrip()


@dataclass(frozen=True)
class Token:
    """One whitespace-separated token and its absolute byte span."""

    text: str
    start: int
    end: int
    line: int

    @property
    def span(self) -> tuple[int, int]:
        return (self.start, self.end)


def split_lines(data: bytes) -> list[Line]:
    """Split on CRLF / LF / CR, keeping the exact terminator of every line.

    ``.soa`` is a DOS text file, but the reader is deliberately tolerant of a
    stray LF or CR so that a corrupt layout is *reported* instead of silently
    normalised by a round-trip.
    """
    lines: list[Line] = []
    i, n = 0, len(data)
    while i < n:
        j = i
        while j < n and data[j] not in (0x0A, 0x0D):
            j += 1
        start, end = i, j
        if j >= n:
            eol = b""
            i = j
        elif data[j] == 0x0D and j + 1 < n and data[j + 1] == 0x0A:
            eol = b"\r\n"
            i = j + 2
        elif data[j] == 0x0D:
            eol = b"\r"
            i = j + 1
        else:
            eol = b"\n"
            i = j + 1
        lines.append(Line(len(lines), start, end, eol,
                          data[start:end].decode("latin1")))
        if j >= n:
            break
    return lines


def tokenize_line(line: Line, data: bytes) -> list[Token]:
    """Whitespace-split one line the way ``str.split()`` does, with spans.

    latin-1 maps one byte to one character, so scanning ``str.isspace()``
    reproduces ``str.split()`` exactly (including ``\\x85`` / ``\\xa0``).
    """
    out: list[Token] = []
    i, n = 0, len(line.text)
    while i < n:
        while i < n and line.text[i].isspace():
            i += 1
        if i >= n:
            break
        start = i
        while i < n and not line.text[i].isspace():
            i += 1
        out.append(Token(line.text[start:i], line.start + start,
                         line.start + i, line.index))
    return out


def rejoin(data: bytes, lines: list[Line]) -> bytes:
    """Reassemble the file from its line spans; a proof the spans cover it."""
    parts: list[bytes] = []
    pos = 0
    for line in lines:
        if line.start < pos or line.end < line.start or line.eol not in (
                b"", b"\n", b"\r", b"\r\n"):
            raise ScanError(f"line spans are out of order at {line.start}")
        parts.append(data[pos:line.start])
        parts.append(line.text.encode("latin1"))
        parts.append(line.eol)
        pos = line.end + len(line.eol)
    parts.append(data[pos:])
    return b"".join(parts)


# ---------------------------------------------------------------------------
# own reader (independent of gm/soa.py, same grammar)
# ---------------------------------------------------------------------------

@dataclass
class SoaDoc:
    """A walked ``.soa``: exact lines, header tokens, name tokens."""

    path: str
    data: bytes
    lines: list[Line]
    header: list[Token]
    header_line: int
    names: list[Token]

    # -- header values ----------------------------------------------------
    @property
    def mode(self) -> int:
        return int(self.header[0].text)

    @property
    def period(self) -> int:
        return int(self.header[1].text)

    @property
    def declared(self) -> int:
        return int(self.header[2].text)

    @property
    def loop(self) -> int:
        return int(self.header[3].text)

    @property
    def size(self) -> int:
        return len(self.data)

    @property
    def name_texts(self) -> list[str]:
        return [t.text for t in self.names]

    # -- engine view ------------------------------------------------------
    @property
    def period_ticks(self) -> int:
        """``period * 91 / 100``, truncated, exactly as ``0x42badf``."""
        return self.period * oracle.ENGINE_TICK_HZ // 100

    @property
    def special(self) -> bool:
        return self.mode not in (0, 1)

    @property
    def short(self) -> bool:
        return len(self.names) < self.declared

    @property
    def truncated(self) -> bool:
        return len(self.names) > self.declared

    @property
    def loadable(self) -> bool:
        """Whether ``0x42b81b`` would register this animation."""
        return (1 <= self.declared <= MAX_FRAMES
                and len(self.names) >= self.declared
                and (self.special or self.period_ticks > 0))

    @property
    def engine_names(self) -> list[str]:
        return self.name_texts[:self.declared]

    @property
    def layout(self) -> str:
        """Header widths per token, e.g. ``"x xx x x"`` (shipped: 5 shapes).

        Widths, not just counts: the shipped headers are single-space
        separated, so this string pins both the separator style and the number
        of digits -- exactly what a token edit may change.
        """
        tokens = self.lines[self.header_line].text.split()
        return " ".join("x" * len(tok) for tok in tokens)

    @property
    def blank_lines(self) -> int:
        return sum(1 for line in self.lines if line.blank)

    @property
    def padded_lines(self) -> int:
        return sum(1 for line in self.lines if line.padded)

    @property
    def bare_lf(self) -> int:
        return sum(1 for line in self.lines if line.eol == b"\n")

    @property
    def bare_cr(self) -> int:
        return sum(1 for line in self.lines if line.eol == b"\r")

    @property
    def final_eol(self) -> bytes:
        return self.lines[-1].eol if self.lines else b""


def walk(data: bytes, path: str = "") -> SoaDoc:
    """Parse ``.soa`` bytes into spans; refuses anything the loader cannot read.

    The grammar is the loader's (``0x42ba36``..``0x42bb4a``): a four-number
    header and then whitespace-separated sprite names, of which only the first
    ``declared`` are read.  The walker keeps *every* name token (like
    ``gm/soa.py``), so extra lines survive a round-trip and are reported as
    ``truncated`` instead of being dropped.
    """
    lines = split_lines(data)
    if not lines:
        raise ScanError("empty .soa")
    header_line = None
    for line in lines:
        if not line.blank:
            header_line = line.index
            break
    if header_line is None:
        raise ScanError("no header line (the file has no non-blank line)")
    header_tokens = tokenize_line(lines[header_line], data)
    if len(header_tokens) != 4:
        raise ScanError(
            f"header must be exactly four integers (mode period frames loop); "
            f"got {len(header_tokens)} token(s) on line {header_line + 1}")
    values = []
    for position, token in enumerate(header_tokens):
        try:
            values.append(int(token.text))
        except ValueError:
            raise ScanError(
                f"header token {position + 1} is not an integer: "
                f"{token.text!r}") from None
    mode, period, declared, loop = values
    if not 1 <= declared <= MAX_FRAMES:
        raise ScanError(
            f"declared frames = {declared}: the loader accepts 1..{MAX_FRAMES} "
            f"only (0x42baf3: cmp eax, 0x20)")
    if not MODE_MIN <= mode <= MODE_MAX:
        raise ScanError(
            f"mode = {mode}: shipped modes are 0, 1 and 100..102 "
            f"(docs/SOA.md §1/§6)")
    if period < PERIOD_MIN or period > PERIOD_MAX:
        raise ScanError(
            f"period = {period}: outside {PERIOD_MIN}..{PERIOD_MAX} "
            f"(shipped range is 1..600 centiseconds)")
    names: list[Token] = []
    for line in lines[header_line + 1:]:
        names.extend(tokenize_line(line, data))
    if not names:
        raise ScanError("no sprite names after the header")
    return SoaDoc(path=path, data=data, lines=lines, header=header_tokens,
                  header_line=header_line, names=names)


# -- independent engine timeline -------------------------------------------

def _ceil_div(a: int, b: int) -> int:
    return -((-a) // b)


@dataclass(frozen=True)
class Interval:
    """One frame's hold time: ``end_tick`` is exclusive, ``None`` = forever."""

    index: int
    name: str
    start_tick: int
    end_tick: int | None

    @property
    def duration_ticks(self) -> int | None:
        return None if self.end_tick is None else self.end_tick - self.start_tick

    @property
    def start_ms(self) -> int:
        return self.start_tick * 1000 // oracle.ENGINE_TICK_HZ

    @property
    def duration_ms(self) -> int | None:
        ticks = self.duration_ticks
        return None if ticks is None else ticks * 1000 // oracle.ENGINE_TICK_HZ


def timeline(mode: int, period_ticks: int, names: list[str]) -> list[Interval]:
    """Frame intervals exactly as ``0x41de7d``/``gm/soa.py`` compute them."""
    if not names:
        return []
    if mode not in (0, 1):
        return [Interval(0, names[0], 0, None)]
    if period_ticks <= 0:
        raise ScanError("period is 0 ticks: the engine divides by zero")
    out: list[Interval] = []
    n = len(names)
    for k in range(n):
        start = _ceil_div(k * period_ticks, n)
        end = _ceil_div((k + 1) * period_ticks, n)
        if k == n - 1 and mode == 0:
            out.append(Interval(k, names[k], start, None))
        else:
            out.append(Interval(k, names[k], start, end))
    return out


def doc_intervals(doc: SoaDoc) -> list[Interval]:
    return timeline(doc.mode, doc.period_ticks, doc.engine_names)


def oracle_timeline(model) -> list:
    return model.timeline()


# ---------------------------------------------------------------------------
# header fields
# ---------------------------------------------------------------------------

@dataclass(frozen=True)
class FieldSpec:
    """What a field name addresses: one header token and its value range."""

    name: str
    kind: str            # "mode" | "period" | "loop"
    token: Token
    low: int
    high: int
    doc: str


FIELD_DOCS = {
    "mode": ("playback mode: 0 = one shot, 1 = loop, 100/101 = clock, "
             "102 = film (docs/SOA.md §4/§6)"),
    "period": ("duration of the WHOLE cycle in centiseconds; the loader "
               "stores period*91/100 ticks (0x42bad9)"),
    "loop": ("dead field: parsed by the loader (0x42ba99) but never read at "
             "runtime; .spritani files get loop=0 forced (0x42bad2)"),
}

FIELD_RANGE = {
    "mode": (MODE_MIN, MODE_MAX),
    "period": (PERIOD_MIN, PERIOD_MAX),
    "loop": (0, 1),
}

FIELD_INDEX = {"mode": 0, "period": 1, "declared": 2, "loop": 3}


def resolve_field(doc: SoaDoc, name: str) -> FieldSpec:
    """``mode`` / ``period`` / ``loop`` -> the header token to rewrite."""
    text = name.strip()
    if text == "declared":
        raise EditError(
            "declared is not editable: it is the number of names the loader "
            "reads (0x42baf3), so changing it adds or removes frames. That is "
            "a data edit, not a token edit -- out of scope for this slice")
    if text not in FIELD_INDEX:
        raise EditError(f"bad field name {text!r}: expected mode, period or "
                        f"loop (declared is read-only on purpose)")
    low, high = FIELD_RANGE[text]
    return FieldSpec(text, text, doc.header[FIELD_INDEX[text]], low, high,
                     FIELD_DOCS[text])


def parse_int_value(text: str, what: str) -> int:
    try:
        return int(text.strip(), 0)
    except ValueError:
        raise EditError(f"{what}: not an integer: {text!r}") from None


def validate(spec: FieldSpec, value: int, doc: SoaDoc | None = None) -> None:
    """Check a value against the measured corpus range and engine limits."""
    if not spec.low <= value <= spec.high:
        raise EditError(f"{spec.name}={value}: outside the measured range "
                        f"{spec.low}..{spec.high} (see --audit)")
    if spec.name == "period" and doc is not None and not doc.special:
        if value * oracle.ENGINE_TICK_HZ // 100 <= 0:
            raise EditError(
                f"period={value}: converts to 0 engine ticks; 0x41de7d would "
                f"divide by zero (needs period >= 2 for mode {doc.mode})")


def parse_assignment(doc: SoaDoc, assignment: str) -> tuple[FieldSpec, int]:
    """``NAME=VALUE`` -> field spec and integer value (validated)."""
    if "=" not in assignment:
        raise EditError("--field needs NAME=VALUE")
    name, _, value_text = assignment.partition("=")
    spec = resolve_field(doc, name)
    value = parse_int_value(value_text, spec.name)
    validate(spec, value, doc)
    return spec, value


def current_value(doc: SoaDoc, spec: FieldSpec) -> int:
    return int(spec.token.text)


def oracle_value(model, spec: FieldSpec) -> int:
    return int({"mode": model.mode, "period": model.period,
                "declared": model.declared_frames, "loop": model.loop}[spec.name])


# ---------------------------------------------------------------------------
# delay -> period (the format has no per-frame delay)
# ---------------------------------------------------------------------------

def interval_ticks(frame: int, period_ticks: int, frames: int) -> int:
    """Hold time of frame ``frame`` in ticks for this period."""
    return (_ceil_div((frame + 1) * period_ticks, frames)
            - _ceil_div(frame * period_ticks, frames))


def ticks_for_ms(ms: int) -> tuple[int, int]:
    """Tick range whose 91 Hz duration floors to ``ms`` milliseconds."""
    low = _ceil_div(ms * oracle.ENGINE_TICK_HZ, 1000)
    high = _ceil_div((ms + 1) * oracle.ENGINE_TICK_HZ, 1000) - 1
    return low, max(low, high)


def delay_to_period(doc: SoaDoc, frame: int, delay_ms: int) -> dict:
    """Smallest cycle period whose ``frame`` holds exactly ``delay_ms`` ms.

    The engine has no per-frame delay field: ``0x41de7d`` derives each frame's
    share from the cycle period, so the request is only representable when some
    period makes ``ceil((k+1)p/n) - ceil(k p/n)`` last that many milliseconds.
    Nothing is rounded silently -- an impossible delay is refused together with
    the nearest representable ones.
    """
    frames = doc.declared
    if not doc.loadable:
        raise EditError(
            f"the engine would refuse this file (declared {doc.declared}, "
            f"{len(doc.names)} name(s)); a delay edit would not be played -- "
            f"fix the frame list first")
    if doc.special:
        raise EditError(
            f"mode {doc.mode} is a special mode (100..102): 0x44dc50 ignores "
            f"the period and redraws frame 0 every tick, so --delay is "
            f"meaningless here (use --field period=... if you really mean it)")
    if not 0 <= frame < frames:
        raise EditError(f"--frame {frame}: the animation declares {frames} "
                        f"frame(s) (0..{frames - 1})")
    if delay_ms < 1:
        raise EditError(f"--delay {delay_ms}: a hold time must be at least 1 ms")
    if frames <= 0:
        raise EditError("--delay needs a positive declared frame count")
    low, high = ticks_for_ms(delay_ms)
    best: tuple[int, int] | None = None
    for period_cs in range(PERIOD_MIN, PERIOD_MAX + 1):
        period_ticks = period_cs * oracle.ENGINE_TICK_HZ // 100
        if period_ticks <= 0:
            continue
        held = interval_ticks(frame, period_ticks, frames)
        if low <= held <= high and held * 1000 // oracle.ENGINE_TICK_HZ == delay_ms:
            best = (period_cs, held)
            break
    if best is None:
        achievable = sorted({interval_ticks(frame, cs * oracle.ENGINE_TICK_HZ // 100,
                                            frames) * 1000 // oracle.ENGINE_TICK_HZ
                             for cs in range(PERIOD_MIN, PERIOD_MAX + 1)
                             if cs * oracle.ENGINE_TICK_HZ // 100 > 0})
        lower = [ms for ms in achievable if ms < delay_ms]
        upper = [ms for ms in achievable if ms > delay_ms]
        nearest = []
        if lower:
            nearest.append(lower[-1])
        if upper:
            nearest.append(upper[0])
        raise EditError(
            f"--delay {delay_ms} ms for frame {frame} of {frames}: not "
            f"representable on the 91 Hz clock (frames share the cycle period "
            f"and hold ceil(k*p/n)-ceil((k-1)*p/n) ticks); nearest "
            f"representable: {', '.join(f'{ms} ms' for ms in nearest)}")
    period_cs, held = best
    return {"frame": frame, "frames": frames, "requested_ms": delay_ms,
            "period": period_cs, "period_ticks": period_cs
            * oracle.ENGINE_TICK_HZ // 100, "held_ticks": held,
            "held_ms": held * 1000 // oracle.ENGINE_TICK_HZ}


# ---------------------------------------------------------------------------
# patching and verification
# ---------------------------------------------------------------------------

def splice(data: bytes, replacements: list[tuple[Token, str]]) -> bytes:
    """Replace only the given tokens; every other byte stays where it was."""
    ordered = sorted(replacements, key=lambda item: item[0].start)
    parts: list[bytes] = []
    pos = 0
    for token, text in ordered:
        if token.start < pos:
            raise EditError("overlapping token spans")
        parts.append(data[pos:token.start])
        parts.append(text.encode("latin1"))
        pos = token.end
    parts.append(data[pos:])
    return b"".join(parts)


def patch(doc: SoaDoc, spec: FieldSpec, value: int) -> tuple[bytes, str]:
    text = str(int(value))
    return splice(doc.data, [(spec.token, text)]), text


def aligned_diff(old: bytes, new: bytes) -> int:
    """Bytes that differ when the two texts are aligned from the start."""
    compared = sum(1 for a, b in zip(old, new) if a != b)
    return compared + abs(len(old) - len(new))


def changed_bytes(doc: SoaDoc, spec: FieldSpec, new_bytes: bytes) -> dict:
    """Describe the edit: which token span changed and by how many bytes.

    The edited file is re-walked, so the new text comes from the file itself,
    not from the value we meant to write.
    """
    new_doc = walk(new_bytes, doc.path)
    old_text = spec.token.text
    new_token = new_doc.header[FIELD_INDEX[spec.name]]
    count = aligned_diff(old_text.encode("latin1"),
                         new_token.text.encode("latin1"))
    if doc.name_texts != new_doc.name_texts:
        raise EditError("the edited file changed the name list")
    return {"size": len(new_bytes),
            "size_delta": len(new_bytes) - len(doc.data),
            "tokens": [{"old": old_text, "new": new_token.text,
                        "span": list(spec.token.span), "changed": count}],
            "changed_bytes": count}


def verify_edit(doc: SoaDoc, new_bytes: bytes, spec: FieldSpec, expected: int,
                delay: dict | None = None) -> dict:
    """Prove the edit changed the target token and nothing else."""
    checks: list[dict] = []

    def check(name: str, ok: bool, detail: str = "") -> None:
        checks.append({"check": name, "ok": bool(ok), "detail": detail})

    try:
        new_doc = walk(new_bytes, doc.path)
        check("own reader parses the edited file", True,
              f"{len(new_doc.lines)} lines, {len(new_doc.names)} names")
    except ScanError as exc:
        check("own reader parses the edited file", False, str(exc))
        return {"ok": False, "checks": checks}
    try:
        new_model = oracle.parse(new_bytes)
        check("oracle parses the edited file to EOF", True)
    except ValueError as exc:
        check("oracle parses the edited file to EOF", False, str(exc))
        return {"ok": False, "checks": checks}

    delta = changed_bytes(doc, spec, new_bytes)
    check("size change equals the token width change",
          delta["size_delta"] == len(delta["tokens"][0]["new"].encode("latin1"))
          - len(delta["tokens"][0]["old"].encode("latin1")),
          f"{len(doc.data)} -> {len(new_bytes)} bytes "
          f"({delta['size_delta']:+d})")
    check("line count is unchanged", len(new_doc.lines) == len(doc.lines),
          f"{len(new_doc.lines)} line(s)")
    target_index = FIELD_INDEX[spec.name]
    others_ok = all(
        new_doc.header[i].text == doc.header[i].text
        for i in range(4) if i != target_index)
    check("every other header token is identical", others_ok,
          "mode/period/declared/loop")
    check("the name tokens are identical", doc.name_texts == new_doc.name_texts,
          f"{len(doc.names)} name(s)")

    new_token = new_doc.header[target_index]
    before = doc.data[:spec.token.start]
    after_old = doc.data[spec.token.end:]
    before_new = new_bytes[:new_token.start]
    after_new = new_bytes[new_token.end:]
    check("bytes outside the target token are identical",
          before == before_new and after_old == after_new)

    check("own reader sees the new value",
          current_value(new_doc, resolve_field(new_doc, spec.name)) == expected,
          str(expected))
    check("oracle sees the new value",
          oracle_value(new_model, spec) == expected, str(expected))
    if delay is not None:
        intervals = doc_intervals(new_doc) if new_doc.loadable else []
        ok = (len(intervals) > delay["frame"]
              and intervals[delay["frame"]].duration_ms == delay["requested_ms"]
              and intervals[delay["frame"]].duration_ticks == delay["held_ticks"])
        check("frame holds the requested delay on the 91 Hz clock", ok,
              f"frame {delay['frame']}: "
              + (f"{intervals[delay['frame']].duration_ticks} ticks = "
                 f"{intervals[delay['frame']].duration_ms} ms"
                 if len(intervals) > delay["frame"] else "not loadable"))
        model_intervals = oracle_timeline(new_model)
        ok = (len(model_intervals) > delay["frame"]
              and model_intervals[delay["frame"]].duration_ticks
              == delay["held_ticks"])
        check("oracle agrees on the delay", ok,
              f"frame {delay['frame']}: "
              + (f"{model_intervals[delay['frame']].duration_ticks} ticks"
                 if len(model_intervals) > delay["frame"] else "not loadable"))
        check("the cycle length covers every frame",
              new_doc.period_ticks >= new_doc.declared,
              f"{new_doc.period_ticks} ticks for {new_doc.declared} frame(s)")
    check("changed-byte count is reported", True,
          f"{delta['changed_bytes']} byte(s)")
    return {"ok": all(c["ok"] for c in checks), "checks": checks, "diff": delta,
            "new": new_doc, "new_model": new_model}


# ---------------------------------------------------------------------------
# paths and refusal rules
# ---------------------------------------------------------------------------

def read_bytes(path: str) -> bytes:
    try:
        with open(path, "rb") as fh:
            return fh.read()
    except OSError as exc:
        raise EditError(f"{path}: {exc}") from None


def atomic_write(path: str, data: bytes) -> None:
    """Write through a temporary file in the target directory, then replace."""
    directory = os.path.dirname(os.path.abspath(path)) or "."
    try:
        os.makedirs(directory, exist_ok=True)
        fd, temp = tempfile.mkstemp(prefix=".soawrite-", dir=directory)
        try:
            with os.fdopen(fd, "wb") as fh:
                fh.write(data)
                fh.flush()
                os.fsync(fh.fileno())
            os.replace(temp, path)
        except BaseException:
            try:
                os.unlink(temp)
            except OSError:
                pass
            raise
    except OSError as exc:
        raise EditError(f"{path}: {exc}") from None


def write_bytes(path: str, data: bytes) -> None:
    atomic_write(path, data)


def require_soa(path: str) -> str:
    """Input files must be ``.soa`` -- everything else is a mistyped call."""
    if not path.lower().endswith(".soa"):
        raise EditError(f"not a .soa file: {path}")
    if not os.path.isfile(path):
        raise EditError(f"no such file: {path}")
    return path


def _real(path: str) -> str:
    return os.path.realpath(os.path.abspath(path))


def protected_roots(game: str) -> list[str]:
    """Directories a write must never touch: game, mirror and this checkout."""
    roots = []
    for candidate in (game, DEFAULT_GAME, MIRROR, ROOT):
        if candidate:
            roots.append(_real(candidate))
    return sorted(set(roots))


def refuse_unsafe_out(out_path: str, game: str) -> None:
    """Never write into the game, the regress mirror or this repository."""
    if not out_path.lower().endswith(".soa"):
        raise EditError(f"--out must be a .soa path: {out_path}")
    target = _real(out_path)
    for root in protected_roots(game):
        if target == root or target.startswith(root + os.sep):
            kind = "the game installation" if root in (
                _real(game), _real(DEFAULT_GAME)) else (
                "the regress mirror" if root == _real(MIRROR)
                else "the repository")
            raise EditError(
                f"refusing to write inside {kind}: {out_path} "
                f"(write to /tmp/opencode or /var/tmp)")


def resolve_input(name: str, game: str = DEFAULT_GAME) -> str:
    """``pulsar`` / ``pulsar.soa`` / a path -> an existing ``.soa``."""
    if os.path.isfile(name):
        return require_soa(name)
    base = name if name.lower().endswith(".soa") else name + ".soa"
    for folder in ("anims", "spritani", "Sobanim", "."):
        direct = os.path.join(game, folder, base)
        if os.path.isfile(direct):
            return direct
    low = base.lower()
    for dirpath, dirnames, filenames in os.walk(game):
        dirnames[:] = sorted(d for d in dirnames if not d.startswith("."))
        for filename in sorted(filenames):
            if filename.lower() == low:
                return os.path.join(dirpath, filename)
    raise EditError(f"animation not found: {name} (looked in {game})")


def corpus_files(root: str) -> list[str]:
    """Every ``.soa`` under ``root`` (recursive; shipped: anims/spritani/Sobanim)."""
    found: list[str] = []
    for dirpath, dirnames, filenames in os.walk(root):
        dirnames[:] = sorted(d for d in dirnames
                             if not d.startswith("."))
        for filename in sorted(filenames):
            if filename.lower().endswith(".soa"):
                found.append(os.path.join(dirpath, filename))
    return found


def shipped_same_relative(path: str, game: str) -> str | None:
    """The shipped file with the same relative path (``anims/x.soa``), if any."""
    parts = os.path.abspath(path).split(os.sep)
    for i, part in enumerate(parts[:-1]):
        if part.lower() in ("anims", "spritani", "sobanim"):
            candidate = os.path.join(game, *parts[i:])
            if os.path.isfile(candidate):
                return candidate
    base = os.path.basename(path)
    for folder in ("anims", "spritani", "Sobanim", ""):
        candidate = os.path.join(game, folder, base) if folder \
            else os.path.join(game, base)
        if os.path.isfile(candidate):
            return candidate
    return None


def default_against(path: str, game: str) -> str | None:
    candidate = shipped_same_relative(path, game)
    if candidate is None or _real(candidate) == _real(path):
        return None
    return candidate


# ---------------------------------------------------------------------------
# structure checks
# ---------------------------------------------------------------------------

def model_of(data: bytes):
    return oracle.parse(data)


def structure_report(doc: SoaDoc, model) -> dict:
    """Structural checks: own reader vs oracle vs the engine timeline."""
    checks: list[dict] = []
    warnings: list[str] = []

    def check(name: str, ok: bool, detail: str = "") -> None:
        checks.append({"check": name, "ok": bool(ok), "detail": detail})

    names = doc.name_texts
    check("line scanner re-joins the file byte-exactly",
          rejoin(doc.data, doc.lines) == doc.data,
          f"{len(doc.lines)} line(s), final EOL "
          f"{'CRLF' if doc.final_eol == CRLF else repr(doc.final_eol)}")
    check("header is four integers on one line", len(doc.header) == 4,
          f"mode {doc.mode}, period {doc.period}, declared {doc.declared}, "
          f"loop {doc.loop}")
    check("declared frames within the loader limit",
          1 <= doc.declared <= MAX_FRAMES, f"{doc.declared} (loader: 1..32)")
    check("own header equals the oracle header",
          (doc.mode, doc.period, doc.declared, doc.loop)
          == (model.mode, model.period, model.declared_frames, model.loop),
          f"{model.mode} {model.period} {model.declared_frames} {model.loop}")
    check("own name tokens equal the oracle names", names == model.names,
          f"{len(names)} name(s)")
    own = doc_intervals(doc) if doc.loadable else []
    theirs = oracle_timeline(model) if doc.loadable else []
    check("own timeline equals the oracle timeline",
          [(i.index, i.start_tick, i.end_tick)
           for i in own] == [(t.index, t.start_tick, t.end_tick)
                             for t in theirs],
          f"{len(own)} frame(s), {doc.period_ticks} ticks")
    check("the file is CRLF-only", doc.bare_lf == 0 and doc.bare_cr == 0,
          f"{doc.bare_lf} bare LF, {doc.bare_cr} bare CR")
    check("every name line holds one token",
          all(1 == len(tokenize_line(line, doc.data))
              for line in doc.lines[doc.header_line + 1:] if not line.blank),
          f"{len(doc.names)} tokens in "
          f"{len(doc.lines) - doc.header_line - 1} line(s)")
    if not doc.loadable:
        warnings.append(
            f"the engine would NOT load this file: declared {doc.declared}, "
            f"{len(names)} name(s), period {doc.period_ticks} ticks")
    if doc.short:
        warnings.append(
            f"only {len(names)} name(s) for declared {doc.declared}: "
            f"0x42b81b refuses the file (fscanf gets no 6th name)")
    if doc.truncated:
        warnings.append(
            f"{len(names) - doc.declared} extra name(s) after declared "
            f"{doc.declared}: the loader never reads them (docs/SOA.md §8)")
    if doc.special:
        warnings.append(f"mode {doc.mode} is a special mode: 0x44dc50 "
                        f"returns frame 0 and redraws its texture")
    if doc.padded_lines:
        warnings.append(f"{doc.padded_lines} padded line(s) (bytes preserved)")
    if doc.blank_lines:
        warnings.append(f"{doc.blank_lines} blank line(s) (bytes preserved)")
    if doc.final_eol != CRLF:
        warnings.append("no final CRLF")
    return {"ok": all(c["ok"] for c in checks), "checks": checks,
            "warnings": warnings, "loadable": doc.loadable}


def diff_report(old: bytes, new: bytes) -> dict:
    """Contiguous byte ranges that differ."""
    if len(old) == len(new):
        offsets = [i for i, (a, b) in enumerate(zip(old, new)) if a != b]
        ranges: list[list[int]] = []
        for offset in offsets:
            if ranges and offset == ranges[-1][1]:
                ranges[-1][1] = offset + 1
            else:
                ranges.append([offset, offset + 1])
        return {"same_size": True, "changed_bytes": len(offsets),
                "size_delta": 0, "ranges": ranges[:20],
                "ranges_total": len(ranges)}
    return {"same_size": False, "changed_bytes": abs(len(new) - len(old)),
            "size_delta": len(new) - len(old), "ranges": [],
            "ranges_total": 0}


def diff_summary(diff: dict) -> str:
    if diff["same_size"]:
        text = f"{diff['changed_bytes']} byte(s) differ"
    else:
        text = f"size differs by {diff['size_delta']:+d} byte(s)"
    return text


# ---------------------------------------------------------------------------
# modes: list / get / set / check
# ---------------------------------------------------------------------------

def local_ids(doc: SoaDoc) -> list[int]:
    """1-based ordinal of each name's first occurrence inside this file.

    The engine registry (``0x76ed40``) assigns ids globally, so a file-local
    ordinal is all a single descriptor can tell; it is labelled ``local id``
    everywhere and never presented as the runtime id.
    """
    seen: dict[str, int] = {}
    out: list[int] = []
    for name in doc.name_texts:
        if name not in seen:
            seen[name] = len(seen) + 1
        out.append(seen[name])
    return out


def list_file(path: str, limit: int = 0,
              game: str = DEFAULT_GAME) -> dict:
    path = resolve_input(path, game)
    data = read_bytes(path)
    doc = walk(data, path)
    model = model_of(data)
    structure = structure_report(doc, model)
    ids = local_ids(doc)
    frames = []
    if doc.loadable:
        for interval in doc_intervals(doc):
            frames.append({"frame": interval.index, "name": interval.name,
                           "local_id": ids[interval.index],
                           "start_tick": interval.start_tick,
                           "end_tick": interval.end_tick,
                           "ticks": interval.duration_ticks,
                           "ms": interval.duration_ms,
                           "start_ms": interval.start_ms,
                           "end_ms": (None if interval.end_tick is None else
                                      interval.end_tick * 1000
                                      // oracle.ENGINE_TICK_HZ)})
    else:
        for index, name in enumerate(doc.name_texts[:doc.declared]):
            frames.append({"frame": index, "name": name,
                           "local_id": ids[index], "start_tick": None,
                           "end_tick": None, "ticks": None, "ms": None,
                           "start_ms": None, "end_ms": None})
    return {"path": path, "size": len(data), "mode": doc.mode,
            "period": doc.period, "period_ticks": doc.period_ticks,
            "declared": doc.declared, "names": len(doc.names),
            "loop": doc.loop, "special": doc.special,
            "loadable": doc.loadable, "short": doc.short,
            "truncated": doc.truncated, "structure": structure,
            "frames": frames, "shown": len(frames),
            "blank_lines": doc.blank_lines, "padded_lines": doc.padded_lines,
            "final_eol": repr(doc.final_eol), "layout": doc.layout}


def mode_word(doc_mode: int) -> str:
    return {0: "one shot", 1: "loop", 100: "clock (low-res)",
            101: "clock (high-res)", 102: "film"}.get(doc_mode, "special")


def print_list(rep: dict, limit: int = 0) -> None:
    print(f"file: {rep['path']} ({rep['size']} bytes)")
    print(f"mode: {rep['mode']} ({mode_word(rep['mode'])})  period: "
          f"{rep['period']} cs = {rep['period_ticks']} ticks  declared: "
          f"{rep['declared']}  loop: {rep['loop']}  names: {rep['names']}")
    engine = ("loadable" if rep["loadable"] else "REFUSED by the loader")
    extra = []
    if rep["short"]:
        extra.append("short")
    if rep["truncated"]:
        extra.append("extra names")
    print(f"engine: {engine}" + (f" ({', '.join(extra)})" if extra else "")
          + f"; layout {rep['layout']}, blank lines {rep['blank_lines']}, "
          f"padded lines {rep['padded_lines']}, final EOL {rep['final_eol']}")
    frames = rep["frames"]
    if rep["loadable"]:
        print(f"frames: {len(frames)} (delay = engine ticks / ms; the format "
              f"has no per-frame field)")
    else:
        print(f"frames: {len(frames)} named, but the engine would refuse the "
              f"file (no timeline)")
    print(f"  {'frame':>5}  {'hold ticks':>10}  {'hold ms':>7}  "
          f"{'at ms':>6}  {'local id':>8}  name")
    shown = frames
    if limit and limit * 2 < len(frames):
        shown = frames[:limit] + [None] + frames[-limit:]
    for item in shown:
        if item is None:
            print(f"  ... {len(frames) - 2 * limit} frame(s) omitted ...")
            continue
        if item["ticks"] is None and item["start_tick"] is None:
            hold_ticks, hold_ms, at_ms = "?", "?", "?"
        else:
            hold_ticks = ("held" if item["ticks"] is None
                          else str(item["ticks"]))
            hold_ms = ("held" if item["ms"] is None else str(item["ms"]))
            at_ms = str(item["start_ms"])
        print(f"  {item['frame']:5d}  {hold_ticks:>10}  {hold_ms:>7}  "
              f"{at_ms:>6}  {item['local_id']:8d}  {item['name']}")


def get_field(path: str, name: str, game: str = DEFAULT_GAME) -> dict:
    path = resolve_input(path, game)
    data = read_bytes(path)
    doc = walk(data, path)
    model = model_of(data)
    spec = resolve_field(doc, name)
    return {"path": path, "size": len(data), "field": spec.name,
            "value": current_value(doc, spec),
            "oracle_value": oracle_value(model, spec),
            "span": list(spec.token.span), "token": spec.token.text,
            "doc": spec.doc}


def print_get(rep: dict) -> None:
    print(f"file: {rep['path']} ({rep['size']} bytes)")
    print(f"  {rep['field']} = {rep['value']} (oracle {rep['oracle_value']})")
    print(f"  {rep['doc']}")
    print(f"  token {rep['span'][0]}..{rep['span'][1]}: {rep['token']!r}")


def set_field(path: str, assignment: str, out_path: str,
              game: str = DEFAULT_GAME, frame: int | None = None,
              delay: int | None = None) -> dict:
    path = resolve_input(path, game)
    refuse_unsafe_out(out_path, game)
    if _real(out_path) == _real(path):
        raise EditError(f"--out is the input file: {out_path} (refusing to "
                        f"edit in place; the source stays untouched)")
    data = read_bytes(path)
    doc = walk(data, path)
    delay_info = None
    if delay is not None:
        if frame is None:
            frame = 0
        if assignment is not None:
            raise EditError("use either --field or --frame/--delay, not both")
        delay_info = delay_to_period(doc, frame, delay)
        spec = resolve_field(doc, "period")
        value = delay_info["period"]
        validate(spec, value, doc)
    else:
        if frame is not None:
            raise EditError("--frame is only meaningful with --delay")
        spec, value = parse_assignment(doc, assignment)
    before = current_value(doc, spec)
    new_bytes, _text = patch(doc, spec, value)
    report = verify_edit(doc, new_bytes, spec, value, delay_info)
    if not report["ok"]:
        raise EditError("edit verification failed: " + "; ".join(
            c["detail"] or c["check"]
            for c in report["checks"] if not c["ok"]))
    write_bytes(out_path, new_bytes)
    stored = read_bytes(out_path)
    new_doc = walk(stored, out_path)
    return {"source": path, "out": out_path, "field": spec.name,
            "before": before, "after": value, "noop": stored == data,
            "source_size": len(data), "size": len(new_bytes),
            "size_delta": len(new_bytes) - len(data),
            "changed_bytes": report["diff"]["changed_bytes"],
            "tokens": report["diff"]["tokens"], "checks": report["checks"],
            "delay": delay_info,
            "period_ticks": new_doc.period_ticks,
            "ok": stored == new_bytes}


def print_set(rep: dict) -> None:
    print(f"{rep['source']} -> {rep['out']}")
    print(f"{rep['field']}: {rep['before']} -> {rep['after']} "
          f"({rep['source_size']} -> {rep['size']} bytes, "
          f"delta {rep['size_delta']:+d})")
    if rep.get("delay"):
        info = rep["delay"]
        print(f"frame {info['frame']} of {info['frames']}: requested "
              f"{info['requested_ms']} ms -> period {info['period']} cs "
              f"({info['period_ticks']} ticks), frame holds "
              f"{info['held_ticks']} ticks = {info['held_ms']} ms")
    for item in rep["tokens"]:
        if item.get("changed"):
            print(f"  token {item['span'][0]}..{item['span'][1]}: "
                  f"{item['old']!r} -> {item['new']!r} "
                  f"({item['changed']} byte(s) changed)")
    for check in rep["checks"]:
        mark = "ok  " if check["ok"] else "FAIL"
        print(f"  [{mark}] {check['check']}"
              + (f" ({check['detail']})" if check["detail"] else ""))
    if rep["noop"]:
        print("note: the value was already present; the file is byte-identical")
    print(f"result: {'OK' if rep['ok'] else 'FAIL'}")


def check_file(path: str, against: str | None = None,
               field: str | None = None,
               game: str = DEFAULT_GAME) -> dict:
    path = resolve_input(path, game)
    data = read_bytes(path)
    doc = walk(data, path)
    model = model_of(data)
    structure = structure_report(doc, model)
    rep: dict = {"path": path, "size": len(data), "structure": structure,
                 "against": None, "field": None,
                 "ok": structure["ok"], "loadable": structure["loadable"]}
    if against is None:
        against = default_against(path, game)
        if against is not None:
            rep["note"] = f"compared against the shipped {against}"
        elif shipped_same_relative(path, game) is not None:
            rep["note"] = ("this is the shipped file itself; pass --against to "
                           "compare an edited copy")
        else:
            rep["note"] = ("no --against and no shipped file with this "
                           "relative path: byte comparison skipped")
    if field is not None and against is None:
        raise EditError("--field/--period needs --against (or a shipped file "
                        "with the same relative path) to compare with")
    if against is not None:
        against = require_soa(against)
        old_bytes = read_bytes(against)
        old_doc = walk(old_bytes, against)
        old_model = model_of(old_bytes)
        old_structure = structure_report(old_doc, old_model)
        rep["against"] = against
        rep["against_structure"] = old_structure
        rep["ok"] = rep["ok"] and old_structure["ok"]
        checks: list[dict] = []

        def check(name: str, ok: bool, detail: str = "") -> None:
            checks.append({"check": name, "ok": bool(ok), "detail": detail})

        check("name token counts match the source",
              len(doc.names) == len(old_doc.names),
              f"{len(old_doc.names)} vs {len(doc.names)}")
        check("line count and line endings match the source",
              len(doc.lines) == len(old_doc.lines)
              and [line.eol for line in doc.lines]
              == [line.eol for line in old_doc.lines],
              f"{len(old_doc.lines)} vs {len(doc.lines)} line(s)")
        if field is not None:
            old_spec, value = parse_assignment(old_doc, field)
            expected_bytes, _ = patch(old_doc, old_spec, value)
            diff = diff_report(old_bytes, data)
            check("the file is exactly the source with the field patched",
                  data == expected_bytes, diff_summary(diff))
            new_spec = resolve_field(doc, old_spec.name)
            check("the field carries the expected value",
                  current_value(doc, new_spec) == value,
                  str(current_value(doc, new_spec)))
            target_index = FIELD_INDEX[old_spec.name]
            changed = [name for name in ("mode", "period", "declared", "loop")
                       if FIELD_INDEX[name] != target_index
                       and doc.header[FIELD_INDEX[name]].text
                       != old_doc.header[FIELD_INDEX[name]].text]
            check("only the target header token differs",
                  not changed and doc.name_texts == old_doc.name_texts,
                  f"unexpected: {changed or 'none'}; "
                  f"layout {old_doc.layout} -> {doc.layout}")
            rep["field"] = {"name": old_spec.name, "value": value,
                            "diff": diff,
                            "changed_bytes": diff["changed_bytes"]}
        else:
            diff = diff_report(old_bytes, data)
            check("the file is byte-identical to the source",
                  data == old_bytes, diff_summary(diff))
            rep["diff"] = diff
            changed = {name: [old, new]
                       for name, old, new in (("mode", old_doc.mode, doc.mode),
                                              ("period", old_doc.period,
                                               doc.period),
                                              ("loop", old_doc.loop, doc.loop))
                       if old != new}
            if changed:
                rep["changed_fields"] = changed
        rep["checks"] = checks
        rep["ok"] = rep["ok"] and all(c["ok"] for c in checks)
    return rep


def print_check(rep: dict) -> None:
    print(f"check: {rep['path']} ({rep['size']} bytes)")
    for check in rep["structure"]["checks"]:
        mark = "ok  " if check["ok"] else "FAIL"
        print(f"  [{mark}] {check['check']}"
              + (f" ({check['detail']})" if check["detail"] else ""))
    for warning in rep["structure"]["warnings"]:
        print(f"  [warn] {warning}")
    if rep.get("against"):
        print(f"against: {rep['against']}")
        for check in rep.get("checks", []):
            mark = "ok  " if check["ok"] else "FAIL"
            print(f"  [{mark}] {check['check']}"
                  + (f" ({check['detail']})" if check["detail"] else ""))
        if rep.get("field"):
            field_rep = rep["field"]
            print(f"field: {field_rep['name']} = {field_rep['value']}, "
                  f"{diff_summary(field_rep['diff'])} "
                  f"({field_rep['changed_bytes']} byte(s) changed)")
        elif rep.get("changed_fields"):
            for name, (old, new) in rep["changed_fields"].items():
                print(f"differs: {name} {old} -> {new} "
                      f"(a --field expectation would verify this edit)")
    elif rep.get("note"):
        print(f"note: {rep['note']}")
    print(f"result: {'OK' if rep['ok'] else 'FAIL'}")


# ---------------------------------------------------------------------------
# corpus modes
# ---------------------------------------------------------------------------

def _scratch_dir(tag: str) -> str:
    base = os.environ.get("SOAWRITE_TMP", "/tmp/opencode")
    try:
        os.makedirs(base, exist_ok=True)
    except OSError:
        base = tempfile.gettempdir()
    return tempfile.mkdtemp(prefix=f"soawrite-{tag}-", dir=base)


def roundtrip_corpus(root: str = DEFAULT_GAME, verbose: bool = False) -> dict:
    if os.path.isfile(root):
        files = [require_soa(root)]
    elif os.path.isdir(root):
        files = corpus_files(root)
    else:
        raise EditError(f"no such directory: {root}")
    if not files:
        raise EditError(f"no .soa files under {root}")
    started = time.perf_counter()
    scratch = _scratch_dir("rt")
    identical = canonical = semantic = rejoined = walked = 0
    loadable = timeline_ok = 0
    failures: list[dict] = []
    names_total = sizes_total = 0
    try:
        for number, path in enumerate(files):
            name = os.path.basename(path)
            try:
                data = read_bytes(path)
                sizes_total += len(data)
                doc = walk(data, path)
                walked += 1
                names_total += len(doc.names)
                loadable += 1 if doc.loadable else 0
                if rejoin(data, doc.lines) == data:
                    rejoined += 1
                model = model_of(data)
                text = model.to_bytes()
                canonical += 1 if b"\r\n".join(
                    line.encode("latin1")
                    for line in model.render_lines()) == data else 0
                target = os.path.join(scratch, f"{number:04d}-{name}")
                write_bytes(target, text)
                stored = read_bytes(target)
                if stored == data:
                    identical += 1
                else:
                    failures.append({
                        "file": path, "why": "write differs",
                        "detail": diff_summary(diff_report(data, stored))})
                    continue
                again = model_of(stored)
                same = ((again.mode, again.period, again.declared_frames,
                         again.loop, again.names)
                        == (doc.mode, doc.period, doc.declared, doc.loop,
                            doc.name_texts))
                if same:
                    semantic += 1
                else:
                    failures.append({"file": path, "why": "semantic re-parse",
                                     "detail": "fields or names differ"})
                if doc.loadable:
                    try:
                        own = [(i.start_tick, i.end_tick)
                               for i in doc_intervals(doc)]
                        theirs = [(t.start_tick, t.end_tick)
                                  for t in oracle_timeline(again)]
                        if own == theirs:
                            timeline_ok += 1
                        else:
                            failures.append({"file": path,
                                             "why": "timeline differs",
                                             "detail": f"{len(own)} vs "
                                                       f"{len(theirs)}"})
                    except (ValueError, ScanError) as exc:
                        failures.append({"file": path, "why": "timeline",
                                         "detail": str(exc)})
            except (EditError, ScanError, ValueError, OSError) as exc:
                failures.append({"file": path, "why": "error",
                                 "detail": str(exc)})
            if verbose and failures and failures[-1]["file"] == path:
                print(f"  FAIL {name}: {failures[-1]['detail']}",
                      file=sys.stderr)
    finally:
        shutil.rmtree(scratch, ignore_errors=True)
    elapsed = time.perf_counter() - started
    total = len(files)
    return {"root": root, "files": total, "identical": identical,
            "bytes": sizes_total, "names": names_total, "loadable": loadable,
            "walked": walked, "rejoined": rejoined,
            "canonical_exact": canonical, "semantic": semantic,
            "timeline_ok": timeline_ok,
            "seconds": round(elapsed, 3), "failures": failures,
            "ok": (identical == total and walked == total
                   and rejoined == total and semantic == total
                   and not failures)}


def print_roundtrip(rep: dict) -> None:
    total = rep["files"]
    print(f"{rep['identical']}/{total} identical, {rep['bytes']} bytes, "
          f"{rep['names']} names, {rep['seconds']:.2f} s ({rep['root']})")
    print(f"  own reader: {rep['walked']}/{total} parsed, line-span re-join "
          f"{rep['rejoined']}/{total}")
    print(f"  semantic re-parse (parse -> to_bytes -> parse): "
          f"{rep['semantic']}/{total}")
    print(f"  engine timeline agrees after the round-trip: "
          f"{rep['timeline_ok']}/{rep['loadable']} loadable file(s)")
    print(f"  canonical render_lines() identical to the shipped file: "
          f"{rep['canonical_exact']}/{total} (the rest keep their layout "
          f"because to_bytes() re-joins the stored lines)")
    if rep["failures"]:
        print(f"  failures: {len(rep['failures'])}")
        for item in rep["failures"][:10]:
            print(f"    {item['file']}: {item['why']} {item['detail']}")
    print(f"result: {'OK' if rep['ok'] else 'FAIL'}")


def _median(values: list[int]) -> float:
    ordered = sorted(values)
    if not ordered:
        return 0.0
    middle = len(ordered) // 2
    if len(ordered) % 2:
        return float(ordered[middle])
    return (ordered[middle - 1] + ordered[middle]) / 2.0


def audit_corpus(root: str = DEFAULT_GAME) -> dict:
    files = corpus_files(root) if os.path.isdir(root) else [require_soa(root)]
    if not files:
        raise EditError(f"no .soa files under {root}")
    modes: dict[int, int] = {}
    periods: dict[int, int] = {}
    sizes: list[int] = []
    declared_total = names_total = 0
    declared_values: list[int] = []
    zero_frames = 0
    zero_files = 0
    min_ticks_per_frame: float | None = None
    loadable = short = truncated = special = 0
    short_files: list[str] = []
    extra_name_files: list[str] = []
    blank_lines = padded_lines = 0
    no_final_crlf = bare_newlines = 0
    layouts: dict[str, int] = {}
    errors: list[str] = []
    canonical = 0
    bytes_total = 0
    for path in files:
        try:
            data = read_bytes(path)
            bytes_total += len(data)
            sizes.append(len(data))
            doc = walk(data, path)
            modes[doc.mode] = modes.get(doc.mode, 0) + 1
            periods[doc.period] = periods.get(doc.period, 0) + 1
            declared_total += doc.declared
            declared_values.append(doc.declared)
            names_total += len(doc.names)
            loadable += 1 if doc.loadable else 0
            if doc.short:
                short += 1
                short_files.append(os.path.relpath(path, root)
                                   if os.path.isdir(root)
                                   else os.path.basename(path))
            if doc.truncated:
                truncated += 1
                extra_name_files.append(os.path.relpath(path, root)
                                        if os.path.isdir(root)
                                        else os.path.basename(path))
            special += 1 if doc.special else 0
            blank_lines += doc.blank_lines
            padded_lines += doc.padded_lines
            no_final_crlf += 1 if doc.final_eol != CRLF else 0
            bare_newlines += doc.bare_lf + doc.bare_cr
            layouts[doc.layout] = layouts.get(doc.layout, 0) + 1
            if doc.loadable and not doc.special:
                intervals = doc_intervals(doc)
                zeros = sum(1 for i in intervals
                            if i.duration_ticks == 0)
                if zeros:
                    zero_frames += zeros
                    zero_files += 1
                per_frame = doc.period_ticks / doc.declared
                if min_ticks_per_frame is None or per_frame < min_ticks_per_frame:
                    min_ticks_per_frame = per_frame
            model = model_of(data)
            if b"\r\n".join(line.encode("latin1")
                            for line in model.render_lines()) == data:
                canonical += 1
        except (EditError, ScanError, ValueError, OSError) as exc:
            errors.append(f"{path}: {exc}")

    def top(mapping: dict, count: int = 8) -> list[dict]:
        items = sorted(mapping.items(), key=lambda item: (-item[1], item[0]))
        return [{"value": k, "files": v} for k, v in items[:count]]

    return {
        "root": root, "files": len(files), "bytes": bytes_total,
        "modes": [{"mode": k, "files": modes[k]} for k in sorted(modes)],
        "mode_files": {str(k): modes[k] for k in sorted(modes)},
        "frames_declared": declared_total,
        "names": names_total,
        "frames_per_file": {"min": min(declared_values, default=0),
                            "max": max(declared_values, default=0),
                            "median": _median(declared_values)},
        "frames_zero_delay": zero_frames,
        "files_with_zero_delay": zero_files,
        "min_ticks_per_frame": (round(min_ticks_per_frame, 3)
                                if min_ticks_per_frame is not None else None),
        "periods": {"distinct": len(periods),
                    "min": min(periods, default=0),
                    "max": max(periods, default=0),
                    "top": top(periods)},
        "sizes": {"min": min(sizes, default=0), "max": max(sizes, default=0),
                  "median": _median(sizes)},
        "loadable": loadable, "short": short, "truncated": truncated,
        "short_files": short_files, "extra_name_files": extra_name_files,
        "special_modes": special,
        "blank_lines": blank_lines, "padded_lines": padded_lines,
        "files_without_final_crlf": no_final_crlf,
        "bare_newlines": bare_newlines,
        "header_layouts": [{"layout": k, "files": layouts[k]}
                           for k in sorted(layouts)],
        "canonical_render_lines": canonical,
        "errors": errors, "ok": not errors,
    }


def print_audit(rep: dict) -> None:
    print(f"audit: {rep['files']} files, {rep['bytes']} bytes, "
          f"{rep['frames_declared']} declared frames, {rep['names']} name "
          f"tokens ({rep['root']})")
    print("  modes: " + ", ".join(f"{item['mode']}={item['files']}"
                                  for item in rep["modes"]))
    f = rep["frames_per_file"]
    print(f"  frames/file: min {f['min']}, median {f['median']:g}, "
          f"max {f['max']}; loadable {rep['loadable']}/{rep['files']} "
          f"(short {rep['short']}, extra names {rep['truncated']}, "
          f"special modes {rep['special_modes']})")
    print(f"  frames with zero delay: {rep['frames_zero_delay']} in "
          f"{rep['files_with_zero_delay']} file(s); min ticks/frame "
          f"{rep['min_ticks_per_frame']}")
    if rep["short_files"]:
        print("  engine refuses (declared > names): "
              + ", ".join(rep["short_files"]))
    if rep["extra_name_files"]:
        print("  extra names the loader never reads (declared < names): "
              + ", ".join(rep["extra_name_files"]))
    periods = rep["periods"]
    print(f"  period: {periods['distinct']} distinct, {periods['min']}.."
          f"{periods['max']} cs; top " + ", ".join(
              f"{item['value']}={item['files']}" for item in periods["top"]))
    sizes = rep["sizes"]
    print(f"  size: min {sizes['min']}, median {sizes['median']:g}, max "
          f"{sizes['max']} bytes; blank lines {rep['blank_lines']}, padded "
          f"lines {rep['padded_lines']}, files without final CRLF "
          f"{rep['files_without_final_crlf']}, bare LF/CR {rep['bare_newlines']}")
    print("  header layouts: " + ", ".join(
        f"'{item['layout']}'={item['files']}" for item in rep["header_layouts"]))
    print(f"  canonical render_lines() identical: "
          f"{rep['canonical_render_lines']}/{rep['files']}")
    if rep["errors"]:
        print(f"  errors: {len(rep['errors'])}")
        for line in rep["errors"][:5]:
            print(f"    {line}")
    print(f"result: {'OK' if rep['ok'] else 'FAIL'}")


# ---------------------------------------------------------------------------
# self-test (no game data)
# ---------------------------------------------------------------------------

SYNTH = (b"1 20 4 1\r\n"
         b"baby1\r\n"
         b"baby2\r\n"
         b"baby3\r\n"
         b"baby4\r\n")

SYNTH_MESSY = (b"0 600 3 1\n"
               b"one   \r\n"
               b"\r\n"
               b"two\n"
               b"three")

SYNTH_SPECIAL = (b"100 1 2 0\r\n"
                 b"DCLOCK1\r\n"
                 b"dclocksm\r\n")


def selftest() -> tuple[bool, list[dict]]:
    results: list[dict] = []

    def check(name: str, ok: bool, detail: str = "") -> None:
        results.append({"check": name, "ok": bool(ok), "detail": detail})

    def expect_error(name: str, fn, kind=EditError) -> None:
        try:
            fn()
        except kind as exc:
            check(name, True, str(exc))
        except Exception as exc:                        # noqa: BLE001
            check(name, False, f"wrong exception {type(exc).__name__}: {exc}")
        else:
            check(name, False, "no error raised")

    # 1. lines and spans
    doc = walk(SYNTH, "SYNTH.SOA")
    check("lines: CRLF split gives five lines", len(doc.lines) == 5,
          f"{len(doc.lines)} line(s)")
    check("lines: re-join is byte-exact", rejoin(SYNTH, doc.lines) == SYNTH)
    check("lines: header spans slice back",
          [SYNTH[t.start:t.end].decode() for t in doc.header]
          == ["1", "20", "4", "1"])
    check("lines: header values", (doc.mode, doc.period, doc.declared,
                                   doc.loop) == (1, 20, 4, 1))
    check("lines: names in order", doc.name_texts == [
        "baby1", "baby2", "baby3", "baby4"])
    check("lines: layout string", doc.layout == "x xx x x", doc.layout)
    check("lines: period_ticks = 20*91/100", doc.period_ticks == 18,
          str(doc.period_ticks))

    # 2. tolerant layout
    messy = walk(SYNTH_MESSY, "MESSY.SOA")
    check("layout: bare LF and unterminated last line are preserved",
          rejoin(SYNTH_MESSY, messy.lines) == SYNTH_MESSY
          and messy.bare_lf == 2 and messy.final_eol == b"")
    check("layout: blank lines are skipped for names, kept for bytes",
          messy.name_texts == ["one", "two", "three"]
          and messy.blank_lines == 1, str(messy.name_texts))
    check("layout: a padded name line is reported",
          messy.padded_lines == 1, str(messy.padded_lines))
    check("layout: oracle agrees on the messy file",
          model_of(SYNTH_MESSY).names == messy.name_texts)

    # 3. engine timeline (independent of the oracle)
    own = doc_intervals(doc)
    theirs = oracle_timeline(model_of(SYNTH))
    check("timeline: 4 frames, 18 ticks, own == oracle",
          [(i.start_tick, i.end_tick) for i in own]
          == [(t.start_tick, t.end_tick) for t in theirs]
          and doc.period_ticks == 18,
          str([(i.start_tick, i.end_tick) for i in own]))
    one_shot = walk(b"0 20 4 1\r\nA\r\nB\r\nC\r\nD\r\n", "X.SOA")
    check("timeline: mode 0 last interval is open ended",
          doc_intervals(one_shot)[-1].duration_ticks is None
          and doc_intervals(one_shot)[0].duration_ticks == 5)
    zero = walk(b"1 2 32 1\r\n" + b"\r\n".join(
        f"n{k}".encode() for k in range(32)) + b"\r\n", "Z.SOA")
    check("timeline: a short period gives zero-length frames",
          any(i.duration_ticks == 0 for i in doc_intervals(zero)),
          f"{sum(1 for i in doc_intervals(zero) if i.duration_ticks == 0)} "
          f"frame(s) of 32")
    special = walk(SYNTH_SPECIAL, "S.SOA")
    check("timeline: special mode 100 is a single held frame",
          doc_intervals(special)[0].duration_ticks is None
          and special.period_ticks == 0 and special.loadable)

    # 4. splice identity over every header token
    identity = splice(SYNTH, [(t, t.text) for t in doc.header])
    check("splice: replacing every header token by itself is a no-op",
          identity == SYNTH)

    # 5. edits change only the target token
    for name, value in (("period", 30), ("period", 150), ("mode", 0),
                        ("loop", 0)):
        spec = resolve_field(doc, name)
        validate(spec, value, doc)
        new_bytes, _ = patch(doc, spec, value)
        report = verify_edit(doc, new_bytes, spec, value)
        check(f"edit: {name}={value} verified", report["ok"],
              "; ".join(c["detail"] for c in report["checks"] if not c["ok"]))

    spec = resolve_field(doc, "period")
    new_bytes, _ = patch(doc, spec, 30)
    rep = changed_bytes(doc, spec, new_bytes)
    check("edit: period 20 -> 30 changes one byte, size unchanged",
          rep["changed_bytes"] == 1 and rep["size_delta"] == 0,
          f"{rep['changed_bytes']} byte(s), {rep['size_delta']:+d}")
    new_bytes, _ = patch(doc, spec, 150)
    rep = changed_bytes(doc, spec, new_bytes)
    check("edit: period 20 -> 150 changes bytes and grows the file",
          rep["changed_bytes"] >= 2 and rep["size_delta"] == 1,
          f"{rep['changed_bytes']} byte(s), {rep['size_delta']:+d}")
    check("edit: the source document is untouched",
          walk(SYNTH, "SYNTH.SOA") == doc)
    same, _ = patch(doc, spec, 20)
    check("edit: writing the current value is a byte-exact no-op",
          same == SYNTH and verify_edit(doc, same, spec, 20)["ok"])
    loop = resolve_field(doc, "loop")
    loop_bytes, _ = patch(doc, loop, 0)
    check("edit: loop is one token, the names stay put",
          walk(loop_bytes, "L.SOA").name_texts == doc.name_texts)

    # 6. --delay: cycle period for an exact frame hold
    info = delay_to_period(doc, 0, 54)
    check("delay: 54 ms on frame 0 of 4 -> period 19 cs",
          info["period"] == 19 and info["held_ticks"] == 5
          and info["held_ms"] == 54, str(info))
    spec = resolve_field(doc, "period")
    delayed, _ = patch(doc, spec, info["period"])
    report = verify_edit(doc, delayed, spec, info["period"], info)
    check("delay: the edited file holds frame 0 for 54 ms", report["ok"],
          "; ".join(c["detail"] for c in report["checks"] if not c["ok"]))
    check("delay: the oracle sees the same hold",
          oracle_timeline(model_of(delayed))[0].duration_ms == 54)
    expect_error("refuse: an unrepresentable delay (45 ms of 4 frames)",
                 lambda: delay_to_period(doc, 0, 45))
    expect_error("refuse: delay on a special mode",
                 lambda: delay_to_period(special, 0, 100))
    expect_error("refuse: frame index out of range",
                 lambda: delay_to_period(doc, 4, 54))
    expect_error("refuse: zero delay",
                 lambda: delay_to_period(doc, 0, 0))

    # 7. refusals
    expect_error("refuse: declared is not editable",
                 lambda: resolve_field(doc, "declared"))
    expect_error("refuse: unknown field", lambda: resolve_field(doc, "speed"))
    expect_error("refuse: mode out of range",
                 lambda: validate(resolve_field(doc, "mode"), 103, doc))
    expect_error("refuse: loop out of range",
                 lambda: validate(resolve_field(doc, "loop"), 2, doc))
    expect_error("refuse: period 0 for a normal mode",
                 lambda: validate(resolve_field(doc, "period"), 0, doc))
    expect_error("refuse: period above the cap",
                 lambda: validate(resolve_field(doc, "period"), 40000, doc))
    expect_error("refuse: non-integer assignment",
                 lambda: parse_assignment(doc, "period=abc"))
    expect_error("refuse: assignment without =",
                 lambda: parse_assignment(doc, "period"))
    expect_error("refuse: non-.soa input", lambda: require_soa("/tmp/x.txt"))
    expect_error("refuse: empty file", lambda: walk(b""), ScanError)
    expect_error("refuse: header with three numbers",
                 lambda: walk(b"1 20 4\r\nA\r\n"), ScanError)
    expect_error("refuse: non-integer header",
                 lambda: walk(b"1 x 4 1\r\nA\r\n"), ScanError)
    expect_error("refuse: declared 0",
                 lambda: walk(b"1 20 0 1\r\nA\r\n"), ScanError)
    expect_error("refuse: declared 33",
                 lambda: walk(b"1 20 33 1\r\n" + b"\r\n".join(
                     f"n{k}".encode() for k in range(33)) + b"\r\n"), ScanError)
    expect_error("refuse: no names", lambda: walk(b"1 20 4 1\r\n"), ScanError)
    expect_error("refuse: implausible period",
                 lambda: walk(b"1 99999 1 1\r\nA\r\n"), ScanError)
    expect_error("refuse: writing into the game directory",
                 lambda: refuse_unsafe_out(
                     os.path.join(DEFAULT_GAME, "anims", "X.soa"), DEFAULT_GAME))
    expect_error("refuse: writing into the mirror",
                 lambda: refuse_unsafe_out(
                     os.path.join(MIRROR, "anims", "X.soa"), DEFAULT_GAME))
    expect_error("refuse: writing into the repository",
                 lambda: refuse_unsafe_out(
                     os.path.join(ROOT, "anims", "X.soa"), DEFAULT_GAME))
    expect_error("refuse: --out without .soa",
                 lambda: refuse_unsafe_out("/tmp/opencode/x.txt", DEFAULT_GAME))
    work = _scratch_dir("refuse")
    try:
        check("paths: a scratch output is allowed",
              refuse_unsafe_out(os.path.join(work, "X.soa"), DEFAULT_GAME) is None)
    finally:
        shutil.rmtree(work, ignore_errors=True)

    # 8. set writes only --out, never the source
    work = _scratch_dir("set")
    try:
        src = os.path.join(work, "SYNTH.soa")
        out = os.path.join(work, "SYNTH-edited.soa")
        write_bytes(src, SYNTH)
        before = read_bytes(src)
        rep = set_field(src, "period=30", out, game=DEFAULT_GAME)
        check("set: writes --out and keeps the source",
              read_bytes(src) == before and read_bytes(out) != before,
              f"{rep['changed_bytes']} byte(s)")
        check("set: the report is verified",
              rep["ok"] and rep["changed_bytes"] == 1)
        rep = set_field(src, None, os.path.join(work, "delayed.soa"),
                        game=DEFAULT_GAME, frame=1, delay=54)
        check("set: --frame/--delay writes the computed period",
              rep["ok"] and rep["after"] == 21
              and rep["delay"]["held_ms"] == 54, str(rep["after"]))
        expect_error("set: refuses --out == input",
                     lambda: set_field(src, "period=30", src))
        expect_error("set: refuses --field with --delay",
                     lambda: set_field(src, "period=30",
                                       os.path.join(work, "x.soa"),
                                       frame=0, delay=54))
        rep = check_file(src, src, None, game=DEFAULT_GAME)
        check("check: identical file + --against is OK", rep["ok"])
        rep = check_file(out, src, "period=30", game=DEFAULT_GAME)
        check("check: edited file + --field is OK", rep["ok"],
              "; ".join(c["detail"] for c in rep.get("checks", [])
                        if not c["ok"]))
        rep = check_file(out, src, "period=31", game=DEFAULT_GAME)
        check("check: a wrong expected value is caught", not rep["ok"])
        rep = check_file(out, src, None, game=DEFAULT_GAME)
        check("check: the byte difference is reported without --field",
              not rep["ok"] and rep["diff"]["changed_bytes"] == 1,
              str(rep.get("diff")))
        broken = os.path.join(work, "BROKEN.soa")
        write_bytes(broken, b"1 20 4 1\r\nonly-one\r\n")
        rep = check_file(broken, None, None, game=DEFAULT_GAME)
        check("check: a short file is structurally valid but not loadable",
              rep["structure"]["ok"] and rep["ok"]
              and not rep["structure"]["loadable"])
        with contextlib.redirect_stdout(io.StringIO()):
            code = main(["--check", broken, "--strict"])
        check("check: --strict fails a file the engine would refuse", code == 1,
              f"exit {code}")
        bad_header = os.path.join(work, "BAD.soa")
        write_bytes(bad_header, b"1 20\r\nA\r\n")
        expect_error("check: a broken header is refused",
                     lambda: check_file(bad_header, None, None,
                                        game=DEFAULT_GAME), ScanError)
    finally:
        shutil.rmtree(work, ignore_errors=True)

    # 9. round-trip and audit on a synthetic corpus
    work = _scratch_dir("rt")
    try:
        write_bytes(os.path.join(work, "A.soa"), SYNTH)
        write_bytes(os.path.join(work, "B.SOA"), SYNTH_MESSY)
        write_bytes(os.path.join(work, "C.soa"), SYNTH_SPECIAL)
        rep = roundtrip_corpus(work)
        check("roundtrip: 3/3 synthetic files are byte-identical",
              rep["identical"] == 3 and rep["files"] == 3 and rep["ok"],
              f"{rep['identical']}/{rep['files']}, {rep['bytes']} bytes")
        audit = audit_corpus(work)
        check("audit: the synthetic corpus is counted",
              audit["files"] == 3 and audit["frames_declared"] == 9
              and audit["names"] == 9 and audit["ok"],
              f"{audit['frames_declared']} frames, {audit['names']} names")
        rep = list_file(os.path.join(work, "A.soa"))
        check("list: 4 frames with local ids and ms holds",
              [item["local_id"] for item in rep["frames"]] == [1, 2, 3, 4]
              and rep["frames"][0]["ticks"] == 5
              and rep["frames"][0]["ms"] == 54,
              str([(i["ticks"], i["ms"]) for i in rep["frames"]]))
    finally:
        shutil.rmtree(work, ignore_errors=True)

    ok = all(item["ok"] for item in results)
    return ok, results


def print_selftest(ok: bool, results: list[dict]) -> None:
    for item in results:
        mark = "ok  " if item["ok"] else "FAIL"
        print(f"  [{mark}] {item['check']}"
              + (f" ({item['detail']})" if item["detail"] else ""))
    passed = sum(1 for item in results if item["ok"])
    print(f"self-test: {passed}/{len(results)} passed")
    print(f"result: {'OK' if ok else 'FAIL'}")


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------

def _add_methods(ap) -> None:
    ap.add_argument("file", nargs="?",
                    help=".soa file (or DIR for --roundtrip/--audit)")
    ap.add_argument("--list", action="store_true", help="header + timeline")
    ap.add_argument("--get", action="store_true", help="read one header field")
    ap.add_argument("--set", action="store_true",
                    help="edit one header field into --out")
    ap.add_argument("--check", action="store_true",
                    help="structure + byte comparison")
    ap.add_argument("--roundtrip", action="store_true",
                    help="read -> write -> compare every .soa under DIR/game")
    ap.add_argument("--audit", action="store_true", help="corpus statistics")
    ap.add_argument("--self-test", action="store_true",
                    help="synthetic tests, no game data")
    ap.add_argument("--against", metavar="FILE",
                    help="--check: source file to compare with")
    ap.add_argument("--field", metavar="NAME[=VALUE]",
                    help="mode|period|loop (--set/--check need NAME=VALUE)")
    ap.add_argument("--period", type=int, metavar="CS",
                    help="--set/--check shorthand for --field period=CS")
    ap.add_argument("--frame", type=int, metavar="N",
                    help="--set: frame whose delay is requested (0-based)")
    ap.add_argument("--delay", type=int, metavar="MS",
                    help="--set: requested hold time of --frame in ms")
    ap.add_argument("--out", metavar="PATH", help="--set: output .soa")
    ap.add_argument("--game", default=os.environ.get("GUNMETAL", DEFAULT_GAME),
                    help="game installation (default %s)" % DEFAULT_GAME)
    ap.add_argument("--mirror", action="store_true",
                    help="--roundtrip/--audit: use the regress mirror as root")
    ap.add_argument("--strict", action="store_true",
                    help="--check: also fail when the engine would refuse")
    ap.add_argument("--limit", type=int, default=0,
                    help="--list: show first/last N frames (0 = all)")
    ap.add_argument("--json", action="store_true", help="JSON report")


def main(argv=None) -> int:
    argv = list(sys.argv[1:] if argv is None else argv)
    if argv:
        head = argv[0].lower()
        aliases = {
            "self-test": "--self-test", "selftest": "--self-test",
            "roundtrip": "--roundtrip", "audit": "--audit",
            "list": "--list", "get": "--get", "set": "--set", "check": "--check",
        }
        if head in aliases:
            argv = [aliases[head]] + argv[1:]
    ap = argparse.ArgumentParser(
        description="Layout-preserving .soa writer/editor (M11.5, slice 4).")
    _add_methods(ap)
    args = ap.parse_args(argv)

    modes = [args.list, args.get, args.set, args.check, args.roundtrip,
             args.audit, args.self_test]
    if sum(bool(m) for m in modes) != 1:
        ap.print_help()
        return 2
    try:
        if args.self_test:
            ok, results = selftest()
            if args.json:
                print(json.dumps({"check": "self-test", "ok": ok,
                                  "passed": sum(1 for r in results if r["ok"]),
                                  "total": len(results), "results": results},
                                 indent=1, ensure_ascii=False))
            else:
                print_selftest(ok, results)
            return 0 if ok else 1

        root = args.file or args.game
        if args.mirror:
            if not os.path.isdir(MIRROR):
                raise EditError(f"the mirror does not exist: {MIRROR}")
            if args.file is None:
                root = MIRROR

        if args.roundtrip:
            rep = roundtrip_corpus(root, verbose=False)
            if args.json:
                print(json.dumps(rep, indent=1, ensure_ascii=False))
            else:
                print_roundtrip(rep)
            return 0 if rep["ok"] else 1

        if args.audit:
            rep = audit_corpus(root)
            if args.json:
                print(json.dumps(rep, indent=1, ensure_ascii=False))
            else:
                print_audit(rep)
            return 0 if rep["ok"] else 1

        if not args.file:
            raise EditError("this mode needs a .soa file")

        if args.list:
            rep = list_file(args.file, max(args.limit, 0), args.game)
            if args.json:
                print(json.dumps(rep, indent=1, ensure_ascii=False))
            else:
                print_list(rep, max(args.limit, 0))
            return 0 if rep["structure"]["ok"] else 1

        if args.get:
            if not args.field:
                raise EditError("--get needs --field NAME")
            rep = get_field(args.file, args.field, args.game)
            if args.json:
                print(json.dumps(rep, indent=1, ensure_ascii=False))
            else:
                print_get(rep)
            return 0

        if args.set:
            if not args.out:
                raise EditError("--set needs --out FILE")
            assignment = args.field
            if args.period is not None:
                if assignment is not None:
                    raise EditError("use either --period or --field, not both")
                assignment = f"period={args.period}"
            if (assignment is None) == (args.delay is None):
                raise EditError("--set needs either --period CS, --field "
                                "NAME=VALUE or --frame N --delay MS")
            rep = set_field(args.file, assignment, args.out, args.game,
                            frame=args.frame, delay=args.delay)
            if args.json:
                print(json.dumps(rep, indent=1, ensure_ascii=False))
            else:
                print_set(rep)
            return 0 if rep["ok"] else 1

        if args.check:
            field = args.field
            if args.period is not None:
                if field is not None:
                    raise EditError("use either --period or --field, not both")
                field = f"period={args.period}"
            rep = check_file(args.file, args.against, field, args.game)
            if args.json:
                print(json.dumps(rep, indent=1, ensure_ascii=False))
            else:
                print_check(rep)
            if args.strict and not rep["structure"]["loadable"]:
                return 1
            return 0 if rep["ok"] else 1

        ap.print_help()
        return 2
    except EditError as exc:
        print(f"soawrite: {exc}", file=sys.stderr)
        return 2
    except ScanError as exc:
        print(f"soawrite: corrupt .soa: {exc}", file=sys.stderr)
        return 2
    except (ValueError, OSError) as exc:
        print(f"soawrite: {exc}", file=sys.stderr)
        return 2
    except Exception as exc:                            # noqa: BLE001
        print(f"soawrite: internal error: {type(exc).__name__}: {exc}",
              file=sys.stderr)
        return 3


if __name__ == "__main__":
    sys.exit(main())
