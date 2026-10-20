"""Gunmetal ``.soo`` — flipbook animations of models (plain text).

Every ``Sobanim/*.soo`` is a tiny text file::

    0            ; mode (0 or 1, only these two occur)
    3            ; declared frame count
    BigGate1 200  ; frame model (SOBS/<name>.sob) and its duration
    BigGate2 6000
    BigGate3 10

The first two numbers may share a line (``0 3``) in three shipped files.  The
count is **not** a comment: the engine loader (``0x42b4e1``) reads exactly
``count`` frames with ``%s %ld`` and never looks at the rest of the file.  So
``nblast``/``nbloom`` (8 declared, 9 listed) play only eight frames and
``ruinbeam`` (5 declared, 6 listed) plays only five; extra lines are dead data.
The frame list is still kept in full here so ``to_bytes`` stays byte-exact.

Semantics recovered from the code (see ``docs/SOO.md``):

* ``mode == 0`` — one shot: the clock is clamped to the last frame, which is
  held (the last frame interpolates to itself).
* ``mode == 1`` — loop: the clock is taken modulo the total duration and the
  last frame interpolates back to the first one.
* Durations are milliseconds on the game clock.
* Each name is a separate ``SOBS/<name>.sob`` pose.  Frames are *not* swapped
  as models: the engine interpolates vertices and per-face data between the
  current and the next pose.  All poses must therefore have the same vertex and
  face counts (checked by the loader).

Layout quirks (CRLF, combined header, missing final newline, trailing blank
lines) are preserved so ``to_bytes`` round-trips byte-exactly.
"""

from __future__ import annotations

from dataclasses import dataclass, field


@dataclass
class Frame:
    """One animation frame: a model name (``SOBS/<name>.sob``) and its time."""

    name: str
    duration: int


@dataclass(frozen=True)
class Timing:
    """One effective frame interval of the engine timeline.

    ``start``/``end`` are milliseconds relative to the animation start, ``end``
    is exclusive.  ``next_index`` is the pose the engine interpolates towards
    during this interval (itself on the last frame of a one-shot, the first
    frame on the last frame of a loop).
    """

    index: int
    name: str
    duration: int
    start: int
    end: int
    next_index: int


@dataclass(frozen=True)
class Pose:
    """Result of :meth:`Soo.frame_at`: the state of one instant of playback."""

    index: int                 # current frame (0-based, engine frames only)
    name: str                  # current pose (SOBS/<name>.sob)
    duration: int              # duration of the current frame, ms
    elapsed: int               # ms spent inside the current frame
    alpha: float               # elapsed / duration, 0..1 (1 at the very end)
    next_index: int            # pose interpolated towards
    next_name: str             # its model name (== name when holding)
    phase: int                 # normalised clock (0..total-1 / 0..total)
    total: int                 # total effective duration, ms
    loop: bool                 # mode == 1


@dataclass
class Soo:
    """A parsed ``.soo`` animation."""

    mode: int
    count: int
    frames: list[Frame] = field(default_factory=list)
    combined: bool = False       # "mode count" on one line
    eol: str = "\r\n"
    trailing: bool = True        # file ends with a newline
    blanks: int = 0              # extra empty lines at the end

    @property
    def duration(self) -> int:
        """Sum of the durations of *all* listed frames (legacy behaviour)."""
        return sum(f.duration for f in self.frames)

    # -- engine view ------------------------------------------------------
    #
    # The loader reads exactly ``count`` frames; the rest of the text file is
    # never consumed.  The helpers below model that behaviour without changing
    # ``frames``/``duration``/``to_bytes`` (which stay byte-exact/lossless).

    @property
    def loop(self) -> bool:
        """``mode == 1`` (proven at ``0x433e93``; see docs/SOO.md)."""
        return self.mode == 1

    @property
    def engine_frames(self) -> list[Frame]:
        """The frames the engine actually loads (first ``count`` entries)."""
        if self.count <= 0:
            return []
        return self.frames[:self.count]

    @property
    def dropped_frames(self) -> list[Frame]:
        """Listed frames after ``count`` — never read by the engine."""
        if self.count <= 0:
            return list(self.frames)
        return self.frames[self.count:]

    @property
    def short(self) -> bool:
        """The file lists fewer frames than the declared count (load fails)."""
        return len(self.frames) < self.count

    @property
    def truncated(self) -> bool:
        """The file lists more frames than the engine reads."""
        return len(self.frames) > self.count

    @property
    def engine_duration(self) -> int:
        """Total duration of the frames the engine loads, in milliseconds."""
        return sum(f.duration for f in self.engine_frames)

    def timeline(self, start: int = 0) -> list[Timing]:
        """Effective engine timeline as frame intervals.

        ``start`` shifts the absolute times; intervals are contiguous and their
        sum is :attr:`engine_duration`.  A loop is not unrolled — the wrap is
        implicit, use :meth:`frame_at` for an arbitrary instant.
        """
        out: list[Timing] = []
        t = int(start)
        frames = self.engine_frames
        n = len(frames)
        for i, f in enumerate(frames):
            if i == n - 1:
                nxt = 0 if self.loop else i
            else:
                nxt = i + 1
            out.append(Timing(i, f.name, f.duration, t, t + f.duration, nxt))
            t += f.duration
        return out

    def frame_at(self, ms: int, start: int = 0) -> Pose:
        """State of the animation ``ms`` milliseconds after ``start``.

        Mirrors ``0x433e00`` exactly for the two shipped modes:

        * negative time is clamped to 0 (``0x433e84``);
        * mode 0: ``t >= total`` becomes ``total - 1`` (last frame held);
        * mode 1: ``t %= total`` (wrap-around);
        * frame is chosen by walking the duration table;
        * ``alpha`` is the fraction of the current frame that has elapsed and
          the engine interpolates towards ``next_index`` by it.

        Raises ``ValueError`` for an empty timeline, non-positive total
        duration, or a mode other than 0/1 (the engine would run off the frame
        array for other values, so they are not modelled).
        """
        frames = self.engine_frames
        if not frames:
            raise ValueError("animation has no frames")
        if self.mode not in (0, 1):
            raise ValueError("unsupported .soo mode %d (engine knows 0/1)"
                             % self.mode)
        total = sum(f.duration for f in frames)
        if total <= 0:
            raise ValueError("animation duration is not positive")

        phase = int(ms) - int(start)
        if phase < 0:
            phase = 0
        if self.mode == 0:
            if phase >= total:
                phase = total - 1
        else:
            phase %= total

        acc = 0
        index = 0
        for i, f in enumerate(frames):
            if phase < acc + f.duration:
                index = i
                break
            acc += f.duration
        else:  # only reachable with non-positive durations
            index = len(frames) - 1
            acc = total - frames[index].duration

        frame = frames[index]
        if index == len(frames) - 1:
            nxt = 0 if self.mode == 1 else index
        else:
            nxt = index + 1
        elapsed = phase - acc
        alpha = elapsed / frame.duration if frame.duration else 1.0
        return Pose(index=index, name=frame.name, duration=frame.duration,
                    elapsed=elapsed, alpha=alpha, next_index=nxt,
                    next_name=frames[nxt].name, phase=phase, total=total,
                    loop=self.mode == 1)

    def to_bytes(self) -> bytes:
        """Serialise back to the original text layout."""
        head = (["%d %d" % (self.mode, self.count)] if self.combined
                else [str(self.mode), str(self.count)])
        lines = head + ["%s %d" % (f.name, f.duration) for f in self.frames]
        text = self.eol.join(lines)
        if self.trailing:
            text += self.eol
        text += self.eol * self.blanks
        return text.encode("latin1")


def parse(data: bytes) -> Soo:
    """Parse ``.soo`` bytes, keeping the original layout flags."""
    text = data.decode("latin1")
    eol = "\r\n" if "\r\n" in text else "\n"
    tokens = text.split()
    if len(tokens) < 2:
        raise ValueError("not a .soo: fewer than two numbers")
    try:
        mode, count = int(tokens[0]), int(tokens[1])
    except ValueError as exc:
        raise ValueError("not a .soo: header is not two integers") from exc
    rest = tokens[2:]
    if len(rest) % 2:
        raise ValueError("not a .soo: odd number of frame tokens")
    frames = []
    for i in range(0, len(rest), 2):
        try:
            frames.append(Frame(rest[i], int(rest[i + 1])))
        except ValueError as exc:
            raise ValueError("not a .soo: bad duration for %r" % rest[i]) from exc
    lines = text.split(eol)
    tail = lines[-1] if lines else ""
    trailing = tail == ""
    body = lines[:-1] if trailing else lines
    blanks = 0
    while body and not body[-1].strip():
        blanks += 1
        body.pop()
    combined = bool(lines and len(lines[0].split()) >= 2)
    return Soo(mode=mode, count=count, frames=frames, combined=combined,
               eol=eol, trailing=trailing, blanks=blanks)


def parse_file(path: str) -> Soo:
    with open(path, "rb") as fh:
        return parse(fh.read())
