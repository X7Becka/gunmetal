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
module exposes the tag stream, the UI elements and a tag-stripped text rather
than a tree.  The engine's loader (``0x497310``) accepts a ``.sml`` path,
appends ``E`` and prefers the encoded ``.sme`` sibling; if that file is absent
it falls back to the plain ``.sml``.  The document must start with ``<d>`` and
end with ``</d>`` (``0x497650``), and the loader records at most 511 tags.

The flat stream is still a UI description.  :func:`elements` exposes the
ordered elements (images, fonts, music, rows, paragraphs, columns,
placeholders and text runs) and :func:`describe` summarises a document;
neither performs font layout, which needs the engine's glyph metrics.

Implementation: ``gm/sml.py`` (``decode``/``encode``/``tags``/``plain``);
CLI ``gm.py sml <paths> <out> [--rebuild DIR]``.
"""

from __future__ import annotations

import re
from bisect import bisect_right
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

#: the engine's loader refuses a document with this many tags (``0x49779c``:
#: ``count >= 0x200`` is an error; the two offset arrays hold 512 entries)
MAX_TAGS = 512


def _valid_document(text: str) -> bool:
    """True when *text* looks like a shipped ``<d>...</d>`` document."""
    ts = tags(text)
    if not ts:
        return False
    if ts[0].close or ts[0].name.lower() != "d":
        return False
    if not ts[-1].close or ts[-1].name.lower() != "d":
        return False
    return not (tag_letters(text) - SHIPPED_TAGS)


def decode_candidates(data: bytes) -> list:
    """Every plausible decode of an ``.sme`` blob, best-effort order.

    The shipped corpus contains two transport anomalies that a plain
    positional decode cannot see:

    * ``brief/level17.sme`` has one *extra* CR/LF inserted after encoding;
      collapsing it to the one space it replaced restores the document
      (:func:`repair_newlines`);
    * ``prelevel/LEVEL20.sme`` and ``prelevel/LEVEL4.sme`` were encoded from
      LF-only text and then shipped with CRLF line endings.  Every CR shifts
      the positional key by one, so the second and later lines decode as
      gibberish.  Removing the CR bytes restores the original byte stream.

    The list is ``[(note, text), ...]`` with notes ``"as-is"``,
    ``"newline-repair"`` and ``"crlf-strip"``.  Only valid ``<d>...</d>``
    decodes are included.
    """
    out = []
    raw = decode_text(data)
    if _valid_document(raw):
        out.append(("as-is", raw))
    repaired = decode_text(data, repair=True)
    if _valid_document(repaired) and all(repaired != t for _n, t in out):
        out.append(("newline-repair", repaired))
    stripped = decode_text(data.replace(b"\r", b""))
    if _valid_document(stripped) and all(stripped != t for _n, t in out):
        out.append(("crlf-strip", stripped))
    return out


def decode_best(data: bytes) -> tuple:
    """Decode an ``.sme`` blob and return ``(text, note)``.

    ``note`` says which repair was needed: ``"as-is"`` for a normally encoded
    file, ``"newline-repair"`` for the level17 anomaly, ``"crlf-strip"`` for
    the LF-encoded/CRLF-transported files, or ``"unrepaired"`` when no variant
    produced a ``<d>...</d>`` document.  Unlike :func:`decode_auto` this looks
    at the document wrapper as well as the tag letters, so it also catches the
    two CRLF-desynchronised files.
    """
    choices = decode_candidates(data)
    if not choices:
        return decode_text(data), "unrepaired"
    if choices[0][0] == "as-is":
        return choices[0][1], "as-is"
    # A single stray CR/LF is the level17 shape; several CRLF runs mean the
    # whole file was transported with the wrong line endings.
    prefer = ("crlf-strip", "newline-repair") if data.count(b"\r\n") > 1 \
        else ("newline-repair", "crlf-strip")
    for note in prefer:
        for cand_note, text in choices:
            if cand_note == note:
                return text, note
    return choices[0][1], choices[0][0]


@dataclass
class Element:
    """One UI element of an ``.sml``/``.sme`` document.

    ``kind`` is ``"doc"``, ``"row"``, ``"paragraph"``, ``"column"``,
    ``"image"``, ``"font"``, ``"music"``, ``"placeholder"``, ``"text"``,
    ``"close"`` or ``"unknown"``.  ``pos`` is the byte offset in the decoded
    text, ``line`` its 1-based source line.  ``value`` is the image name, font
    number, placeholder substitution or tag name; ``attrs`` keeps the raw
    attribute spelling (case included).
    """

    kind: str
    tag: str | None
    pos: int
    attrs: dict = field(default_factory=dict)
    value: object = None
    x: int | None = None
    y: int | None = None
    align: str | None = None
    track: int | None = None
    offset: int | None = None
    line: int = 0
    text: str = ""

    def as_dict(self) -> dict:
        return {
            "kind": self.kind,
            "tag": self.tag,
            "pos": self.pos,
            "line": self.line,
            "value": self.value,
            "x": self.x,
            "y": self.y,
            "align": self.align,
            "track": self.track,
            "offset": self.offset,
            "attrs": self.attrs,
            "text": self.text,
        }


def _attr(attrs: dict, *names) -> object:
    """Case-insensitive attribute lookup (``F``/``f``, ``B``/``b``, ...)."""
    for key, value in attrs.items():
        if key.lower() in names:
            return value
    return None


def _int_attr(attrs: dict, *names) -> int | None:
    value = _attr(attrs, *names)
    if value is None:
        return None
    try:
        return int(str(value).strip())
    except ValueError:
        return None


def _line_starts(text: str) -> list:
    return [0] + [m.end() for m in re.finditer(r"\n", text)]


def _line_of(starts: list, pos: int) -> int:
    return bisect_right(starts, pos)


def elements(text: str) -> list:
    """Ordered UI elements of a decoded document.

    Unlike :func:`plain` this keeps the images, fonts and music and pairs each
    text run with its source line.  Blocks are *stream* markers, not a tree:
    ``<p>`` stays in force until the next block tag, exactly as in the engine.
    """
    out: list[Element] = []
    starts = _line_starts(text)
    last = 0

    def add_text(pos: int, body: str) -> None:
        body = re.sub(r"\s+", " ", body).strip()
        if body:
            out.append(Element("text", None, pos, value=body, text=body,
                               line=_line_of(starts, pos)))

    for m in _TAG.finditer(text):
        add_text(last, text[last:m.start()])
        last = m.end()
        close, name, raw, self_close = (m.group(1), m.group(2), m.group(3),
                                        m.group(4))
        attrs = {k: v for k, v in _ATTR.findall(raw)}
        low = name.lower()
        pos = m.start()
        line = _line_of(starts, pos)
        if close:
            out.append(Element("close", name, pos, attrs, value=name,
                               line=line))
        elif low == "d":
            out.append(Element("doc", name, pos, attrs, value=name, line=line))
        elif low == "i":
            out.append(Element("image", name, pos, attrs,
                               value=_attr(attrs, "f"),
                               x=_int_attr(attrs, "x"),
                               y=_int_attr(attrs, "y"),
                               align=(str(_attr(attrs, "a")).lower()
                                      if _attr(attrs, "a") is not None else None),
                               line=line))
        elif low == "f":
            out.append(Element("font", name, pos, attrs,
                               value=_int_attr(attrs, "s"), line=line))
        elif low == "m":
            out.append(Element("music", name, pos, attrs,
                               value=_int_attr(attrs, "b"),
                               track=_int_attr(attrs, "b"),
                               offset=_int_attr(attrs, "s"), line=line))
        elif low == "r":
            out.append(Element("row", name, pos, attrs, value=name, line=line))
        elif low == "p":
            out.append(Element("paragraph", name, pos, attrs, value=name,
                               line=line))
        elif low == "c":
            out.append(Element("column", name, pos, attrs, value=name,
                               line=line))
        elif low in ("k", "s"):
            out.append(Element("placeholder", name, pos, attrs,
                               value=PLACEHOLDERS.get(name, ""), line=line))
        else:
            out.append(Element("unknown", name, pos, attrs, value=name,
                               line=line))
    add_text(last, text[last:])
    return out


def describe(text: str) -> dict:
    """Summary of a decoded document, suitable for JSON / UI inspection.

    The result is intentionally layout-free: it reports the tag/element
    inventory and the declared UI data (images, fonts, music), not the pixel
    positions that require the engine's font metrics.
    """
    ts = tags(text)
    els = elements(text)
    tag_counts: dict = {}
    attrs: dict = {}
    for t in ts:
        tag_counts[t.name] = tag_counts.get(t.name, 0) + 1
        for key in t.attrs:
            k = f"{t.name.lower()}.{key.lower()}"
            attrs[k] = attrs.get(k, 0) + 1
    known = {t.name for t in ts if not t.close}
    unknown = sorted(known - SHIPPED_TAGS)
    doc_open = bool(ts) and not ts[0].close and ts[0].name.lower() == "d"
    doc_close = bool(ts) and ts[-1].close and ts[-1].name.lower() == "d"
    images = [e.as_dict() for e in els if e.kind == "image"]
    fonts = [e.as_dict() for e in els if e.kind == "font"]
    music = [e.as_dict() for e in els if e.kind == "music"]
    body = plain(text)
    # readable text: ``plain`` resolves placeholders/breaks but keeps tags that
    # carry attributes; strip those for a preview that is pure text
    text_only = re.sub(r"\s+", " ", _TAG.sub("", body)).strip()
    return {
        "chars": len(text),
        "lines": text.count("\n") + 1,
        "tags": len(ts),
        "tag_limit": MAX_TAGS,
        "too_many_tags": len(ts) >= MAX_TAGS,
        "tag_counts": dict(sorted(tag_counts.items())),
        "tag_letters": sorted(known),
        "unknown_tags": unknown,
        "attributes": dict(sorted(attrs.items())),
        "doc": {
            "open": doc_open,
            "close": doc_close,
            "ok": doc_open and doc_close,
        },
        "brackets": {
            "open": text.count("<"),
            "close": text.count(">"),
            "balanced": text.count("<") == text.count(">"),
        },
        "images": images,
        "fonts": fonts,
        "music": music,
        "placeholders": {
            "k": sum(1 for e in els if e.kind == "placeholder"
                     and e.tag.lower() == "k"),
            "s": sum(1 for e in els if e.kind == "placeholder"
                     and e.tag.lower() == "s"),
        },
        "text_runs": sum(1 for e in els if e.kind == "text"),
        "plain": body,
        "body": text_only,
        "text_preview": text_only[:400],
    }
