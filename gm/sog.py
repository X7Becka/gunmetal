"""Gunmetal ".sog" songs (MUSIC/*.sog).

A song is **just a text playlist**: one sample name per non-empty line.  The
engine plays the listed samples back to back (see the error string
``Unable to load sample '%s' for song 'music\\%s.sog'`` in ``gundx.exe``).

The samples themselves are *not* in sounds/SOU.DAT; they live in a second
BIGFILE archive shipped next to the songs:

    MUSIC/SOU.IDX + MUSIC/SOU.DAT   3092 entries, every one a SOU2 blob

So a song is  <list of names>  ->  MUSIC/SOU.DAT entries.

Layout (all 651 shipped .sog files):

    line 1..N:  sample name (8 chars, matches a BIGFILE entry name)
"""

from __future__ import annotations

from dataclasses import dataclass, field


@dataclass
class Sog:
    names: list[str] = field(default_factory=list)
    lines: list[str] = field(default_factory=list)  # exact text lines (for round-trip)

    @property
    def track_count(self) -> int:
        return len(self.names)

    def to_bytes(self) -> bytes:
        """Re-serialise with CRLF endings (byte-exact round-trip of shipped files)."""
        lines = self.lines or self.render_lines()
        return b"\r\n".join(ln.encode("latin1") for ln in lines)

    def render_lines(self) -> list[str]:
        """Canonical lines built from the names (for new or edited playlists)."""
        return list(self.names)


def from_names(names: list[str]) -> "Sog":
    s = Sog(list(names))
    s.lines = s.render_lines()
    return s


def parse(data: bytes) -> Sog:
    text = data.decode("latin1")
    names = [ln.strip() for ln in text.replace("\r\n", "\n").replace("\r", "\n").split("\n")]
    return Sog([n for n in names if n], lines=text.split("\r\n"))


def parse_file(path: str) -> Sog:
    with open(path, "rb") as f:
        return parse(f.read())
