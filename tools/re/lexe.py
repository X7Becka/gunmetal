#!/usr/bin/env python3
"""Parser for the DOS LE (Linear Executable) build of Gunmetal (``gun.exe``).

The DOS release of Gunmetal (1998) ships as an MZ executable whose ``e_lfanew``
points at an ``LE`` header.  It is *not* a PE image, so neither ``objdump`` nor
``tools/re/exelib.py`` can read it.  This module implements a self-contained
reader for that format: MZ -> LE header, object table, page mapping, fixup
page table + fixup records, import/name tables.

Every offset used here was verified against the shipped file
(``/mnt/d/games/Gunmetal 3dfx/gun.exe``, 3 041 664 bytes, SHA-256
773cac90bf6b281142837003c3430155de599d26cbfdc79b78638a016879b83d);
the evidence is written up in ``docs/LEXE.md``.

Usage::

    python3 tools/re/lexe.py --game "/mnt/d/games/Gunmetal 3dfx" --info
    python3 tools/re/lexe.py --game "/mnt/d/games/Gunmetal 3dfx" --segments
    python3 tools/re/lexe.py --game "/mnt/d/games/Gunmetal 3dfx" --fixups
    python3 tools/re/lexe.py --game "/mnt/d/games/Gunmetal 3dfx" --strings --min-len 8
    python3 tools/re/lexe.py --game "/mnt/d/games/Gunmetal 3dfx" --dump 1:0x8f614:32
    python3 tools/re/lexe.py --game "/mnt/d/games/Gunmetal 3dfx" --info --json
    python3 tools/re/lexe.py --game "/mnt/d/games/Gunmetal 3dfx" --selftest
"""
import argparse
import json
import os
import struct
import sys

# --------------------------------------------------------------------------
# header layout (offsets are relative to the LE header, i.e. to e_lfanew)
# --------------------------------------------------------------------------
# name, size, comment.  "verified" fields were checked against the data of
# gun.exe; see docs/LEXE.md for the proof of every single one of them.
HEADER_FIELDS = (
    (0x00, "signature", 2, "b'LE'"),
    (0x02, "byte_order", 1, "0 = little endian"),
    (0x03, "word_order", 1, "0 = little endian"),
    (0x04, "format_level", 4, "0"),
    (0x08, "cpu_type", 2, "2 = 80386"),
    (0x0A, "os_type", 2, "1 = OS/2"),
    (0x0C, "module_version", 4, "0"),
    (0x10, "module_flags", 4, "0x200"),
    (0x14, "unk_14", 4, "unidentified (666; the module really has 670 pages)"),
    (0x18, "eip_object", 4, "1-based object number holding the entry point"),
    (0x1C, "eip_offset", 4, "entry point offset inside that object"),
    (0x20, "esp_object", 4, "1-based object number holding the initial stack"),
    (0x24, "esp_offset", 4, "stack offset inside that object"),
    (0x28, "page_size", 4, "bytes per data page (4096)"),
    (0x2C, "last_page_size", 4, "bytes of the final, short data page (384)"),
    (0x30, "unk_30", 4, "unidentified (285388)"),
    (0x34, "unk_34", 4, "unidentified (0)"),
    (0x38, "unk_38", 4, "unidentified (288244)"),
    (0x3C, "unk_3c", 4, "unidentified (0)"),
    (0x40, "object_table_offset", 4, "LE relative; 0xC4 -> file 0x316C"),
    (0x44, "object_count", 4, "number of object table records (7)"),
    (0x48, "object_page_map_offset", 4, "LE relative; 0x16C -> file 0x3214"),
    (0x4C, "unk_4c", 4, "unidentified (0)"),
    (0x50, "module_name_offset", 4, "LE relative; 0xBE4 -> file 0x3C8C ('gun')"),
    (0x54, "unk_54", 4, "unidentified (0)"),
    (0x58, "resident_name_table_offset", 4, "LE relative; 0xBE4 -> file 0x3C8C"),
    (0x5C, "import_module_name_table_offset", 4,
     "LE relative; 0xBEB -> file 0x3C93 (empty)"),
    (0x60, "unk_60", 4, "unidentified (0)"),
    (0x64, "unk_64", 4, "unidentified (0)"),
    (0x68, "fixup_page_table_offset", 4, "LE relative; 0xBEC -> file 0x3C94"),
    (0x6C, "fixup_record_table_offset", 4, "LE relative; 0x1668 -> file 0x4710"),
    (0x70, "import_module_table_offset", 4,
     "LE relative; 0x466B7 -> file 0x4975F (empty)"),
    (0x74, "unk_74", 4, "unidentified (0)"),
    (0x78, "non_resident_name_table_offset", 4,
     "LE relative; 0x466B7 -> file 0x4975F (empty)"),
    (0x7C, "unk_7c", 4, "unidentified (0)"),
    (0x80, "data_pages_file_offset", 4, "ABSOLUTE file offset of data page 1"),
    (0x84, "unk_84", 4, "unidentified (0)"),
    (0x88, "unk_88", 4, "unidentified (0)"),
    (0x8C, "unk_8c", 4, "unidentified (0)"),
    (0x90, "unk_90", 4, "unidentified (0)"),
    (0x94, "unk_94", 4, "unidentified (7)"),
    (0x98, "unk_98", 4, "unidentified (0)"),
    (0x9C, "unk_9c", 4, "unidentified (0)"),
)

HEADER_SIZE = 0xC0
OBJECT_RECORD_SIZE = 24

# object flags (bits 0..2 are the classical R/W/X triplet)
OBJ_READABLE = 0x01
OBJ_WRITABLE = 0x02
OBJ_EXECUTABLE = 0x04
OBJ_FLAG_NAMES = ((OBJ_READABLE, "R"), (OBJ_WRITABLE, "W"), (OBJ_EXECUTABLE, "X"))

# fixup "source" kinds (low nibble of the first fixup byte)
FIXUP_SRC_KINDS = {
    0x00: "byte",
    0x01: "offset16-module",
    0x02: "selector16",
    0x03: "pointer16:16",
    0x04: "offset16-module-rel",
    0x05: "offset16",
    0x06: "pointer16:32",
    0x07: "offset32",
    0x08: "offset32-self",
    0x09: "offset32-module-rel",
}

# fixup "target" kinds (low two bits of the second fixup byte)
FIXUP_TGT_KINDS = {
    0x00: "internal",
    0x01: "import-ordinal",
    0x02: "import-name",
    0x03: "entry-table",
}


class LEError(ValueError):
    """Raised for every malformed input (never IndexError/struct.error)."""


def _need(data, off, size, what):
    """Bounds check helper: raises LEError instead of letting a slice silently
    return short data."""
    if off < 0 or size < 0:
        raise LEError("%s: negative offset/size (%d, %d)" % (what, off, size))
    if off + size > len(data):
        raise LEError("%s: 0x%x+%d is outside the file (size 0x%x)"
                      % (what, off, size, len(data)))


class LEObject:
    """One 24 byte record of the LE object table."""

    __slots__ = ("index", "virtual_size", "virtual_address", "flags",
                 "first_page", "page_count", "reserved", "file_offset")

    def __init__(self, index, virtual_size, virtual_address, flags,
                 first_page, page_count, reserved):
        self.index = index              # 1-based, as used by the header
        self.virtual_size = virtual_size
        self.virtual_address = virtual_address
        self.flags = flags
        self.first_page = first_page    # 1-based data page number
        self.page_count = page_count
        self.reserved = reserved
        self.file_offset = None         # filled in by LEImage

    @property
    def attributes(self):
        return "".join(n for bit, n in OBJ_FLAG_NAMES if self.flags & bit)

    def as_dict(self, image=None):
        out = {
            "index": self.index,
            "virtual_address": self.virtual_address,
            "virtual_size": self.virtual_size,
            "flags": self.flags,
            "attributes": self.attributes,
            "first_page": self.first_page,
            "page_count": self.page_count,
            "reserved": self.reserved,
        }
        if image is not None:
            out["file_offset"] = self.file_offset
            out["file_size"] = image.object_file_size(self)
        return out


class Fixup:
    """One LE fixup record."""

    __slots__ = ("page", "src_offset", "src_kind", "tgt_kind", "target_object",
                 "target_offset", "target_va", "additive")

    def __init__(self, page, src_offset, src_kind, tgt_kind, target_object,
                 target_offset, target_va, additive):
        self.page = page                # 0-based data page number
        self.src_offset = src_offset    # byte offset inside the page
        self.src_kind = src_kind
        self.tgt_kind = tgt_kind
        self.target_object = target_object
        self.target_offset = target_offset
        self.target_va = target_va
        self.additive = additive

    @property
    def src_kind_name(self):
        return FIXUP_SRC_KINDS.get(self.src_kind, "0x%02x" % self.src_kind)

    @property
    def tgt_kind_name(self):
        return FIXUP_TGT_KINDS.get(self.tgt_kind, "0x%02x" % self.tgt_kind)

    def as_dict(self):
        return {
            "page": self.page,
            "src_offset": self.src_offset,
            "src_kind": self.src_kind,
            "src_kind_name": self.src_kind_name,
            "tgt_kind": self.tgt_kind,
            "tgt_kind_name": self.tgt_kind_name,
            "target_object": self.target_object,
            "target_offset": self.target_offset,
            "target_va": self.target_va,
            "additive": self.additive,
        }


class LEImage:
    """A parsed LE executable."""

    def __init__(self, data, path=None):
        self.data = data
        self.path = path
        self.le_offset = 0
        self.header = {}
        self.objects = []
        self.object_page_map = []        # raw u32 table (semantics unresolved)
        self.fixup_page_table = []       # cumulative offsets, one per page + 1
        self.fixups = []
        self.imports = []                # imported modules
        self.names = []                  # length prefixed names
        self.entry_points = []           # exported entry points (none here)
        self._parse()

    # -- low level helpers ------------------------------------------------
    def _u8(self, off, what):
        _need(self.data, off, 1, what)
        return self.data[off]

    def _u16(self, off, what):
        _need(self.data, off, 2, what)
        return struct.unpack_from("<H", self.data, off)[0]

    def _u32(self, off, what):
        _need(self.data, off, 4, what)
        return struct.unpack_from("<I", self.data, off)[0]

    def _bytes(self, off, size, what):
        _need(self.data, off, size, what)
        return self.data[off:off + size]

    # -- parsing -----------------------------------------------------------
    def _parse(self):
        data = self.data
        if len(data) < 0x40:
            raise LEError("file is too small to be an MZ executable (%d bytes)"
                          % len(data))
        if data[0:2] != b"MZ":
            raise LEError("not an MZ executable (signature %r)" % data[0:2])
        self.le_offset = self._u32(0x3C, "e_lfanew")
        _need(data, self.le_offset, 2, "LE header signature")
        sig = data[self.le_offset:self.le_offset + 2]
        if sig not in (b"LE", b"LX"):
            raise LEError("no LE/LX header at e_lfanew 0x%x (found %r)"
                          % (self.le_offset, sig))
        self._parse_header()
        self._parse_objects()
        self._parse_object_page_map()
        self._parse_names()
        self._parse_imports()
        self._parse_fixups()

    def _parse_header(self):
        base = self.le_offset
        for off, name, size, _comment in HEADER_FIELDS:
            abs_off = base + off
            if name == "signature":
                self.header[name] = self._bytes(abs_off, 2, "header signature")
            elif size == 1:
                self.header[name] = self._u8(abs_off, "header %s" % name)
            elif size == 2:
                self.header[name] = self._u16(abs_off, "header %s" % name)
            else:
                self.header[name] = self._u32(abs_off, "header %s" % name)
        h = self.header
        if h["signature"] not in (b"LE", b"LX"):
            raise LEError("bad LE signature %r" % (h["signature"],))
        if h["byte_order"] != 0:
            raise LEError("unsupported byte order %d (only little endian)"
                          % h["byte_order"])
        if h["word_order"] != 0:
            raise LEError("unsupported word order %d (only little endian)"
                          % h["word_order"])
        if h["page_size"] == 0:
            raise LEError("page size is zero")
        if h["page_size"] & (h["page_size"] - 1):
            raise LEError("page size 0x%x is not a power of two"
                          % h["page_size"])
        if h["last_page_size"] > h["page_size"]:
            raise LEError("last page size 0x%x exceeds the page size 0x%x"
                          % (h["last_page_size"], h["page_size"]))
        if h["object_count"] == 0:
            raise LEError("the module declares no objects")
        self.header_offset = base

    def _le_off(self, rel, what):
        """LE relative offset -> absolute file offset."""
        off = self.le_offset + rel
        _need(self.data, off, 0, what)
        return off

    def _parse_objects(self):
        h = self.header
        base = self._le_off(h["object_table_offset"], "object table")
        _need(self.data, base, h["object_count"] * OBJECT_RECORD_SIZE,
              "object table")
        for i in range(h["object_count"]):
            off = base + i * OBJECT_RECORD_SIZE
            vs, va, fl, fp, pc, res = struct.unpack_from("<IIIIII", self.data, off)
            self.objects.append(LEObject(i + 1, vs, va, fl, fp, pc, res))
        # page ranges must be inside the module
        self.num_pages = 0
        for obj in self.objects:
            if obj.page_count == 0:
                raise LEError("object %d declares zero pages" % obj.index)
            last = obj.first_page + obj.page_count - 1
            if obj.first_page < 1:
                raise LEError("object %d: first page %d is invalid"
                              % (obj.index, obj.first_page))
            if last > self.num_pages:
                self.num_pages = last
        # file offset of every object (first page of the object)
        for obj in self.objects:
            obj.file_offset = self.page_file_offset(obj.first_page)
        # the data pages must fit into the file
        last = self.page_file_offset(self.num_pages) + self.page_file_size(self.num_pages)
        if last > len(self.data):
            raise LEError("data pages end at 0x%x, outside the file (0x%x)"
                          % (last, len(self.data)))

    def _parse_object_page_map(self):
        rel = self.header["object_page_map_offset"]
        if not rel:
            return
        base = self._le_off(rel, "object page map")
        # the table holds one u32 per data page; its length is unknown, so it
        # is read up to the next table (the fixup page table) or to EOF.
        limit = len(self.data)
        fix_rel = self.header["fixup_page_table_offset"]
        if fix_rel:
            limit = min(limit, self._le_off(fix_rel, "fixup page table"))
        n = (limit - base) // 4
        if n < 0:
            raise LEError("object page map at 0x%x is outside the file" % base)
        # one entry per data page; the rest of the area belongs to other tables
        n = min(n, self.num_pages)
        self.object_page_map = list(struct.unpack_from("<%dI" % n, self.data, base)) \
            if n else []

    def _read_names(self, rel, what):
        """Read a table of length prefixed names (LE relative offset)."""
        out = []
        if not rel:
            return out
        base = self._le_off(rel, what)
        off = base
        while off < len(self.data):
            length = self.data[off]
            off += 1
            if length == 0:
                break
            chunk = self._bytes(off, length, what)
            off += length
            out.append(chunk.decode("latin-1"))
        return out

    def _parse_names(self):
        """Module name, resident and non resident names."""
        self.module_name = None
        names = self._read_names(self.header.get("module_name_offset") or 0,
                                 "module name table")
        if names:
            self.module_name = names[0]
        for key in ("resident_name_table_offset",
                    "non_resident_name_table_offset"):
            for name in self._read_names(self.header.get(key) or 0, key):
                if name not in self.names:
                    self.names.append(name)
        if self.module_name and self.module_name not in self.names:
            self.names.insert(0, self.module_name)

    def _parse_imports(self):
        """Imported modules.

        The import module name table (header +0x5C) holds the length prefixed
        names of the imported modules, the import module table (header +0x70)
        one index per imported module.  gun.exe imports nothing (the Watcom
        run time is linked statically), so both tables are empty.
        """
        names = self._read_names(
            self.header.get("import_module_name_table_offset") or 0,
            "import module name table")
        for i, name in enumerate(names, start=1):
            self.imports.append({"index": i, "name": name, "procedures": []})
        # the import module table is a list of u16 indices into the names
        rel = self.header.get("import_module_table_offset") or 0
        if rel and names:
            base = self._le_off(rel, "import module table")
            for i in range(len(names)):
                self._u16(base + 2 * i, "import module table")

    def _parse_fixups(self):
        h = self.header
        pt_rel = h["fixup_page_table_offset"]
        rec_rel = h["fixup_record_table_offset"]
        if not pt_rel or not rec_rel:
            return
        pt_abs = self._le_off(pt_rel, "fixup page table")
        rec_abs = self._le_off(rec_rel, "fixup record table")
        # the fixup page table holds one cumulative offset per data page plus a
        # terminating entry with the total size of the fixup record table.
        count = self.num_pages + 1
        _need(self.data, pt_abs, 4 * count, "fixup page table")
        self.fixup_page_table = list(
            struct.unpack_from("<%dI" % count, self.data, pt_abs))
        for i in range(1, count):
            if self.fixup_page_table[i] < self.fixup_page_table[i - 1]:
                raise LEError("fixup page table is not monotonic at index %d" % i)
        total = self.fixup_page_table[-1]
        _need(self.data, rec_abs, total, "fixup record table")
        blob = self._bytes(rec_abs, total, "fixup record table")
        for page in range(self.num_pages):
            start = self.fixup_page_table[page]
            end = self.fixup_page_table[page + 1]
            if end == start:
                continue
            for fx in self._decode_fixups(blob[start:end], page):
                self.fixups.append(fx)

    def _decode_fixups(self, blob, page=0):
        out = []
        pos = 0
        end = len(blob)
        while pos < end:
            if pos + 2 > end:
                raise LEError("truncated fixup record at +0x%x" % pos)
            src_type = blob[pos]
            tgt_flags = blob[pos + 1]
            pos += 2
            src_kind = src_type & 0x0F
            has_list = bool(src_type & 0x20)
            tgt_kind = tgt_flags & 0x03
            has_add = bool(tgt_flags & 0x04)
            ofs32 = bool(tgt_flags & 0x10)
            add32 = bool(tgt_flags & 0x20)
            obj16 = bool(tgt_flags & 0x40)
            ord8 = bool(tgt_flags & 0x80)
            if has_list:
                if pos >= end:
                    raise LEError("truncated fixup source list")
                count = blob[pos]
                pos += 1
            else:
                if pos + 2 > end:
                    raise LEError("truncated fixup source offset")
                src_offset = struct.unpack_from("<H", blob, pos)[0]
                pos += 2
                count = 1
            target_object = None
            target_offset = 0
            if tgt_kind == 0x00:                       # internal reference
                if obj16:
                    if pos + 2 > end:
                        raise LEError("truncated fixup target object")
                    target_object = struct.unpack_from("<H", blob, pos)[0]
                    pos += 2
                else:
                    if pos >= end:
                        raise LEError("truncated fixup target object")
                    target_object = blob[pos]
                    pos += 1
                if src_kind != 0x02:                  # not a bare selector
                    if ofs32:
                        if pos + 4 > end:
                            raise LEError("truncated fixup target offset")
                        target_offset = struct.unpack_from("<I", blob, pos)[0]
                        pos += 4
                    else:
                        if pos + 2 > end:
                            raise LEError("truncated fixup target offset")
                        target_offset = struct.unpack_from("<H", blob, pos)[0]
                        pos += 2
            else:
                pos += 2 if obj16 else 1
                if tgt_kind == 0x01 and ord8:
                    pos += 1
                else:
                    pos += 4 if ofs32 else 2
            additive = 0
            if has_add:
                if add32:
                    if pos + 4 > end:
                        raise LEError("truncated fixup additive value")
                    additive = struct.unpack_from("<I", blob, pos)[0]
                    pos += 4
                else:
                    if pos + 2 > end:
                        raise LEError("truncated fixup additive value")
                    additive = struct.unpack_from("<H", blob, pos)[0]
                    pos += 2
            if has_list:
                if pos + 2 * count > end:
                    raise LEError("truncated fixup source list")
                offsets = [struct.unpack_from("<H", blob, pos + 2 * j)[0]
                           for j in range(count)]
                pos += 2 * count
            else:
                offsets = [src_offset]
            va = self.object_base(target_object)
            target_va = (va + target_offset) if va is not None else None
            for off in offsets:
                out.append(Fixup(page, off, src_kind, tgt_kind, target_object,
                                 target_offset, target_va, additive))
        return out

    # -- page / address mapping -------------------------------------------
    @property
    def page_size(self):
        return self.header["page_size"]

    def page_file_offset(self, page1):
        """File offset of data page `page1` (1-based)."""
        if page1 < 1:
            raise LEError("invalid page number %d" % page1)
        return self.header["data_pages_file_offset"] + (page1 - 1) * self.page_size

    def page_file_size(self, page1):
        """Number of bytes of data page `page1` that are present in the file."""
        if page1 == self.num_pages:
            return self.header["last_page_size"]
        return self.page_size

    def object_of_page(self, page1):
        for obj in self.objects:
            if obj.first_page <= page1 < obj.first_page + obj.page_count:
                return obj
        return None

    def object_base(self, index):
        """Virtual address of the object with the given 1-based index."""
        if not index or index < 1 or index > len(self.objects):
            return None
        return self.objects[index - 1].virtual_address

    def object_page_span(self, obj):
        """(first page, last page) of an object, 1-based, inclusive."""
        return obj.first_page, obj.first_page + obj.page_count - 1

    def object_file_size(self, obj):
        """Bytes of `obj` that are backed by the file."""
        total = 0
        for page in range(obj.first_page, obj.first_page + obj.page_count):
            total += self.page_file_size(page)
        return total

    def object_offset_to_file(self, index, offset):
        """Map (object, offset) to a file offset (None when not backed)."""
        if index < 1 or index > len(self.objects):
            raise LEError("no such object: %d" % index)
        obj = self.objects[index - 1]
        if offset < 0:
            raise LEError("negative offset %d" % offset)
        page = obj.first_page + offset // self.page_size
        inside = offset % self.page_size
        if page >= obj.first_page + obj.page_count:
            return None
        file_off = self.page_file_offset(page) + inside
        if file_off >= len(self.data):
            return None
        return file_off

    def va_to_file(self, va):
        """Map a virtual address to a file offset (None when not backed)."""
        for obj in self.objects:
            if obj.virtual_address <= va < obj.virtual_address + obj.virtual_size:
                return self.object_offset_to_file(obj.index, va - obj.virtual_address)
        return None

    def read_at(self, file_offset, size):
        _need(self.data, file_offset, size, "read at 0x%x" % file_offset)
        return self.data[file_offset:file_offset + size]

    def parse_entry_table(self, offset, size, entry_size=3):
        """Parse an LE entry table (the exported entry points).

        The table is a sequence of *bundles*: a 4 byte offset, a 2 byte entry
        count and a 2 byte flags word, each bundle followed by `count` entries
        of `entry_size` bytes (3 bytes in the 16 bit form: a 2 byte offset and
        a 1 byte flags word).

        gun.exe exports nothing and no header field could be tied to an entry
        table, so this reader is not used for it; it is kept for other LE
        files and for manual use (``image.parse_entry_table(off, size)``).
        """
        out = []
        blob = self._bytes(offset, size, "entry table")
        pos = 0
        while pos + 8 <= len(blob):
            bundle_offset, count, flags = struct.unpack_from("<IHH", blob, pos)
            pos += 8
            entries = []
            for _ in range(count):
                if pos + entry_size > len(blob):
                    raise LEError("entry table: truncated bundle")
                if entry_size == 3:
                    e_off, e_flags = struct.unpack_from("<HB", blob, pos)
                else:
                    e_off, e_flags = struct.unpack_from("<IH", blob, pos)
                pos += entry_size
                entries.append({"offset": e_off, "flags": e_flags})
            out.append({"bundle": bundle_offset, "flags": flags, "entries": entries})
        return out

    # -- reporting ---------------------------------------------------------
    def info(self):
        h = self.header
        return {
            "path": self.path,
            "file_size": len(self.data),
            "mz_lfanew": self.le_offset,
            "signature": h["signature"].decode("latin-1"),
            "byte_order": h["byte_order"],
            "word_order": h["word_order"],
            "format_level": h["format_level"],
            "cpu_type": h["cpu_type"],
            "os_type": h["os_type"],
            "module_version": h["module_version"],
            "module_flags": h["module_flags"],
            "page_size": h["page_size"],
            "last_page_size": h["last_page_size"],
            "num_pages": self.num_pages,
            "object_count": len(self.objects),
            "fixup_count": len(self.fixups),
            "pages_with_fixups": sum(1 for i in range(self.num_pages)
                                      if self.fixup_page_table[i + 1]
                                      > self.fixup_page_table[i]),
            "import_count": len(self.imports),
            "imports": [i["name"] for i in self.imports],
            "entry_point_count": len(self.entry_points),
            "names": list(self.names),
            "eip_object": h["eip_object"],
            "eip_offset": h["eip_offset"],
            "eip_va": self.entry_point_va(),
            "esp_object": h["esp_object"],
            "esp_offset": h["esp_offset"],
            "esp_va": self.stack_va(),
            "data_pages_file_offset": h["data_pages_file_offset"],
            "object_page_map_entries": len(self.object_page_map),
            "fixup_record_table_size": (self.fixup_page_table[-1]
                                        if self.fixup_page_table else 0),
        }

    def entry_point_va(self):
        base = self.object_base(self.header["eip_object"])
        if base is None:
            return None
        return base + self.header["eip_offset"]

    def stack_va(self):
        base = self.object_base(self.header["esp_object"])
        if base is None:
            return None
        return base + self.header["esp_offset"]

    def segments(self):
        out = []
        for obj in self.objects:
            first, last = self.object_page_span(obj)
            out.append({
                "object": obj.index,
                "va": obj.virtual_address,
                "size": obj.virtual_size,
                "attributes": obj.attributes,
                "flags": obj.flags,
                "pages": "%d..%d" % (first, last),
                "file_offset": obj.file_offset,
                "file_size": self.object_file_size(obj),
                "over_declared_size": obj.virtual_size > self.object_file_size(obj),
            })
        return out

    def strings(self, min_len=4, limit=None):
        """ASCII strings found in the data pages, with their virtual address."""
        out = []
        for obj in self.objects:
            for page in range(obj.first_page, obj.first_page + obj.page_count):
                size = self.page_file_size(page)
                off = self.page_file_offset(page)
                chunk = self._bytes(off, size, "data page %d" % page)
                base_va = (obj.virtual_address
                           + (page - obj.first_page) * self.page_size)
                run = bytearray()
                run_at = 0
                for i, byte in enumerate(chunk + b"\x00"):
                    if 0x20 <= byte < 0x7F:
                        if not run:
                            run_at = i
                        run.append(byte)
                        continue
                    if len(run) >= min_len:
                        out.append({
                            "va": base_va + run_at,
                            "file_offset": off + run_at,
                            "length": len(run),
                            "text": run.decode("latin-1"),
                        })
                    run = bytearray()
        out.sort(key=lambda s: s["file_offset"])
        if limit is not None:
            out = out[:limit]
        return out


# --------------------------------------------------------------------------
# helpers
# --------------------------------------------------------------------------
def find_le_exe(game_dir):
    """Return the LE executable of an installation directory."""
    if not os.path.isdir(game_dir):
        raise LEError("not a directory: %s" % game_dir)
    candidates = []
    for name in sorted(os.listdir(game_dir)):
        if not name.lower().endswith(".exe"):
            continue
        path = os.path.join(game_dir, name)
        try:
            with open(path, "rb") as fh:
                head = fh.read(0x40)
        except OSError:
            continue
        if len(head) < 0x40 or head[:2] != b"MZ":
            continue
        lfanew = struct.unpack_from("<I", head, 0x3C)[0]
        try:
            with open(path, "rb") as fh:
                fh.seek(lfanew)
                sig = fh.read(2)
        except OSError:
            continue
        if sig in (b"LE", b"LX"):
            candidates.append(path)
    if not candidates:
        raise LEError("no LE executable found in %s" % game_dir)
    for path in candidates:
        if os.path.basename(path).lower() == "gun.exe":
            return path
    return candidates[0]


def load_image(path):
    with open(path, "rb") as fh:
        data = fh.read()
    return LEImage(data, path=path)


def _fmt_header(image):
    h = image.header
    lines = ["file            : %s (%d bytes)" % (image.path, len(image.data)),
             "MZ e_lfanew     : 0x%x" % image.le_offset,
             "signature       : %s   byte/word order: %d/%d (little/little)"
             % (h["signature"].decode("latin-1"), h["byte_order"], h["word_order"]),
             "format level    : %d   cpu type: %d   os type: %d"
             % (h["format_level"], h["cpu_type"], h["os_type"]),
             "module version  : %d   flags: 0x%x" % (h["module_version"],
                                                     h["module_flags"]),
             "module name     : %s" % (image.module_name,),
             "entry point     : object %d + 0x%x -> VA 0x%x"
             % (h["eip_object"], h["eip_offset"], image.entry_point_va() or 0),
             "initial stack   : object %d + 0x%x -> VA 0x%x"
             % (h["esp_object"], h["esp_offset"], image.stack_va() or 0),
             "page size       : %d bytes, last page %d bytes, pages: %d"
             % (h["page_size"], h["last_page_size"], image.num_pages),
             "objects         : %d" % len(image.objects),
             "data pages      : file 0x%x .. 0x%x"
             % (h["data_pages_file_offset"], len(image.data)),
             "fixups          : %d records on %d pages"
             % (len(image.fixups),
                sum(1 for i in range(image.num_pages)
                    if image.fixup_page_table[i + 1] > image.fixup_page_table[i])),
             "imports         : %d %s"
             % (len(image.imports), [i["name"] for i in image.imports]),
             "names           : %s" % (image.names,)]
    return lines


def _fmt_segments(image):
    lines = ["obj  va        size      attr pages      file off    file size"]
    for seg in image.segments():
        lines.append("%3d  0x%08x  0x%08x  %-4s %-11s 0x%08x  0x%06x%s"
                     % (seg["object"], seg["va"], seg["size"], seg["attributes"],
                        seg["pages"], seg["file_offset"], seg["file_size"],
                        "  (size > pages)" if seg["over_declared_size"] else ""))
    return lines


def _fmt_fixups(image, limit=None):
    lines = ["page  src off  src kind    target            tgt off     target va"]
    for i, fx in enumerate(image.fixups):
        if limit is not None and i >= limit:
            lines.append("... %d more" % (len(image.fixups) - limit))
            break
        lines.append("%4d  0x%04x   %-9s obj %-3s offset 0x%06x  0x%08x"
                     % (fx.page, fx.src_offset, fx.src_kind_name,
                        fx.target_object, fx.target_offset,
                        fx.target_va or 0))
    return lines


def _fmt_strings(strings, limit=None):
    lines = []
    for s in strings:
        lines.append("0x%08x  file 0x%08x  %3d  %r"
                     % (s["va"], s["file_offset"], s["length"], s["text"]))
        if limit is not None and len(lines) >= limit:
            break
    return lines


def _parse_dump_spec(spec):
    parts = spec.split(":")
    if len(parts) != 3:
        raise LEError("--dump wants OBJECT:OFFSET:LENGTH, got %r" % spec)
    try:
        obj = int(parts[0], 0)
        off = int(parts[1], 0)
        length = int(parts[2], 0)
    except ValueError:
        raise LEError("--dump: bad numbers in %r" % spec)
    return obj, off, length


def _hexdump(blob, base):
    lines = []
    for i in range(0, len(blob), 16):
        chunk = blob[i:i + 16]
        hexs = " ".join("%02x" % b for b in chunk)
        text = "".join(chr(b) if 0x20 <= b < 0x7F else "." for b in chunk)
        lines.append("%08x  %-47s  %s" % (base + i, hexs, text))
    return lines


# --------------------------------------------------------------------------
# self test
# --------------------------------------------------------------------------
def selftest(path, verbose=True):
    """Parse the real executable and check the invariants of the format.

    Returns a list of (ok, message) pairs.
    """
    results = []

    def check(ok, msg):
        results.append((bool(ok), msg))

    with open(path, "rb") as fh:
        data = fh.read()
    image = LEImage(data, path=path)

    # 1. header sanity
    check(image.data[:2] == b"MZ", "MZ signature at offset 0")
    check(image.header["signature"] in (b"LE", b"LX"),
          "LE signature at e_lfanew 0x%x" % image.le_offset)
    check(image.header["byte_order"] == 0 and image.header["word_order"] == 0,
          "little endian byte and word order")

    # 2. objects and pages
    total_pages = sum(o.page_count for o in image.objects)
    check(total_pages == image.num_pages,
          "object page counts sum to the module page count (%d == %d)"
          % (total_pages, image.num_pages))
    seen = []
    for obj in image.objects:
        first, last = image.object_page_span(obj)
        seen.extend(range(first, last + 1))
    check(sorted(seen) == list(range(1, image.num_pages + 1)),
          "object page ranges tile pages 1..%d exactly once" % image.num_pages)
    for obj in image.objects:
        off = image.page_file_offset(obj.first_page)
        check(off < len(image.data),
              "object %d first page inside the file (0x%x)" % (obj.index, off))

    # 3. every page must be inside the file
    last_off = image.page_file_offset(image.num_pages)
    last_size = image.page_file_size(image.num_pages)
    check(last_off + last_size == len(image.data),
          "data pages end at the end of the file (0x%x + 0x%x == 0x%x)"
          % (last_off, last_size, len(image.data)))
    for page in range(1, image.num_pages + 1):
        off = image.page_file_offset(page)
        size = image.page_file_size(page)
        if off + size > len(image.data):
            check(False, "page %d leaves the file" % page)
            break
    else:
        check(True, "all %d data pages are inside the file" % image.num_pages)

    # 4. the entry point must land inside an object
    eip_obj = image.header["eip_object"]
    check(1 <= eip_obj <= len(image.objects),
          "entry point object number %d is valid" % eip_obj)
    if 1 <= eip_obj <= len(image.objects):
        obj = image.objects[eip_obj - 1]
        eip_off = image.header["eip_offset"]
        check(eip_off < obj.virtual_size,
              "entry point offset 0x%x is inside object %d (size 0x%x)"
              % (eip_off, obj.index, obj.virtual_size))
        file_off = image.object_offset_to_file(eip_obj, eip_off)
        check(file_off is not None, "entry point maps to file offset 0x%x" % (file_off or 0))

    # 5. fixups
    check(len(image.fixups) > 0, "at least one fixup record (%d)" % len(image.fixups))
    mono = all(image.fixup_page_table[i] <= image.fixup_page_table[i + 1]
               for i in range(len(image.fixup_page_table) - 1))
    check(mono, "fixup page table is monotonic")
    rec_end = image.le_offset + image.header["fixup_record_table_offset"] \
        + image.fixup_page_table[-1]
    check(rec_end <= len(image.data),
          "fixup record table ends inside the file (0x%x)" % rec_end)
    # the fixup sites must still hold the target offset (pre-relocation value)
    ok = bad = skipped = 0
    for fx in image.fixups:
        if fx.tgt_kind != 0 or fx.target_object is None:
            skipped += 1
            continue
        if fx.src_kind not in (0x05, 0x07):
            skipped += 1        # 16:16 pointer and self relative fixups
            continue
        if fx.src_offset >= image.page_size:
            skipped += 1        # sentinel offsets (0xFFFD..0xFFFF)
            continue
        off = image.page_file_offset(fx.page + 1) + fx.src_offset
        if off + 4 > len(image.data):
            skipped += 1
            continue
        raw32 = struct.unpack_from("<I", image.data, off)[0]
        raw16 = struct.unpack_from("<H", image.data, off)[0]
        if raw32 == fx.target_offset or raw16 == fx.target_offset:
            ok += 1
        else:
            bad += 1
    check(bad == 0,
          "all %d plain fixup sites still hold their target offset "
          "(%d ok, %d skipped, %d bad)" % (ok + bad, ok, skipped, bad))

    # 6. imports
    check(len(image.imports) == 0 or all(i["name"] for i in image.imports),
          "import table is consistent (%d imported modules)" % len(image.imports))

    # 7. corrupted headers must raise ValueError, never IndexError
    corruptions = [
        ("e_lfanew out of file", lambda b: b[:0x3C] + struct.pack("<I", 0xFFFFFF00) + b[0x40:]),
        ("e_lfanew without LE", lambda b: b[:0x3C] + struct.pack("<I", 0x10) + b[0x40:]),
        ("bad LE signature", lambda b: b[:image.le_offset] + b"PX" + b[image.le_offset + 2:]),
        ("big endian byte order", lambda b: b[:image.le_offset + 0x02] + b"\x01" + b[image.le_offset + 0x03:]),
        ("object count overflow", lambda b: b[:image.le_offset + 0x44] + struct.pack("<I", 0x10000) + b[image.le_offset + 0x48:]),
        ("object table offset out of file", lambda b: b[:image.le_offset + 0x40] + struct.pack("<I", 0xFFFF00) + b[image.le_offset + 0x44:]),
        ("zero page size", lambda b: b[:image.le_offset + 0x28] + struct.pack("<I", 0) + b[image.le_offset + 0x2C:]),
        ("data pages offset out of file", lambda b: b[:image.le_offset + 0x80] + struct.pack("<I", 0xFFFFFF00) + b[image.le_offset + 0x84:]),
        ("fixup page table offset out of file", lambda b: b[:image.le_offset + 0x68] + struct.pack("<I", 0xFFFF00) + b[image.le_offset + 0x6C:]),
        ("fixup record table offset out of file", lambda b: b[:image.le_offset + 0x6C] + struct.pack("<I", 0xFFFF00) + b[image.le_offset + 0x70:]),
    ]
    broken = 0
    for label, mutate in corruptions:
        blob = bytes(mutate(bytearray(data)))
        try:
            LEImage(blob, path=path)
        except ValueError:
            broken += 1
        except Exception as exc:                       # noqa: BLE001
            check(False, "corruption %r raised %s instead of ValueError"
                  % (label, type(exc).__name__))
        else:
            check(False, "corruption %r was not detected" % label)
    check(broken == len(corruptions),
          "%d/%d corrupted headers are rejected with ValueError"
          % (broken, len(corruptions)))

    if verbose:
        for ok, msg in results:
            print("%s %s" % ("PASS" if ok else "FAIL", msg))
        failed = sum(1 for ok, _ in results if not ok)
        print("selftest: %d checks, %d failed" % (len(results), failed))
    return results


# --------------------------------------------------------------------------
# CLI
# --------------------------------------------------------------------------
def main(argv=None):
    parser = argparse.ArgumentParser(
        description="Parse the DOS LE executable of Gunmetal (gun.exe).")
    parser.add_argument("exe", nargs="?", help="path to the LE executable")
    parser.add_argument("--game", help="game installation directory")
    parser.add_argument("--exe", dest="exe_opt", help="path to the LE executable")
    parser.add_argument("--info", action="store_true", help="header summary")
    parser.add_argument("--segments", action="store_true",
                        help="list the objects as segments")
    parser.add_argument("--fixups", action="store_true", help="list fixups")
    parser.add_argument("--strings", action="store_true", help="list strings")
    parser.add_argument("--min-len", type=int, default=4,
                        help="minimum string length (default 4)")
    parser.add_argument("--limit", type=int, default=None,
                        help="limit the number of listed items")
    parser.add_argument("--dump", metavar="OBJECT:OFFSET:LEN",
                        help="hex dump LEN bytes at OBJECT:OFFSET")
    parser.add_argument("--json", action="store_true", help="machine readable output")
    parser.add_argument("--selftest", action="store_true",
                        help="parse the real file and check the invariants")
    args = parser.parse_args(argv)

    path = args.exe_opt or args.exe
    if not path and args.game:
        try:
            path = find_le_exe(args.game)
        except LEError as exc:
            print("error: %s" % exc, file=sys.stderr)
            return 2
    if not path:
        parser.error("no executable given (use --game DIR, --exe FILE or a path)")
    if not os.path.isfile(path):
        print("error: no such file: %s" % path, file=sys.stderr)
        return 2

    if args.selftest:
        results = selftest(path)
        failed = sum(1 for ok, _ in results if not ok)
        return 1 if failed else 0

    try:
        image = load_image(path)
    except LEError as exc:
        print("error: %s" % exc, file=sys.stderr)
        return 2
    except OSError as exc:
        print("error: %s" % exc, file=sys.stderr)
        return 2

    want_info = args.info or not (args.segments or args.fixups or args.strings
                                  or args.dump)

    if args.json:
        out = {"info": image.info(), "segments": image.segments()}
        if args.fixups:
            out["fixups"] = [f.as_dict() for f in image.fixups]
        if args.strings:
            out["strings"] = image.strings(args.min_len, args.limit)
        if args.dump:
            try:
                obj, off, length = _parse_dump_spec(args.dump)
                file_off = image.object_offset_to_file(obj, off)
            except LEError as exc:
                out["dump"] = {"error": str(exc)}
            else:
                if file_off is None:
                    out["dump"] = {"error": "object %d offset 0x%x is not backed by a file page"
                                             % (obj, off)}
                else:
                    blob = image.read_at(file_off, min(length, len(image.data) - file_off))
                    out["dump"] = {"object": obj, "offset": off, "length": len(blob),
                                   "file_offset": file_off, "hex": blob.hex()}
        print(json.dumps(out, indent=2))
        return 0

    if want_info:
        for line in _fmt_header(image):
            print(line)
    if args.segments:
        print()
        for line in _fmt_segments(image):
            print(line)
    if args.fixups:
        print()
        for line in _fmt_fixups(image, args.limit):
            print(line)
    if args.strings:
        print()
        for line in _fmt_strings(image.strings(args.min_len), args.limit):
            print(line)
    if args.dump:
        try:
            obj, off, length = _parse_dump_spec(args.dump)
            file_off = image.object_offset_to_file(obj, off)
        except LEError as exc:
            print("error: %s" % exc, file=sys.stderr)
            return 2
        if file_off is None:
            print("error: object %d offset 0x%x is not backed by a file page"
                  % (obj, off), file=sys.stderr)
            return 2
        blob = image.read_at(file_off, min(length, len(image.data) - file_off))
        base = image.objects[obj - 1].virtual_address + off
        print()
        for line in _hexdump(blob, base):
            print(line)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
