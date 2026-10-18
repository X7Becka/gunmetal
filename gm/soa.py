"""Gunmetal ".soa" animation descriptors (anims/*.SOA).

Plain DOS text, CRLF line endings.  Layout (verified on all 77 shipped files):

    line 1:  <mode> <period> <frames> <loop>
    line 2+: one frame/texture name per line

``frames`` is the number of names the engine reads; a handful of authored files
carry extra or missing trailing lines, so the parser keeps *all* the names it
finds and reports the declared count separately.

Fields:
    mode    - 0 or 1; the shipped files use 1 except a few (OilBrn1, PLSBLST,
              nuke1, x, splash_g, splash_h) which use 0.
    period  - per-frame step/duration parameter (engine units, e.g. 48, 72,
              128, 256); identical for both fields in the clock files (100/1).
    frames  - number of frame names
    loop    - 1 = loop, 0 = play once (the dclock*/FILM files use 0)
"""

from __future__ import annotations

from dataclasses import dataclass, field


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
