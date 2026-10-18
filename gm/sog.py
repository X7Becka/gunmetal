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

    @property
    def track_count(self) -> int:
        return len(self.names)


def parse(data: bytes) -> Sog:
    text = data.decode("latin1")
    names = [ln.strip() for ln in text.replace("\r\n", "\n").replace("\r", "\n").split("\n")]
    return Sog([n for n in names if n])


def parse_file(path: str) -> Sog:
    with open(path, "rb") as f:
        return parse(f.read())
