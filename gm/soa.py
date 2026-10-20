"""Gunmetal ".soa" sprite/texture animation descriptors (anims/*.soa).

Plain DOS text, CRLF line endings::

    line 1:  <mode> <period> <frames> <loop>
    line 2+: one sprite/texture name per line

The engine loader is ``0x42b81b`` (called as ``load(name, kind)``); the frame
selector is ``0x41de7d``; see ``docs/SOA.md`` for the disassembly evidence.

Semantics (recovered from ``gundx.exe``):

* ``frames`` is authoritative: the loader reads exactly that many names with
  ``fscanf("%s")``.  Extra lines are never read; fewer lines make the whole
  load fail.  ``frames`` must be 1..32.
* ``period`` is the duration of the **whole cycle in centiseconds** (1/100 s),
  not a per-frame step.  The loader converts it to the engine clock:
  ``period_ticks = period * 91 / 100`` (truncating).  The engine clock runs at
  91 ticks/s (the main loop is capped at 11 ms/frame and the wall clock treats
  91 ticks as one second).
* ``mode 0`` = one shot: frames advance over one period, then the last frame is
  held forever (and an effect sprite is freed after the period).
* ``mode 1`` = loop: ``t %= period_ticks`` and the frames repeat.
* ``mode 100/101/102`` = special: the frame 0 texture is redrawn each tick with
  the current local time (``DCLOCK*``) or the film frame (``FILM``); the frame
  selector always returns frame 0.
* ``loop`` is parsed and stored per sprite entry, but the 91 Hz runtime never
  reads it: one-shot vs loop is decided by ``mode``.  The loader also forces
  ``loop = 0`` for ``kind 1`` (``spritani/``) files.
* A missing ``.soa`` is not an error: the loader falls back to a static sprite
  with the same name, and returns a *positive* sprite id instead of the
  negative animation id.

The parser keeps *all* names it finds and reports the declared count
separately, so the byte-exact round-trip is preserved even for files whose
declared count does not match the list.
"""

from __future__ import annotations

from dataclasses import dataclass, field

#: Engine clock rate: 0x4074e0 sleeps ``elapsed + 11`` ms per frame and
#: 0x44db80 counts 91 ticks as one second.
ENGINE_TICK_HZ = 91

#: ``period`` is authored in centiseconds: 0x42badf does ``period * 0x5b / 100``.
CENTISECONDS_PER_SECOND = 100

#: Modes that bypass the frame formula (0x44dc50) and always use frame 0.
SPECIAL_MODES = (100, 101, 102)

#: Loader limits (0x42baf3: ``frames <= 0x20``; 0x42b8ed loops over the list).
MAX_FRAMES = 32


def ticks_at(ms: int) -> int:
    """Convert wall-clock milliseconds to engine ticks (91 Hz)."""
    return (int(ms) * ENGINE_TICK_HZ) // 1000


def ms_at(tick: int) -> int:
    """Convert engine ticks to wall-clock milliseconds (floor)."""
    return (int(tick) * 1000) // ENGINE_TICK_HZ


def _ceil_div(a: int, b: int) -> int:
    return -((-a) // b)


@dataclass(frozen=True)
class Timing:
    """One effective frame interval of the engine timeline.

    ``start_tick``/``end_tick`` are engine ticks relative to the animation
    start; ``end_tick`` is exclusive and ``None`` means "held forever" (last
    frame of a one-shot, or the single frame of a special mode).
    ``start_ms``/``end_ms`` are the same boundaries in milliseconds (floored).
    """

    index: int
    name: str
    start_tick: int
    end_tick: int | None
    start_ms: int
    end_ms: int | None

    @property
    def duration_ticks(self) -> int | None:
        if self.end_tick is None:
            return None
        return self.end_tick - self.start_tick

    @property
    def duration_ms(self) -> int | None:
        if self.end_ms is None:
            return None
        return self.end_ms - self.start_ms


@dataclass(frozen=True)
class Pose:
    """Result of :meth:`Soa.frame_at`: the frame active at one instant."""

    index: int              # 0-based index into the engine frame list
    name: str               # sprite/texture name (``pics/<name>.sbm``)
    tick: int               # engine ticks since the animation start
    period_ticks: int       # normalised period (0 for special modes)
    loop: bool              # mode == 1
    mode: int


@dataclass
class Soa:
    mode: int
    period: int
    declared_frames: int
    loop: int
    names: list[str] = field(default_factory=list)
    lines: list[str] = field(default_factory=list)  # exact text lines (for round-trip)

    @property
    def frame_count(self) -> int:
        return self.declared_frames

    # -- engine view ------------------------------------------------------
    #
    # The loader (0x42b81b) reads exactly ``declared_frames`` names and stores
    # ``period * 91 / 100``; the player (0x41de7d) uses those values.  These
    # helpers model the engine without changing ``names``/``to_bytes``.

    @property
    def period_ticks(self) -> int:
        """``period`` converted to engine ticks (``* 91 / 100``, truncated)."""
        value = abs(int(self.period)) * ENGINE_TICK_HZ // CENTISECONDS_PER_SECOND
        return value if self.period >= 0 else -value

    @property
    def looped(self) -> bool:
        """True for ``mode 1`` (the only looping mode)."""
        return self.mode == 1

    @property
    def special(self) -> bool:
        """True for the clock/film modes (100..102) that bypass the formula."""
        return self.mode not in (0, 1)

    @property
    def short(self) -> bool:
        """The file lists fewer names than declared: the engine refuses it."""
        return len(self.names) < self.declared_frames

    @property
    def truncated(self) -> bool:
        """The file lists more names than the engine reads."""
        return len(self.names) > self.declared_frames

    @property
    def loadable(self) -> bool:
        """Whether the loader would register this animation."""
        return (1 <= self.declared_frames <= MAX_FRAMES
                and len(self.names) >= self.declared_frames)

    def engine_names(self) -> list[str]:
        """The names the engine actually reads (first ``declared_frames``)."""
        if self.declared_frames <= 0:
            return []
        return list(self.names[:self.declared_frames])

    def timeline(self) -> list[Timing]:
        """One full playback, as contiguous frame intervals.

        The intervals are in engine ticks.  For ``mode 1`` they cover exactly
        one period and then repeat; for ``mode 0`` the last interval is open
        ended (the engine holds the last frame).  Some frames can be zero
        ticks long when ``declared_frames`` is large relative to the period.
        """
        names = self.engine_names()
        if not names:
            return []
        if self.special or self.mode not in (0, 1):
            return [Timing(0, names[0], 0, None, 0, None)]
        period = self.period_ticks
        if period <= 0:
            raise ValueError("period %d is not positive after conversion "
                             "to engine ticks" % self.period)
        n = len(names)
        out: list[Timing] = []
        for k in range(n):
            start = _ceil_div(k * period, n)
            end = _ceil_div((k + 1) * period, n)
            if k == n - 1 and self.mode == 0:
                out.append(Timing(k, names[k], start, None, ms_at(start), None))
            else:
                out.append(Timing(k, names[k], start, end,
                                  ms_at(start), ms_at(end)))
        return out

    def frame_at(self, ms: int) -> Pose:
        """Frame active ``ms`` milliseconds after the animation started.

        Mirrors ``0x41de7d`` for the two shipped modes::

            t = ticks_at(ms)
            if mode == 0 and t >= period_ticks: return declared - 1
            return ((t % period_ticks) * declared) // period_ticks

        Modes other than 0/1 go through ``0x44dc50``, which always returns
        frame 0 (the special clock/film modes also redraw that texture, which
        is not modelled here).

        Raises ``ValueError`` for a non-loadable file, a negative time (the
        engine has no clamp for ``.soa`` and would read before the array) and
        for a non-positive converted period (the engine divides by zero).
        """
        if not self.loadable:
            raise ValueError(
                "engine would refuse this .soa (declared %d, %d names)"
                % (self.declared_frames, len(self.names)))
        names = self.engine_names()
        if ms < 0:
            raise ValueError("negative time is not modelled (engine has no clamp)")
        if self.mode not in (0, 1):
            return Pose(0, names[0], ticks_at(ms), 0, False, self.mode)
        period = self.period_ticks
        if period <= 0:
            raise ValueError("period %d is zero after conversion to engine "
                             "ticks: engine divides by zero" % self.period)
        t = ticks_at(ms)
        n = len(names)
        if self.mode == 0 and t >= period:
            index = n - 1
        else:
            index = (t % period) * n // period
        return Pose(index, names[index], t, period, self.mode == 1, self.mode)

    def frame_name_at(self, ms: int) -> str:
        """Convenience wrapper for ``frame_at(ms).name``."""
        return self.frame_at(ms).name

    def to_bytes(self) -> bytes:
        """Re-serialise with CRLF endings (byte-exact round-trip of shipped files)."""
        lines = self.lines or self.render_lines()
        return b"\r\n".join(ln.encode("latin1") for ln in lines)

    def render_lines(self) -> list[str]:
        """Canonical lines built from the fields/names (for new or edited files)."""
        return [f"{self.mode} {self.period} {self.declared_frames} {self.loop}"] + list(self.names)


def from_names(mode: int, period: int, loop: int, names: list[str]) -> "Soa":
    """Build a fresh descriptor; ``declared_frames`` defaults to ``len(names)``."""
    a = Soa(mode, period, len(names), loop, list(names))
    a.lines = a.render_lines()
    return a


def parse(data: bytes) -> Soa:
    text = data.decode("latin1")
    lines = text.replace("\r\n", "\n").replace("\r", "\n").split("\n")
    if lines and lines[-1] == "":
        lines = lines[:-1]
    stripped = [ln.strip() for ln in lines]
    nonempty = [ln for ln in stripped if ln]
    if not nonempty:
        raise ValueError("empty .soa")
    head = nonempty[0].split()
    if len(head) < 4:
        raise ValueError(f"bad .soa header {nonempty[0]!r}")
    mode, period, frames, loop = (int(x) for x in head[:4])
    return Soa(mode, period, frames, loop, nonempty[1:],
               lines=text.split("\r\n"))
