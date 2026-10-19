"""Gunmetal ".sml"/".sme" UI markup and briefing texts.

The ``.sml`` files are plain markup; the shipped ``.sme`` files are the
*encoded* form of the same language.  The cipher is a positional Caesar:

    encoded[i] = letter ? shift(plain[i], +i % 10) : plain[i]

i.e. every letter is shifted forward by ``byte index % 10`` positions inside
its own alphabet (case preserved), and every other byte is copied unchanged.
Decoding is the same with ``-i % 10``; because the shift only depends on the
position, ``encode(decode(x)) == x`` for any input.

Evidence: of the 147 shipped ``.sme``/``.sml`` pairs, 131 decode
byte-identically (``decode(LEVEL1.SME) == level1.sml`` and so on); the other 16
are revisions (the ``.sme`` side is longer in 9 of them, shorter in 3 and of
equal length in 4), and at least one shows the ``.sme`` side is the corrected
text: ``help.sme`` decodes to ``Toggle Player ID`` (the ``.sml`` has the typo
``PLayer``) plus a ``P - Pause`` line the ``.sml`` lacks.  All 151 ``.sme``
decode to well-formed markup, so the whole set is encoded.

The markup is a flat stream of tags, e.g.

    <d><r><f s="2">dmills=niacdispatch</f><s><p>Morning, <k>. Lieutenant ...

Tags are *not* always closed (``<p>`` runs until the next block tag), so this
module exposes the tag stream and a tag-stripped text rather than a tree; the
engine's UI loader (the meaning of ``<d>``/``<r>``/``<c>``/``<I x= y= f= a=>``
…, the ``<k>`` player-name placeholder and the ``name=value`` payloads) is not
reversed yet.

Implementation: ``gm/sml.py`` (``decode``/``encode``/``tags``/``plain``);
CLI ``gm.py sml <paths> <out> [--rebuild DIR]``.
"""

from __future__ import annotations

import re
from dataclasses import dataclass, field

#: shift cycle: byte ``i`` of a letter is moved by ``i % SHIFT_MOD`` letters
SHIFT_MOD = 10

#: block-level tags that ``plain`` turns into line breaks
BREAK_TAGS = ("d", "D", "r", "R", "p", "P")

_TAG = re.compile(r"<(/?)([A-Za-z][A-Za-z0-9]*)((?:\s+[^<>]*?)?)(/?)>")
_ATTR = re.compile(r"([A-Za-z][A-Za-z0-9]*)\s*=\s*\"([^\"]*)\"")


@dataclass
class Tag:
    """One markup tag: ``name``, attributes, and whether it closes/empties."""

    name: str
    attrs: dict = field(default_factory=dict)
    raw_attrs: str = ""
    close: bool = False
    self_close: bool = False
    pos: int = 0

    def __str__(self) -> str:
        if self.close:
            return f"</{self.name}>"
        tail = self.raw_attrs + ("/" if self.self_close else "")
        return f"<{self.name}{tail}>"


def _shift(data: bytes, sign: int) -> bytes:
    out = bytearray(len(data))
    for i, b in enumerate(data):
        s = (i % SHIFT_MOD) * sign
        if 65 <= b <= 90:
            out[i] = 65 + (b - 65 + s) % 26
        elif 97 <= b <= 122:
            out[i] = 97 + (b - 97 + s) % 26
        else:
            out[i] = b
    return bytes(out)


def decode(data: bytes) -> bytes:
    """Plain text of an encoded ``.sme`` blob (``encode``'s inverse)."""
    return _shift(data, -1)


def encode(data: bytes) -> bytes:
    """Encode plain markup into the ``.sme`` form (``decode``'s inverse)."""
    return _shift(data, +1)


def decode_text(data: bytes) -> str:
    """``decode`` + latin1 (the shipped files are 8-bit ASCII)."""
    return decode(data).decode("latin1")


def tags(text: str) -> list[Tag]:
    """Every tag of the markup, in order (names keep their case)."""
    out = []
    for m in _TAG.finditer(text):
        close, name, raw, self_close = m.group(1), m.group(2), m.group(3), m.group(4)
        attrs = {k: v for k, v in _ATTR.findall(raw)}
        out.append(Tag(name, attrs, raw, close == "/", self_close == "/",
                       m.start()))
    return out


def plain(text: str, breaks: tuple = BREAK_TAGS,
          placeholders: tuple = ("k", "K")) -> str:
    """Readable text of the markup.

    Block tags (``breaks``) become line breaks, placeholder tags (``<k>`` is
    the player's callsign) are kept as they are, tags with attributes are kept
    verbatim (images ``<I x= y= f= a=>``, fonts ``<f s=>``, sounds ``<M B= S=>``),
    and the remaining structural tags are dropped.
    """
    out: list[str] = []
    last = 0
    for m in _TAG.finditer(text):
        out.append(text[last:m.start()])
        last = m.end()
        close, name, raw = m.group(1), m.group(2), m.group(3)
        if close == "/":
            continue
        if name in breaks:
            out.append("\n")
        elif name in placeholders:
            out.append(m.group(0))
        elif raw.strip():
            out.append(m.group(0))
    out.append(text[last:])
    lines = [ln.strip() for ln in "".join(out).split("\n")]
    return "\n".join(ln for ln in lines if ln)

