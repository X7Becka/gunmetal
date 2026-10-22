#!/usr/bin/env python3
"""Audit a Gunmetal installation against the shipped SHA-256 manifest.

What it answers
---------------
"Does the installation on this disk still contain the files the project
shipped in ``docs/manifests/gunmetal-3dfx-136.sha256``, byte for byte?"

The manifest covers the 2161 *game* files of the v1.36 3dfx release
(373.7 MiB).  The audit hashes every file of the installation and classifies
each manifest entry:

``ok``        the file is on disk under its manifest name and the SHA-256
              matches;
``renamed``   the manifest content exists, but under a *different* path
              (a pure rename, or one half of a swap);
``missing``   neither the name nor the content is on disk;
``modified``  the name is on disk, but the bytes differ and the expected
              content is nowhere in the installation;
``volatile``  like ``modified``, but the file is rewritten by the game at
              run time (``GUNMETAL.CFG``, the player's configuration) -- the
              manifest holds an earlier snapshot, so this is expected drift
              and never fails the audit;
``extra``     a file on disk that the manifest does not list.

Swaps
-----
The shipped installation of this disk has a known quirk: the level 1 and
level 2 triples are exchanged -- ``level1.sbt`` holds the bytes of
``level2.sbt`` and vice versa (the same for ``.sbw`` and ``.sbc``).  That
is a property of the medium, not damage, so the audit must not call it
"modified".  Content is therefore located by *hash*: a manifest entry
whose expected digest is found on disk under another name is reported as
``renamed`` and the affected files are grouped into *swap groups* -- the
connected components of the "content of A lives in B" relation.  A
component that is a cycle of length >= 2 is a swap (``kind`` ``"swap"``),
everything else is a plain rename (``kind`` ``"rename"``).  With the
shipped installation this yields exactly three groups of two files
(``level1.sbt`` <-> ``level2.sbt``, ``level1.sbw`` <-> ``level2.sbw`` and
``level1.sbc`` <-> ``level2.sbc``): the groups are the minimal cycles of
the exchange, not one group of six.

Exit codes
----------
``0``  only ``ok`` files plus (known) renames/swaps;
``2``  at least one ``missing`` / ``modified`` / ``extra`` file -- or any
       rename/swap when ``--strict`` is given;
``1``  bad input (no installation, no manifest, bad ``--jobs``).

Nothing is ever written to the installation or to the mirror: the tool only
reads.  ``--json PATH`` writes the report (``-`` means stdout).

    python3 tools/re/install_audit.py --game "/mnt/d/games/Gunmetal 3dfx"
    python3 tools/re/install_audit.py --game "$HOME/.cache/gunmetal-re/game" \
        --json /var/tmp/audit-mirror.json
    python3 tools/re/install_audit.py --strict          # swaps are a failure
    python3 tools/re/install_audit.py --selftest        # synthetic tree
"""
from __future__ import annotations

import argparse
import ast
import hashlib
import json
import os
import sys
import time
from concurrent.futures import ThreadPoolExecutor

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
DEFAULT_GAME = os.environ.get("GUNMETAL_GAME", "/mnt/d/games/Gunmetal 3dfx")
DEFAULT_MANIFEST = os.path.join(ROOT, "docs", "manifests",
                                "gunmetal-3dfx-136.sha256")
# Digest of the shipped manifest as documented in docs/BASELINE.md
# (`gm.py manifest` digests the sorted "<sha256>  <path>" lines).
KNOWN_MANIFEST_DIGEST = (
    "b1d6e04321e011e8ea52a96df9c9cb738dcd8e950ae2465c339af880308601e4")
CHUNK = 1 << 20

# Extensions that `gm.py manifest` treats as non-game files (INVENTORY rows
# whose status starts with "не игровой").  Mirrored here as a fallback; the
# live table is parsed out of gm.py at run time (see `non_game_extensions`).
NON_GAME_EXT = frozenset({
    ".dxw", ".png", ".jpg", ".html", ".css", ".txt", ".docx", ".rar", ".bat",
    ".ico", ".isu", ".pif", ".ini", ".log", ".tmp", ".bak", ".conf", ".js",
    ".wav",
})
NON_GAME_MARK = "не игровой"          # gm.py INVENTORY status prefix

# Files the game rewrites at run time.  ``GUNMETAL.CFG`` is the player's
# configuration: the game saves it on exit (tools/re/init_flow.py: "config",
# 'сохранение: "GUNMETAL.CFG" в режиме "wb"'), so its bytes depend on when
# the game last ran.  The manifest holds an earlier snapshot; a difference
# here is expected drift, not damage, and is reported as ``volatile``.
VOLATILE_FILES = frozenset({"GUNMETAL.CFG"})


# --------------------------------------------------------------------------
# manifest
# --------------------------------------------------------------------------

def parse_manifest(path: str) -> dict[str, str]:
    """Read a ``sha256sum`` manifest into ``{rel_path: sha256}``.

    Accepts both the text and the binary (``*name``) form of ``sha256sum``.
    Raises ``ValueError`` on a malformed line.
    """
    entries: dict[str, str] = {}
    with open(path, "r", encoding="utf-8", errors="replace") as fh:
        for lineno, line in enumerate(fh, 1):
            line = line.rstrip("\r\n")
            if not line.strip():
                continue
            digest, sep, name = line.partition("  ")
            if not sep:
                digest, _, name = line.partition(" ")
                name = name.lstrip("*")
            digest = digest.strip().lower()
            name = name.strip().replace("\\", "/").lstrip("/")
            if len(digest) != 64 or not all(c in "0123456789abcdef" for c in digest):
                raise ValueError("%s:%d: bad sha256 %r" % (path, lineno, digest))
            if not name:
                raise ValueError("%s:%d: empty path" % (path, lineno))
            entries[name] = digest
    if not entries:
        raise ValueError("%s: manifest is empty" % path)
    return entries


def manifest_digest(entries: dict[str, str]) -> str:
    """Summary digest of the manifest (``gm.py manifest`` convention)."""
    h = hashlib.sha256()
    for rel in sorted(entries):
        h.update(("%s  %s\n" % (entries[rel], rel)).encode("utf-8"))
    return h.hexdigest()


# --------------------------------------------------------------------------
# installation scan
# --------------------------------------------------------------------------

def non_game_extensions(root: str = ROOT) -> tuple[set[str], bool]:
    """Extensions excluded from the manifest, taken from ``gm.py``.

    ``gm.py``'s ``INVENTORY`` table marks some extensions as "не игровой"
    (DXWnd exports, docs, logs ...); ``gm.py manifest`` skips them, and so
    must the audit, otherwise every installation would report thousands of
    "extra" files.  The table is parsed (not imported) so that this tool
    stays independent of the oracle and cannot execute anything.
    """
    path = os.path.join(root, "gm.py")
    try:
        with open(path, "r", encoding="utf-8") as fh:
            src = fh.read()
        tree = ast.parse(src)
    except (OSError, SyntaxError, ValueError):
        return set(NON_GAME_EXT), False
    for node in tree.body:
        if not isinstance(node, ast.Assign):
            continue
        if not any(isinstance(t, ast.Name) and t.id == "INVENTORY"
                   for t in node.targets):
            continue
        try:
            table = ast.literal_eval(node.value)
        except (ValueError, TypeError):
            break
        return ({ext for ext, pair in table.items()
                 if len(pair) == 2 and str(pair[1]).startswith(NON_GAME_MARK)},
                True)
    return set(NON_GAME_EXT), False


def is_game_file(rel: str, ext: str, non_game: set[str], include_saves: bool,
                 include_all: bool) -> bool:
    """Does the file belong to the shipped game file set?"""
    if not include_all and ext in non_game:
        return False
    if not include_saves and rel.split("/")[0].upper() == "SAVEGAME":
        return False
    return True


def scan_install(root: str, non_game: set[str], include_saves: bool,
                 include_all: bool) -> tuple[dict[str, str], list[str], int]:
    """Walk *root* and return ``({rel: abspath}, [skipped links], bytes)``.

    Only regular files are collected.  Symlinks (and Windows junctions,
    which drvfs exposes as directory symlinks) are never followed and are
    reported separately; a directory reached twice (hard link / junction
    loop) is visited once.
    """
    files: dict[str, str] = {}
    links: list[str] = []
    total = 0
    seen_dirs: set[tuple[int, int]] = set()
    stack: list[tuple[str, str]] = [(root, "")]
    while stack:
        abs_dir, rel_dir = stack.pop()
        try:
            st = os.stat(abs_dir)
        except OSError:
            continue
        key = (st.st_dev, st.st_ino)
        if key in seen_dirs:
            continue
        seen_dirs.add(key)
        try:
            entries = list(os.scandir(abs_dir))
        except OSError:
            continue
        for entry in entries:
            rel = entry.name if not rel_dir else rel_dir + "/" + entry.name
            try:
                if entry.is_symlink():
                    links.append(rel)
                    continue
                if entry.is_dir(follow_symlinks=False):
                    stack.append((entry.path, rel))
                    continue
                if not entry.is_file(follow_symlinks=False):
                    continue
            except OSError:
                continue
            ext = os.path.splitext(entry.name)[1].lower()
            if not is_game_file(rel, ext, non_game, include_saves, include_all):
                continue
            try:
                total += entry.stat(follow_symlinks=False).st_size
            except OSError:
                pass
            files[rel] = entry.path
    return files, sorted(links), total


def sha256_file(path: str) -> str:
    """SHA-256 of *path*, streamed (never read into memory as a whole)."""
    h = hashlib.sha256()
    with open(path, "rb") as fh:
        while True:
            block = fh.read(CHUNK)
            if not block:
                break
            h.update(block)
    return h.hexdigest()


def hash_all(files: dict[str, str], jobs: int, quiet: bool) -> dict[str, str]:
    """Hash every file, *jobs* at a time, reporting progress on stderr."""
    digests: dict[str, str] = {}
    total = len(files)
    if not total:
        return digests
    done = 0
    t0 = time.time()
    last = 0.0

    def worker(item):
        rel, path = item
        return rel, sha256_file(path)

    with ThreadPoolExecutor(max_workers=max(1, jobs)) as pool:
        for rel, digest in pool.map(worker, files.items()):
            digests[rel] = digest
            done += 1
            now = time.time()
            if not quiet and (done == total or done % 250 == 0
                              or now - last > 2.0):
                last = now
                sys.stderr.write("  hashed %d/%d (%.0f%%) %.1fs\n"
                                 % (done, total, 100.0 * done / total,
                                    now - t0))
                sys.stderr.flush()
    return digests


# --------------------------------------------------------------------------
# classification
# --------------------------------------------------------------------------

def _components(mapping: dict[str, str]) -> list[list[str]]:
    """Connected components of the undirected graph of *mapping* edges."""
    parent: dict[str, str] = {}

    def find(node: str) -> str:
        parent.setdefault(node, node)
        while parent[node] != node:
            parent[node] = parent[parent[node]]
            node = parent[node]
        return node

    def union(a: str, b: str) -> None:
        ra, rb = find(a), find(b)
        if ra != rb:
            parent[ra] = rb

    for src, dst in mapping.items():
        union(src, dst)
    groups: dict[str, list[str]] = {}
    for node in parent:
        groups.setdefault(find(node), []).append(node)
    return [sorted(g) for g in groups.values()]


def _group(kind: str, names: list[str], manifest: dict[str, str],
           renamed: list[dict]) -> dict:
    """Build a swap/rename group description.

    ``names`` are manifest paths, ``sha256`` the expected digests of those
    paths (same order); ``members`` details every file whose content was
    found under a different name, ``absent`` lists the names of the group
    whose own content was not found anywhere.
    """
    by_name = {item["name"]: item for item in renamed}
    members = []
    for name in names:
        item = by_name.get(name)
        if item is None:
            continue
        members.append({"name": name,
                        "expected_sha256": item["expected_sha256"],
                        "actual_path": item["actual_path"],
                        "actual_sha256": item["actual_sha256"]})
    absent = [name for name in names if name not in by_name]
    return {"kind": kind, "names": list(names),
            "sha256": [manifest.get(name, "") for name in names],
            "members": members, "absent": absent}


def classify(manifest: dict[str, str], disk: dict[str, str],
             digests: dict[str, str],
             volatile: frozenset[str] = frozenset()) -> dict:
    """Classify manifest entries and disk files.

    Manifest side: ``ok`` (name and bytes in place), ``renamed`` (content
    found under another path), ``missing`` (neither), ``modified`` (name in
    place, bytes differ, expected content nowhere) and ``volatile`` (same,
    but the file is rewritten by the game at run time).  Disk side:
    ``extra`` (a file the manifest does not list and that hosts no
    relocated content).
    """
    by_digest: dict[str, list[str]] = {}
    for rel, digest in digests.items():
        by_digest.setdefault(digest, []).append(rel)
    for paths in by_digest.values():
        paths.sort()

    ok: list[str] = []
    for rel, expected in sorted(manifest.items()):
        if digests.get(rel) == expected:
            ok.append(rel)
    ok_set = set(ok)

    renamed: list[dict] = []
    missing: list[str] = []
    modified: list[dict] = []
    changed: list[dict] = []
    used: set[str] = set()

    for rel, expected in sorted(manifest.items()):
        if rel in ok_set:
            continue
        candidates = [p for p in by_digest.get(expected, [])
                      if p not in ok_set and p not in used and p != rel]
        if candidates:
            actual = candidates[0]
            used.add(actual)
            renamed.append({
                "name": rel,
                "expected_sha256": expected,
                "actual_path": actual,
                "actual_sha256": digests[actual],
            })
        elif rel in digests:
            row = {"name": rel, "expected_sha256": expected,
                   "actual_sha256": digests[rel]}
            if rel in volatile:
                changed.append(row)
            else:
                modified.append(row)
        else:
            missing.append(rel)

    claimed = {item["actual_path"] for item in renamed}
    extra = sorted(p for p in disk if p not in manifest and p not in claimed)

    # Groups: the content of A lives in B.  Only manifest names take part,
    # so a content that ended up under a foreign name stays a plain rename.
    mapping = {item["name"]: item["actual_path"] for item in renamed
               if item["actual_path"] in manifest}
    groups: list[dict] = []
    grouped: set[str] = set()
    for comp in _components(mapping):
        indeg: dict[str, int] = {}
        for node in comp:
            indeg[mapping[node]] = indeg.get(mapping[node], 0) + 1
        closed = all(mapping.get(node) in comp for node in comp)
        # A swap is a closed cycle: every member's content is the next
        # member's content (level1.sbt <-> level2.sbt).
        is_swap = (closed and len(comp) >= 2
                   and all(count == 1 for count in indeg.values()))
        groups.append(_group("swap" if is_swap else "rename", comp,
                             manifest, renamed))
        grouped.update(comp)
    # Relocated content that ended up under a name outside the manifest.
    for item in renamed:
        if item["name"] not in grouped:
            groups.append(_group("rename", [item["name"]], manifest, renamed))
            grouped.add(item["name"])
    groups.sort(key=lambda g: (g["kind"], g["names"]))
    return {"ok": ok, "renamed": renamed, "missing": missing,
            "modified": modified, "extra": extra, "swaps": groups,
            "volatile": changed}


# --------------------------------------------------------------------------
# report
# --------------------------------------------------------------------------

def build_report(game: str, manifest_path: str, manifest: dict[str, str],
                 scan: dict[str, str], digests: dict[str, str],
                 links: list[str], total_bytes: int, seconds: float,
                 jobs: int, strict: bool, include_all: bool,
                 include_saves: bool, filter_source: str,
                 volatile: frozenset[str]) -> dict:
    result = classify(manifest, scan, digests, volatile)
    # A manifest file that exists on disk but was filtered out of the scan
    # (non-game extension, SAVEGAME/, symlink) is neither missing nor
    # audited -- report it separately instead of crying "missing".
    filtered = []
    for name in list(result["missing"]):
        if os.path.lexists(os.path.join(game, name.replace("/", os.sep))):
            filtered.append(name)
            result["missing"].remove(name)
    swaps = result["swaps"]
    swap_files = sum(len(g["members"]) for g in swaps if g["kind"] == "swap")
    rename_files = sum(len(g["members"]) for g in swaps if g["kind"] == "rename")
    problems = (len(result["missing"]) + len(result["modified"])
                + len(result["extra"]))
    failed = problems > 0 or (strict and (swap_files or rename_files))
    return {
        "game": os.path.abspath(game),
        "manifest": os.path.abspath(manifest_path),
        "manifest_files": len(manifest),
        "manifest_digest": manifest_digest(manifest),
        "manifest_is_baseline": manifest_digest(manifest) == KNOWN_MANIFEST_DIGEST,
        "scanned_files": len(scan),
        "scanned_bytes": total_bytes,
        "skipped_symlinks": links,
        "skipped_filtered": filtered,
        "volatile_files": sorted(volatile),
        "filter": {"source": filter_source, "include_all": include_all,
                   "include_saves": include_saves},
        "jobs": jobs,
        "seconds": round(seconds, 3),
        "counts": {
            "ok": len(result["ok"]),
            "renamed": len(result["renamed"]),
            "swap_files": swap_files,
            "rename_files": rename_files,
            "swap_groups": len([g for g in swaps if g["kind"] == "swap"]),
            "missing": len(result["missing"]),
            "modified": len(result["modified"]),
            "extra": len(result["extra"]),
            "filtered": len(filtered),
            "volatile": len(result["volatile"]),
        },
        "ok": not failed,
        "strict": strict,
        "exit_code": 2 if failed else 0,
        "swaps": swaps,
        "renamed": result["renamed"],
        "missing": result["missing"],
        "modified": result["modified"],
        "extra": result["extra"],
        "volatile": result["volatile"],
    }


def print_summary(report: dict, stream=sys.stdout) -> None:
    counts = report["counts"]
    write = lambda text: stream.write(text + "\n")      # noqa: E731
    write("install audit: %s" % report["game"])
    write("manifest    : %s (%d files, digest %s%s)"
          % (report["manifest"], report["manifest_files"],
             report["manifest_digest"][:16],
             "" if report["manifest_is_baseline"] else " -- NOT the shipped one"))
    write("scanned     : %d files, %.1f MiB in %.1fs (%d workers)"
          % (report["scanned_files"], report["scanned_bytes"] / 1048576.0,
             report["seconds"], report["jobs"]))
    write("")
    write("ok       %5d" % counts["ok"])
    write("renamed  %5d   (%d swap groups / %d files, %d plain renames)"
          % (counts["renamed"], counts["swap_groups"], counts["swap_files"],
             counts["rename_files"]))
    write("missing  %5d" % counts["missing"])
    write("modified %5d" % counts["modified"])
    write("extra    %5d" % counts["extra"])
    write("volatile %5d   (rewritten by the game at run time)"
          % counts["volatile"])
    for group in report["swaps"]:
        write("  %-6s %s" % (group["kind"],
                            " <-> ".join(group["names"])))
    for item in report["renamed"][:20]:
        write("  renamed: %s holds the content of %s"
              % (item["actual_path"], item["name"]))
    for name in report["missing"][:20]:
        write("  missing: %s" % name)
    for item in report["modified"][:20]:
        write("  modified: %s" % item["name"])
    for name in report["extra"][:20]:
        write("  extra: %s" % name)
    for item in report.get("volatile", [])[:20]:
        write("  volatile: %s (expected to differ from the snapshot)"
              % item["name"])
    if report["skipped_symlinks"]:
        write("  skipped %d symlink(s)" % len(report["skipped_symlinks"]))
    write("")
    write("RESULT: %s" % ("FAIL" if not report["ok"] else "ok"))


# --------------------------------------------------------------------------
# self-test
# --------------------------------------------------------------------------

def selftest(verbose: bool = True) -> int:
    """Classify a synthetic installation: swap, rename, modified, missing,
    extra.  Runs in a temporary directory, touches nothing else."""
    import shutil
    import tempfile

    tmp = tempfile.mkdtemp(prefix="gm-install-audit-", dir="/var/tmp")
    try:
        def write(name: str, data: bytes) -> None:
            with open(os.path.join(tmp, name), "wb") as fh:
                fh.write(data)

        def sha(data: bytes) -> str:
            return hashlib.sha256(data).hexdigest()

        # manifest: a (ok), b <-> c (swapped), e (missing), f (renamed to
        # g.txt), h (on disk with other bytes -> modified)
        manifest = {
            "a.txt": sha(b"aaa"), "b.txt": sha(b"bbb"), "c.txt": sha(b"ccc"),
            "e.txt": sha(b"eee"), "f.txt": sha(b"fff"), "h.txt": sha(b"hhh"),
        }
        write("a.txt", b"aaa")
        write("b.txt", b"ccc")          # b holds the content of c
        write("c.txt", b"bbb")          # c holds the content of b
        write("g.txt", b"fff")          # f was renamed to g.txt
        write("h.txt", b"HHH")          # wrong bytes
        write("z.txt", b"zzz")          # not in the manifest at all
        files, links, _size = scan_install(tmp, set(), False, True)
        digests = hash_all(files, 2, quiet=True)
        result = classify(manifest, files, digests)
        checks = [
            ("ok == [a.txt]", result["ok"] == ["a.txt"]),
            ("renamed == 3", len(result["renamed"]) == 3),
            ("swap group of 2",
             any(g["kind"] == "swap" and g["names"] == ["b.txt", "c.txt"]
                 for g in result["swaps"])),
            ("rename group of 1",
             any(g["kind"] == "rename" and g["names"] == ["f.txt"]
                 for g in result["swaps"])),
            ("missing == [e.txt]", result["missing"] == ["e.txt"]),
            ("modified == [h.txt]",
             [m["name"] for m in result["modified"]] == ["h.txt"]),
            ("extra == [z.txt]", result["extra"] == ["z.txt"]),
            ("no symlink skipped", links == []),
        ]
        volatile = classify(manifest, files, digests, frozenset({"h.txt"}))
        checks.append(("volatile == [h.txt]",
                       [v["name"] for v in volatile["volatile"]] == ["h.txt"]
                       and volatile["modified"] == []))
        bad = [name for name, good in checks if not good]
        for name, good in checks:
            if verbose:
                print("  %-22s %s" % (name, "ok" if good else "FAIL"))
        if verbose:
            print("selftest: %d/%d checks passed"
                  % (len(checks) - len(bad), len(checks)))
        return 0 if not bad else 1
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


# --------------------------------------------------------------------------
# CLI
# --------------------------------------------------------------------------

def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--game", default=DEFAULT_GAME,
                    help="installation to audit (default: %(default)s)")
    ap.add_argument("--manifest", default=DEFAULT_MANIFEST,
                    help="sha256 manifest (default: %(default)s)")
    ap.add_argument("--json", metavar="PATH",
                    help="write the JSON report here ('-' = stdout)")
    ap.add_argument("--quiet", action="store_true",
                    help="no progress on stderr, no summary on stdout")
    ap.add_argument("--jobs", type=int, default=min(16, os.cpu_count() or 4),
                    help="parallel hashers (default: %(default)s)")
    ap.add_argument("--strict", action="store_true",
                    help="treat renames/swaps as a failure (exit code 2)")
    ap.add_argument("--all", action="store_true",
                    help="also audit non-game files (DXWnd exports, docs, ...)")
    ap.add_argument("--include-saves", action="store_true",
                    help="also audit SAVEGAME/ (user saves)")
    ap.add_argument("--volatile", metavar="NAME", action="append", default=[],
                    help="extra file that the game rewrites at run time "
                         "(repeatable); such files are reported but never "
                         "fail the audit")
    ap.add_argument("--no-volatile", action="store_true",
                    help="do not treat %s as volatile"
                         % ", ".join(sorted(VOLATILE_FILES)))
    ap.add_argument("--selftest", action="store_true",
                    help="run the synthetic self-test and exit")
    opts = ap.parse_args(argv)

    if opts.selftest:
        return selftest()

    if opts.jobs < 1:
        print("install_audit: --jobs must be >= 1", file=sys.stderr)
        return 1
    if not os.path.isdir(opts.game):
        print("install_audit: %s is not a directory" % opts.game,
              file=sys.stderr)
        return 1
    if not os.path.isfile(opts.manifest):
        print("install_audit: %s is not a file" % opts.manifest, file=sys.stderr)
        return 1

    try:
        manifest = parse_manifest(opts.manifest)
    except (OSError, ValueError) as exc:
        print("install_audit: %s" % exc, file=sys.stderr)
        return 1

    non_game, from_gm = non_game_extensions()
    filter_source = "gm.py INVENTORY" if from_gm else "built-in table"
    volatile = set() if opts.no_volatile else set(VOLATILE_FILES)
    volatile.update(opts.volatile)

    t0 = time.time()
    scan, links, total_bytes = scan_install(opts.game, non_game,
                                            opts.include_saves, opts.all)
    if not opts.quiet:
        sys.stderr.write("install_audit: %d files to hash (%d workers)\n"
                         % (len(scan), opts.jobs))
        sys.stderr.flush()
    digests = hash_all(scan, opts.jobs, opts.quiet)
    seconds = time.time() - t0

    report = build_report(opts.game, opts.manifest, manifest, scan, digests,
                          links, total_bytes, seconds, opts.jobs, opts.strict,
                          opts.all, opts.include_saves, filter_source,
                          frozenset(volatile))

    if opts.json:
        blob = json.dumps(report, indent=2, ensure_ascii=False)
        if opts.json == "-":
            sys.stdout.write(blob + "\n")
        else:
            parent = os.path.dirname(os.path.abspath(opts.json))
            if parent:
                os.makedirs(parent, exist_ok=True)
            with open(opts.json, "w", encoding="utf-8", newline="\n") as fh:
                fh.write(blob + "\n")
            if not opts.quiet:
                print("report: %s" % opts.json)
    if not opts.quiet:
        print_summary(report)
    return report["exit_code"]


if __name__ == "__main__":
    raise SystemExit(main())
