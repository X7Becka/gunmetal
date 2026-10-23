#!/usr/bin/env python3
"""Разрешение имён сэмплов: ``SOUND_SAMPLE_TABLE`` (тип 12 ``.sbt``) -> ``sounds/SOU.IDX``.

Задача ``M7.6``: «отсутствующие имена — лог/тишина, не падение». Источник числа
95 в ``ROADMAP.md`` не был задокументирован; инструмент показывает, что 95 —
это не «ненайденные имена», а число имён, для которых есть строка в
``Sounds/sounds.sub`` (мёртвая в v1.36, см. ниже). Реально не разрешается
**0** имён из 528.

Что делает оригинал (полностью — ``docs/SOUND_NAMES.md``):

* загрузчик таблицы ``0x40b670`` читает из записи 12 ``.sbt`` ровно
  ``300*u32`` счётчиков (``0x4b0``) и ``300*9`` имён (``0xa8c``) и для каждого
  слота с ``count > 0`` и непустым именем зовёт ``0x40ade0(name)``;
* ``0x40ade0`` -> ``0x40af30(dir, name)`` -> ``0x40d1a0`` открывает
  ``sounds\\<NAME>.SOU`` в BIGFILE-архиве, а при промахе пробует второй
  каталог ``music`` (``[0x4c8774]`` = 0/1, ``0x40ae3c``) — единственный
  фолбэк оригинала; Имя **приводится к верхнему
  регистру** (``0x480180``: ``a-z -> A-Z`` на месте, ``0x40d1e6``; флаг
  ``[0x4bf120] == 0`` в образе — ветка Win32-поиска файла не выполняется), а
  ключом поиска служат первые **8 байт** (``0x40d1f0..0x40d20d``), которые
  сравниваются с именами записей индекса как два dword (``0x40d6b3..0x40d6c2``).
  Отсюда: регистр не важен, длина > 8 обрезается, имена NUL-дополнены;
* алиасы ``sounds.sub`` в этом билде **не работают**: загрузчик ``0x40b510``
  ждёт ``"%ld"``-счётчик и строки ``"%ld %s %s"``, а поставочный файл — пары
  ``name<TAB>name`` без счётчика, поэтому ``fscanf`` возвращает не 1,
  ``[0x4cf6b8]`` обнуляется и ``0x40b510`` отдаёт ``-2``; вызывающий
  ``0x44b0a5`` результат игнорирует;
* промах: ``0x40ade0``/``0x40af30`` отдают ``-1`` (нет записи) или ``-2``
  (нет свободной записи/битая шапка), хэндл слота ``0x4ca720[i]`` остаётся
  отрицательным (в режиме с музыкой, ``[0x4a6820] == 0``, — ``0``, который
  тоже никогда не выдаётся как номер записи); при проигрывании такой индекс отбрасывается молча
  (``0x40bec0``/``0x40bd80``: ``< 0`` -> выход без звука) — тишина, без
  падения. Сообщение ``Warning:  Unable to load sample …`` есть только у
  жёстко зашитых имён (``0x48f778``, ``0x40ab20``), у имён из ``.sbt`` лога нет.

Инструмент только читает установку (``.sbt`` + ``SOU.IDX``/``SOU.DAT``),
ничего не декодирует и не требует Ghidra.

Usage::

    python3 tools/re/sound_names.py --self-test
    python3 tools/re/sound_names.py --report
    python3 tools/re/sound_names.py --report --json
    python3 tools/re/sound_names.py --resolve alarmk
    python3 tools/re/sound_names.py --resolve 'sounds\\ALARMK' --json

Имя задаётся так, как оно лежит в таблице уровня — без расширения (расширение
``.SOU`` движок добавляет сам, ``0x40af30``); путь ``dir\\name`` допустим.

Коды возврата: 0 — успех/имя найдено, 1 — self-test упал или имя не найдено.
"""
from __future__ import annotations

import argparse
import collections
import glob
import json
import os
import re
import struct
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
for _p in (ROOT, HERE):
    if _p not in sys.path:
        sys.path.insert(0, _p)

from gm import bigfile as gm_bigfile      # noqa: E402
from gm import sbt as gm_sbt              # noqa: E402

DEFAULT_DIR = (os.environ.get("GUNMETAL_GAME")
               or "/mnt/d/games/Gunmetal 3dfx")

# --- константы (см. docs/SOUND_NAMES.md) -----------------------------------
RECORD_TYPE = 12          # SOUND_SAMPLE_TABLE в .sbt
TABLE_SLOTS = 300         # 0x12c: 0x40af30 (ebx < 0x12c), 0x40a4b0
SLOT_BYTES = 9            # 0xa8c / 300
COUNTS_BYTES = 0x4B0      # 300 * u32
NAMES_BYTES = 0xA8C       # 300 * 9
NAME_FIELD = 8            # имя записи BIGFILE
NAME_KEY = 8              # ключ поиска 0x40d1f0 / 0x40d6b3 — 8 байт
ALIAS_MAX = 150           # 0x96: 0x40b578 (предел строк sounds.sub)
ALIAS_ENTRY = 0x24        # 36 байт: threshold(4) + name1(16) + name2(16)
SAMPLE_INDEXES = 0x12C    # 300 записей 0x83d960

# имена, «похожие на имя файла»: BIGFILE-имена — 1..8 байт из этого алфавита
FILENAME_RE = re.compile(r"^[A-Za-z0-9_+.\-]{1,8}$")

# --- адреса (docs/SOUND_NAMES.md §3) --------------------------------------
ADDR_TABLE_LOAD = 0x40B670     # загрузчик записи 12 -> 0x4c9c90/0x4ca720
ADDR_TABLE_SAVE = 0x40B620     # сохранение таблицы (сейв)
ADDR_LOAD_BY_NAME = 0x40ADE0   # name -> запись 0x83d960 (dir/base split)
ADDR_OPEN_SOU = 0x40AF30       # dir+name -> "SOU", поиск свободной записи
ADDR_OPEN_ARCHIVE = 0x40D1A0   # сборка пути + поиск в BIGFILE
ADDR_UPPERCASE = 0x480180      # a-z -> A-Z на месте (0x4bf120 == 0)
ADDR_ENTRY_CMP = 0x40D6B3      # сравнение 8-байтного ключа с записью индекса
ADDR_FIND_OR_ADD = 0x40B9C0    # name -> слот имени (нормализация, добавление)
ADDR_RELEASE = 0x40BBE0        # освобождение слота по refcount
ADDR_ALIAS_LOAD = 0x40B510     # sounds.sub (формат DOS, в v1.36 не грузится)
ADDR_ALIAS_LOOKUP = 0x40B800   # name1 -> name2 при threshold > [0x4bf290]
ADDR_QUEUE_2D = 0x40BEC0       # индекс слота -> запись, отбрасывает < 0
ADDR_QUEUE_3D = 0x40BD80       # то же для 3D-очереди 0x62ce18
ADDR_PLAY = 0x40A4B0           # запись 0x83d960: id < 300 и size > 0
ADDR_NAME_OF = 0x40B940        # индекс -> имя или "none" (0x4a6968)

# адреса для отчёта (--report --json) и docs/SOUND_NAMES.md §3
ADDRESSES = {
    "table_load": (ADDR_TABLE_LOAD,
                   "запись 12 .sbt -> 0x4cebd8 / 0x4c9c90 / 0x4ca720"),
    "table_save": (ADDR_TABLE_SAVE, "та же раскладка -> поток сейва"),
    "load_by_name": (ADDR_LOAD_BY_NAME,
                     "name -> dir/base, sounds/music, 0x40af30"),
    "open_sou": (ADDR_OPEN_SOU, "свободная запись 0x83d960 + разбор шапки"),
    "open_archive": (ADDR_OPEN_ARCHIVE,
                     "BIGFILE: путь, ключ, 0x480180, 0x40d6b3"),
    "uppercase": (ADDR_UPPERCASE, "a-z -> A-Z на месте ([0x4bf120] == 0)"),
    "entry_cmp": (ADDR_ENTRY_CMP, "8-байтный ключ против записей индекса"),
    "find_or_add": (ADDR_FIND_OR_ADD, "слот имени: нормализация/добавление"),
    "release": (ADDR_RELEASE, "освобождение слота по счётчикам"),
    "alias_load": (ADDR_ALIAS_LOAD, "sounds.sub (формат DOS, не грузится)"),
    "alias_lookup": (ADDR_ALIAS_LOOKUP, "name1 -> name2 при пороге > класса RAM"),
    "queue_2d": (ADDR_QUEUE_2D, "индекс слота -> запись; < 0 отбрасывается"),
    "queue_3d": (ADDR_QUEUE_3D, "то же для 3D-очереди 0x62ce18"),
    "play": (ADDR_PLAY, "id < 300 и size > 0, иначе -1"),
    "name_of": (ADDR_NAME_OF, "индекс -> имя или \"none\""),
}


class Fail(Exception):
    """Ошибка разбора данных (не «ненайденное имя»)."""


def find_sounds_sub(game_dir: str) -> str | None:
    """Путь к ``sounds.sub`` рядом с ``SOU.IDX`` (``Sounds``/``sounds``).

    В установке каталог называется ``Sounds``, но на drvfs регистр не важен;
    зеркало и Linux-копии держат ``sounds``. Проверяем оба написания.
    """
    for sub in ("sounds", "Sounds"):
        path = os.path.join(game_dir, sub, "sounds.sub")
        if os.path.isfile(path):
            return path
    return None


# ---------------------------------------------------------------------------
# каталог SOU.IDX / SOU.DAT
# ---------------------------------------------------------------------------
def load_index(idx_path: str, dat_path: str | None = None) -> dict:
    """Записи каталога: имя (8 байт), смещение, размер.

    ``SOU.IDX`` — BIGFILE (``gm/bigfile.py``): ``magic[7] "BIGFILE"``,
    ``u8 0x1a``, ``u32 count``, далее ``count * { char name[8]; u32 offset; }``.
    Размер блоба — разность смещений, последний — до конца ``.dat``.
    """
    if dat_path is None:
        dat_path = os.path.join(os.path.dirname(idx_path), "SOU.DAT")
    try:
        idx = gm_bigfile.BigFile(idx_path)
    except Exception as exc:                       # noqa: BLE001
        raise Fail("не читается %s: %r" % (idx_path, exc)) from exc
    entries = list(idx.entries)
    if not entries:
        raise Fail("пустой индекс %s" % idx_path)
    if dat_path and os.path.isfile(dat_path):
        total = os.path.getsize(dat_path)
    else:
        total = entries[-1].offset                     # без .dat размеров нет
    out = []
    for i, e in enumerate(entries):
        end = entries[i + 1].offset if i + 1 < len(entries) else total
        if end < e.offset:
            raise Fail("смещения %s не возрастают (запись %d)" % (idx_path, i))
        out.append({"index": i, "name": e.name, "offset": e.offset,
                    "size": end - e.offset})
    return {"path": idx_path, "dat": dat_path, "entries": out,
            "dat_size": total}


def index_keys(index: dict) -> dict:
    """Ключ 8 байт -> запись. Ключ = имя записи, NUL-дополненное до 8.

    Имена индекса в поставке уже в верхнем регистре и не длиннее 8 байт;
    игра сравнивает именно 8 байт (``0x40d6b3``), поэтому ключ фиксированной
    длины, а не строка.
    """
    keys = {}
    for e in index["entries"]:
        raw = e["name"].encode("latin1")[:NAME_FIELD]
        keys[raw.ljust(NAME_KEY, b"\0")] = e
    return keys


def archive_key(name: str) -> bytes:
    """Ключ поиска записи: верхний регистр (``0x480180``) + 8 байт.

    ``0x480180`` поднимает регистр латиницы на месте; ключом становятся
    первые 8 байт уже поднятой строки (``0x40d1f0``, ``0x40d1f6``).
    """
    return name.encode("latin1", "replace")[:NAME_KEY].upper().ljust(
        NAME_KEY, b"\0")


def basename(name: str) -> str:
    """Отрезать каталог так же, как ``0x40ade0`` (последний ':' или '\\')."""
    cut = max(name.rfind("\\"), name.rfind(":"))
    return name[cut + 1:] if cut >= 0 else name


def slot_name(name: str) -> str:
    """Как ``0x40b9c0``: хвост после последнего '\\', не длиннее 8 символов."""
    tail = name[name.rfind("\\") + 1:]
    return tail[:8]


def _keys_of(index: dict) -> dict:
    keys = index.get("_keys")
    if keys is None:
        keys = index_keys(index)
        index["_keys"] = keys
    return keys


def resolve(index: dict, name: str, aliases: dict | None = None,
            music: dict | None = None) -> dict:
    """Разрешить имя так, как это делает оригинал.

    Порядок: алиас (если таблица ``sounds.sub`` загружена) -> каталог
    ``sounds`` -> каталог ``music`` (``0x40ade0``: ``[0x4c8774]`` = 0/1,
    ``sounds``/``music``, при неудаче переключается на второй, ``0x40ae10`` ..
    ``0x40ae51``).

    Возвращает словарь с полями ``query``, ``basename``, ``key`` (8 байт),
    ``entry`` (запись каталога или ``None``), ``via`` (``direct`` /
    ``fallback`` / ``alias`` / ``None``), ``archive`` (``sounds``/``music``).
    Отсутствие имени — не исключение: это штатный результат (в оригинале
    ``-1``, тишина).
    """
    base = basename(name)
    via = None
    if aliases:
        # 0x40b800: lookup(name1) -> name2, порог сравнивается с классом RAM.
        # В поставке таблица пуста, поэтому алиасы по умолчанию выключены.
        alt = aliases.get(base.upper())
        if alt:
            base = alt
            via = "alias"
    key = archive_key(base)
    hit = _keys_of(index).get(key)
    archive = "sounds" if hit else None
    if hit is None and music is not None:
        hit = _keys_of(music).get(key)
        if hit:
            archive = "music"
            via = via or "fallback"
    if hit is not None and via is None:
        via = "direct" if archive == "sounds" else "fallback"
    return {"query": name, "basename": base, "key": key.decode("latin1"),
            "entry": hit, "via": via, "archive": archive}


# ---------------------------------------------------------------------------
# sounds.sub
# ---------------------------------------------------------------------------
def parse_sub_dos(path: str) -> dict:
    """Пары ``replaced<TAB>replacement`` (формат DOS-версии, ``docs/SUB.md`` §5)."""
    try:
        raw = open(path, "rb").read().decode("latin1")
    except OSError as exc:
        raise Fail("не читается %s: %r" % (path, exc)) from exc
    pairs = []
    for line in raw.replace("\r\n", "\n").split("\n"):
        if not line.strip():
            continue
        parts = line.split("\t") if "\t" in line else line.split()
        if len(parts) < 2:
            continue
        pairs.append((parts[0], parts[1]))
    if not pairs:
        raise Fail("%s: ни одной пары" % path)
    return {"path": path, "pairs": pairs,
            "map": {a.upper(): b for a, b in pairs}}


def parse_sub_v136(path: str) -> dict:
    """Проверить, загрузит ли файл ``0x40b510``: ``"%ld"`` + ``"%ld %s %s"``.

    Возвращает ``{"loadable": bool, "reason": str}``. Поставочный
    ``Sounds/sounds.sub`` не загружается: первая строка — пара имён без
    счётчика, ``fscanf("%ld")`` не сматчится, функция вернёт ``-2``.
    """
    raw = open(path, "rb").read().decode("latin1", "replace")
    first = raw.replace("\r\n", "\n").split("\n")[0] if raw else ""
    m = re.match(r"\s*([+-]?\d+)", first)
    if not m:
        return {"loadable": False, "lines": len(
            [l for l in raw.replace("\r\n", "\n").split("\n") if l.strip()]),
            "reason": "нет счётчика в первой строке: %r" % first[:32]}
    count = int(m.group(1))
    return {"loadable": 0 < count <= ALIAS_MAX, "count": count,
            "reason": "счётчик %d (предел %d)" % (count, ALIAS_MAX)}


# ---------------------------------------------------------------------------
# .sbt: имена уровней
# ---------------------------------------------------------------------------
def level_names(path: str) -> list:
    """Имена слотов ``SOUND_SAMPLE_TABLE`` уровня (непустые, в порядке слотов)."""
    try:
        sbt = gm_sbt.parse_file(path)
    except Exception as exc:                       # noqa: BLE001
        raise Fail("не читается %s: %r" % (path, exc)) from exc
    recs = [r for r in sbt.records if r.type == RECORD_TYPE]
    if not recs:
        raise Fail("%s: нет записи %d (SOUND_SAMPLE_TABLE)"
                   % (path, RECORD_TYPE))
    table = recs[0].payload
    if len(table.names) != TABLE_SLOTS:
        raise Fail("%s: %d слотов вместо %d"
                   % (path, len(table.names), TABLE_SLOTS))
    if len(table.counts) != TABLE_SLOTS:
        raise Fail("%s: %d счётчиков вместо %d"
                   % (path, len(table.counts), TABLE_SLOTS))
    if len(table.counts) * 4 != COUNTS_BYTES:
        raise Fail("%s: область счётчиков %d байт вместо %d"
                   % (path, len(table.counts) * 4, COUNTS_BYTES))
    if len(table.name_slots) != NAMES_BYTES:
        raise Fail("%s: область имён %d байт вместо %d"
                   % (path, len(table.name_slots), NAMES_BYTES))
    if len(table.names) * SLOT_BYTES != NAMES_BYTES:
        raise Fail("%s: %d слотов по %d байт" % (path, len(table.names),
                                                 SLOT_BYTES))
    if SAMPLE_INDEXES != TABLE_SLOTS:
        raise Fail("таблица имён и массив записей разной длины")
    out = []
    for i, (name, count) in enumerate(zip(table.names, table.counts)):
        if name and count > 0:
            out.append((i, name, count))
    return out


# ---------------------------------------------------------------------------
# отчёт по всем уровням
# ---------------------------------------------------------------------------
def alias_impact(index: dict, music: dict | None, sub: dict,
                 folded_names: set) -> dict:
    """Что было бы, если бы ``sounds.sub`` загружался (он не загружается).

    Считаем по 528 именам уровней: сколько получают алиас, у скольких цель
    отсутствует в каталоге (была бы тишина) и у скольких подстановка ведёт на
    *другую* запись, чем прямое совпадение.
    """
    keys_s = _keys_of(index)
    keys_m = _keys_of(music) if music is not None else {}
    used, broken, same, changed = [], [], 0, 0
    for name in sorted(folded_names):
        target = sub["map"].get(name)
        if target is None:
            continue
        tkey = archive_key(target)
        if tkey not in keys_s and tkey not in keys_m:
            broken.append({"name": name, "target": target})
            continue
        used.append(name)
        direct = keys_s.get(archive_key(name))
        alt = keys_s.get(tkey) or keys_m.get(tkey)
        if direct is not None and alt is not None and direct["index"] == alt["index"]:
            same += 1
        else:
            changed += 1
    return {"alias_targets": len(sub["pairs"]),
            "alias_levels_used": len(used) + len(broken),
            "alias_would_resolve": len(used),
            "alias_would_break": broken,
            "alias_same_record": same,
            "alias_changed_record": changed}


def build_report(game_dir: str, use_aliases: bool = False) -> dict:
    idx_path = os.path.join(game_dir, "sounds", "SOU.IDX")
    if not os.path.isfile(idx_path):
        raise Fail("нет %s" % idx_path)
    index = load_index(idx_path)
    index["_keys"] = index_keys(index)
    music_path = os.path.join(game_dir, "MUSIC", "SOU.IDX")
    music = None
    if os.path.isfile(music_path):
        music = load_index(music_path, os.path.join(game_dir, "MUSIC",
                                                    "SOU.DAT"))
        music["_keys"] = index_keys(music)

    sub_path = find_sounds_sub(game_dir)
    sub = parse_sub_dos(sub_path) if sub_path else None
    aliases = sub["map"] if (sub and use_aliases) else None
    sub_info = {"path": sub_path, "present": sub is not None}
    f0 = f1 = set()
    if sub is not None:
        sub_info.update(parse_sub_v136(sub_path))
        f0 = {a.upper() for a, _ in sub["pairs"]}
        f1 = {b.upper() for _, b in sub["pairs"]}
        sub_info["pairs"] = len(sub["pairs"])
        sub_info["first_col"] = len(f0)
        sub_info["second_col"] = len(f1)

    levels = []
    refs = 0
    spellings = collections.Counter()
    via = collections.Counter()
    for path in sorted(glob.glob(os.path.join(game_dir, "*.sbt"))):
        lvl = os.path.basename(path)[:-4]
        names = level_names(path)
        refs += len(names)
        resolved = 0
        missing = []
        exact = 0
        for slot, name, _count in names:
            spellings[name] += 1
            hit = resolve(index, name, aliases, music)
            if hit["entry"] is not None:
                resolved += 1
                via[hit["archive"] or "?"] += 1
                if hit["via"] == "direct" and name == hit["entry"]["name"]:
                    exact += 1
            else:
                missing.append({"slot": slot, "name": name})
        folded = len({n.upper() for _s, n, _c in names})
        levels.append({"level": lvl, "slots": len(names), "distinct": folded,
                       "resolved": resolved,
                       "missing": missing, "exact_case": exact,
                       "file": path})

    folded_all = {n.upper() for n in spellings}
    fold_groups = collections.defaultdict(set)
    for n in spellings:
        fold_groups[n.upper()].add(n)
    index_names = {e["name"] for e in index["entries"]}
    exact_spellings = {n for n in spellings if n in index_names}
    resolved_folded = {n for n in folded_all
                       if resolve(index, n, aliases, music)["entry"] is not None}
    missing_folded = sorted(folded_all - resolved_folded)
    bad_names = sorted(n for n in spellings if not FILENAME_RE.match(n))
    too_long = sorted(n for n in spellings if len(n) > NAME_FIELD)

    if sub is not None:
        sub_info["names_in_first"] = len(folded_all & f0)
        sub_info["names_in_second"] = len(folded_all & f1)
        sub_info["names_in_neither"] = len(folded_all - (f0 | f1))
        sub_info["used"] = bool(aliases)
        sub_info.update(alias_impact(index, music, sub, folded_all))

    return {
        "game_dir": game_dir,
        "addresses": ADDRESSES,
        "index": {"path": idx_path, "entries": len(index["entries"]),
                  "dat_size": index["dat_size"],
                  "unique_names": len(index["_keys"]),
                  "music_path": music_path if music else None,
                  "music_entries": len(music["entries"]) if music else 0},
        "levels": levels,
        "totals": {
            "levels": len(levels),
            "refs": refs,
            "spellings": len(spellings),
            "folded": len(folded_all),
            "resolved_folded": len(resolved_folded),
            "missing_folded": len(missing_folded),
            "missing": missing_folded,
            "exact_spellings": len(exact_spellings),
            "fold_groups_multi": sum(1 for v in fold_groups.values()
                                     if len(v) > 1),
            "fold_extra_spellings": len(spellings) - len(folded_all),
            "resolved_via_sounds": via.get("sounds", 0),
            "resolved_via_music": via.get("music", 0),
            "not_filename_like": len(bad_names),
            "not_filename_like_list": bad_names[:20],
            "longer_than_8": too_long,
        },
        "sounds_sub": sub_info,
    }


def print_report(rep: dict) -> None:
    t = rep["totals"]
    print("каталог: %s (%d записей, .dat %d байт)"
          % (rep["index"]["path"], rep["index"]["entries"],
             rep["index"]["dat_size"]))
    print("уровень    слотов  имён  разрешено  нет")
    for lv in rep["levels"]:
        print("%-10s %6d %5d %10d %4d"
              % (lv["level"], lv["slots"], lv["distinct"],
                 lv["resolved"], len(lv["missing"])))
    print("%-10s %6d %5d %10d %4d"
          % ("итого", t["refs"], t["folded"], t["resolved_folded"],
             t["missing_folded"]))
    print()
    print("уровней: %d; слотов с именем: %d; разных написаний: %d; "
          "разных имён без регистра: %d"
          % (t["levels"], t["refs"], t["spellings"], t["folded"]))
    print("разрешено (0x480180 + 0x40d6b3): %d/%d; ненайденных: %d"
          % (t["resolved_folded"], t["folded"], t["missing_folded"]))
    print("  через sounds: %d; через music (фолбэк 0x40ae3c): %d"
          % (t["resolved_via_sounds"], t["resolved_via_music"]))
    if t["missing"]:
        print("НЕ НАЙДЕНЫ (%d): %s" % (t["missing_folded"],
                                       ", ".join(t["missing"])))
        for lv in rep["levels"]:
            if lv["missing"]:
                print("  %-10s %s" % (lv["level"], ", ".join(
                    "%s(слот %d)" % (m["name"], m["slot"])
                    for m in lv["missing"])))
    else:
        print("ненайденных имён нет")
    print("точное совпадение регистра: %d/%d написаний; "
          "групп с разным регистром: %d (+%d написаний)"
          % (t["exact_spellings"], t["spellings"], t["fold_groups_multi"],
             t["fold_extra_spellings"]))
    print("не похожи на имя файла (^[A-Za-z0-9_+.\\-]{1,8}$): %d"
          % t["not_filename_like"])
    if t["not_filename_like_list"]:
        print("  %s" % ", ".join(t["not_filename_like_list"]))
    if t["longer_than_8"]:
        print("длиннее 8 байт (обрезаются ключом): %s"
              % ", ".join(t["longer_than_8"]))
    sub = rep["sounds_sub"]
    print()
    if not sub.get("present"):
        print("sounds.sub: нет файла")
        return
    print("sounds.sub: %d пар; загружается 0x40b510: %s (%s)"
          % (sub["pairs"], "да" if sub.get("loadable") else "НЕТ",
             sub.get("reason", "")))
    print("  имён в 1-й колонке: %d; во 2-й: %d; ни в одной: %d; "
          "алиасы применены: %s"
          % (sub["names_in_first"], sub["names_in_second"],
             sub["names_in_neither"], "да" if sub["used"] else "нет"))
    if "alias_levels_used" in sub:
        print("  если применить: %d имён; целей нет в каталоге у %d (%s); "
              "та же запись у %d; другая запись у %d"
              % (sub["alias_levels_used"], len(sub["alias_would_break"]),
                 ", ".join("%s->%s" % (b["name"], b["target"])
                           for b in sub["alias_would_break"]) or "—",
                 sub["alias_same_record"], sub["alias_changed_record"]))


# ---------------------------------------------------------------------------
# --self-test
# ---------------------------------------------------------------------------
class _Check:
    def __init__(self):
        self.count = 0
        self.failed = []

    def __call__(self, cond, what):
        self.count += 1
        if not cond:
            self.failed.append(what)
            print("FAIL: %s" % what)


def _synth_index(names, dat_size=0x10000):
    entries = []
    off = 0
    for i, n in enumerate(names):
        entries.append({"index": i, "name": n, "offset": off, "size": 0x100})
        off += 0x100
    return {"path": "<synthetic>", "dat": "<synthetic>", "entries": entries,
            "dat_size": dat_size}


def constants_selftest(c: _Check) -> None:
    """Константы формата — те же числа, что в коде оригинала."""
    c(TABLE_SLOTS == 300 and SAMPLE_INDEXES == 0x12C,
      "300 слотов имён и 300 записей сэмплов (0x12c)")
    c(SLOT_BYTES == 9 and SLOT_BYTES * TABLE_SLOTS == NAMES_BYTES == 0xA8C,
      "9 байт * 300 = 0xa8c")
    c(COUNTS_BYTES == 0x4B0 and COUNTS_BYTES == TABLE_SLOTS * 4,
      "0x4b0 = 300 * u32")
    c(NAME_FIELD == 8 and NAME_KEY == 8, "имя записи и ключ — 8 байт")
    c(ALIAS_MAX == 0x96 and ALIAS_ENTRY == 0x24, "0x96 записей по 0x24 байта")
    c(RECORD_TYPE == 12, "SOUND_SAMPLE_TABLE = тип 12")
    c(len(ADDRESSES) == 15 and all(isinstance(v[0], int)
                                   for v in ADDRESSES.values()),
      "ADDRESSES: 15 адресов")


def synthetic_selftest(c: _Check) -> None:
    idx = _synth_index(["ALARMK", "ADON", "ABCDEFGH", "AB", "UPDOWN"])
    # точное совпадение
    r = resolve(idx, "ALARMK")
    c(r["entry"] is not None and r["entry"]["name"] == "ALARMK",
      "точное имя ALARMK находится")
    # регистр не важен (0x480180)
    c(resolve(idx, "alarmk")["entry"] is not None, "alarmk -> ALARMK (регистр)")
    c(resolve(idx, "AlArMk")["entry"] is not None, "AlArMk -> ALARMK")
    # ключ — 8 байт, NUL-дополнение: префикс не должен совпадать
    c(resolve(idx, "ALAR")["entry"] is None, "ALAR не совпадает с ALARMK")
    c(resolve(idx, "ALARMKX")["entry"] is None,
      "ALARMKX не совпадает с ALARMK (ключ 8 байт)")
    # ровно 8 символов
    c(resolve(idx, "abcdefgh")["entry"]["name"] == "ABCDEFGH",
      "8 символов: abcdefgh -> ABCDEFGH")
    c(resolve(idx, "abcdefghi")["entry"]["name"] == "ABCDEFGH",
      "9 символов обрезаются ключом до 8: abcdefghi -> ABCDEFGH")
    # путь отрезается (0x40ade0)
    c(resolve(idx, r"sounds\ALARMK")["entry"] is not None,
      "путь sounds\\ALARMK отрезается")
    c(resolve(idx, "c:ADON")["entry"] is not None, "c:ADON -> ADON")
    c(resolve(idx, r"x\y\updown")["entry"] is not None,
      r"x\y\updown -> UPDOWN")
    # двухсимвольное имя
    c(resolve(idx, "ab")["entry"]["name"] == "AB", "ab -> AB")
    # промах — не исключение
    c(resolve(idx, "NOSUCH")["entry"] is None, "NOSUCH: None, без исключения")
    c(resolve(idx, "")["entry"] is None, "пустое имя: None")
    # ключ фиксированной длины
    c(len(archive_key("a")) == NAME_KEY and archive_key("a") == b"A\0\0\0\0\0\0\0",
      "archive_key: 8 байт, NUL-дополнение, верхний регистр")
    # нормализация слота 0x40b9c0
    c(slot_name(r"sounds\ABCDEFGHIJ") == "ABCDEFGH", "slot_name: 8 символов")
    c(slot_name("ABCDEFGHIJ") == "ABCDEFGH", "slot_name: без пути")
    c(basename(r"sounds\x.SOU") == "x.SOU", "basename: '\\'")
    c(basename("c:x") == "x", "basename: ':'")
    # алиасы
    al = {"ALARMK": "ALARMBIG"}
    idx2 = _synth_index(["ALARMBIG"])
    r = resolve(idx2, "alarmk", al)
    c(r["entry"] is not None and r["via"] == "alias", "алиас alarmk -> alarmkbig")
    c(resolve(idx2, "alarmk")["entry"] is None, "без алиасов alarmk не найден")
    # фолбэк во второй каталог (0x40ade0: sounds -> music)
    mus = _synth_index(["DM1A01"])
    r = resolve(idx2, "dm1a01", None, mus)
    c(r["entry"] is not None and r["via"] == "fallback"
      and r["archive"] == "music", "фолбэк sounds -> music")
    c(resolve(idx2, "dm1a01")["entry"] is None,
      "без второго каталога dm1a01 не найден")
    r = resolve(idx2, "alarmbig", None, mus)
    c(r["archive"] == "sounds" and r["via"] == "direct",
      "своя запись важнее фолбэка")
    # «похоже на имя файла»
    c(bool(FILENAME_RE.match("AB")), "FILENAME_RE: AB")
    c(not FILENAME_RE.match("A B"), "FILENAME_RE: пробел отвергнут")
    c(not FILENAME_RE.match(""), "FILENAME_RE: пусто отвергнуто")
    c(not FILENAME_RE.match("ABCDEFGHI"), "FILENAME_RE: 9 символов")
    # разбор sounds.sub в формате DOS
    with tempfile.NamedTemporaryFile("wb", suffix=".sub", delete=False) as fh:
        fh.write(b"alarmk\talarmkbig\r\namboilr2\tamboilr1\r\n\r\n")
        tmp = fh.name
    try:
        sub = parse_sub_dos(tmp)
        c(len(sub["pairs"]) == 2 and sub["map"]["ALARMK"] == "alarmkbig",
          "parse_sub_dos: пары и карта")
        v = parse_sub_v136(tmp)
        c(v["loadable"] is False, "parse_sub_v136: без счётчика не грузится")
        with open(tmp, "wb") as fh:
            fh.write(b"2\r\n20 repl1 targ1\r\n20 repl2 targ2\r\n")
        v = parse_sub_v136(tmp)
        c(v["loadable"] is True and v["count"] == 2,
          "parse_sub_v136: со счётчиком грузится")
    finally:
        os.unlink(tmp)


def _write_level(path: str, names) -> None:
    """Минимальный .sbt: шапка, запись 12 с именами, EOF (255)."""
    counts = [1 if n else 0 for n in names]
    table = gm_sbt.SampleTable(tuple(counts), tuple(names))
    body = gm_sbt.encode_payload(RECORD_TYPE, table)
    head = gm_sbt.MAGIC + b"\0" * (80 - len(gm_sbt.MAGIC)) + struct.pack("<I", 0)
    with open(path, "wb") as fh:
        fh.write(head + bytes((RECORD_TYPE,)) + body + bytes((255,)))


def report_selftest(c: _Check) -> None:
    """Отчёт на синтетической установке: путь «найдено/не найдено»."""
    tmp = tempfile.mkdtemp(prefix="sound_names_")
    try:
        os.mkdir(os.path.join(tmp, "sounds"))
        blobs = [("ALARMK", b"SOU1" + b"\0" * 12),
                 ("DOORPS1", b"SOU1" + b"\0" * 12)]
        gm_bigfile.pack(blobs, os.path.join(tmp, "sounds", "SOU.IDX"),
                        overwrite=True)
        names = [""] * TABLE_SLOTS
        names[0] = "alarmk"          # найдётся после верхнего регистра
        names[1] = "doorps1"         # найдётся
        _write_level(os.path.join(tmp, "level0.sbt"), names)
        names2 = list(names)
        names2[0] = "nosuch"         # промах
        _write_level(os.path.join(tmp, "level1.sbt"), names2)

        rep = build_report(tmp)
        c(rep["totals"]["levels"] == 2, "синтетика: два уровня")
        c(rep["totals"]["refs"] == 4, "синтетика: 4 слота с именем")
        c(rep["totals"]["folded"] == 3, "синтетика: 3 разных имени")
        c(rep["totals"]["missing_folded"] == 1, "синтетика: 1 ненайденное имя")
        c(rep["totals"]["missing"] == ["NOSUCH"],
          "синтетика: в списке NOSUCH")
        lv = {r["level"]: r for r in rep["levels"]}
        c(len(lv["level0"]["missing"]) == 0, "синтетика: level0 без промахов")
        c(lv["level1"]["missing"] == [{"slot": 0, "name": "nosuch"}],
          "синтетика: level1 слот 0 не найден")
        c(rep["index"]["entries"] == 2, "синтетика: 2 записи каталога")
        c(rep["index"]["dat_size"] == 32, "синтетика: размер .dat из файла")
        c(rep["sounds_sub"]["present"] is False, "синтетика: sounds.sub нет")
    finally:
        for name in os.listdir(tmp):
            p = os.path.join(tmp, name)
            if os.path.isdir(p):
                for f in os.listdir(p):
                    os.unlink(os.path.join(p, f))
                os.rmdir(p)
        for name in os.listdir(tmp):
            os.unlink(os.path.join(tmp, name))
        os.rmdir(tmp)


def real_selftest(c: _Check, game_dir: str) -> None:
    idx_path = os.path.join(game_dir, "sounds", "SOU.IDX")
    files = sorted(glob.glob(os.path.join(game_dir, "*.sbt")))
    if not (os.path.isfile(idx_path) and files):
        print("NOTE: нет установки (%s) — реальные проверки пропущены"
              % game_dir)
        return
    index = load_index(idx_path)
    c(len(index["entries"]) == 999, "SOU.IDX: 999 записей")
    c(index["entries"][0]["offset"] == 0, "SOU.IDX: первая запись с 0")
    c(index["dat_size"] == 26738998, "sounds/SOU.DAT: 26 738 998 байт")
    c(sum(e["size"] for e in index["entries"]) == index["dat_size"],
      "SOU.IDX: размеры покрывают .dat без дыр")
    c(len({e["name"] for e in index["entries"]}) == 999,
      "SOU.IDX: имена уникальны")
    c(all(e["name"] == e["name"].upper() for e in index["entries"]),
      "SOU.IDX: имена в верхнем регистре")
    c(all(len(e["name"]) <= NAME_FIELD for e in index["entries"]),
      "SOU.IDX: имена не длиннее 8 байт")

    rep = build_report(game_dir)
    t = rep["totals"]
    c(t["levels"] == 36, "уровней 36 (найдено %d)" % t["levels"])
    c(t["refs"] == 5800, "слотов с именем 5800 (получено %d)" % t["refs"])
    c(t["spellings"] == 639, "разных написаний 639 (получено %d)"
      % t["spellings"])
    c(t["folded"] == 528, "разных имён без регистра 528 (получено %d)"
      % t["folded"])
    c(t["missing_folded"] == 0, "все имена разрешаются (нет %d)"
      % t["missing_folded"])
    c(t["exact_spellings"] == 228, "точный регистр: 228 (получено %d)"
      % t["exact_spellings"])
    c(t["not_filename_like"] == 0, "все имена похожи на имя файла")
    sub = rep["sounds_sub"]
    c(sub["pairs"] == 161, "sounds.sub: 161 пара")
    c(sub["names_in_first"] == 95, "sounds.sub: 95 имён в 1-й колонке")
    c(sub["names_in_second"] == 42, "sounds.sub: 42 имени во 2-й колонке")
    c(sub["names_in_neither"] == 391, "sounds.sub: 391 имя не упомянуто")
    c(sub["loadable"] is False, "sounds.sub: 0x40b510 его не загружает")
    c(sub["alias_levels_used"] == 95 and sub["alias_would_resolve"] == 93
      and len(sub["alias_would_break"]) == 2
      and sub["alias_same_record"] == 0,
      "sounds.sub: 95 имён, 93 сменили бы запись, 2 -> тишина")
    c(not sub["used"], "sounds.sub: алиасы не применяются по умолчанию")
    # выборочная сверка: имена, которые точно есть/нет в каталоге
    c(resolve(index, "alarmk")["entry"]["name"] == "ALARMK",
      "реальное: alarmk -> ALARMK")
    c(resolve(index, "ADON")["entry"]["name"] == "ADON", "реальное: ADON")
    c(resolve(index, "updown")["entry"]["name"] == "UPDOWN",
      "реальное: updown -> UPDOWN (жёстко зашитое имя 0x4bdf7c)")
    c(resolve(index, "nosuchsample")["entry"] is None,
      "реальное: nosuchsample не найден")
    music_path = os.path.join(game_dir, "MUSIC", "SOU.IDX")
    if os.path.isfile(music_path):
        music = load_index(music_path,
                           os.path.join(game_dir, "MUSIC", "SOU.DAT"))
        c(len(music["entries"]) == 3092, "MUSIC/SOU.IDX: 3092 записи")
        c(resolve(index, "DM1A01", None, music)["archive"] == "music",
          "реальное: DM1A01 находится только через music")
        c(t["resolved_via_music"] == 0,
          "реальное: ни одно имя уровня не уходит в music")


def self_test(game_dir: str) -> int:
    c = _Check()
    constants_selftest(c)
    synthetic_selftest(c)
    report_selftest(c)
    real_selftest(c, game_dir)
    print("\nself-test: %s (%d проверок)"
          % ("OK" if not c.failed else "FAIL", c.count))
    return 0 if not c.failed else 1


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------
def _entry_json(hit: dict) -> dict:
    e = hit["entry"]
    return {"query": hit["query"], "basename": hit["basename"],
            "key": hit["key"], "resolved": e is not None,
            "via": hit["via"], "archive": hit["archive"],
            "entry": None if e is None else {
                "name": e["name"], "index": e["index"],
                "offset": e["offset"], "size": e["size"]}}


def main(argv) -> int:
    ap = argparse.ArgumentParser(
        description="имена SOUND_SAMPLE_TABLE -> sounds/SOU.IDX (M7.6)")
    ap.add_argument("--report", action="store_true",
                    help="отчёт по всем уровням")
    ap.add_argument("--resolve", metavar="NAME", action="append", default=[],
                    help="разрешить имя (как в таблице, без расширения)")
    ap.add_argument("--aliases", action="store_true",
                    help="применить sounds.sub (в поставке не загружается)")
    ap.add_argument("--self-test", action="store_true", help="встроенные тесты")
    ap.add_argument("--json", action="store_true", help="вывод в JSON")
    ap.add_argument("--dir", default=DEFAULT_DIR, help="каталог игры")
    opts = ap.parse_args(argv)

    if opts.self_test:
        try:
            return self_test(opts.dir)
        except Exception as exc:                   # noqa: BLE001
            print("FAIL: непредвиденная ошибка self-test: %r" % (exc,))
            return 1

    try:
        if opts.report:
            rep = build_report(opts.dir, use_aliases=opts.aliases)
            if opts.json:
                print(json.dumps(rep, ensure_ascii=False, indent=1))
            else:
                print_report(rep)
            return 0

        if opts.resolve:
            idx_path = os.path.join(opts.dir, "sounds", "SOU.IDX")
            if not os.path.isfile(idx_path):
                raise Fail("нет %s" % idx_path)
            index = load_index(idx_path)
            music_path = os.path.join(opts.dir, "MUSIC", "SOU.IDX")
            music = None
            if os.path.isfile(music_path):
                music = load_index(music_path, os.path.join(
                    opts.dir, "MUSIC", "SOU.DAT"))
            aliases = None
            sub_path = find_sounds_sub(opts.dir)
            if opts.aliases and sub_path:
                aliases = parse_sub_dos(sub_path)["map"]
            hits = [_entry_json(resolve(index, n, aliases, music))
                    for n in opts.resolve]
            if opts.json:
                print(json.dumps(hits if len(hits) > 1 else hits[0],
                                 ensure_ascii=False, indent=1))
            else:
                for h in hits:
                    if h["resolved"]:
                        e = h["entry"]
                        note = ""
                        if h["via"] == "alias":
                            note = ", алиас"
                        elif h["archive"] == "music":
                            note = ", фолбэк в каталог music"
                        print("%s -> %s (%s, запись %d, offset %d, %d байт%s)"
                              % (h["query"], e["name"], h["archive"],
                                 e["index"], e["offset"], e["size"], note))
                    else:
                        print("%s -> не найдено (оригинал: -1, тишина, "
                              "без падения)" % h["query"])
            return 0 if all(h["resolved"] for h in hits) else 1

        ap.print_help()
        return 0
    except Fail as exc:
        print("FAIL: %s" % exc)
        return 1


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
