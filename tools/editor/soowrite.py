#!/usr/bin/env python3
"""Layout-preserving writer/editor for Gunmetal ``.soo`` model animations
(``M11.5``, slice 6).

``gm/soo.py`` (``M1.3``, ``docs/SOO.md``) reads every shipped ``.soo`` and
``Soo.to_bytes()`` re-joins the parsed fields into text: the byte-exact
round-trip of the 40 shipped files is 40/40 (``docs/SOO_WRITE.md``).  What that
writer cannot do is *edit* one field without rebuilding the whole text from the
fields -- and the layout is not canonical: three files put ``mode count`` on one
line, three files have no final newline, 13 files carry trailing blank lines,
and separators are whatever the level designer typed.

This tool adds the missing half: an edit **rewrites only the bytes of the
edited token** (``mode`` or one frame duration).  ``.soo`` is a plain-text
flipbook descriptor -- two header numbers, then ``<pose name> <ticks>`` lines --
so a field lives in exactly one whitespace-delimited token, and replacing that
token in place leaves every other byte (CRLF endings, combined header, blank
lines, the missing final newline) untouched.  Only the width of the new number
can change the file length (``100`` -> ``200`` keeps it, ``100`` -> ``1000``
adds one byte).

What the loader really reads (``0x42b4e1``, proven; ``docs/SOO.md`` §2)
-----------------------------------------------------------------------
* two integers ``mode count`` via ``fscanf("%ld %ld")``;
* exactly ``count`` frames via ``fscanf("%s %ld")`` -- the rest of the file is
  never consumed (``0x42b637``), and ``1 <= count <= 0x20`` (``0x42b5f1``);
* the player (``0x433e00``) knows only ``mode`` 0 (one-shot, last frame held,
  ``0x433e8b``) and 1 (loop, ``0x433eb4``); durations are 91 Hz engine ticks.

So ``mode`` and every frame duration are proven, single-token fields.  ``count``
is **not** editable: it is the number of poses the loader reads, so changing it
adds or removes frames.  Frame names are references to ``SOBS/<name>.sob`` and
are not edited either.

What is proven here
-------------------
* ``--roundtrip`` reads every shipped ``.soo``, re-joins it from this tool's own
  line spans, writes it back through the oracle into a scratch directory, reads
  the bytes from disk and compares them with the original: **40/40
  byte-identical**.  It also re-parses semantically and compares the engine
  timeline.
* ``--audit`` measures the corpus (modes, counts, dead lines, durations, sizes,
  layout quirks, missing pose models) -- numbers, not guesses.
* ``--set`` edits ``mode`` or a frame duration and proves that only the target
  token changed (size delta, line count, every other token, all bytes outside
  the span, own reader + oracle + engine timeline).

Command surface
---------------
``--list FILE``            header, per-frame ticks/ms/start and ``.sob`` status.
``--get FILE --field NAME`` one proven field with its byte span.
``--set FILE --field NAME=VALUE --out OUT``
                           token-level edit; refuses the game/mirror/repo.
``--check FILE [--against FILE] [--field NAME=VALUE]``
                           structure + byte comparison.
``--roundtrip [DIR]``      read -> write -> compare every ``.soo`` under DIR
                           (default: the game installation).
``--audit [DIR]``          corpus statistics.
``--self-test``            synthetic tests, no game data.
``--json``                 machine-readable report for every mode.

Fields: ``mode`` (0/1), ``count`` (read-only), ``durN`` (frame ``N``, 0-based;
``dur[N]``/``durationN``/``frameN`` are accepted spellings).

Exit codes: ``0`` success, ``1`` verification failed, ``2`` bad input or a
refused edit (message on stderr), ``3`` internal error.  No traceback escapes
:func:`main`.
"""
from __future__ import annotations

import argparse
import json
import os
import re
import shutil
import sys
import tempfile
import time
from dataclasses import dataclass

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
if ROOT not in sys.path:
    sys.path.insert(0, ROOT)

from gm import soo as oracle                      # noqa: E402

DEFAULT_GAME = "/mnt/d/games/Gunmetal 3dfx"
DEFAULT_OUT_DIR = "/tmp/opencode/soowrite"
MIRROR = os.path.expanduser("~/.cache/gunmetal-re/game")
SIBLING_REPO = os.path.expanduser("~/gunmetal-re")

#: The loader refuses more than 32 frames (``0x42b5f1``: ``1 <= count <= 0x20``).
MAX_FRAMES = 32

#: Shipped modes are only 0 and 1; the player normalises exactly these two and
#: runs off the frame array for anything else, so the tool stays inside them.
MODE_MIN, MODE_MAX = 0, 1

#: Durations are 32-bit engine ticks.  Shipped range is 9..6000; the cap only
#: stops silly input.  Zero is refused separately (``0x433f24`` divides by it).
DURATION_MIN, DURATION_MAX = 1, 65535

#: ``mode`` (0/1), ``count`` (read-only) and per-frame durations.
FIELD_RE = re.compile(r"^(?:dur|duration|frame)(?:\[(\d+)\]|(\d+))$", re.I)

CRLF = b"\r\n"


class EditError(Exception):
    """Bad input or a refused edit; the message is meant for the user."""


class ScanError(Exception):
    """The bytes do not walk as a ``.soo`` (own reader)."""


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
    def span(self) -> tuple[int, int]:
        return (self.start, self.end)


@dataclass(frozen=True)
class Token:
    """A whitespace-delimited token with its absolute byte span."""

    text: str
    start: int
    end: int
    line: int

    @property
    def span(self) -> tuple[int, int]:
        return (self.start, self.end)


def split_lines(data: bytes) -> list[Line]:
    """Split on CRLF / LF / CR, keeping the exact terminator of every line.

    ``.soo`` is a DOS text file, but the reader is deliberately tolerant of a
    stray LF or CR so a corrupt layout is *reported* instead of silently
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
# own reader (independent of gm/soo.py, same grammar)
# ---------------------------------------------------------------------------

@dataclass(frozen=True)
class Frame:
    """One ``<name> <duration>`` line with both token spans."""

    name: Token
    duration: Token
    value: int
    line: int


@dataclass
class SooDoc:
    """A walked ``.soo``: exact lines, header tokens, frame tokens."""

    path: str
    data: bytes
    lines: list[Line]
    header: list[Token]              # [mode, count]
    combined: bool                   # "mode count" on one physical line
    frames: list[Frame]
    blanks: list[int]                # indices of blank lines
    interior_blanks: list[int]       # blank lines before the last frame

    # -- header -----------------------------------------------------------
    @property
    def mode(self) -> int:
        return int(self.header[0].text)

    @property
    def count(self) -> int:
        return int(self.header[1].text)

    @property
    def size(self) -> int:
        return len(self.data)

    @property
    def names(self) -> list[str]:
        return [f.name.text for f in self.frames]

    @property
    def durations(self) -> list[int]:
        return [f.value for f in self.frames]

    @property
    def total(self) -> int:
        """Sum of *all* listed frames, in 91 Hz ticks."""
        return sum(f.value for f in self.frames)

    # -- engine view ------------------------------------------------------
    @property
    def engine_frames(self) -> list[Frame]:
        """The frames the loader actually reads (first ``count``)."""
        if self.count <= 0:
            return []
        return self.frames[:self.count]

    @property
    def dropped_frames(self) -> list[Frame]:
        """Listed frames after ``count`` -- never read by the engine."""
        if self.count <= 0:
            return list(self.frames)
        return self.frames[self.count:]

    @property
    def short(self) -> bool:
        """Fewer listed frames than the declared count (the loader fails)."""
        return len(self.frames) < self.count

    @property
    def truncated(self) -> bool:
        """More listed frames than the engine reads (dead data)."""
        return len(self.frames) > self.count

    @property
    def engine_duration(self) -> int:
        return sum(f.value for f in self.engine_frames)

    @property
    def mode_known(self) -> bool:
        return self.mode in (0, 1)

    @property
    def count_in_range(self) -> bool:
        return 1 <= self.count <= MAX_FRAMES

    @property
    def grammar_loadable(self) -> bool:
        """The loader's own checks pass (count range, enough frames, modes)."""
        return (self.count_in_range and not self.short and bool(self.frames)
                and self.mode_known)


def _int_token(token: Token, what: str) -> int:
    try:
        return int(token.text)
    except ValueError:
        raise ScanError(
            f"line {token.line + 1}: {what} is not an integer: "
            f"{token.text!r}") from None


def walk(data: bytes, path: str = "") -> SooDoc:
    """Parse ``.soo`` bytes into exact spans; independent of the oracle.

    Grammar: two header integers (split over two lines or on one line), then
    one ``<name> <duration>`` pair per non-blank line.  Blank lines are only
    accepted at the end -- anything else cannot be re-serialised by
    ``Soo.to_bytes()`` and is reported instead of dropped.
    """
    if not data:
        raise ScanError("empty file (a .soo needs a mode and a count)")
    lines = split_lines(data)
    tokens = [tokenize_line(line, data) for line in lines]
    rows = [(line, toks) for line, toks in zip(lines, tokens) if toks]
    if not rows:
        raise ScanError("no tokens (a .soo needs a mode and a count)")

    first_line, first = rows[0]
    if len(first) == 1:
        if len(rows) < 2:
            raise ScanError("header needs a count on the second line")
        second_line, second = rows[1]
        if len(second) != 1:
            raise ScanError(
                f"line {second_line.index + 1}: the count line holds "
                f"{len(second)} token(s), expected one")
        combined = False
        header = [first[0], second[0]]
        rest = rows[2:]
    elif len(first) == 2:
        combined = True
        header = list(first)
        rest = rows[1:]
    else:
        raise ScanError(
            f"line {first_line.index + 1}: the header holds {len(first)} "
            f"token(s), expected 'mode' + 'count' or 'mode count'")

    mode = _int_token(header[0], "mode")
    count = _int_token(header[1], "count")
    if mode < -2**31 or mode > 2**31 - 1:
        raise ScanError(f"mode is outside the 32-bit field: {mode}")
    if count < -2**31 or count > 2**31 - 1:
        raise ScanError(f"count is outside the 32-bit field: {count}")

    frames: list[Frame] = []
    for line, toks in rest:
        if len(toks) != 2:
            raise ScanError(
                f"line {line.index + 1}: {len(toks)} token(s), a frame needs "
                f"'name duration'")
        value = _int_token(toks[1], f"duration of {toks[0].text!r}")
        if value < -2**31 or value > 2**31 - 1:
            raise ScanError(
                f"line {line.index + 1}: duration is outside the 32-bit "
                f"field: {value}")
        frames.append(Frame(toks[0], toks[1], value, line.index))

    blanks = [line.index for line, toks in zip(lines, tokens) if not toks]
    last_frame_line = frames[-1].line if frames else -1
    interior = [i for i in blanks if i < last_frame_line]
    return SooDoc(path=path, data=data, lines=lines, header=header,
                  combined=combined, frames=frames, blanks=blanks,
                  interior_blanks=interior)


def parse_file(path: str) -> SooDoc:
    return walk(read_bytes(path), path)


# ---------------------------------------------------------------------------
# engine timeline (own view, mirrors gm/soo.py's Soo.timeline)
# ---------------------------------------------------------------------------

@dataclass(frozen=True)
class Interval:
    """One effective frame interval; all times are 91 Hz ticks."""

    index: int
    name: str
    duration: int
    start: int
    end: int
    next_index: int

    @property
    def duration_ms(self) -> float:
        return self.duration * 1000.0 / oracle.ENGINE_TICK_HZ

    @property
    def start_ms(self) -> float:
        return self.start * 1000.0 / oracle.ENGINE_TICK_HZ


def doc_intervals(doc: SooDoc) -> list[Interval]:
    """Engine timeline of the frames the loader reads (empty if not loadable)."""
    frames = doc.engine_frames
    if not frames or not doc.mode_known:
        return []
    out: list[Interval] = []
    t = 0
    n = len(frames)
    for i, frame in enumerate(frames):
        if i == n - 1:
            nxt = 0 if doc.mode == 1 else i
        else:
            nxt = i + 1
        out.append(Interval(i, frame.name.text, frame.value, t,
                            t + frame.value, nxt))
        t += frame.value
    return out


def oracle_intervals(model) -> list[Interval]:
    """The same timeline computed from a ``gm.soo.Soo`` model."""
    frames = model.engine_frames
    if not frames or model.mode not in (0, 1):
        return []
    out: list[Interval] = []
    t = 0
    n = len(frames)
    for i, frame in enumerate(frames):
        if i == n - 1:
            nxt = 0 if model.mode == 1 else i
        else:
            nxt = i + 1
        out.append(Interval(i, frame.name, frame.duration, t,
                            t + frame.duration, nxt))
        t += frame.duration
    return out


# ---------------------------------------------------------------------------
# fields
# ---------------------------------------------------------------------------

@dataclass(frozen=True)
class FieldSpec:
    """A proven, single-token field resolved against a concrete document."""

    label: str          # user-facing name, e.g. "mode" or "dur2"
    kind: str           # "mode" | "count" | "duration"
    frame: int          # 0-based frame index for "duration", else -1
    token: Token
    value: int


def resolve_field(doc: SooDoc, name: str) -> FieldSpec:
    """``mode`` / ``count`` / ``durN`` -> the exact token that carries it."""
    low = name.strip().lower()
    if low == "mode":
        return FieldSpec("mode", "mode", -1, doc.header[0], doc.mode)
    if low == "count":
        return FieldSpec("count", "count", -1, doc.header[1], doc.count)
    match = FIELD_RE.match(low)
    if match:
        index = int(match.group(1) if match.group(1) is not None
                    else match.group(2))
        if index >= len(doc.frames):
            raise EditError(
                f"frame {index}: the file lists {len(doc.frames)} frame(s) "
                f"(0..{len(doc.frames) - 1})")
        frame = doc.frames[index]
        return FieldSpec(f"dur{index}", "duration", index, frame.duration,
                         frame.value)
    raise EditError(f"unknown field {name!r} (known: mode, count, durN)")


def parse_int_value(text: str, what: str) -> int:
    try:
        return int(text.strip())
    except ValueError:
        raise EditError(f"{what} must be a decimal integer: {text!r}") from None


def validate(spec: FieldSpec, value: int, doc: SooDoc | None = None) -> None:
    """Refuse values the engine cannot play (proven loader/player limits)."""
    if spec.kind == "mode":
        if not MODE_MIN <= value <= MODE_MAX:
            raise EditError(
                f"mode={value}: the player normalises only 0 (one-shot, "
                f"0x433e8b) and 1 (loop, 0x433eb4); any other value runs past "
                f"the frame array")
        return
    if spec.kind == "count":
        raise EditError(
            "count is not editable: it is the number of frames the loader "
            "reads (0x42b5f1/0x42b637), so changing it adds or removes poses")
    if spec.kind != "duration":
        raise EditError(f"field {spec.label} is not editable")
    if value == 0:
        raise EditError(
            "duration=0: the player divides by the frame duration "
            "(0x433f24) and its selection loop never leaves the frame; "
            f"use {DURATION_MIN}..{DURATION_MAX} ticks")
    if not DURATION_MIN <= value <= DURATION_MAX:
        raise EditError(
            f"{spec.label}={value}: duration must be "
            f"{DURATION_MIN}..{DURATION_MAX} engine ticks "
            f"(shipped range is 9..6000)")


def parse_assignment(doc: SooDoc, assignment: str) -> tuple[FieldSpec, int]:
    if "=" not in assignment:
        raise EditError(f"--field needs NAME=VALUE, got {assignment!r}")
    name, _, raw = assignment.partition("=")
    spec = resolve_field(doc, name)
    value = parse_int_value(raw, spec.label)
    validate(spec, value, doc)
    return spec, value


def oracle_value(model, spec: FieldSpec) -> int:
    if spec.kind == "mode":
        return model.mode
    if spec.kind == "count":
        return model.count
    return model.frames[spec.frame].duration


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


def patch(doc: SooDoc, spec: FieldSpec, value: int) -> tuple[bytes, str]:
    text = str(int(value))
    return splice(doc.data, [(spec.token, text)]), text


def aligned_diff(old: bytes, new: bytes) -> int:
    """Bytes that differ when the two texts are aligned from the start."""
    compared = sum(1 for a, b in zip(old, new) if a != b)
    return compared + abs(len(old) - len(new))


def token_of(doc: SooDoc, spec: FieldSpec) -> Token:
    """The same field's token in another (re-walked) document."""
    if spec.kind == "mode":
        return doc.header[0]
    if spec.kind == "count":
        return doc.header[1]
    if spec.frame >= len(doc.frames):
        raise EditError(
            f"frame {spec.frame}: the edited file lists only "
            f"{len(doc.frames)} frame(s)")
    return doc.frames[spec.frame].duration


def current_value(doc: SooDoc, spec: FieldSpec) -> int:
    if spec.kind == "mode":
        return doc.mode
    if spec.kind == "count":
        return doc.count
    return doc.frames[spec.frame].value


def changed_bytes(doc: SooDoc, spec: FieldSpec, new_bytes: bytes) -> dict:
    """Describe the edit from the *file itself*, not from the intended value."""
    new_doc = walk(new_bytes, doc.path)
    old_text = spec.token.text
    new_token = token_of(new_doc, spec)
    count = aligned_diff(old_text.encode("latin1"),
                         new_token.text.encode("latin1"))
    return {
        "size": len(new_bytes),
        "size_delta": len(new_bytes) - len(doc.data),
        "old": old_text,
        "new": new_token.text,
        "span": list(spec.token.span),
        "changed_bytes": count,
    }


def verify_edit(doc: SooDoc, new_bytes: bytes, spec: FieldSpec,
                expected: int) -> dict:
    """Prove the edit changed the target token and nothing else."""
    checks: list[dict] = []

    def check(name: str, ok: bool, detail: str = "") -> None:
        checks.append({"check": name, "ok": bool(ok), "detail": detail})

    try:
        new_doc = walk(new_bytes, doc.path)
        check("own reader parses the edited file", True,
              f"{len(new_doc.lines)} line(s), {len(new_doc.frames)} frame(s)")
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
    width_old = len(delta["old"].encode("latin1"))
    width_new = len(delta["new"].encode("latin1"))
    check("size change equals the token width change",
          delta["size_delta"] == width_new - width_old,
          f"{len(doc.data)} -> {len(new_bytes)} bytes "
          f"({delta['size_delta']:+d})")
    check("line count is unchanged", len(new_doc.lines) == len(doc.lines),
          f"{len(new_doc.lines)} line(s)")
    check("the header layout is unchanged",
          new_doc.combined == doc.combined
          and [len(t.text) for t in new_doc.header] == [len(t.text)
                                                        for t in doc.header],
          "combined" if new_doc.combined else "split")

    other_header = all(new_doc.header[i].text == doc.header[i].text
                       for i in range(2)
                       if not (spec.kind == "mode" and i == 0)
                       and not (spec.kind == "count" and i == 1))
    check("every other header token is identical", other_header, "mode/count")

    names_ok = doc.names == new_doc.names
    check("the frame name tokens are identical", names_ok,
          f"{len(doc.frames)} name(s)")
    other_durations = all(
        new_doc.frames[i].value == doc.frames[i].value
        for i in range(min(len(doc.frames), len(new_doc.frames)))
        if not (spec.kind == "duration" and i == spec.frame))
    check("every other frame duration is identical", other_durations,
          f"{len(doc.frames)} duration(s)")

    new_token = token_of(new_doc, spec)
    check("bytes outside the target token are identical",
          doc.data[:spec.token.start] == new_bytes[:new_token.start]
          and doc.data[spec.token.end:] == new_bytes[new_token.end:])

    check("own reader sees the new value",
          current_value(new_doc, spec) == expected, str(expected))
    check("oracle sees the new value",
          oracle_value(new_model, spec) == expected, str(expected))

    if spec.kind == "mode":
        check("the engine loop flag follows the mode",
              new_model.loop == (expected == 1)
              and doc_intervals(new_doc) == oracle_intervals(new_model),
              f"loop={new_model.loop}")
    else:
        old_total = doc.engine_duration
        new_total = new_doc.engine_duration
        delta_total = (expected - spec.value if spec.frame < doc.count
                       else 0)
        check("the engine total changes by exactly the duration delta",
              new_total - old_total == delta_total,
              f"{old_total} -> {new_total} ticks ({delta_total:+d})")
        check("own timeline equals the oracle timeline after the edit",
              doc_intervals(new_doc) == oracle_intervals(new_model),
              f"{len(doc_intervals(new_doc))} interval(s)")

    check("changed-byte count is reported", True,
          f"{delta['changed_bytes']} byte(s)")
    return {"ok": all(c["ok"] for c in checks), "checks": checks,
            "diff": delta, "new_doc": new_doc, "new_model": new_model}


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
        fd, temp = tempfile.mkstemp(prefix=".soowrite-", dir=directory)
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


def require_soo(path: str) -> str:
    """Input files must be ``.soo`` -- everything else is a mistyped call."""
    if not path.lower().endswith(".soo"):
        raise EditError(f"not a .soo file: {path}")
    if not os.path.isfile(path):
        raise EditError(f"no such file: {path}")
    return path


def _real(path: str) -> str:
    return os.path.realpath(os.path.abspath(path))


def protected_roots(game: str) -> list[str]:
    """Directories a write must never touch: game, mirror and the checkouts."""
    roots = []
    for candidate in (game, DEFAULT_GAME, MIRROR, ROOT, SIBLING_REPO):
        if candidate:
            roots.append(_real(candidate))
    return sorted(set(roots))


def refuse_unsafe_out(out_path: str, game: str) -> None:
    """Never write into the game, the regress mirror or a repository."""
    if not out_path.lower().endswith(".soo"):
        raise EditError(f"--out must be a .soo path: {out_path}")
    target = _real(out_path)
    for root in protected_roots(game):
        if target == root or target.startswith(root + os.sep):
            if root in (_real(game), _real(DEFAULT_GAME)):
                kind = "the game installation"
            elif root == _real(MIRROR):
                kind = "the regress mirror"
            else:
                kind = "the repository"
            raise EditError(
                f"refusing to write inside {kind}: {out_path} "
                f"(write to /tmp/opencode or /var/tmp)")


def resolve_input(name: str, game: str = DEFAULT_GAME) -> str:
    """``Cultist`` / ``Cultist.soo`` / a path -> an existing ``.soo``."""
    if os.path.isfile(name):
        return require_soo(name)
    base = name if name.lower().endswith(".soo") else name + ".soo"
    for folder in ("Sobanim", "."):
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
    """Every ``.soo`` under ``root`` (recursive; shipped: ``Sobanim/``)."""
    if not os.path.isdir(root):
        raise EditError(f"no such directory: {root}")
    found: list[str] = []
    for dirpath, dirnames, filenames in os.walk(root):
        dirnames[:] = sorted(d for d in dirnames if not d.startswith("."))
        for filename in sorted(filenames):
            if filename.lower().endswith(".soo"):
                found.append(os.path.join(dirpath, filename))
    return found


def shipped_same_relative(path: str, game: str) -> str | None:
    """The shipped file with the same relative path (``Sobanim/x.soo``)."""
    parts = os.path.abspath(path).split(os.sep)
    for i, part in enumerate(parts[:-1]):
        if part.lower() == "sobanim":
            candidate = os.path.join(game, *parts[i:])
            if os.path.isfile(candidate):
                return candidate
    base = os.path.basename(path)
    for folder in ("Sobanim", ""):
        candidate = (os.path.join(game, folder, base) if folder
                     else os.path.join(game, base))
        if os.path.isfile(candidate):
            return candidate
    return None


def default_against(path: str, game: str) -> str | None:
    candidate = shipped_same_relative(path, game)
    if candidate is None or _real(candidate) == _real(path):
        return None
    return candidate


def sob_index(game: str) -> set[str] | None:
    """Lower-cased pose model names present in ``SOBS/`` (``None`` = no game)."""
    folder = os.path.join(game, "SOBS")
    try:
        entries = os.listdir(folder)
    except OSError:
        return None
    return {name[:-4].lower() for name in entries
            if name.lower().endswith(".sob")}


# ---------------------------------------------------------------------------
# structure checks
# ---------------------------------------------------------------------------

def structure_report(doc: SooDoc, model) -> dict:
    """Structural checks: own reader vs oracle vs the engine timeline."""
    checks: list[dict] = []
    warnings: list[str] = []

    def check(name: str, ok: bool, detail: str = "") -> None:
        checks.append({"check": name, "ok": bool(ok), "detail": detail})

    check("line scanner re-joins the file byte-exactly",
          rejoin(doc.data, doc.lines) == doc.data,
          f"{len(doc.lines)} line(s), final EOL "
          f"{(doc.lines[-1].eol or b'none')!r}")
    check("the header is two decimal integers",
          doc.header[0].text == str(doc.mode)
          and doc.header[1].text == str(doc.count),
          f"mode {doc.mode}, count {doc.count} "
          f"({'combined' if doc.combined else 'split'} header)")
    check("the declared count is within the loader limit",
          doc.count_in_range,
          f"{doc.count} (loader: 1..{MAX_FRAMES}, 0x42b5f1)")
    check("own header equals the oracle header",
          (doc.mode, doc.count) == (model.mode, model.count),
          f"{model.mode} {model.count}")
    check("own frames equal the oracle frames",
          doc.names == [f.name for f in model.frames]
          and doc.durations == [f.duration for f in model.frames],
          f"{len(doc.frames)} frame(s)")
    own = doc_intervals(doc)
    ref = oracle_intervals(model)
    check("own engine timeline equals the oracle timeline",
          own == ref,
          f"{len(ref)} interval(s), {doc.engine_duration} ticks")
    bare_lf = doc.data.count(b"\n") - doc.data.count(CRLF)
    bare_cr = doc.data.count(b"\r") - doc.data.count(CRLF)
    check("the file is CRLF-only", bare_lf == 0 and bare_cr == 0,
          f"{bare_lf} bare LF, {bare_cr} bare CR")
    check("every frame line holds one 'name duration' pair",
          all(len(tokenize_line(doc.lines[f.line], doc.data)) == 2
              for f in doc.frames),
          f"{len(doc.frames)} frame line(s)")

    if doc.short:
        warnings.append(
            f"the file declares {doc.count} frame(s) but lists "
            f"{len(doc.frames)}: the loader fails (0x42b637)")
    if doc.truncated:
        warnings.append(
            f"{len(doc.dropped_frames)} listed frame(s) after count are never "
            f"read (0x42b637): "
            + ", ".join(f.name.text for f in doc.dropped_frames))
    if doc.interior_blanks:
        warnings.append(
            "blank line(s) between frames at line(s) "
            + ", ".join(str(i + 1) for i in doc.interior_blanks)
            + ": the oracle would drop them")
    if not doc.mode_known:
        warnings.append(f"mode {doc.mode} is not 0/1; the player has no "
                        f"normalisation for it")

    return {"ok": all(c["ok"] for c in checks), "checks": checks,
            "warnings": warnings, "loadable": doc.grammar_loadable,
            "frames_listed": len(doc.frames), "frames_read": len(doc.engine_frames),
            "ticks": doc.engine_duration, "mode": doc.mode, "count": doc.count,
            "combined": doc.combined,
            "blanks": len(doc.blanks), "interior_blanks": len(doc.interior_blanks)}


def first_diff(old: bytes, new: bytes) -> dict:
    """Position and context of the first differing byte, for a report."""
    limit = min(len(old), len(new))
    offset = next((i for i in range(limit) if old[i] != new[i]), limit)
    return {"offset": offset,
            "old": old[offset:offset + 16].hex(),
            "new": new[offset:offset + 16].hex(),
            "size_delta": len(new) - len(old),
            "changed_bytes": aligned_diff(old, new),
            "identical": old == new}


def diff_summary(diff: dict) -> str:
    if diff["identical"]:
        return "identical"
    return (f"size differs by {diff['size_delta']:+d} byte(s), first diff at "
            f"offset {diff['offset']}; aligned byte diff "
            f"{diff['changed_bytes']} (a width change shifts the tail)")


# ---------------------------------------------------------------------------
# list / get
# ---------------------------------------------------------------------------

def list_report(path: str, game: str = DEFAULT_GAME, limit: int = 0) -> dict:
    path = resolve_input(path, game)
    doc = parse_file(path)
    model = oracle.parse(doc.data)
    structure = structure_report(doc, model)
    index = sob_index(game)
    intervals = doc_intervals(doc)
    frames = []
    for i, frame in enumerate(doc.frames):
        interval = intervals[i] if i < len(intervals) else None
        sob = None
        if index is not None:
            sob = frame.name.text.lower() in index
        frames.append({
            "index": i,
            "name": frame.name.text,
            "duration": frame.value,
            "duration_ms": round(frame.value * 1000.0 / oracle.ENGINE_TICK_HZ, 1),
            "start_ms": round(interval.start_ms, 1) if interval else None,
            "engine": i < doc.count,
            "sob": sob,
        })
    return {"file": path, "size": doc.size, "mode": doc.mode, "count": doc.count,
            "combined": doc.combined, "frames": frames, "total": doc.total,
            "engine_total": doc.engine_duration,
            "engine_ms": round(doc.engine_duration * 1000.0
                               / oracle.ENGINE_TICK_HZ, 1),
            "blanks": len(doc.blanks), "structure": structure, "limit": limit}


def mode_word(mode: int) -> str:
    if mode == 0:
        return "one-shot"
    if mode == 1:
        return "loop"
    return "unknown mode"


def print_list(rep: dict, limit: int = 0) -> None:
    print(f"file: {rep['file']} ({rep['size']} bytes)")
    print(f"mode: {rep['mode']} ({mode_word(rep['mode'])}), "
          f"declared count: {rep['count']}, frames listed: "
          f"{len(rep['frames'])}")
    print(f"engine: reads {rep['count']} frame(s), {rep['engine_total']} ticks "
          f"= {rep['engine_ms']} ms; layout "
          f"{'combined' if rep['combined'] else 'split'} header, "
          f"blank lines {rep['blanks']}")
    print("  #  name                  ticks      ms   start ms  engine  sob")
    rows = rep["frames"]
    if limit and len(rows) > 2 * limit:
        shown = rows[:limit] + [None] + rows[-limit:]
    else:
        shown = rows
    for row in shown:
        if row is None:
            print("  ...")
            continue
        sob = {True: "ok", False: "MISSING", None: "?"}[row["sob"]]
        print(f"{row['index']:3d}  {row['name']:<20} {row['duration']:6d} "
              f"{row['duration_ms']:7.1f} "
              f"{'' if row['start_ms'] is None else format(row['start_ms'], '9.1f')} "
              f"{'yes' if row['engine'] else 'no ':>6}  {sob}")
    for warning in rep["structure"]["warnings"]:
        print(f"warning: {warning}")


def get_field(path: str, name: str, game: str = DEFAULT_GAME) -> dict:
    path = resolve_input(path, game)
    doc = parse_file(path)
    spec = resolve_field(doc, name)
    return {"file": path, "field": spec.label, "kind": spec.kind,
            "frame": spec.frame, "value": spec.value,
            "span": list(spec.token.span),
            "line": spec.token.line + 1, "size": doc.size,
            "editable": spec.kind != "count"}


def print_get(rep: dict) -> None:
    print(f"file: {rep['file']} ({rep['size']} bytes)")
    note = ""
    if not rep["editable"]:
        note = ("  [read-only: the loader reads exactly this many frames, "
                "0x42b637]")
    print(f"{rep['field']} = {rep['value']}  "
          f"(token {rep['span'][0]}..{rep['span'][1]}, line {rep['line']})"
          f"{note}")


# ---------------------------------------------------------------------------
# set
# ---------------------------------------------------------------------------

def set_field(path: str, assignment: str, out_path: str,
              game: str = DEFAULT_GAME) -> dict:
    source = resolve_input(path, game)
    if _real(out_path) == _real(source):
        raise EditError("--out is the input file; pick a different path")
    refuse_unsafe_out(out_path, game)
    data = read_bytes(source)
    doc = walk(data, source)
    spec, value = parse_assignment(doc, assignment)

    new_bytes, text = patch(doc, spec, value)
    result = verify_edit(doc, new_bytes, spec, value)
    rep = {"ok": result["ok"], "source": source, "out": out_path,
           "field": spec.label, "kind": spec.kind, "frame": spec.frame,
           "old": spec.value, "new": value, "token": text,
           "source_size": len(data), "size": len(new_bytes),
           "size_delta": len(new_bytes) - len(data),
           "checks": result["checks"]}
    if result["ok"]:
        atomic_write(out_path, new_bytes)
        rep["written"] = True
    else:
        rep["written"] = False
    return rep


def print_set(rep: dict) -> None:
    print(f"{rep['source']} -> {rep['out']}")
    print(f"{rep['field']}: {rep['old']} -> {rep['new']} "
          f"({rep['source_size']} -> {rep['size']} bytes, "
          f"delta {rep['size_delta']:+d})")
    for item in rep["checks"]:
        print(f"  [{'ok  ' if item['ok'] else 'FAIL'}] {item['check']}"
              + (f" ({item['detail']})" if item.get("detail") else ""))
    if rep["written"]:
        print(f"wrote {rep['out']}")
    print(f"result: {'OK' if rep['ok'] else 'FAIL'}")


# ---------------------------------------------------------------------------
# check
# ---------------------------------------------------------------------------

def check_file(path: str, against: str | None = None,
               field: str | None = None, game: str = DEFAULT_GAME) -> dict:
    path = resolve_input(path, game)
    data = read_bytes(path)
    doc = walk(data, path)
    model = oracle.parse(data)
    structure = structure_report(doc, model)

    rep: dict = {"file": path, "size": len(data), "structure": structure,
                 "ok": structure["ok"], "against": None, "field": None,
                 "loadable": structure["loadable"], "note": ""}

    if against is None:
        against = default_against(path, game)
        if against is not None:
            rep["note"] = f"compared against the shipped {against}"
        elif shipped_same_relative(path, game) is not None:
            rep["note"] = ("this is the shipped file itself; pass --against "
                           "to compare an edited copy")
        else:
            rep["note"] = ("no --against and no shipped file with this name: "
                           "byte comparison skipped")

    if field is not None and against is None:
        raise EditError("--field needs --against (or a shipped file with the "
                        "same name) to compare with")

    if against:
        against = require_soo(against)
        old_bytes = read_bytes(against)
        old_doc = walk(old_bytes, against)
        old_model = oracle.parse(old_bytes)
        old_structure = structure_report(old_doc, old_model)
        rep["against"] = {"path": against, "size": len(old_bytes),
                          "structure": old_structure, "checks": [],
                          "diff": None, "ok": old_structure["ok"]}
        rep["ok"] = rep["ok"] and old_structure["ok"]
        checks = rep["against"]["checks"]

        def check(name: str, ok: bool, detail: str = "") -> None:
            checks.append({"check": name, "ok": bool(ok), "detail": detail})

        check("name token counts match the source",
              len(doc.frames) == len(old_doc.frames),
              f"{len(old_doc.frames)} vs {len(doc.frames)}")
        check("line count and line endings match the source",
              len(doc.lines) == len(old_doc.lines)
              and [l.eol for l in doc.lines]
              == [l.eol for l in old_doc.lines],
              f"{len(old_doc.lines)} vs {len(doc.lines)} line(s)")

        if field is not None:
            name, _, raw = field.partition("=")
            old_spec = resolve_field(old_doc, name)
            value = parse_int_value(raw, old_spec.label)
            expected_bytes, text = patch(old_doc, old_spec, value)
            diff = first_diff(old_bytes, data)
            check("the file is exactly the source with the field patched",
                  data == expected_bytes, diff_summary(diff))
            new_spec = resolve_field(doc, old_spec.label)
            check("the field carries the expected value",
                  current_value(doc, new_spec) == value,
                  f"{old_spec.label} = {current_value(doc, new_spec)}")
            unexpected = []
            if old_doc.names != doc.names:
                unexpected.append("names")
            for i, label in enumerate(("mode", "count")):
                if i == 0 and old_spec.kind == "mode":
                    continue
                if i == 1 and old_spec.kind == "count":
                    continue
                if doc.header[i].text != old_doc.header[i].text:
                    unexpected.append(label)
            if old_spec.kind == "duration":
                if any(doc.frames[i].value != old_doc.frames[i].value
                       for i in range(len(doc.frames))
                       if i != old_spec.frame):
                    unexpected.append("other durations")
            check("only the target token differs", not unexpected,
                  "unexpected: " + ", ".join(unexpected)
                  if unexpected else
                  f"{old_spec.label} only, layout "
                  f"{'combined' if doc.combined else 'split'} -> "
                  f"{'combined' if old_doc.combined else 'split'}")
            rep["field"] = {
                "name": old_spec.label, "value": value, "token": text,
                "old_token": old_spec.token.text, "diff": diff,
                "token_changed": aligned_diff(
                    old_spec.token.text.encode("latin1"),
                    text.encode("latin1")),
                "changed_bytes": diff["changed_bytes"]}
        else:
            diff = first_diff(old_bytes, data)
            check("the file is byte-identical to the source",
                  data == old_bytes, diff_summary(diff))
            changed = {}
            if old_doc.mode != doc.mode:
                changed["mode"] = [old_doc.mode, doc.mode]
            if old_doc.count != doc.count:
                changed["count"] = [old_doc.count, doc.count]
            if old_doc.names != doc.names:
                changed["names"] = [len(old_doc.names), len(doc.names)]
            if old_doc.durations != doc.durations:
                changed["durations"] = [old_doc.durations, doc.durations]
            if changed:
                rep["changed_fields"] = changed
            rep["diff"] = diff
        rep["against"]["diff"] = (rep["field"]["diff"] if rep["field"]
                                  else rep.get("diff"))
        rep["against"]["ok"] = all(c["ok"] for c in checks)
        rep["ok"] = rep["ok"] and rep["against"]["ok"]

    return rep


def print_check(rep: dict) -> None:
    print(f"check: {rep['file']} ({rep['size']} bytes)")
    for item in rep["structure"]["checks"]:
        print(f"  [{'ok  ' if item['ok'] else 'FAIL'}] {item['check']}"
              + (f" ({item['detail']})" if item.get("detail") else ""))
    for warning in rep["structure"]["warnings"]:
        print(f"  [warn] {warning}")
    if rep.get("note"):
        print(f"note: {rep['note']}")
    if rep["against"]:
        print(f"against: {rep['against']['path']} "
              f"({rep['against']['size']} bytes)")
        for item in rep["against"]["checks"]:
            print(f"  [{'ok  ' if item['ok'] else 'FAIL'}] {item['check']}"
                  + (f" ({item['detail']})" if item.get("detail") else ""))
        if rep["field"]:
            field = rep["field"]
            print(f"field: {field['name']} = {field['value']}, token "
                  f"{field['old_token']!r} -> {field['token']!r} "
                  f"({field['token_changed']} byte(s) in the token, file "
                  f"{field['diff']['size_delta']:+d} byte(s))")
        elif rep.get("changed_fields"):
            print("changed fields: " + ", ".join(
                f"{name} {values[0]} -> {values[1]}"
                for name, values in rep["changed_fields"].items()))
    print(f"result: {'OK' if rep['ok'] else 'FAIL'}")


# ---------------------------------------------------------------------------
# round-trip over the corpus
# ---------------------------------------------------------------------------

def _scratch_dir(tag: str) -> str:
    base = DEFAULT_OUT_DIR
    try:
        os.makedirs(base, exist_ok=True)
    except OSError:
        base = tempfile.gettempdir()
    return tempfile.mkdtemp(prefix=tag + "-", dir=base)


def _layout_reason(doc: SooDoc) -> str:
    """Why the oracle would not reproduce these bytes, in human words."""
    reasons = []
    if doc.interior_blanks:
        reasons.append("interior blank line(s) at "
                       + ", ".join(str(i + 1) for i in doc.interior_blanks))
    padded = [str(l.index + 1) for l in doc.lines
              if l.text != l.text.rstrip()]
    if padded:
        reasons.append("trailing whitespace on line(s) " + ", ".join(padded))
    noncanon = []
    for frame in doc.frames:
        line = doc.lines[frame.line]
        if line.text != f"{frame.name.text} {frame.value}":
            noncanon.append(str(line.index + 1))
    if noncanon:
        reasons.append("non-canonical 'name<space>duration' on line(s) "
                       + ", ".join(noncanon))
    return "; ".join(reasons)


def roundtrip_corpus(root: str = DEFAULT_GAME, verbose: bool = False) -> dict:
    files = corpus_files(root)
    if not files:
        raise EditError(f"no .soo files under {root}")
    scratch = _scratch_dir("roundtrip")
    start = time.perf_counter()
    checks = {"rejoin": 0, "oracle_parse": 0, "semantic": 0,
              "timeline": 0, "timeline_checked": 0}
    identical = 0
    total_bytes = 0
    mismatches: list[dict] = []
    per_file: list[dict] = []

    def fail(item: dict, reason: str) -> None:
        if not item.get("reason"):
            item["reason"] = reason
            mismatches.append(item)

    try:
        for path in files:
            total_bytes += os.path.getsize(path)
            item = {"path": path,
                    "rel": os.path.relpath(path, root),
                    "size": os.path.getsize(path)}
            data = read_bytes(path)
            try:
                doc = walk(data, path)
            except ScanError as exc:
                fail(item, f"own reader: {exc}")
                per_file.append(item)
                continue
            item["rejoin"] = rejoin(data, doc.lines) == data
            checks["rejoin"] += int(item["rejoin"])
            if not item["rejoin"]:
                fail(item, "line spans do not re-join the file")

            try:
                model = oracle.parse(data)
                checks["oracle_parse"] += 1
            except ValueError as exc:
                fail(item, f"oracle: {exc}")
                per_file.append(item)
                continue

            rebuilt = model.to_bytes()
            out = os.path.join(scratch, item["rel"])
            atomic_write(out, rebuilt)
            written = read_bytes(out)
            item["written_size"] = len(written)
            item["identical"] = written == data
            if item["identical"]:
                identical += 1
            else:
                diff = first_diff(data, written)
                item["diff"] = diff
                fail(item, _layout_reason(doc)
                     or f"first diff at offset {diff['offset']}")

            try:
                again = oracle.parse(written)
                same = (again.mode == model.mode
                        and again.count == model.count
                        and [(f.name, f.duration) for f in again.frames]
                        == [(f.name, f.duration) for f in model.frames]
                        and again.combined == model.combined
                        and again.eol == model.eol
                        and again.trailing == model.trailing
                        and again.blanks == model.blanks)
                checks["semantic"] += int(same)
                if not same:
                    fail(item, "semantic re-parse differs")
            except ValueError as exc:
                fail(item, f"re-parse: {exc}")

            try:
                walk(written, out)
                if doc.grammar_loadable:
                    checks["timeline_checked"] += 1
                    same_timeline = (doc_intervals(doc)
                                     == oracle_intervals(again))
                    checks["timeline"] += int(same_timeline)
                    if not same_timeline:
                        fail(item, "engine timeline differs")
            except ScanError as exc:
                fail(item, f"edited walk: {exc}")
            per_file.append(item)
            if verbose:
                print(("  ok   " if item.get("identical") else "  FAIL ")
                      + f"{item['rel']} ({item['size']} bytes)")
    finally:
        shutil.rmtree(scratch, ignore_errors=True)

    seconds = time.perf_counter() - start
    ok = (identical == len(files)
          and checks["rejoin"] == len(files)
          and checks["oracle_parse"] == len(files)
          and checks["semantic"] == len(files)
          and not mismatches)
    return {"ok": ok, "root": root, "files": len(files),
            "identical": identical, "bytes": total_bytes,
            "seconds": round(seconds, 3), "checks": checks,
            "mismatches": mismatches, "per_file": per_file}


def print_roundtrip(rep: dict) -> None:
    print(f"roundtrip: {rep['identical']}/{rep['files']} identical, "
          f"{rep['bytes']} bytes, {rep['seconds']} s ({rep['root']})")
    c = rep["checks"]
    print(f"  own reader: {c['rejoin']}/{rep['files']} parsed, "
          f"line-span re-join {c['rejoin']}/{rep['files']}")
    print(f"  read from disk after write: {rep['identical']}/{rep['files']}")
    print(f"  semantic re-parse (parse -> to_bytes -> parse): "
          f"{c['semantic']}/{rep['files']}")
    print(f"  engine timeline agrees after the round-trip: "
          f"{c['timeline']}/{c['timeline_checked']} loadable file(s)")
    for item in rep["mismatches"]:
        print(f"  [FAIL] {item['rel']} ({item['size']} bytes): "
              f"{item.get('reason', 'differs')}")
    print(f"result: {'OK' if rep['ok'] else 'FAIL'}")


# ---------------------------------------------------------------------------
# audit
# ---------------------------------------------------------------------------

def _median(values: list[int]) -> float:
    ordered = sorted(values)
    if not ordered:
        return 0.0
    middle = len(ordered) // 2
    if len(ordered) % 2:
        return float(ordered[middle])
    return (ordered[middle - 1] + ordered[middle]) / 2.0


def _hist(values: list[int]) -> dict[str, int]:
    """Full distribution: ``{value: count}`` sorted by value."""
    tally: dict[int, int] = {}
    for value in values:
        tally[value] = tally.get(value, 0) + 1
    return {str(value): n for value, n in sorted(tally.items())}


def _top(values: list[int], count: int = 6) -> str:
    tally: dict[int, int] = {}
    for value in values:
        tally[value] = tally.get(value, 0) + 1
    items = sorted(tally.items(), key=lambda kv: (-kv[1], kv[0]))[:count]
    return ", ".join(f"{value}={n}" for value, n in items)


def audit_corpus(root: str = DEFAULT_GAME, game: str | None = None) -> dict:
    files = corpus_files(root)
    if not files:
        raise EditError(f"no .soo files under {root}")
    game = game or DEFAULT_GAME
    index = sob_index(game)
    start = time.perf_counter()

    sizes: list[int] = []
    counts: list[int] = []
    per_file_frames: list[int] = []
    modes: dict[int, int] = {}
    durations: list[int] = []
    total_ticks = 0
    engine_ticks = 0
    file_ticks: list[int] = []
    distinct_names: set[str] = set()
    frames_total = 0
    engine_frames_total = 0
    combined = 0
    final_eol = 0
    blank_lines = 0
    files_with_blanks = 0
    interior_blanks = 0
    bare_lf = 0
    bare_cr = 0
    padded_lines = 0
    short_files: list[str] = []
    dead_files: list[str] = []
    dead_frames = 0
    missing_files: list[str] = []
    missing_names: set[str] = set()
    missing_slots = 0
    loadable = 0
    unknown_mode: list[str] = []
    name_tokens = 0
    duplicate_names: list[str] = []
    count_vs_listed: dict[str, int] = {"equal": 0, "short": 0, "extra": 0}

    for path in files:
        rel = os.path.relpath(path, root)
        data = read_bytes(path)
        doc = walk(data, path)
        sizes.append(doc.size)
        counts.append(doc.count)
        per_file_frames.append(len(doc.frames))
        modes[doc.mode] = modes.get(doc.mode, 0) + 1
        frames_total += len(doc.frames)
        engine_frames_total += len(doc.engine_frames)
        durations.extend(f.value for f in doc.frames)
        total_ticks += doc.total
        engine_ticks += doc.engine_duration
        file_ticks.append(doc.engine_duration)
        combined += int(doc.combined)
        final_eol += int(bool(doc.lines[-1].eol))
        blank_lines += len(doc.blanks)
        files_with_blanks += int(bool(doc.blanks))
        interior_blanks += len(doc.interior_blanks)
        bare_lf += data.count(b"\n") - data.count(CRLF)
        bare_cr += data.count(b"\r") - data.count(CRLF)
        padded_lines += sum(1 for l in doc.lines if l.text != l.text.rstrip())
        if doc.short:
            short_files.append(rel)
            count_vs_listed["short"] += 1
        elif doc.truncated:
            dead_files.append(rel)
            dead_frames += len(doc.dropped_frames)
            count_vs_listed["extra"] += 1
        else:
            count_vs_listed["equal"] += 1
        if not doc.mode_known:
            unknown_mode.append(rel)
        names = doc.names
        name_tokens += len(names)
        distinct_names.update(names)
        if len(set(names)) != len(names):
            duplicate_names.append(rel)
        if index is not None:
            missing = sorted({n for n in names if n.lower() not in index})
            if missing:
                missing_files.append(rel)
                missing_names.update(missing)
                missing_slots += sum(1 for n in names
                                     if n.lower() not in index)
            elif doc.grammar_loadable:
                loadable += 1
        elif doc.grammar_loadable:
            loadable += 1

    seconds = time.perf_counter() - start
    return {
        "ok": True, "root": root, "files": len(files), "bytes": sum(sizes),
        "seconds": round(seconds, 3),
        "frames": frames_total, "engine_frames": engine_frames_total,
        "dead_frames": dead_frames,
        "modes": {str(k): v for k, v in sorted(modes.items())},
        "counts": {"min": min(counts), "max": max(counts),
                   "median": _median(counts), "distinct": len(set(counts)),
                   "top": _top(counts)},
        "frames_per_file": {"min": min(per_file_frames),
                            "max": max(per_file_frames),
                            "median": _median(per_file_frames),
                            "hist": _hist(per_file_frames)},
        "ticks": {"total": total_ticks, "engine": engine_ticks,
                  "min_file": min(file_ticks), "max_file": max(file_ticks),
                  "seconds": round(engine_ticks / oracle.ENGINE_TICK_HZ, 1)},
        "durations": {"min": min(durations), "max": max(durations),
                      "distinct": len(set(durations)),
                      "top": _top(durations)},
        "sizes": {"min": min(sizes), "max": max(sizes),
                  "median": _median(sizes), "total": sum(sizes)},
        "layout": {"combined": combined, "split": len(files) - combined,
                   "final_eol": final_eol,
                   "no_final_eol": len(files) - final_eol,
                   "blank_lines": blank_lines,
                   "files_with_blanks": files_with_blanks,
                   "interior_blanks": interior_blanks,
                   "bare_lf": bare_lf, "bare_cr": bare_cr,
                   "padded_lines": padded_lines},
        "count_vs_listed": count_vs_listed,
        "short_files": short_files, "dead_files": dead_files,
        "unknown_mode": unknown_mode,
        "names": {"tokens": name_tokens, "distinct": len(distinct_names),
                  "duplicate_in_file": duplicate_names},
        "missing": {"files": len(missing_files), "names": sorted(missing_names),
                    "slots": missing_slots, "files_list": missing_files},
        "loadable": loadable,
        "loadable_note": ("grammar + pose models resolved"
                          if index is not None
                          else "grammar only (no game installation at "
                               f"{game})"),
    }


def print_audit(rep: dict) -> None:
    print(f"audit: {rep['files']} files, {rep['bytes']} bytes, "
          f"{rep['frames']} frames ({rep['engine_frames']} read by the "
          f"engine), {rep['seconds']} s ({rep['root']})")
    print("  modes: " + ", ".join(f"{k}={v}"
                                  for k, v in rep["modes"].items()))
    c = rep["counts"]
    print(f"  declared count: min {c['min']}, median {c['median']}, "
          f"max {c['max']}; {c['distinct']} distinct; top {c['top']}")
    f = rep["frames_per_file"]
    print(f"  frames/file: min {f['min']}, median {f['median']}, "
          f"max {f['max']}; distribution "
          + ", ".join(f"{k}={v}" for k, v in f["hist"].items()))
    d = rep["durations"]
    print(f"  durations (ticks): min {d['min']}, max {d['max']}, "
          f"{d['distinct']} distinct; top {d['top']}")
    print(f"  total ticks: {rep['ticks']['total']} listed, "
          f"{rep['ticks']['engine']} read "
          f"({rep['ticks']['seconds']} s)")
    s = rep["sizes"]
    print(f"  size: min {s['min']}, median {s['median']}, max {s['max']} "
          f"bytes; total {s['total']}")
    l = rep["layout"]
    print(f"  layout: combined header {l['combined']}, split "
          f"{l['split']}; final EOL {l['final_eol']}, without "
          f"{l['no_final_eol']}; blank lines {l['blank_lines']} in "
          f"{l['files_with_blanks']} file(s) (interior "
          f"{l['interior_blanks']}); padded lines {l['padded_lines']}; "
          f"bare LF/CR {l['bare_lf']}/{l['bare_cr']}")
    cv = rep["count_vs_listed"]
    print(f"  count vs listed: equal {cv['equal']}, short {cv['short']}, "
          f"dead lines {cv['extra']} file(s)/{rep['dead_frames']} frame(s)")
    for rel in rep["dead_files"]:
        print(f"    dead: {rel}")
    if rep["short_files"]:
        print(f"  loader would fail (declared > listed): "
              + ", ".join(rep["short_files"]))
    if rep["missing"]["files"]:
        print(f"  pose models missing (no SOBS/<name>.sob): "
              f"{rep['missing']['files']} file(s), "
              f"{len(rep['missing']['names'])} name(s) in "
              f"{rep['missing']['slots']} frame slot(s): "
              + ", ".join(rep["missing"]["names"]))
        print("    " + ", ".join(rep["missing"]["files_list"]))
    if rep["unknown_mode"]:
        print("  modes outside 0/1: " + ", ".join(rep["unknown_mode"]))
    print(f"  loadable: {rep['loadable']}/{rep['files']} "
          f"({rep['loadable_note']})")
    print(f"  names: {rep['names']['tokens']} tokens, "
          f"{rep['names']['distinct']} distinct"
          + (f"; repeated within a file: "
             + ", ".join(rep["names"]["duplicate_in_file"])
             if rep["names"]["duplicate_in_file"] else ""))
    print("result: OK")


# ---------------------------------------------------------------------------
# self-test
# ---------------------------------------------------------------------------

CANON = b"0\r\n2\r\nposeA 100\r\nposeB 50\r\n"
LOOP = b"1\r\n2\r\nposeA 100\r\nposeB 50\r\n"
COMBINED = b"0 3\r\nbamfc 20\r\nbamfb 20\r\nbamfa 20\r\n"
NO_EOL = b"0\r\n3\r\nBigGate1 200\r\nBigGate2 6000\r\nBigGate3 10"
BLANKS = b"0\r\n1\r\nx 10\r\n\r\n\r\n"
LF_ONLY = b"0\n1\nx 10\n"
CR_ONLY = b"0\r1\rx 10\r"
TABS = b"0\t2\r\nposeA\t100\r\nposeB\t50\r\n"
SPACES = b"0   2\r\nposeA    100\r\nposeB  50\r\n"
TRUNCATED = b"0\r\n1\r\nx 10\r\ny 20\r\n"
SHORT = b"0\r\n3\r\nx 10\r\n"


def selftest() -> tuple[bool, list[dict]]:
    results: list[dict] = []

    def check(name: str, ok: bool, detail: str = "") -> None:
        results.append({"check": name, "ok": bool(ok), "detail": detail})

    def raises(name: str, fn, needle: str = "") -> None:
        try:
            fn()
        except (EditError, ScanError) as exc:
            ok = needle.lower() in str(exc).lower() if needle else True
            check(name, ok, str(exc)[:100])
        except Exception as exc:                        # noqa: BLE001
            check(name, False, f"{type(exc).__name__}: {exc}")
        else:
            check(name, False, "no exception")

    def diff_count(old: bytes, new: bytes) -> int:
        return aligned_diff(old, new)

    # -- reader and layout ------------------------------------------------
    doc = walk(CANON)
    check("line-span re-join is byte-exact", rejoin(CANON, doc.lines) == CANON)
    check("canonical header and frames",
          (doc.mode, doc.count, doc.names, doc.durations)
          == (0, 2, ["poseA", "poseB"], [100, 50]))
    check("CRLF and final EOL detected",
          doc.lines[0].eol == b"\r\n" and doc.lines[-1].eol == b"\r\n")
    comb = walk(COMBINED)
    check("combined 'mode count' header is one line",
          comb.combined and (comb.mode, comb.count) == (0, 3)
          and len(comb.frames) == 3)
    bare = walk(NO_EOL)
    check("missing final newline is preserved",
          bare.lines[-1].eol == b"" and bare.names[-1] == "BigGate3")
    blank = walk(BLANKS)
    check("trailing blank lines are counted",
          len(blank.blanks) == 2 and blank.interior_blanks == [])
    check("LF-only layout is accepted", walk(LF_ONLY).lines[0].eol == b"\n")
    check("CR-only layout is accepted", walk(CR_ONLY).lines[0].eol == b"\r")
    tabs = walk(TABS)
    check("tab separators parse and re-join",
          tabs.durations == [100, 50] and rejoin(TABS, tabs.lines) == TABS)
    spaces = walk(SPACES)
    check("multi-space separators re-join byte-exactly",
          rejoin(SPACES, spaces.lines) == SPACES
          and spaces.durations == [100, 50])
    check("the oracle would normalise a non-canonical layout",
          oracle.parse(SPACES).to_bytes() != SPACES,
          "oracle collapses separators: byte splicing is required")
    for label, blob in (("canonical", CANON), ("combined", COMBINED),
                        ("no final EOL", NO_EOL), ("blank lines", BLANKS)):
        check(f"oracle round-trips {label} bytes",
              oracle.parse(blob).to_bytes() == blob)

    # -- timeline ---------------------------------------------------------
    intervals = doc_intervals(doc)
    check("own timeline: 2 intervals, 150 ticks",
          [(i.duration, i.start, i.next_index) for i in intervals]
          == [(100, 0, 1), (50, 100, 1)])
    loop_intervals = doc_intervals(walk(LOOP))
    check("mode 1 wraps the last interval to frame 0",
          loop_intervals[-1].next_index == 0)
    check("own timeline equals the oracle timeline",
          intervals == oracle_intervals(oracle.parse(CANON)))

    # -- patching ---------------------------------------------------------
    spec = resolve_field(doc, "mode")
    new_bytes, _ = patch(doc, spec, 1)
    check("mode patch rewrites one byte",
          new_bytes == LOOP and diff_count(CANON, new_bytes) == 1)
    spec = resolve_field(doc, "dur1")
    same_width, _ = patch(doc, spec, 70)
    check("duration patch keeps the width (50 -> 70)",
          len(same_width) == len(CANON)
          and diff_count(CANON, same_width) == 1
          and same_width == b"0\r\n2\r\nposeA 100\r\nposeB 70\r\n")
    spec = resolve_field(doc, "dur0")
    grown, _ = patch(doc, spec, 1000)
    check("duration patch grows the token (100 -> 1000)",
          len(grown) == len(CANON) + 1
          and grown == b"0\r\n2\r\nposeA 1000\r\nposeB 50\r\n")

    # -- verification -----------------------------------------------------
    rep = verify_edit(doc, LOOP, resolve_field(doc, "mode"), 1)
    check("verify_edit accepts an in-place mode patch",
          rep["ok"] and all(c["ok"] for c in rep["checks"]),
          f"{len(rep['checks'])} checks")
    rep2 = verify_edit(doc, grown, resolve_field(doc, "dur0"), 1000)
    check("verify_edit accepts a width-changing duration patch",
          rep2["ok"], f"{len(rep2['checks'])} checks")
    check("oracle sees the edited duration",
          oracle.parse(grown).frames[0].duration == 1000)
    model = oracle.parse(LOOP)
    check("mode 1 edit sets the oracle loop flag", model.loop is True)
    check("duration edit changes the engine total by the delta",
          walk(grown).engine_duration - doc.engine_duration == 900)

    # -- refusals ---------------------------------------------------------
    raises("refuses count edits",
           lambda: validate(resolve_field(doc, "count"), 1), "count is not")
    raises("refuses mode 2",
           lambda: validate(resolve_field(doc, "mode"), 2), "normalises only")
    raises("refuses a zero duration",
           lambda: validate(resolve_field(doc, "dur0"), 0), "divides")
    raises("refuses a negative duration",
           lambda: validate(resolve_field(doc, "dur1"), -5))
    raises("refuses an out-of-range frame index",
           lambda: resolve_field(doc, "dur9"), "frame 9")
    raises("refuses an unknown field",
           lambda: resolve_field(doc, "speed"), "unknown field")
    raises("refuses an empty file", lambda: walk(b""), "empty")
    raises("refuses an odd frame line",
           lambda: walk(b"0\r\n1\r\nx\r\n"), "name duration")
    raises("refuses a non-integer duration",
           lambda: walk(b"0\r\n1\r\nx abc\r\n"), "not an integer")
    raises("refuses a non-integer header",
           lambda: walk(b"x\r\n1\r\npose 10\r\n"), "not an integer")
    raises("refuses a non-.soo input", lambda: require_soo("x.txt"), "not a .soo")
    raises("refuses output inside the game",
           lambda: refuse_unsafe_out(
               os.path.join(DEFAULT_GAME, "Sobanim", "x.soo"), DEFAULT_GAME),
           "game installation")
    raises("refuses output inside the mirror",
           lambda: refuse_unsafe_out(os.path.join(MIRROR, "x.soo"),
                                     DEFAULT_GAME), "mirror")
    raises("refuses output inside the repository",
           lambda: refuse_unsafe_out(os.path.join(ROOT, "x.soo"),
                                     DEFAULT_GAME), "repository")

    # -- end-to-end on synthetic files ------------------------------------
    scratch = _scratch_dir("selftest")
    try:
        src = os.path.join(scratch, "a.soo")
        dst = os.path.join(scratch, "b.soo")
        atomic_write(src, CANON)
        raises("refuses --out equal to the input",
               lambda: set_field(src, "mode=1", src), "--out is the input")
        result = set_field(src, "dur0=1000", dst)
        check("--set writes a verified file", result["ok"] and result["written"],
              f"{result['size']} bytes, {result['size_delta']:+d}")
        check("the written file carries the new value",
              read_bytes(dst) == b"0\r\n2\r\nposeA 1000\r\nposeB 50\r\n")
        checked = check_file(dst, src, "dur0=1000")
        check("--check accepts the verified edit",
              checked["ok"] and checked["against"]["ok"])
        checked = check_file(dst, src)
        check("--check without --field rejects a differing file",
              not checked["ok"])
        good = check_file(dst)
        check("--check structure of the edited file is clean",
              good["structure"]["ok"] and good["structure"]["loadable"])
        atomic_write(os.path.join(scratch, "short.soo"), SHORT)
        short = check_file(os.path.join(scratch, "short.soo"))
        check("--check reports declared > listed as not loadable",
              short["structure"]["ok"] and not short["structure"]["loadable"]
              and short["structure"]["warnings"])
        atomic_write(os.path.join(scratch, "trunc.soo"), TRUNCATED)
        trunc = check_file(os.path.join(scratch, "trunc.soo"))
        check("--check warns about dead lines after count",
              trunc["structure"]["ok"]
              and any("never read" in w for w in trunc["structure"]["warnings"]))
        padded = walk(b"0\r\n1\r\npose 10 \r\n")
        check("trailing spaces are preserved and explained",
              rejoin(padded.data, padded.lines) == padded.data
              and "trailing whitespace" in _layout_reason(padded))
        payload = json.dumps({"roundtrip": result}, ensure_ascii=False)
        check("reports are JSON-serialisable", '"written": true' in payload)
    finally:
        shutil.rmtree(scratch, ignore_errors=True)

    return all(r["ok"] for r in results), results


def print_selftest(ok: bool, results: list[dict]) -> None:
    passed = sum(1 for r in results if r["ok"])
    for item in results:
        print(f"  [{'ok  ' if item['ok'] else 'FAIL'}] {item['check']}"
              + (f" ({item['detail']})" if item.get("detail") else ""))
    print(f"self-test: {passed}/{len(results)} checks, "
          f"result: {'OK' if ok else 'FAIL'}")


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------

def _add_methods(ap) -> None:
    ap.add_argument("file", nargs="?",
                    help=".soo file (or DIR for --roundtrip/--audit)")
    ap.add_argument("--list", action="store_true", help="header + frames")
    ap.add_argument("--get", action="store_true", help="read one field")
    ap.add_argument("--set", action="store_true",
                    help="edit one field into --out")
    ap.add_argument("--check", action="store_true",
                    help="structure + byte comparison")
    ap.add_argument("--roundtrip", action="store_true",
                    help="read -> write -> compare every .soo under DIR/game")
    ap.add_argument("--audit", action="store_true", help="corpus statistics")
    ap.add_argument("--self-test", action="store_true",
                    help="synthetic tests, no game data")
    ap.add_argument("--against", metavar="FILE",
                    help="--check: source file to compare with")
    ap.add_argument("--field", metavar="NAME[=VALUE]",
                    help="mode|count|durN (--set/--check need NAME=VALUE)")
    ap.add_argument("--out", metavar="PATH", help="--set: output .soo")
    ap.add_argument("--game", default=os.environ.get("GUNMETAL", DEFAULT_GAME),
                    help="game installation (default %s)" % DEFAULT_GAME)
    ap.add_argument("--mirror", action="store_true",
                    help="--roundtrip/--audit: use the regress mirror as root")
    ap.add_argument("--strict", action="store_true",
                    help="--check: also fail when the engine would refuse")
    ap.add_argument("--limit", type=int, default=0,
                    help="--list: show first/last N frames (0 = all)")
    ap.add_argument("--verbose", action="store_true",
                    help="--roundtrip: print every file")
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
        description="Layout-preserving .soo writer/editor (M11.5, slice 6).")
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
            rep = roundtrip_corpus(root, verbose=args.verbose)
            if args.json:
                print(json.dumps(rep, indent=1, ensure_ascii=False))
            else:
                print_roundtrip(rep)
            return 0 if rep["ok"] else 1

        if args.audit:
            game = (MIRROR if args.mirror and args.game == DEFAULT_GAME
                    else args.game)
            rep = audit_corpus(root, game)
            if args.json:
                print(json.dumps(rep, indent=1, ensure_ascii=False))
            else:
                print_audit(rep)
            return 0 if rep["ok"] else 1

        if not args.file:
            raise EditError("this mode needs a .soo file")

        if args.list:
            rep = list_report(args.file, args.game, max(args.limit, 0))
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
            if not args.field or "=" not in args.field:
                raise EditError("--set needs --field NAME=VALUE")
            rep = set_field(args.file, args.field, args.out, args.game)
            if args.json:
                print(json.dumps(rep, indent=1, ensure_ascii=False))
            else:
                print_set(rep)
            return 0 if rep["ok"] else 1

        if args.check:
            rep = check_file(args.file, args.against, args.field, args.game)
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
        print(f"soowrite: {exc}", file=sys.stderr)
        return 2
    except ScanError as exc:
        print(f"soowrite: corrupt .soo: {exc}", file=sys.stderr)
        return 2
    except (ValueError, OSError) as exc:
        print(f"soowrite: {exc}", file=sys.stderr)
        return 2
    except Exception as exc:                            # noqa: BLE001
        print(f"soowrite: internal error: {type(exc).__name__}: {exc}",
              file=sys.stderr)
        return 3


if __name__ == "__main__":
    sys.exit(main())
