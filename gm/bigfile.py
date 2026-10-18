"""Gunmetal "BIGFILE" archives (.idx + .dat pairs).

Verified layout:

    IDX:  "BIGFILE" (7 bytes)  0x1A  u32 count
          count * { char name[8]; u32 offset }

    DAT:  raw concatenation of blobs.  Blob size = next offset - offset,
          the last one runs to EOF.

Names are 8 chars, NUL padded, and are the *virtual* file names: the engine
addresses entries as `PICS\\<name>.sbm`, `sounds\\<name>.sou`, ...

Payload types seen:
    FRONTEND/BMP.DAT    Windows BMP files
    PICS/SBM.DAT        .sbm textures (see gm/sbm.py)
    sprites/SBM.DAT     .sbm sprites (same format, 1 mip level)
    sounds/SOU.DAT      .sou samples (see gm/sou.py)
"""

from __future__ import annotations

import os
import struct
from dataclasses import dataclass

MAGIC = b"BIGFILE"


@dataclass
class Entry:
    name: str
    offset: int
    size: int

    def data(self, dat: bytes) -> bytes:
        return dat[self.offset : self.offset + self.size]


class BigFile:
    def __init__(self, idx_path: str, dat_path: str | None = None):
        self.idx_path = idx_path
        self.dat_path = dat_path or os.path.splitext(idx_path)[0] + ".DAT"
        if not os.path.exists(self.dat_path):
            self.dat_path = os.path.splitext(idx_path)[0] + ".dat"

        idx = open(idx_path, "rb").read()
        if idx[: len(MAGIC)] != MAGIC:
            raise ValueError(f"{idx_path}: not a BIGFILE index")
        self.count = struct.unpack_from("<I", idx, 8)[0]
        need = 12 + self.count * 12
        if len(idx) < need:
            raise ValueError(f"{idx_path}: index too short for {self.count} entries")

        raw: list[tuple[str, int]] = []
        for i in range(self.count):
            o = 12 + 12 * i
            name = idx[o : o + 8].rstrip(b"\0").decode("latin1")
            off = struct.unpack_from("<I", idx, o + 8)[0]
            raw.append((name, off))

        self.dat_size = os.path.getsize(self.dat_path)
        self.entries: list[Entry] = []
        for i, (name, off) in enumerate(raw):
            end = raw[i + 1][1] if i + 1 < len(raw) else self.dat_size
            self.entries.append(Entry(name, off, end - off))

    def __iter__(self):
        return iter(self.entries)

    def __getitem__(self, name: str) -> Entry:
        key = name.lower()
        for e in self.entries:
            if e.name.lower() == key:
                return e
        raise KeyError(name)

    def read(self, entry: Entry) -> bytes:
        with open(self.dat_path, "rb") as f:
            f.seek(entry.offset)
            return f.read(entry.size)

    def extract_all(self, outdir: str, suffix: str = ".bin", on_entry=None) -> int:
        os.makedirs(outdir, exist_ok=True)
        done = set()
        n = 0
        data = open(self.dat_path, "rb").read()
        for e in self.entries:
            name = e.name or f"unnamed_{e.offset:08x}"
            key = name.lower()
            if key in done:
                name = f"{name}_{e.offset:08x}"
            done.add(key)
            if on_entry is not None:
                blob = on_entry(e, e.data(data))
            else:
                blob = e.data(data)
            if blob is None:
                continue
            with open(os.path.join(outdir, name + suffix), "wb") as f:
                f.write(blob)
            n += 1
        return n

    def dat_blobs(self) -> list[tuple[str, bytes]]:
        """Every entry as ``(name, data)``, in index order."""
        data = open(self.dat_path, "rb").read()
        return [(e.name, e.data(data)) for e in self.entries]


# ---------------------------------------------------------------------------
# writing / repacking
#
# The shipped indexes are gap-free: entry i+1 starts exactly where entry i
# ends, and the names are NUL-padded 8-byte fields.  Rebuilding an index this
# way reproduces the original bytes exactly (verified on all five archives),
# so a rewrite only has to keep blobs contiguous and names <= 8 chars.
# ---------------------------------------------------------------------------

def encode_name(name: str) -> bytes:
    raw = name.encode("latin1")
    if not 1 <= len(raw) <= 8:
        raise ValueError(f"BIGFILE name must be 1..8 bytes: {name!r}")
    return raw + b"\0" * (8 - len(raw))


def build_index(pairs) -> bytes:
    """Build index bytes from ``(name, size)`` pairs (offsets are cumulative)."""
    pairs = list(pairs)
    out = bytearray(MAGIC + b"\x1a" + struct.pack("<I", len(pairs)))
    off = 0
    for name, size in pairs:
        out += encode_name(name) + struct.pack("<I", off)
        off += size
    return bytes(out)


def pack(blobs, idx_path: str, dat_path: str | None = None) -> tuple[int, int]:
    """Write a new ``.idx``/``.dat`` pair from ``(name, data)`` blobs.

    Returns ``(count, dat_size)``.  The ``.dat`` path defaults to the ``.IDX``
    sibling (uppercase ``.DAT`` first, like the shipped archives).
    """
    blobs = list(blobs)
    dat_path = dat_path or os.path.splitext(idx_path)[0] + ".DAT"
    idx = build_index((name, len(data)) for name, data in blobs)
    with open(dat_path, "wb") as f:
        for _name, data in blobs:
            f.write(data)
    with open(idx_path, "wb") as f:
        f.write(idx)
    return len(blobs), sum(len(d) for _n, d in blobs)


def replace_entry(idx_path: str, name: str, blob: bytes,
                  out_idx_path: str, out_dat_path: str | None = None) -> str:
    """Copy the archive, replacing/adding entry *name* with *blob*.

    Existing entry order is preserved (the new data may be any size).  Returns
    the written ``.dat`` path.
    """
    bf = BigFile(idx_path)
    items = bf.dat_blobs()
    key = name.lower()
    for i, (n, _d) in enumerate(items):
        if n.lower() == key:
            items[i] = (n, blob)
            break
    else:
        items.append((name, blob))
    _count, _size = pack(items, out_idx_path, out_dat_path)
    return out_dat_path or os.path.splitext(out_idx_path)[0] + ".DAT"
