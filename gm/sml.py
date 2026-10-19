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
``PLayer``) plus a ``P - Pause`` line the ``.sml`` lacks.  Every one of the
151 ``.sme`` decodes to the shipped tag letters, which is what validates the
shift; the single exception, ``brief/level17.sme``, carries a stray CR/LF at
offset 70 that only the repair in :func:`decode_auto` undoes.

The markup is a flat stream of tags, and the engine's tag dispatcher
(``0x497d30``: the byte after ``<`` picks a handler through the class table
``0x498448`` and the jump table ``0x4983f8``) recognises only the nine letters
in :data:`TAG_ROLES` -- ``R P S K F C X M I`` plus ``/``; every other letter,
``d``/``D`` included, is ignored.  ``<k>`` inserts the callsign string
"Soldier" (``ds:0x4bc2b8``), ``<s>`` inserts ``" "`` (``ds:0x4ab74c``),
``<f s=>`` selects the font, ``<c>`` marks a centred column of a ``<r>`` row,
``<x>`` sets a percentage position, ``<M B= S=>`` starts music and
``<I x= y= f= a=>`` places a sprite; a newline byte in the file is turned into
a space (``0x497784``).

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

#: block-level tags that ``plain`` turns into line breaks (proven: the engine
#: dispatches on the byte after '<' at ``0x497d30`` through the class table
#: ``0x498448`` / jump table ``0x4983f8``; ``r``/``R`` start a row, ``p``/``P``
#: indent a paragraph, ``c``/``C`` centre one column, ``f``/``F`` select a font)
BREAK_TAGS = ("r", "R", "p", "P")

#: letters the engine dispatches; every other byte inside a tag is ignored
TAG_ROLES = {
    "r": "new row (line)",
    "p": "paragraph: shifts the line width by half the text length",
    "c": "one centred column of the row (its text is the following segment)",
    "f": "font: the ``s``/``S`` attribute selects the font (0x498580)",
    "x": 'horizontal position in percent of the width (value*W/100, "<X 2>")',
    "k": 'player callsign placeholder, inserted from ds:0x4bc2b8 "Soldier"',
    's': 'inserts the one-character string ds:0x4ab74c " "',
    "m": "music: attr ``B`` goes to ds:0x68cc8c (track), ``S`` to ds:0x68cc88",
    "i": "image: attrs ``x``/``y`` position, ``f`` sprite name, ``a`` action key",
    "/": "closing tag: pops the markup state (ds:0x68e660)",
}

#: substitutions ``plain`` makes for the placeholder tags (proven strings)
PLACEHOLDERS = {"k": "Soldier", "K": "Soldier", "s": " ", "S": " "}

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


def repair_newlines(data: bytes) -> bytes:
    """Replace CR/LF runs with one space -- the engine's own ``\\n`` rule.

    One shipped file (``brief/level17.sme``) carries a stray ``\\r\\n`` at
    offset 70.  Because both the engine (``0x497650`` counts every byte) and
    this decoder shift by the *byte* index, that break desynchronises the
    whole tail into gibberish; collapsing it to the single space the position
    originally held restores the briefing.
    """
    return re.sub(rb"[\r\n]+", b" ", data)


def decode_text(data: bytes, repair: bool = False) -> str:
    """``decode`` + latin1 (the shipped files are 8-bit ASCII).

    With ``repair`` a stray CR/LF run is first collapsed to one space, so a
    file mangled by an editor still reads correctly (see ``repair_newlines``).
    """
    if repair and (b"\r" in data or b"\n" in data):
        data = repair_newlines(data)
    return decode(data).decode("latin1")


#: tag letters used by the shipped markup (the engine handles a subset)
SHIPPED_TAGS = frozenset("dDrRfFpPcCsSkIM")


def tag_letters(text: str) -> set:
    """The opening tag letters of *text* (used to validate a decode)."""
    return {t.name for t in tags(text) if not t.close}


def decode_auto(data: bytes) -> tuple[str, bool]:
    """Decode an ``.sme`` blob, repairing a stray CR/LF only when needed.

    The shift is positional, so a CR/LF inserted by an editor after encoding
    desynchronises the tail (and the engine, which counts every byte, garbles
    it too).  The as-is decode is trusted while it yields the shipped tag
    letters; otherwise the newline run is collapsed to the single space it
    replaced and the result is returned with ``True``.  Only
    ``brief/level17.sme`` needs that in the shipped data.
    """
    text = decode_text(data)
    if tag_letters(text) <= SHIPPED_TAGS:
        return text, False
    fixed = decode_text(data, repair=True)
    if tag_letters(fixed) <= SHIPPED_TAGS:
        return fixed, True
    return text, False


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
          placeholders: dict = PLACEHOLDERS, column_gap: str = "  ") -> str:
    """Readable text of the markup, following the engine's tag semantics.

    ``<r>``/``<p>`` become line breaks, ``<c>`` separates the columns of a row
    with ``column_gap``, the placeholders are substituted (``<k>`` is the
    callsign, ``<s>`` is a space) and tags carrying attributes are kept
    verbatim (images ``<I x= y= f= a=>``, music ``<M B= S=>``, fonts
    ``<f s="2">``); all other tags -- including ``<d>``/``<D>`` -- are dropped,
    exactly like the engine ignores them.
    """
    out: list[str] = []
    last = 0
    for m in _TAG.finditer(text):
        out.append(text[last:m.start()])
        last = m.end()
        close, name, raw = m.group(1), m.group(2), m.group(3)
        if close == "/":
            continue
        low = name.lower()
        if low in ("r", "p"):
            out.append("\n")
        elif low == "c":
            out.append(column_gap)
        elif name in placeholders:
            out.append(placeholders[name])
        elif raw.strip():
            out.append(m.group(0))
    out.append(text[last:])
    lines = [ln.strip() for ln in "".join(out).split("\n")]
    return "\n".join(ln for ln in lines if ln)

