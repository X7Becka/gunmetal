"""Gunmetal ``.soo`` — flipbook animations of models (plain text).

Every ``Sobanim/*.soo`` is a tiny text file::

    0            ; mode (0 or 1, only these two occur)
    3            ; declared frame count
    BigGate1 200  ; frame model (SOBS/<name>.sob) and its duration
    BigGate2 6000
    BigGate3 10

The first two numbers may share a line (``0 3``) in three shipped files and the
declared count is not always the real number of frames (``nblast``/``nbloom``
say 8 but list 9, ``ruinbeam`` says 5 but lists 6), so the frame list is the
authoritative one.  Layout quirks (CRLF, combined header, missing final newline,
trailing blank lines) are preserved so ``to_bytes`` round-trips byte-exactly.
"""

from __future__ import annotations

from dataclasses import dataclass, field


@dataclass
class Frame:
    """One animation frame: a model name (``SOBS/<name>.sob``) and its time."""

    name: str
    duration: int


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
        """Sum of the frame durations."""
        return sum(f.duration for f in self.frames)

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
