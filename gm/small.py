"""Small Gunmetal formats: ``.sub``, ``.skk``/``.skb``, ``.sni``, ``.key``, ``.hi``.

* ``.sub`` — substitution lists.  ``textures.sub``/``sprites.sub`` start with a
  line holding the entry count, then ``<number> <from> <to>`` per line;
  ``sounds.sub`` has no count and is tab separated (``from<TAB>to``).
* ``.skk``/``.skb`` — kill messages, one per line, ``%S`` placeholders.
* ``.sni`` — a single path string (``.\\r\\n`` in the shipped file).
* ``.key`` — 376 bytes: ``name[16]`` + 45 records of 8 bytes.  The loader
  (``0x48534a``) reads 0x178 bytes and copies only the first 31 records into a
  0x108-byte runtime struct, so the runtime part is ``name[16] + 31*8``; the
  trailing 112 bytes are kept verbatim here.
* ``.hi`` — 9 records of 64 bytes: ``i32 score`` + ``char name[60]``; loaded at
  ``0x45c8b3`` with ``fread(rec, 0x40, 1, f)``.

Every parser keeps the raw bytes it does not interpret (names, tail), so
``to_bytes()`` round-trips the shipped files exactly.
"""

from __future__ import annotations

import struct
from dataclasses import dataclass, field


# --- .sub ------------------------------------------------------------------
@dataclass
class SubEntry:
    value: int | None
    left: str
    right: str


@dataclass
class SubFile:
    entries: list[SubEntry] = field(default_factory=list)
    count: int | None = None          # leading line of textures/sprites.sub
    tab: bool = False                 # sounds.sub uses tabs, no count
    eol: str = "\r\n"
    trailing: bool = True
    blanks: int = 0                   # empty lines after the last entry

    def to_bytes(self) -> bytes:
        lines = []
        if self.count is not None:
            lines.append(str(self.count))
        sep = "\t" if self.tab else " "
        for e in self.entries:
            parts = ([str(e.value)] if e.value is not None else []) + [e.left, e.right]
            lines.append(sep.join(parts))
        text = self.eol.join(lines)
        if self.trailing:
            text += self.eol
        text += self.eol * self.blanks
        return text.encode("latin1")


def parse_sub(data: bytes) -> SubFile:
    text = data.decode("latin1")
    eol = "\r\n" if "\r\n" in text else "\n"
    parts = text.split(eol)
    trailing = text.endswith(eol)
    if trailing and parts and not parts[-1]:
        parts.pop()
    blanks = 0
    while parts and not parts[-1].strip():
        blanks += 1
        parts.pop()
    lines = [ln for ln in parts if ln.strip()]
    count = None
    tab = "\t" in text
    if lines and not tab and len(lines[0].split()) == 1:
        try:
            count = int(lines[0])
            lines = lines[1:]
        except ValueError:
            pass
    entries = []
    for line in lines:
        parts = [p for p in line.replace("\t", " ").split(" ") if p]
        if tab or len(parts) == 2:
            if len(parts) != 2:
                raise ValueError(".sub: expected 'from<TAB>to', got %r" % line)
            entries.append(SubEntry(None, parts[0], parts[1]))
        elif len(parts) == 3:
            entries.append(SubEntry(int(parts[0]), parts[1], parts[2]))
        else:
            raise ValueError(".sub: expected 2 or 3 fields, got %r" % line)
    return SubFile(entries=entries, count=count, tab=tab, eol=eol,
                   trailing=trailing, blanks=blanks)


# --- .skk / .skb -----------------------------------------------------------
@dataclass
class KillFile:
    lines: list[str] = field(default_factory=list)
    eol: str = "\r\n"
    trailing: bool = True

    @property
    def messages(self) -> list[str]:
        return [ln for ln in self.lines if ln.strip()]

    def to_bytes(self) -> bytes:
        text = self.eol.join(self.lines)
        if self.trailing:
            text += self.eol
        return text.encode("latin1")


def parse_kill(data: bytes) -> KillFile:
    text = data.decode("latin1")
    eol = "\r\n" if "\r\n" in text else "\n"
    trailing = text.endswith(eol)
    lines = text.split(eol)
    if trailing:
        lines.pop()
    return KillFile(lines=lines, eol=eol, trailing=trailing)


# --- .sni ------------------------------------------------------------------
def parse_sni(data: bytes) -> str:
    return data.decode("latin1").rstrip("\r\n")


def sni_to_bytes(text: str, eol: str = "\r\n") -> bytes:
    return (text.rstrip("\r\n") + eol).encode("latin1")


# --- .key ------------------------------------------------------------------
@dataclass
class KeyConfig:
    raw_name: bytes = b""
    entries: list[tuple[int, int]] = field(default_factory=list)   # (code, extra)

    @property
    def name(self) -> str:
        return self.raw_name.split(b"\0")[0].decode("latin1")

    def to_bytes(self) -> bytes:
        out = bytearray(self.raw_name[:16].ljust(16, b"\0"))
        for code, extra in self.entries:
            out += struct.pack("<II", code, extra)
        return bytes(out)


def parse_key(data: bytes) -> KeyConfig:
    if len(data) < 16 or (len(data) - 16) % 8:
        raise ValueError(".key: expected name[16] + 8-byte records, got %d bytes"
                         % len(data))
    entries = [struct.unpack_from("<II", data, 16 + 8 * i)
               for i in range((len(data) - 16) // 8)]
    return KeyConfig(raw_name=data[:16], entries=entries)


# --- .hi -------------------------------------------------------------------
@dataclass
class HighScores:
    records: list[dict] = field(default_factory=list)   # {"score", "raw"}

    def to_bytes(self) -> bytes:
        out = bytearray()
        for rec in self.records:
            out += struct.pack("<i", rec["score"])
            out += rec["raw"][:60].ljust(60, b"\0")
        return bytes(out)


def parse_hi(data: bytes) -> HighScores:
    if len(data) % 64:
        raise ValueError(".hi: size %d is not a multiple of 64" % len(data))
    records = []
    for i in range(0, len(data), 64):
        records.append({"score": struct.unpack_from("<i", data, i)[0],
                        "raw": data[i + 4:i + 64]})
    return HighScores(records=records)


def record_name(rec: dict) -> str:
    return rec["raw"].split(b"\0")[0].decode("latin1")


# --- .cfg ------------------------------------------------------------------
@dataclass
class Config:
    """A ``GUNMETAL.CFG`` / ``CONTROLS.CFG`` blob (8192 bytes in the supply).

    The file starts with ``GUNMETAL Configuration File. av<version>`` followed
    by 0x1A; the remaining fields are not mapped yet, so they are kept verbatim.
    """

    raw: bytes = b""
    header: str = ""
    version: str = ""

    def to_bytes(self) -> bytes:
        return self.raw


def parse_cfg(data: bytes) -> Config:
    head = data.split(b"\x1a")[0].decode("latin1")
    version = ""
    marker = head.find("av")
    if marker >= 0:
        version = head[marker + 2:].strip()
    return Config(raw=data, header=head, version=version)
