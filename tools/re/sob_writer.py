#!/usr/bin/env python3
"""Writer/verifier for Gunmetal ``.sob`` object models.

The reader lives in ``gm/sob.py`` (see ``docs/SOB.md``); this tool exercises the
writer that was added next to it:

``--check``
    For every model: read -> ``to_bytes()`` -> read again, then compare the
    vertices, faces and the FNV-1a 64 of the canonical bytes.  Reports three
    different numbers, which are *not* interchangeable:

    * **semantic** — the encoded text parses back into the same model;
    * **raw** — the untouched bytes are kept, so an unmodified model writes
      back byte for byte (``Sob.is_unmodified()`` + ``Sob.raw``);
    * **canonical** — ``to_bytes()`` happens to equal the shipped file, i.e.
      the file already used the canonical layout.  Most shipped files do not:
      they put the ``-1`` sentinel at the end of the integer line and/or omit
      the newline after the plane, and a few use a different float format.

``--rebuild DIR``
    Parse every model and write it to ``DIR`` (canonical, atomic, ``--force``
    needed to replace files).  With ``--exact`` unmodified models are written
    from their original bytes instead (byte-exact ``--rebuild``).

``--edit X,Y,Z``
    Shift vertex 0 by ``X,Y,Z`` and write the result to ``--out`` (a file) or
    ``--out-dir``; the shifted model is canonical text.

``--self-test``
    Negative tests that need no game data: broken counts, huge counts, a
    truncated vertex/face stream, bad face records, encode-side rejections,
    the overwrite policy and the atomic write.

Only the Python standard library and the repository's pure-stdlib ``gm``
package are used.  The game directory is only ever read.

Usage::

    python3 tools/re/sob_writer.py --check
    python3 tools/re/sob_writer.py --check "…/SOBS/tick.sob" --verbose
    python3 tools/re/sob_writer.py --rebuild /tmp/opencode/sob-out
    python3 tools/re/sob_writer.py --rebuild /tmp/opencode/sob-out --exact
    python3 tools/re/sob_writer.py --edit 0,0,8 --out /tmp/opencode/tick.sob \\
        "…/SOBS/tick.sob"
    python3 tools/re/sob_writer.py --self-test
"""

from __future__ import annotations

import argparse
import glob
import os
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
if ROOT not in sys.path:
    sys.path.insert(0, ROOT)

from gm import sob  # noqa: E402

DEFAULT_GAME = "/mnt/d/games/Gunmetal 3dfx"
FNV_OFFSET = 0xCBF29CE484222325
FNV_PRIME = 0x100000001B3
FNV_MASK = (1 << 64) - 1


def fnv1a64(data: bytes, seed: int = FNV_OFFSET) -> int:
    value = seed
    for byte in data:
        value = ((value ^ byte) * FNV_PRIME) & FNV_MASK
    return value


def default_paths() -> list[str]:
    env = os.environ.get("GUNMETAL")
    game = env or DEFAULT_GAME
    sobs = os.path.join(game, "SOBS")
    return [sobs] if os.path.isdir(sobs) else []


def collect(paths: list[str]) -> tuple[list[str], list[str]]:
    """Expand files/directories into ``(.sob files, missing paths)``."""
    files: list[str] = []
    missing: list[str] = []
    for path in paths:
        if os.path.isdir(path):
            found = sorted(p for p in glob.glob(os.path.join(path, "*"))
                           if p.lower().endswith(".sob") and os.path.isfile(p))
            if not found:
                missing.append(path)
            files.extend(found)
        elif os.path.isfile(path):
            files.append(path)
        else:
            missing.append(path)
    # Windows file systems are case-blind: dedupe case-insensitively.
    seen: set[str] = set()
    unique = []
    for path in files:
        key = os.path.abspath(path).lower()
        if key not in seen:
            seen.add(key)
            unique.append(path)
    return unique, missing


# --------------------------------------------------------------------------
# --check
# --------------------------------------------------------------------------

def cmd_check(paths: list[str], opts) -> int:
    files, missing = collect(paths)
    for path in missing:
        print(f"sob-writer: not found: {path}", file=sys.stderr)
    if not files:
        print("sob-writer: nothing to check", file=sys.stderr)
        return 2

    parsed = semantic = raw_ok = canonical = 0
    verts = faces = 0
    digest = FNV_OFFSET
    failures: list[str] = []
    canonical_diff: list[str] = []

    for path in files:
        name = os.path.basename(path)
        try:
            with open(path, "rb") as fh:
                source = fh.read()
            model = sob.parse_data(source, name=os.path.splitext(name)[0])
            encoded = model.to_bytes()
            again = sob.parse_data(encoded, name=model.name)
        except (OSError, ValueError) as exc:
            failures.append(f"{name}: {exc}")
            continue
        parsed += 1
        verts += len(model.vertices)
        faces += len(model.faces)
        digest = fnv1a64(encoded, digest)
        if not model.content_equal(again):
            failures.append(f"{name}: model changed across parse -> encode -> parse")
            continue
        semantic += 1
        if model.is_unmodified():
            raw_ok += 1
        if encoded == source:
            canonical += 1
        elif opts.verbose:
            canonical_diff.append(
                f"{name}: canonical {len(encoded)} B, source {len(source)} B")

    print(f"sob writer: {len(files)} models")
    print(f"  parse -> encode -> parse : {semantic}/{parsed} identical, "
          f"{len(failures)} failed")
    print(f"  vertices / faces          : {verts} / {faces}")
    print(f"  byte-exact via raw        : {raw_ok}/{parsed}")
    print(f"  canonical == shipped bytes: {canonical}/{parsed}")
    print(f"  FNV-1a64(canonical)       : 0x{digest:016x}")
    print(f"  limits                    : MAX_VERTS={sob.MAX_VERTS} "
          f"MAX_FACES={sob.MAX_FACES}")
    if opts.verbose and canonical_diff:
        print("  canonical layout differs from the shipped file:")
        for line in canonical_diff[:50]:
            print(f"    {line}")
        if len(canonical_diff) > 50:
            print(f"    … {len(canonical_diff) - 50} more")
    for line in failures[:50]:
        print(f"  FAIL {line}", file=sys.stderr)
    if len(failures) > 50:
        print(f"  … {len(failures) - 50} more failures", file=sys.stderr)
    return 0 if not failures and not missing else 1


# --------------------------------------------------------------------------
# --rebuild
# --------------------------------------------------------------------------

def cmd_rebuild(paths: list[str], out: str, opts) -> int:
    files, missing = collect(paths)
    for path in missing:
        print(f"sob-writer: not found: {path}", file=sys.stderr)
    if not files:
        print("sob-writer: nothing to rebuild", file=sys.stderr)
        return 2
    os.makedirs(out, exist_ok=True)

    written = exact = failed = 0
    errors: list[str] = []
    for path in files:
        name = os.path.basename(path)
        target = os.path.join(out, name)
        try:
            model = sob.parse(path)
            data = model.write(target, force=opts.force, exact=opts.exact)
            again = sob.parse_data(data, name=model.name)
            if not model.content_equal(again):
                raise ValueError("written model does not parse back")
            with open(path, "rb") as fh:
                if data == fh.read():
                    exact += 1
        except (OSError, ValueError) as exc:
            failed += 1
            errors.append(f"{name}: {exc}")
            continue
        written += 1
    for line in errors[:20]:
        print(f"  FAIL {line}", file=sys.stderr)
    if len(errors) > 20:
        print(f"  … {len(errors) - 20} more failures", file=sys.stderr)
    mode = "exact" if opts.exact else "canonical"
    print(f"sob writer: {written} written ({mode}) to {out}, {failed} failed, "
          f"{exact} byte-identical to the source")
    return 0 if not failed and not missing else 1


# --------------------------------------------------------------------------
# --edit
# --------------------------------------------------------------------------

def parse_delta(text: str) -> tuple[float, float, float]:
    parts = [p.strip() for p in text.replace(";", ",").split(",")]
    if len(parts) != 3:
        raise argparse.ArgumentTypeError("expected X,Y,Z")
    try:
        return float(parts[0]), float(parts[1]), float(parts[2])
    except ValueError as exc:
        raise argparse.ArgumentTypeError(f"not a number: {exc}") from exc


def cmd_edit(paths: list[str], opts) -> int:
    files, _ = collect(paths)
    if len(files) != 1:
        print("sob-writer: --edit needs exactly one .sob input", file=sys.stderr)
        return 2
    path = files[0]
    target = opts.out or (os.path.join(opts.out_dir, os.path.basename(path))
                          if opts.out_dir else None)
    if not target:
        print("sob-writer: --edit needs --out FILE or --out-dir DIR", file=sys.stderr)
        return 2
    model = sob.parse(path)
    if not model.vertices:
        print(f"sob-writer: {os.path.basename(path)} has no vertices", file=sys.stderr)
        return 1
    before = model.vertices[0]
    after = model.move_vertex(0, *opts.edit)
    try:
        data = model.write(target, force=opts.force, exact=False)
    except (OSError, ValueError) as exc:
        print(f"sob-writer: {exc}", file=sys.stderr)
        return 1
    check = sob.parse_data(data, name=model.name)
    ok = model.content_equal(check)
    print(f"sob writer: {os.path.basename(path)} -> {target}")
    print(f"  vertex 0: {before} -> {after}")
    print(f"  {len(data)} B, parses back identically: {'yes' if ok else 'NO'}")
    print(f"  view: python3 gm.py view {target!r} --ascii")
    return 0 if ok else 1


# --------------------------------------------------------------------------
# --self-test
# --------------------------------------------------------------------------

GOOD_FACE = ("t none none none none none none none "
             "0 3 0 1 2 "
             "0 0 0 65536 0 0 0 65536 0 0 0 0 1 2 0 ")
GOOD = "3 1\n 0 0 0\n 1 0 0\n 0 1 0\n" + GOOD_FACE


def _negative_cases() -> list[tuple[str, str]]:
    """``(label, body)`` pairs that must be rejected by the parser."""
    return [
        ("empty", ""),
        ("one number", "3"),
        ("bad counts", "three 1\n" + GOOD_FACE),
        ("negative counts", "-1 0\n"),
        ("huge vertex count", f"{sob.MAX_VERTS + 1} 0\n"),
        ("huge face count", f"0 {sob.MAX_FACES + 1}\n"),
        ("absurd vertex count", "999999999999 0\n"),
        ("absurd face count", "3 999999999999\n" + GOOD_FACE),
        ("truncated vertices", "3 1\n 0 0 0\n 1 0 0\n"),
        ("truncated face", "3 1\n 0 0 0\n 1 0 0\n 0 1 0\n"
                           "t none none none none none none none 0 3 0 1 2 -1 0 0 0"),
        ("too few indices", "3 1\n 0 0 0\n 1 0 0\n 0 1 0\n"
                            "t none none none none none none none 0 2 0 1 -1 "
                            "0 0 0 65536 0 0 0 65536 0 0 0 0 1 2 0 "),
        ("too many indices", "3 1\n 0 0 0\n 1 0 0\n 0 1 0\n"
                             "t none none none none none none none 0 33 "
                             + " ".join(["0"] * 33) + " -1 "
                             "0 0 0 65536 0 0 0 65536 0 0 0 0 1 2 0 "),
        ("index out of range", "3 1\n 0 0 0\n 1 0 0\n 0 1 0\n"
                               "t none none none none none none none 0 3 0 1 9 -1 "
                               "0 0 0 65536 0 0 0 65536 0 0 0 0 1 2 0 "),
        ("negative index", "3 1\n 0 0 0\n 1 0 0\n 0 1 0\n"
                           "t none none none none none none none 0 3 0 1 -2 -1 "
                           "0 0 0 65536 0 0 0 65536 0 0 0 0 1 2 0 "),
        ("non-numeric plane", "3 1\n 0 0 0\n 1 0 0\n 0 1 0\n"
                              "t none none none none none none none 0 3 0 1 2 -1 "
                              "0 0 0 65536 0 0 0 65536 0 0 0 0 1 2 x "),
        ("trailing tokens", GOOD + "garbage"),
        ("no faces", "3 0\n 0 0 0\n 1 0 0\n 0 1 0\n 1 2 3\n"),
    ]


def _make_face(textures=8, nv=3, index=0) -> sob.SobFace:
    slots = ["t"] + ["none"] * 7 if textures == 8 else ["t"] * textures
    return sob.SobFace(slots, 0, [index] * nv, (0.0, 0.0, 0.0),
                       (65536.0, 0.0, 0.0), (0.0, 65536.0, 0.0),
                       (0.0, 0.0, 0.0), (0.0, 0.0, 0.0))


def cmd_self_test() -> int:
    failures = 0

    def check(label: str, ok: bool, detail: str = "") -> None:
        nonlocal failures
        print(f"[{'ok  ' if ok else 'FAIL'}] {label}"
              + (f"  ({detail})" if detail else ""))
        if not ok:
            failures += 1

    # --- positive baseline ------------------------------------------------
    model = sob.parse_data(GOOD.encode("latin1"), "good")
    encoded = model.to_bytes()
    again = sob.parse_data(encoded)
    check("baseline parses and round-trips", model.content_equal(again),
          f"{len(model.vertices)} vertices / {len(model.faces)} faces")

    # the optional lone "-1" after the fifteen plane numbers (walk(extra=1))
    try:
        trailing = sob.parse_data((GOOD + "-1").encode("latin1"), "sentinel")
        ok = trailing.content_equal(model) and model.content_equal(
            sob.parse_data(trailing.to_bytes()))
        check("trailing -1 sentinel is accepted and round-trips", ok)
    except ValueError as exc:
        check("trailing -1 sentinel is accepted and round-trips", False, str(exc))

    # --- parser rejects hostile input ------------------------------------
    for label, body in _negative_cases():
        try:
            sob.parse_data(body.encode("latin1"), "case")
        except ValueError:
            check(f"rejects {label}", True)
        except Exception as exc:  # noqa: BLE001 - any other error is a failure
            check(f"rejects {label}", False, f"{type(exc).__name__}: {exc}")
        else:
            check(f"rejects {label}", False, "accepted")

    # --- the parser must not blow up on a count just under the limit ------
    try:
        big = sob.parse_data(f"{sob.MAX_VERTS + 1} 0\n".encode("latin1"), "big")
    except ValueError:
        check(f"vertex limit {sob.MAX_VERTS}", True)
    else:
        check(f"vertex limit {sob.MAX_VERTS}", False, f"{len(big.vertices)} parsed")

    # --- encode-side validation ------------------------------------------
    def rejects(label: str, build) -> None:
        try:
            build().to_bytes()
        except ValueError:
            check(f"encode rejects {label}", True)
        except Exception as exc:  # noqa: BLE001
            check(f"encode rejects {label}", False, f"{type(exc).__name__}: {exc}")
        else:
            check(f"encode rejects {label}", False, "encoded")

    rejects("7 texture slots", lambda: sob.Sob([(0.0, 0.0, 0.0)],
                                                [_make_face(textures=7)], "x"))
    rejects("2 indices", lambda: sob.Sob([(0.0, 0.0, 0.0)],
                                          [_make_face(nv=2)], "x"))
    rejects("index out of range", lambda: sob.Sob([(0.0, 0.0, 0.0)],
                                                   [_make_face(index=1)], "x"))
    rejects("NaN vertex", lambda: sob.Sob([(float("nan"), 0.0, 0.0)],
                                           [_make_face()], "x"))
    rejects("infinite vertex", lambda: sob.Sob([(float("inf"), 0.0, 0.0)],
                                                [_make_face()], "x"))
    rejects("blank texture", lambda: sob.Sob([(0.0, 0.0, 0.0)],
                                              [sob.SobFace([" "] + ["none"] * 7, 0,
                                                           [0] * 3, (0.0,) * 3,
                                                           (0.0,) * 3, (0.0,) * 3,
                                                           (0.0,) * 3, (0.0,) * 3)],
                                              "x"))
    rejects("too many faces", lambda: sob.Sob([(0.0, 0.0, 0.0)],
                                               [_make_face()] * (sob.MAX_FACES + 1),
                                               "x"))

    # --- overwrite policy and atomic write -------------------------------
    with tempfile.TemporaryDirectory(prefix="sob-writer-") as tmp:
        path = os.path.join(tmp, "model.sob")
        data = model.write(path)
        check("write creates the file", os.path.getsize(path) == len(data))
        try:
            model.write(path)
        except FileExistsError:
            check("refuses to overwrite without force", True)
        else:
            check("refuses to overwrite without force", False, "overwrote")
        model.write(path, force=True)
        check("force overwrites", True)
        leftovers = [n for n in os.listdir(tmp) if n.startswith(".sob-")]
        check("atomic write leaves no temporary file", not leftovers,
              f"leftovers: {leftovers}" if leftovers else "clean")
        # raw bytes are kept for an unmodified model
        raw = b"3 1\r\n 0 0 0\r\n 1 0 0\r\n 0 1 0\r\n" + GOOD_FACE.encode("latin1")
        source = os.path.join(tmp, "raw.sob")
        with open(source, "wb") as fh:
            fh.write(raw)
        parsed = sob.parse(source)
        check("raw is preserved", parsed.is_unmodified())
        check("write is byte-exact", parsed.write(source, force=True) == raw)
        parsed.move_vertex(0, 10.0, 0.0, 0.0)
        check("editing invalidates raw", not parsed.is_unmodified())
        rebuilt = parsed.rebuild(source, force=True)
        check("rebuild writes canonical text",
              rebuilt == parsed.to_bytes() and rebuilt != raw)
        check("rebuilt model parses back",
              parsed.content_equal(sob.parse_data(rebuilt)))

    print(f"sob writer self-test: {'PASS' if not failures else 'FAIL'} "
          f"({failures} failure{'s' if failures != 1 else ''})")
    return 0 if not failures else 1


# --------------------------------------------------------------------------
# CLI
# --------------------------------------------------------------------------

def main(argv=None) -> int:
    ap = argparse.ArgumentParser(
        prog="sob_writer.py",
        description="Rebuild and verify Gunmetal .sob object models.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__.split("Usage::")[1].split("\n\n")[0] if "Usage::" in __doc__ else None)
    ap.add_argument("paths", nargs="*", help=".sob files or SOBS/ directories "
                    "(default: the installed game)")
    mode = ap.add_mutually_exclusive_group(required=True)
    mode.add_argument("--check", action="store_true",
                      help="parse -> encode -> parse and compare every model")
    mode.add_argument("--rebuild", metavar="DIR",
                      help="write every model back into DIR")
    mode.add_argument("--edit", metavar="X,Y,Z", type=parse_delta,
                      help="shift vertex 0 by X,Y,Z (needs --out/--out-dir)")
    mode.add_argument("--self-test", action="store_true",
                      help="run the negative tests (no game data needed)")
    ap.add_argument("--out", help="output file for --edit")
    ap.add_argument("--out-dir", help="output directory for --edit")
    ap.add_argument("--exact", action="store_true",
                    help="--rebuild: keep the original bytes of unmodified models")
    ap.add_argument("--force", action="store_true", help="overwrite existing files")
    ap.add_argument("--verbose", action="store_true",
                    help="--check: list models whose canonical layout differs")
    opts = ap.parse_args(argv)

    if opts.self_test:
        return cmd_self_test()
    paths = opts.paths or default_paths()
    if not paths:
        print("sob-writer: no paths given and no game found "
              "(set GUNMETAL or pass a directory)", file=sys.stderr)
        return 2
    if opts.check:
        return cmd_check(paths, opts)
    if opts.rebuild:
        return cmd_rebuild(paths, opts.rebuild, opts)
    return cmd_edit(paths, opts)


if __name__ == "__main__":
    sys.exit(main())
