#!/usr/bin/env python3
"""Self test for tools/re/lexe.py (the LE parser of the DOS gun.exe).

Run without any third party dependency::

    python3 tools/re/lexe_test.py                      # uses the game install
    python3 tools/re/lexe_test.py /path/to/gun.exe

Exit code 0 = all checks passed.
"""
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import lexe  # noqa: E402

DEFAULT_GAME = "/mnt/d/games/Gunmetal 3dfx"


def find_exe(argv):
    if argv:
        path = argv[0]
        if os.path.isdir(path):
            return lexe.find_le_exe(path)
        return path
    if os.path.isdir(DEFAULT_GAME):
        return lexe.find_le_exe(DEFAULT_GAME)
    return None


def main(argv):
    path = find_exe(argv)
    if not path:
        print("SKIP: no LE executable found")
        return 0
    failures = 0

    def check(ok, msg):
        nonlocal failures
        if not ok:
            failures += 1
        print("%s %s" % ("PASS" if ok else "FAIL", msg))

    with open(path, "rb") as fh:
        data = fh.read()
    image = lexe.LEImage(data, path=path)

    # --- structure -------------------------------------------------------
    check(image.header["signature"] == b"LE", "LE signature")
    check(image.num_pages == 670, "670 data pages")
    check(len(image.objects) == 7, "7 objects")
    check(image.object_page_map and len(image.object_page_map) == 670,
          "670 object page map entries")
    check(len(image.fixups) == 33055, "33055 fixup records")
    check(image.module_name == "gun", "module name is 'gun'")
    check(image.imports == [], "no imported modules")

    # --- address mapping -------------------------------------------------
    check(image.entry_point_va() == 0xAF614, "entry point VA 0xAF614")
    check(image.object_offset_to_file(2, 0x8F614) == 0xD9E14,
          "entry point maps to file 0xD9E14")
    check(image.va_to_file(0xAF614) == 0xD9E14, "VA -> file mapping")
    check(image.va_to_file(0x10000) == 0x49800, "first object -> file 0x49800")
    check(image.object_offset_to_file(7, 0x1C000) is None,
          "offsets behind the file backed pages are unmapped")
    check(image.page_file_size(670) == 384, "last page is 384 bytes long")

    # --- fixups ----------------------------------------------------------
    plain = [f for f in image.fixups
             if f.tgt_kind == 0 and f.src_offset < image.page_size
             and f.src_kind in (0x05, 0x07)]
    bad = 0
    for fx in plain:
        off = image.page_file_offset(fx.page + 1) + fx.src_offset
        raw32 = struct.unpack_from("<I", image.data, off)[0]
        raw16 = struct.unpack_from("<H", image.data, off)[0]
        if raw32 != fx.target_offset and raw16 != fx.target_offset:
            bad += 1
    check(bad == 0, "all %d plain fixup sites hold their target offset" % len(plain))

    # --- corrupted headers must raise ValueError -------------------------
    cases = {
        "e_lfanew out of file": lambda b: b[:0x3C] + struct.pack("<I", 0xFFFFFF00) + b[0x40:],
        "e_lfanew without LE": lambda b: b[:0x3C] + struct.pack("<I", 0x10) + b[0x40:],
        "bad signature": lambda b: b[:image.le_offset] + b"PX" + b[image.le_offset + 2:],
        "byte order": lambda b: b[:image.le_offset + 0x02] + b"\x01" + b[image.le_offset + 0x03:],
        "object count": lambda b: b[:image.le_offset + 0x44] + struct.pack("<I", 0x10000) + b[image.le_offset + 0x48:],
        "object table offset": lambda b: b[:image.le_offset + 0x40] + struct.pack("<I", 0xFFFF00) + b[image.le_offset + 0x44:],
        "page size": lambda b: b[:image.le_offset + 0x28] + struct.pack("<I", 0) + b[image.le_offset + 0x2C:],
        "data pages offset": lambda b: b[:image.le_offset + 0x80] + struct.pack("<I", 0xFFFFFF00) + b[image.le_offset + 0x84:],
        "fixup page table offset": lambda b: b[:image.le_offset + 0x68] + struct.pack("<I", 0xFFFF00) + b[image.le_offset + 0x6C:],
        "fixup record table offset": lambda b: b[:image.le_offset + 0x6C] + struct.pack("<I", 0xFFFF00) + b[image.le_offset + 0x70:],
    }
    for label, mutate in cases.items():
        blob = bytes(mutate(bytearray(data)))
        try:
            lexe.LEImage(blob, path=path)
        except ValueError:
            check(True, "corruption %r rejected with ValueError" % label)
        except Exception as exc:                       # noqa: BLE001
            check(False, "corruption %r raised %s" % (label, type(exc).__name__))
        else:
            check(False, "corruption %r not detected" % label)

    print("lexe_test: %d failure(s)" % failures)
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
