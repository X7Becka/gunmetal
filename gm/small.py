"""Small Gunmetal formats: ``.sub``, ``.skk``/``.skb``, ``.sni``, ``.key``, ``.hi``.

* ``.sub`` — substitution lists.  ``textures.sub``/``sprites.sub`` start with a
  line holding the entry count, then ``<weight> <replacement> <replaced>`` per
  line; ``sounds.sub`` has no count and is tab separated
  (``replacement<TAB>replaced``).  The engine treats the number as a **RAM
  class** (MB above ~4 MiB): the replacement is used only when the machine has
  that little memory.  It substitutes an image, it does not select animation
  variants — see ``docs/SUB.md``.
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
#: ``fscanf("%s")`` writes into a 256-byte stack buffer (``0x422ff1`` /
#: ``0x422ff8``), so a shipped name can be at most 255 chars.
SUB_NAME_MAX = 255
#: Sprite/texture run-time names live in ``char name[9]`` (``0x76ed47`` /
#: ``0x55a0c7``) and are copied with ``strncpy(..., 9)`` (``0x423244``,
#: ``0x4233d2``), i.e. only the first 8 chars are significant.
SUB_KEY_MAX = 8


@dataclass
class SubEntry:
    """One substitution line.

    ``value`` is the leading number (``None`` in the tab-separated
    ``sounds.sub``).  The engine substitutes the *image* named ``right`` with
    the image named ``left`` — see ``docs/SUB.md`` §2.  The historical field
    names are kept for compatibility; ``weight``/``replacement``/``replaced``
    say what they actually mean.
    """

    value: int | None
    left: str
    right: str

    @property
    def weight(self) -> int | None:
        """RAM-class threshold (MB above ~4 MiB); ``None`` for sounds."""
        return self.value

    @property
    def replacement(self) -> str:
        """Name of the image that is loaded instead (``left``)."""
        return self.left

    @property
    def replaced(self) -> str:
        """Name whose image is substituted (``right``)."""
        return self.right

    def to_text(self, sep: str = " ") -> str:
        parts = ([str(self.value)] if self.value is not None
                 else []) + [self.left, self.right]
        return sep.join(parts)


@dataclass
class SubFile:
    """A parsed ``.sub`` table (``textures.sub``, ``sprites.sub``,
    ``sounds.sub``).  ``to_bytes()`` is byte-exact for the shipped corpus."""

    entries: list[SubEntry] = field(default_factory=list)
    count: int | None = None          # leading line of textures/sprites.sub
    tab: bool = False                 # sounds.sub uses tabs, no count
    eol: str = "\r\n"
    trailing: bool = True
    blanks: int = 0                   # empty lines after the last entry
    lead_blanks: int = 0              # empty lines before the count line
    head_blanks: int = 0              # empty lines between count and first entry
    blank_after: dict[int, int] = field(default_factory=dict)
    count_text: str | None = None     # count line verbatim (spaces kept)

    @property
    def is_counted(self) -> bool:
        return self.count is not None

    def variants(self, weight: int | None = None) -> dict[str, list[str]]:
        """``replaced name -> [replacement names]``, in file order.

        With ``weight`` given, only entries of exactly that weight are used.
        """
        out: dict[str, list[str]] = {}
        for e in self.entries:
            if weight is not None and e.weight != weight:
                continue
            out.setdefault(e.right, []).append(e.left)
        return out

    def weights(self) -> dict[int, int]:
        """``weight -> entry count`` (entries without a number are skipped)."""
        out: dict[int, int] = {}
        for e in self.entries:
            if e.weight is not None:
                out[e.weight] = out.get(e.weight, 0) + 1
        return out

    def active(self, ram_class: int) -> list[SubEntry]:
        """Entries the engine loads on a machine of ``ram_class`` MB.

        ``ram_class`` is ``(dwTotalPhys - 4_000_000) / 1_048_576`` from
        ``GlobalMemoryStatus`` (``0x404630``); the loader keeps an entry when
        ``weight >= ram_class`` (``0x42302c``..``0x423035``).
        """
        return [e for e in self.entries
                if e.weight is not None and e.weight >= ram_class]

    def resolve(self, ram_class: int) -> dict[str, str]:
        """``replaced -> replacement`` for the active entries.

        The first line for a given replaced name wins: the loader refuses to
        re-map a sprite that already has a different substitution
        (``0x42307a`` returns 0 instead of overwriting).
        """
        out: dict[str, str] = {}
        for e in self.active(ram_class):
            out.setdefault(e.right, e.left)
        return out

    def validate(self) -> list[str]:
        """Hard layout errors; an empty list means the table is loadable."""
        bad: list[str] = []
        if self.count is not None:
            if self.count < 0:
                bad.append("negative entry count %d" % self.count)
            elif self.count != len(self.entries):
                bad.append("count %d but %d entries"
                           % (self.count, len(self.entries)))
        for i, e in enumerate(self.entries):
            if self.count is not None and e.weight is None:
                bad.append("entry %d: missing weight" % i)
            if self.count is None and e.weight is not None:
                bad.append("entry %d: unexpected weight" % i)
            if not e.left or not e.right:
                bad.append("entry %d: empty name" % i)
            if len(e.left) > SUB_NAME_MAX or len(e.right) > SUB_NAME_MAX:
                bad.append("entry %d: name longer than %d"
                           % (i, SUB_NAME_MAX))
            if e.weight is not None and e.weight < 0:
                bad.append("entry %d: negative weight %d" % (i, e.weight))
        return bad

    def warnings(self) -> list[str]:
        """Non-fatal issues (the engine would silently truncate/ignore)."""
        warn: list[str] = []
        if self.count is not None:
            for i, e in enumerate(self.entries):
                for name in (e.left, e.right):
                    if len(name) > SUB_KEY_MAX:
                        warn.append("entry %d: %r truncated to %d chars"
                                    % (i, name, SUB_KEY_MAX))
        return warn

    def to_bytes(self) -> bytes:
        lines = [""] * self.lead_blanks
        if self.count is not None:
            raw = self.count_text
            try:
                if raw is None or int(raw) != self.count:
                    raw = None
            except ValueError:
                raw = None
            lines.append(raw if raw is not None else str(self.count))
        lines += [""] * self.head_blanks
        sep = "\t" if self.tab else " "
        for i, e in enumerate(self.entries):
            lines.append(e.to_text(sep))
            lines += [""] * self.blank_after.get(i, 0)
        text = self.eol.join(lines)
        if self.trailing:
            text += self.eol
        text += self.eol * self.blanks
        return text.encode("latin1")


def parse_sub(data: bytes) -> SubFile:
    text = data.decode("latin1")
    if not text:
        return SubFile(trailing=False)
    eol = "\r\n" if "\r\n" in text else "\n"
    parts = text.split(eol)
    trailing = text.endswith(eol)
    if trailing and parts and not parts[-1]:
        parts.pop()
    blanks = 0
    while parts and not parts[-1].strip():
        blanks += 1
        parts.pop()
    if not parts:
        trailing = False          # all-blank file: the EOLs are the blanks

    tab = "\t" in text
    count = None
    count_text = None
    lead = 0
    start = 0
    first = next((i for i, ln in enumerate(parts) if ln.strip()), None)
    if first is not None and not tab:
        fields = parts[first].split()
        if len(fields) == 1:
            try:
                count = int(fields[0])
                count_text = parts[first]
                lead = first
                start = first + 1
            except ValueError:
                pass
    if count is None:
        lead = first if first is not None else 0
        start = lead
    entries: list[SubEntry] = []
    head = 0
    blank_after: dict[int, int] = {}
    pending = 0
    for line in parts[start:]:
        if not line.strip():
            pending += 1
            continue
        fields = [p for p in line.replace("\t", " ").split(" ") if p]
        if tab or len(fields) == 2:
            if len(fields) != 2:
                raise ValueError(".sub: expected 'from<TAB>to', got %r" % line)
            entry = SubEntry(None, fields[0], fields[1])
        elif len(fields) == 3:
            entry = SubEntry(int(fields[0]), fields[1], fields[2])
        else:
            raise ValueError(".sub: expected 2 or 3 fields, got %r" % line)
        if pending:
            if entries:
                blank_after[len(entries) - 1] = pending
            else:
                head += pending
        pending = 0
        entries.append(entry)
    return SubFile(entries=entries, count=count, tab=tab, eol=eol,
                   trailing=trailing, blanks=blanks, lead_blanks=lead,
                   head_blanks=head, blank_after=blank_after,
                   count_text=count_text)


#: Type alias used by the newer readers (``Sub`` is the same object).
Sub = SubFile


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
