"""``GUNMETAL.CFG`` — the binary options file of ``gundx.exe`` (v1.36).

The file is a fixed 0x2000-byte image.  The loader ``0x484cf0`` reads exactly
0x2000 bytes, compares ``0x25`` bytes of magic and then copies a run of dwords
straight out of the buffer into engine globals; the writer ``0x485440`` builds
the same 0x2000-byte image on the stack and dumps it at shutdown
(``0x4908a0``, ``"\n%s is done.\n"``; call at ``0x4908b2``).  Only the ranges below are *defined* by
that code:

    0x000 .. 0x024   magic   "GUNMETAL Configuration File. av1.93\\x1a\\0"
    0x025 .. 0x18f   uninitialised stack bytes in the shipped writer
    0x190 .. 0x8ab   fields (see :data:`FIELDS`)
    0x8ac .. 0x1fff  uninitialised stack bytes in the shipped writer

The writer never touches 0x025..0x18f or 0x8ac..0x1fff (it allocates 0x2030
bytes with ``mov eax,0x2030 / call 0x40d950``, writes the header and the fields,
then ``fwrite(buf, 1, 0x2000, f)``), so those bytes are whatever the stack held
at exit.  They differ between ``GUNMETAL.CFG`` and ``CONTROLS.CFG`` and are
kept verbatim here so that ``parse -> to_bytes`` is byte-exact.

Every field is a little-endian ``u32``.  Characters of the two key-profile
names are stored one per ``u32`` (the loader takes only the low byte), and the
three sensitivities are stored as integer milliseconds which the loader turns
into ``double`` by ``fild qword`` then ``* 0.001`` (``0x4a2910``), with a floor
of 0.2 s (``0x4a2930``).

``CONTROLS.CFG`` carries the same container magic but version ``av1.91`` and a
completely different body (a legacy key-binding dump with the profile names
``GODKEYS``/``PERFECT``/``SPLIT1``); no shipped executable references the name
``CONTROLS.CFG`` at all, and 1.36's loader rejects it on the version byte, so
it is parsed only as an opaque blob (:func:`parse_opaque`).
"""

from __future__ import annotations

import struct
from dataclasses import dataclass, field

MAGIC_PREFIX = b"GUNMETAL Configuration File. av"
MAGIC_TERM = b"\x1a\x00"
HEADER_SIZE = 0x25
DATA_START = 0x190
DATA_END = 0x8AC
FILE_SIZE = 0x2000

#: magic written by this build (``gundx.exe`` 1.36, 1998-10-01)
MAGIC_1_93 = b"GUNMETAL Configuration File. av1.93\x1a\x00"

#: magic of the shipped ``CONTROLS.CFG`` (older build, not read by 1.36)
MAGIC_1_91 = b"GUNMETAL Configuration File. av1.91\x1a\x00"

RECORD_SIZE = 0x18C
RECORD_COUNT = 4
RECORD_BASE = 0x27C
assert RECORD_BASE + RECORD_COUNT * RECORD_SIZE == DATA_END

NAME_LEN = 9

#: ``(offset, attribute, destination global, note)`` — every entry is one u32.
FIELDS = [
    (0x190, "music_volume", 0x4AAC74, "sound options: 'Music Volume:' (0x4bd508)"),
    (0x194, "effects_volume", 0x4AAC78, "sound options: 'Effects Volume:' (0x4bd4f8)"),
    (0x198, "marker", 0x4AAC7C, "loader forces 1; written back verbatim"),
    (0x19C, "swap_left_right", 0x5A494C, "sound options: 'Switch Left Right' (0x4bd4e4)"),
    (0x1A0, "width", 0x83D944,
     "screen width (0x486440 fills it from the mode table 0x4bfd18)"),
    (0x1A4, "height", 0x83D630, "screen height (same mode table)"),
    (0x1A8, "colour_depth", 0x83D190, "8 = 256 colours (compared with 8 at 0x446975)"),
    (0x1AC, "refresh", 0x83D314,
     "one of the five current-mode values filled by 0x486440 (Hz?)"),
    (0x1B0, "mode_flags", 0x83D2DC, "DirectDraw mode flags (bit 0x800 tested)"),
    (0x1B4, "border", 0x4BC1C8, "graphics options: 'Border:' (0x4bcf1c), 0..9 (cmp 9 at 0x48c773)"),
    (0x1B8, "gamma", 0x5A4950, "0 No / 1 Little / 2 Some / 3 Max (0x4bc6d8, '%s Gamma Correction')"),
    (0x1BC, "unknown_1bc", 0x4AAC80, "written and loaded, but never read by the game"),
    (0x1C0, "reflections", 0x4AAC84, "graphics options: 'Reflections' (0x4bcefc)"),
    (0x1C4, "light_maps", 0x4AAC88, "0 off / 1 low / 2 high (0x4bcebc/0x4bcee4/0x4bcecc)"),
    (0x1C8, "dynamic_camera", 0x4AAC8C, "0 none / 1 some / 2 full (0x4bcf38..0x4bcf68)"),
    (0x1CC, "play_flags_p1", 0x5A46F8,
     "play options bit word, player 1 (see PLAY_BITS)"),
    (0x1D0, "play_flags_p2", 0x5A46FC,
     "player 2 copy; the frontend toggles both together"),
    (0x1D4, "deathmatch_type", 0x68CD84, "index into 0x4bc6e8 (Normal/Scavenger/Classic)"),
    (0x1D8, "starting_cash", 0x4BC728,
     "index into the 12 values at 0x4bc6f8 (0x8ca0..0x4c4b40); "
     "'Starting Cash: %ld' (0x4bd374)"),
    (0x1DC, "deathmatch_flags", 0x68CD88,
     "checked bits: 0 Teamplay, 2 No Enemies, 4 Can't Buy Ammo, "
     "5 Handicap, 6 Mouse Cheating (0x4833e0: 0 = empty box)"),
]

#: indices into the axis-preset table at ``0x4bc468`` (18 entries, 12 bytes
#: each: ``{char *name; u32 action_lo; u32 action_hi}``).
AXES = [(0x24C, "axis_left_right", 0x68CD70),
        (0x250, "axis_forward_back", 0x68CD74),
        (0x254, "axis_up_down", 0x68CD78)]
AXIS_NAMES = ["Not Used", "Forward/Back", "Accel Turn L/R", "Accel Turn R/L",
              "Linear Turn L/R", "Linear Turn R/L", "Strafe L/R", "Strafe R/L",
              "Look Up/Down", "Look Down/Up", "Speed +/- Slide",
              "Speed -/+ Slide", "Speed +/- Nudge", "Speed -/+ Nudge",
              "Weapons +/-", "Weapons -/+", "Equip Cycle +/-",
              "Equip Cycle -/+"]

#: action names for the four joystick buttons (table at ``0x4bc558``).
BUTTON_NAMES = ["Move Forward", "Move Backward", "Turn Left", "Turn Right",
                "Look Center", "Fire", "Look Up", "Look Down", "Strafe",
                "Strafe Left", "Strafe Right", "Throttle Up", "Throttle Down",
                "Next Weapon", "Previous Weapon", "Activate",
                "Next Equipment", "Previous Equip.", "Toggle Equip. Use",
                "Headlights"]

#: joystick "Used By" (table at ``0x4bc548``).
USED_BY = ["Not Used", "Player 1", "Player 2"]

#: deathmatch modes (table at ``0x4bc6e8``).
DEATHMATCH_MODES = ["Normal Deathmatch", "Scavenger Hunt Deathmatch",
                    "Deathmatch Classic"]

#: "Starting Cash" values (table at ``0x4bc6f8``, 12 entries).
STARTING_CASH = [0x8CA0, 0xC350, 0x124F8, 0x186A0, 0x249F0, 0x30D40,
                 0x55730, 0x7A120, 0xB71B0, 0xF4240, 0x1E8480, 0x4C4B40]

#: gamma levels (table at ``0x4bc6d8``).
GAMMA_NAMES = ["No", "Little", "Some", "Max"]

#: play-option bits of 0x1CC/0x1D0 (labels at 0x4bcf98..0x4bd02c).
PLAY_BITS = [(0x400, "Skip Empty Weapons"),
             (0x1000, "Don't Auto-Target when Looking"),
             (0x4000, "Skip Deadly Weapons"),
             (0x8000, "Switch Weapon To Picked-Up Ammo"),
             (0x10000, "Auto Switch Weapons When Ammo Depleted"),
             (0x100000, "Mouse Cheating")]


def _u32(data: bytes, off: int) -> int:
    return struct.unpack_from("<I", data, off)[0]


def _pu32(out: bytearray, off: int, value: int) -> None:
    struct.pack_into("<I", out, off, value & 0xFFFFFFFF)


def _i32(value: int) -> int:
    return value - 0x100000000 if value & 0x80000000 else value


@dataclass
class FixedName:
    """``char name[9]`` stored as nine little-endian ``u32`` cells."""

    codes: list[int] = field(default_factory=lambda: [0] * NAME_LEN)

    @property
    def text(self) -> str:
        return "".join(chr(c & 0xFF) for c in self.codes).rstrip("\0")

    @classmethod
    def from_text(cls, text: str) -> "FixedName":
        if len(text) > NAME_LEN:
            raise ValueError("key profile name %r is longer than %d" % (text, NAME_LEN))
        codes = [ord(ch) for ch in text] + [0] * (NAME_LEN - len(text))
        return cls(codes)

    def to_bytes(self) -> bytes:
        return b"".join(struct.pack("<I", c) for c in self.codes)


@dataclass
class AxisEntry:
    """One of the six joystick axis/hat slots of a ``ControlRecord``.

    The two ``u32`` ``value``/``extra`` are copied to
    ``0x4aa894 + 0x30*record + 8*slot``; the low byte of ``flag`` goes to
    ``0x68cd00 + 6*record + slot`` (it is an index into the 18-entry axis
    preset table at ``0x4bc468``, used at ``0x48c213``).  ``value``/``extra``
    are action bit masks; ``-1`` means unassigned.  Their exact per-axis meaning is not proven, so
    they are kept raw (the upper 24 bits of ``flag`` are inert at run time but
    are preserved so the file round-trips even if they are not zero).
    """

    value: int
    extra: int
    flag: int

    @property
    def byte(self) -> int:
        return self.flag & 0xFF


@dataclass
class ButtonEntry:
    """One of the sixteen entries at record offset ``0xCC``.

    ``action`` is a *bit number* (the code does ``1 << action`` at
    ``0x44c054``; ``-1`` = unassigned) copied to
    ``0x4aaa10 + 0x40*record + 4*index``; the low bytes of ``lo``/``hi`` go to
    ``0x68cd30 + 2*(4*record + index // 4)`` (only the last entry of each group
    of four survives at run time, the rest are overwritten in order).  The
    upper 24 bits are inert but preserved for round-trip.
    """

    action: int
    lo: int
    hi: int

    @property
    def lo_byte(self) -> int:
        return self.lo & 0xFF

    @property
    def hi_byte(self) -> int:
        return self.hi & 0xFF


@dataclass
class ControlRecord:
    """One of the four per-device control records (``0x27C + 0x18C*i``).

    They are identical in the shipped ``GUNMETAL.CFG``.  The four blocks are
    indexed by *input channel*; the numbers are action bit masks (the code
    does ``test mask, key_bit`` at ``0x44bfd4``), with ``-1`` meaning unbound.
    Which physical device/channel each slot belongs to is not proven.
    """

    slot: int = 0
    buttons: list[int] = field(default_factory=lambda: [0] * 32)
    axes: list[AxisEntry] = field(
        default_factory=lambda: [AxisEntry(0, 0, 0) for _ in range(6)])
    extra: list[ButtonEntry] = field(
        default_factory=lambda: [ButtonEntry(0, 0, 0) for _ in range(16)])


@dataclass
class Config:
    """A fully parsed ``GUNMETAL.CFG`` (version 1.93 layout)."""

    magic: bytes = MAGIC_1_93
    #: 0x025..0x18F; zero for a fresh file, verbatim from the loaded one
    gap: bytes = b"\0" * (DATA_START - HEADER_SIZE)
    music_volume: int = 0
    effects_volume: int = 0
    marker: int = 1
    swap_left_right: int = 0
    width: int = 640
    height: int = 480
    colour_depth: int = 8
    refresh: int = 0
    mode_flags: int = 0
    border: int = 0
    gamma: int = 0
    unknown_1bc: int = 0
    reflections: int = 0
    light_maps: int = 0
    dynamic_camera: int = 0
    play_flags_p1: int = 0
    play_flags_p2: int = 0
    deathmatch_type: int = 0
    deathmatch_flags: int = 0
    starting_cash: int = 0
    key_names: list[FixedName] = field(
        default_factory=lambda: [FixedName(), FixedName()])
    spare_keys_a: list[int] = field(default_factory=lambda: [-1] * 4)
    spare_keys_b: list[int] = field(default_factory=lambda: [-1] * 4)
    joystick_used_by: int = 0
    axis_left_right: int = 0
    axis_forward_back: int = 0
    axis_up_down: int = 0
    #: three raw millisecond values; the engine reads them as seconds / 1000
    sensitivity: list[int] = field(default_factory=lambda: [200, 200, 200])
    buttons: list[int] = field(default_factory=lambda: [5, 15, 5, 15])
    assign_slot: list[int] = field(default_factory=lambda: [-1, -1])
    records: list[ControlRecord] = field(
        default_factory=lambda: [ControlRecord() for _ in range(RECORD_COUNT)])
    #: 0x8AC..EOF; zero for a fresh file, verbatim from the loaded one
    tail: bytes = b"\0" * (FILE_SIZE - DATA_END)

    # -- derived ---------------------------------------------------------
    @property
    def version(self) -> str:
        return magic_version(self.magic)

    @property
    def sensitivity_seconds(self) -> list[float]:
        return [v / 1000.0 for v in self.sensitivity]

    @property
    def play_options(self) -> list[str]:
        """Names of the play-option bits set in ``play_flags_p1``."""
        return [name for bit, name in PLAY_BITS if self.play_flags_p1 & bit]

    @property
    def gamma_name(self) -> str:
        return GAMMA_NAMES[self.gamma] if 0 <= self.gamma < len(GAMMA_NAMES) else "?"

    def axis_name(self, index: int) -> str:
        return AXIS_NAMES[index] if 0 <= index < len(AXIS_NAMES) else "?%d" % index

    def button_name(self, index: int) -> str:
        return BUTTON_NAMES[index] if 0 <= index < len(BUTTON_NAMES) else "?%d" % index

    # -- serialisation ---------------------------------------------------
    def to_bytes(self) -> bytes:
        if len(self.magic) != HEADER_SIZE:
            raise ValueError("magic must be %d bytes, got %d"
                             % (HEADER_SIZE, len(self.magic)))
        if len(self.gap) != DATA_START - HEADER_SIZE:
            raise ValueError("gap must be %d bytes, got %d"
                             % (DATA_START - HEADER_SIZE, len(self.gap)))
        out = bytearray(DATA_END)
        out[:HEADER_SIZE] = self.magic
        out[HEADER_SIZE:DATA_START] = self.gap
        for off, attr, _dest, _note in FIELDS:
            _pu32(out, off, getattr(self, attr))
        for col, name in enumerate(self.key_names[:2]):
            if len(name.codes) != NAME_LEN:
                raise ValueError("key profile name must have %d characters" % NAME_LEN)
            for k, code in enumerate(name.codes):
                _pu32(out, 0x1E0 + 8 * k + 4 * col, code)
        for i, value in enumerate(self.spare_keys_a[:4]):
            _pu32(out, 0x228 + 4 * i, value)
        for i, value in enumerate(self.spare_keys_b[:4]):
            _pu32(out, 0x238 + 4 * i, value)
        _pu32(out, 0x248, self.joystick_used_by)
        _pu32(out, 0x24C, self.axis_left_right)
        _pu32(out, 0x250, self.axis_forward_back)
        _pu32(out, 0x254, self.axis_up_down)
        for i, value in enumerate(self.sensitivity[:3]):
            _pu32(out, 0x258 + 4 * i, value)
        for i, value in enumerate(self.buttons[:4]):
            _pu32(out, 0x264 + 4 * i, value)
        for i, value in enumerate(self.assign_slot[:2]):
            _pu32(out, 0x274 + 4 * i, value)
        for i, rec in enumerate(self.records[:RECORD_COUNT]):
            base = RECORD_BASE + RECORD_SIZE * i
            _pu32(out, base, rec.slot)
            for k, value in enumerate(rec.buttons[:32]):
                _pu32(out, base + 0x04 + 4 * k, value)
            for k, ax in enumerate(rec.axes[:6]):
                _pu32(out, base + 0x84 + 12 * k, ax.value)
                _pu32(out, base + 0x88 + 12 * k, ax.extra)
                _pu32(out, base + 0x8C + 12 * k, ax.flag)
            for k, bt in enumerate(rec.extra[:16]):
                _pu32(out, base + 0xCC + 12 * k, bt.action)
                _pu32(out, base + 0xD0 + 12 * k, bt.lo)
                _pu32(out, base + 0xD4 + 12 * k, bt.hi)
        return bytes(out) + self.tail


def magic_version(magic: bytes) -> str:
    """``b"...av1.93\\x1a\\0"`` -> ``"1.93"`` (empty when not a config header)."""
    if not magic.startswith(MAGIC_PREFIX):
        return ""
    tail = magic[len(MAGIC_PREFIX):].rstrip(b"\x00")
    return tail[:tail.find(b"\x1a")].decode("latin1") if b"\x1a" in tail else ""


def is_config(data: bytes) -> bool:
    return len(data) >= HEADER_SIZE and data.startswith(MAGIC_PREFIX)


def parse(data: bytes, kind: str = "auto") -> "Config | OpaqueConfig":
    """Parse ``GUNMETAL.CFG`` (*kind* ``"gunmetal"``/``"auto"``) or the legacy
    container (*kind* ``"controls"``/``"opaque"``).

    ``auto`` applies the 1.93 layout when the header says ``av1.93`` and falls
    back to :func:`parse_opaque` for any other version; the body of an older
    file is *not* interpreted because its layout is unproven.
    """
    if kind in ("controls", "opaque"):
        return parse_opaque(data)
    if kind not in ("auto", "gunmetal"):
        raise ValueError("unknown config kind %r" % kind)
    if len(data) < DATA_END:
        raise ValueError("config truncated: %d bytes, need at least %d"
                         % (len(data), DATA_END))
    magic = data[:HEADER_SIZE]
    if not is_config(data):
        raise ValueError("not a Gunmetal config: %r" % magic[:16])
    if magic[-2:] != MAGIC_TERM:
        raise ValueError("bad config magic terminator %r" % magic[-4:])
    version = magic_version(magic)
    if kind == "auto" and version != "1.93":
        return parse_opaque(data)
    if version != "1.93":
        raise ValueError("no proven layout for config version %r (only 1.93)"
                         % (version or "?"))
    cfg = Config()
    cfg.magic = magic
    cfg.gap = data[HEADER_SIZE:DATA_START]
    cfg.tail = data[DATA_END:]
    for off, attr, _dest, _note in FIELDS:
        setattr(cfg, attr, _u32(data, off))
    names = [[0] * NAME_LEN for _ in range(2)]
    for k in range(NAME_LEN):
        names[0][k] = _u32(data, 0x1E0 + 8 * k)
        names[1][k] = _u32(data, 0x1E0 + 8 * k + 4)
    cfg.key_names = [FixedName(names[0]), FixedName(names[1])]
    cfg.spare_keys_a = [_u32(data, 0x228 + 4 * i) for i in range(4)]
    cfg.spare_keys_b = [_u32(data, 0x238 + 4 * i) for i in range(4)]
    cfg.joystick_used_by = _u32(data, 0x248)
    cfg.axis_left_right = _u32(data, 0x24C)
    cfg.axis_forward_back = _u32(data, 0x250)
    cfg.axis_up_down = _u32(data, 0x254)
    cfg.sensitivity = [_u32(data, 0x258 + 4 * i) for i in range(3)]
    cfg.buttons = [_u32(data, 0x264 + 4 * i) for i in range(4)]
    cfg.assign_slot = [_u32(data, 0x274 + 4 * i) for i in range(2)]
    recs = []
    for i in range(RECORD_COUNT):
        base = RECORD_BASE + RECORD_SIZE * i
        rec = ControlRecord(slot=_u32(data, base))
        rec.buttons = [_u32(data, base + 0x04 + 4 * k) for k in range(32)]
        rec.axes = [AxisEntry(_u32(data, base + 0x84 + 12 * k),
                              _u32(data, base + 0x88 + 12 * k),
                              _u32(data, base + 0x8C + 12 * k))
                    for k in range(6)]
        rec.extra = [ButtonEntry(_u32(data, base + 0xCC + 12 * k),
                                 _u32(data, base + 0xD0 + 12 * k),
                                 _u32(data, base + 0xD4 + 12 * k))
                     for k in range(16)]
        recs.append(rec)
    cfg.records = recs
    return cfg


# --- legacy / opaque container --------------------------------------------
@dataclass
class OpaqueConfig:
    """A config with the shared 0x2000-byte container but an unknown body.

    The shipped ``CONTROLS.CFG`` is such a file (``av1.91``, 1998-05-09): it
    holds a key-binding dump of an older build (``GODKEYS`` profile, pairs of
    scancodes at 0x28.. and profile records at 0x9c stride), but no shipped
    executable reads it, so nothing beyond the header is claimed.
    """

    magic: bytes = MAGIC_1_91
    body: bytes = b""

    @property
    def version(self) -> str:
        return magic_version(self.magic)

    def to_bytes(self) -> bytes:
        return self.magic + self.body

    def describe(self) -> str:
        return ("opaque config: magic %r, version %s, %d bytes body"
                % (self.magic.rstrip(b"\0").decode("latin1"),
                   self.version or "?", len(self.body)))


def parse_opaque(data: bytes) -> OpaqueConfig:
    if len(data) < HEADER_SIZE:
        raise ValueError("config too short: %d bytes" % len(data))
    if not is_config(data):
        raise ValueError("not a Gunmetal config: %r" % data[:16])
    return OpaqueConfig(magic=data[:HEADER_SIZE], body=data[HEADER_SIZE:])


# --- human readable dump ---------------------------------------------------
def _fmt_value(attr: str, value: int, cfg: Config) -> str:
    if attr.startswith("axis_"):
        return "%d (%s)" % (value, cfg.axis_name(value))
    if attr in ("buttons", "button"):
        return "%d (%s)" % (value, cfg.button_name(value))
    if attr == "joystick_used_by":
        return "%d (%s)" % (value, USED_BY[value] if 0 <= value < 3 else "?")
    if attr == "deathmatch_type":
        return "%d (%s)" % (value, DEATHMATCH_MODES[value]
                            if 0 <= value < 3 else "?")
    if attr == "starting_cash":
        return "%d (cash %s)" % (value, STARTING_CASH[value]
                                  if 0 <= value < len(STARTING_CASH) else "?")
    if attr == "gamma":
        return "%d (%s)" % (value, GAMMA_NAMES[value]
                            if 0 <= value < len(GAMMA_NAMES) else "?")
    if attr == "play_flags_p1":
        return "0x%05x (%s)" % (value, ", ".join(
            name for bit, name in PLAY_BITS if value & bit) or "none")
    if attr == "play_flags_p2":
        return "0x%05x" % value
    return str(value)


def dump(cfg: Config) -> str:
    """Human-readable field map; offsets match :data:`FIELDS`."""
    lines = ["GUNMETAL.CFG  version %s  %d bytes" % (cfg.version, FILE_SIZE),
             "header 0x000  %r" % cfg.magic,
             "opaque 0x025..0x18f  %d bytes (uninitialised in the writer)"
             % len(cfg.gap)]
    for off, attr, dest, note in FIELDS:
        lines.append("0x%03x  %-18s = %-10s  -> 0x%06x  %s"
                     % (off, attr, _fmt_value(attr, getattr(cfg, attr), cfg),
                        dest, note))
    for i, name in enumerate(cfg.key_names):
        lines.append("0x%03x  key_profile_%d     = %-10r  -> 0x6c2058+%d*0x108"
                     % (0x1E0 + 4 * i, i + 1, name.text, i))
    lines.append("0x228  spare_keys_a      = %s  -> 0x4a5ae8 (unused)"
                 % [_i32(v) for v in cfg.spare_keys_a])
    lines.append("0x238  spare_keys_b      = %s  -> 0x4a5af8 (unused)"
                 % [_i32(v) for v in cfg.spare_keys_b])
    lines.append("0x248  joystick_used_by  = %s  -> 0x5a4938"
                 % _fmt_value("joystick_used_by", cfg.joystick_used_by, cfg))
    for off, attr, dest in AXES:
        lines.append("0x%03x  %-18s = %s  -> 0x%06x"
                     % (off, attr, _fmt_value(attr, getattr(cfg, attr), cfg), dest))
    for i, value in enumerate(cfg.sensitivity):
        lines.append("0x%03x  sensitivity_%d      = %d ms (%.3f s)  -> 0x4aac%02x"
                     % (0x258 + 4 * i, i + 1, value, value / 1000.0,
                        0x38 + 8 * i))
    for i, value in enumerate(cfg.buttons):
        lines.append("0x%03x  joystick_button_%d = %s  -> 0x4aac%02x"
                     % (0x264 + 4 * i, i + 1,
                        _fmt_value("button", value, cfg), 0x10 + 4 * i))
    lines.append("0x274  assign_slot      = %s  -> 0x4bc540/0x4bc544"
                 % [_i32(v) for v in cfg.assign_slot])
    for i, rec in enumerate(cfg.records):
        base = RECORD_BASE + RECORD_SIZE * i
        lines.append("0x%03x  control record %d  (0x%x bytes)"
                     % (base, i, RECORD_SIZE))
        lines.append("   +0x000 slot         = %d" % rec.slot)
        lines.append("   +0x004 buttons[32]  = %s ..."
                     % " ".join(str(_i32(v)) for v in rec.buttons[:16]))
        lines.append("   +0x084 axes[6]      = %s"
                     % ", ".join("(%d,%d,%d)" % (_i32(a.value), _i32(a.extra), a.byte)
                                 for a in rec.axes))
        lines.append("   +0x0cc buttons2[16] = %s"
                     % ", ".join("(%d,%d,%d)" % (_i32(b.action), b.lo_byte, b.hi_byte)
                                 for b in rec.extra))
    lines.append("opaque 0x8ac..0x%03x  %d bytes (uninitialised in the writer)"
                 % (DATA_END + len(cfg.tail), len(cfg.tail)))
    return "\n".join(lines)


def summary(cfg: "Config | OpaqueConfig") -> str:
    """One-line description used by the CLI."""
    if isinstance(cfg, OpaqueConfig):
        return cfg.describe()
    return ("version %s, %dx%dx%d, music %d, effects %d, keys %s/%s, %d records"
            % (cfg.version, cfg.width, cfg.height, cfg.colour_depth,
               cfg.music_volume, cfg.effects_volume,
               cfg.key_names[0].text, cfg.key_names[1].text, len(cfg.records)))
