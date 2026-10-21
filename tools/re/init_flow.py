#!/usr/bin/env python3
"""Инициализация движка gundx.exe: карта шагов, проверка по PE (M2.2.1).

Печатает последовательность bootstrap-а нового движка: шаг → адрес → байты →
глобалы → на что влияет → статус. ``--verify`` сверяет таблицу с реальным
``gundx.exe`` (байты инструкций + таблица импортов), ``--self-test`` проверяет
сам верификатор на синтетическом PE, поэтому работает без игры.

    python3 tools/re/init_flow.py                 # таблица шагов
    python3 tools/re/init_flow.py --mermaid       # диаграмма последовательности
    python3 tools/re/init_flow.py --verify        # сверка с GUNMETAL_EXE
    python3 tools/re/init_flow.py --self-test     # автотест (+ игра, если есть)

Путь к игре: ``$GUNMETAL_EXE`` (по умолчанию — установленная копия, см.
``tools/re/exelib.py``). Только stdlib, exe читается, но не меняется.
Карта цикла после bootstrap — ``docs/MAINLOOP.md``; полный разбор —
``docs/INIT.md``.
"""
import os
import struct
import sys

DEFAULT_EXE = "/mnt/d/games/Gunmetal 3dfx/gundx.exe"
EXE = os.environ.get("GUNMETAL_EXE", DEFAULT_EXE)

# Импорты, которых в бинарнике нет: время и звук сделаны вручную
# (KERNEL32 time + DirectSound streaming), это часть карты инициализации.
NEGATIVE_IMPORTS = ["timeGetTime", "QueryPerformanceCounter", "timeSetEvent",
                    "PlaySound", "midiOutOpen", "DirectPlayCreate"]
# Ровно по одному входу из этих библиотек (создание объекта, дальше — COM).
SINGLE_PEER_IMPORTS = {"DDRAW.dll": ["DirectDrawCreate"],
                       "DSOUND.dll": ["DirectSoundCreate"],
                       "DINPUT.dll": ["DirectInputCreateA"]}


# --------------------------------------------------------------------------
# PE (stdlib): заголовки, секции, импорты
# --------------------------------------------------------------------------
class PE:
    """Минимальный PE32: секции, точка входа, таблица импорта."""

    def __init__(self, data):
        self.data = data
        if data[:2] != b"MZ":
            raise ValueError("not an MZ image")
        pe = struct.unpack_from("<I", data, 0x3C)[0]
        if data[pe:pe + 4] != b"PE\0\0":
            raise ValueError("no PE signature at 0x%x" % pe)
        nsec = struct.unpack_from("<H", data, pe + 6)[0]
        optsz = struct.unpack_from("<H", data, pe + 20)[0]
        self.opt = pe + 24
        self.image_base = struct.unpack_from("<I", data, self.opt + 28)[0]
        self.entry_rva = struct.unpack_from("<I", data, self.opt + 16)[0]
        self.subsystem = struct.unpack_from("<H", data, self.opt + 68)[0]
        self.nrva = struct.unpack_from("<I", data, self.opt + 92)[0]
        base = self.opt + optsz
        self.sections = []
        for i in range(nsec):
            o = base + 40 * i
            name = data[o:o + 8].rstrip(b"\0").decode("latin1")
            vsize, rva, rawsize, rawoff = struct.unpack_from("<IIII", data, o + 8)
            self.sections.append({"name": name, "va": self.image_base + rva,
                                  "vsize": vsize or rawsize, "rawsize": rawsize,
                                  "rawoff": rawoff})
        self.imports = {}
        self.import_libs = {}
        self._parse_imports()

    # -- адреса --------------------------------------------------------
    @property
    def entry_va(self):
        return self.image_base + self.entry_rva

    def va2off(self, va):
        for s in self.sections:
            if s["va"] <= va < s["va"] + s["rawsize"]:
                return s["rawoff"] + (va - s["va"])
        return None

    def read(self, va, size):
        o = self.va2off(va)
        if o is None:
            return None
        return self.data[o:o + size]

    def cstr(self, va, limit=256):
        o = self.va2off(va)
        if o is None:
            return None
        return self.data[o:o + limit].split(b"\0")[0].decode("latin1")

    # -- импорты -------------------------------------------------------
    def _parse_imports(self):
        if self.nrva < 2:
            return
        rva, size = struct.unpack_from("<II", self.data, self.opt + 96 + 8)
        if rva == 0:
            return
        o = self.va2off(self.image_base + rva)
        if o is None:
            return
        while True:
            oft, ts, fc, name_rva, ft = struct.unpack_from("<IIIII", self.data, o)
            if name_rva == 0 and ft == 0:
                break
            dll = self.cstr(self.image_base + name_rva) or "?"
            self.import_libs.setdefault(dll, [])
            thunk = ft or oft
            j = 0
            while True:
                slot = self.image_base + thunk + 4 * j
                v = self.read(slot, 4)
                if v is None or v == b"\0\0\0\0":
                    break
                v = struct.unpack("<I", v)[0]
                if v & 0x80000000:
                    name = "ord%d" % (v & 0xffff)
                else:
                    name = (self.cstr(self.image_base + v + 2) or "?")
                self.imports[slot] = (dll, name)
                self.import_libs[dll].append(name)
                j += 1
            o += 20


# --------------------------------------------------------------------------
# Таблица шагов: (№, фаза, шаг, вид проверки, адрес, байты, extra, глобалы,
#                 эффект, статус)
#   entry   — точка входа PE + байты пролога
#   call    — e8 rel32, extra = целевой VA
#   push    — 68 imm32, extra = значение (VA строки/структуры)
#   iatcall — ff 15 <slot>, extra = (имя, dll) — вызов через таблицу импорта
#   iat     — extra = (имя, dll), addr = адрес слота IAT
#   bytes   — точные байты инструкции (префикс) в addr
#   cstr    — NUL-строка по addr равна extra
# --------------------------------------------------------------------------
STEPS = [
    (1, 'crt', 'PE entry (AddressOfEntryPoint=0x1000), CRT startup', 'entry', 0x401000, '55 8b ec', "", '-', 'SEH frame, heap, argv; затем GetModuleHandleA → WinMain', 'код'),
    (2, 'crt', 'GetVersion → [0x4bee44..0x4bee4c]', 'iat', 0x841318, '', ("GetVersion", "KERNEL32.dll"), '0x4bee40/44/48/4c', 'major/minor/build DOS-версии', 'код'),
    (3, 'crt', 'GetCommandLineA → [0x840b6c]', 'iat', 0x84131c, '', ("GetCommandLineA", "KERNEL32.dll"), '0x840b6c', 'строка разбирается на argc/argv (кавычки, пробелы)', 'код'),
    (4, 'crt', 'GetModuleHandleA(NULL) → hInst для WinMain', 'iat', 0x841330, '', ("GetModuleHandleA", "KERNEL32.dll"), 'eax', 'третий аргумент WinMain', 'код'),
    (5, 'crt', 'call 0x404680 (WinMain)', 'call', 0x40114d, 'e8 2e 35 00 00', 0x404680, '-', 'переход в игру; ret 0x10 — stdcall', 'код'),
    (6, 'window', 'call 0x404320 (app_init: класс, окно, подсистемы)', 'call', 0x404692, 'e8 89 fc ff ff', 0x404320, '-', 'вся цепочка ниже', 'код'),
    (7, 'window', 'LoadCursorA(0, IDC_ARROW=0x7f00)', 'iat', 0x8413f4, '', ("LoadCursorA", "USER32.dll"), '-', 'hCursor в WNDCLASS', 'код'),
    (8, 'window', 'LoadIconA(0, IDI_APPLICATION=0x7f00)', 'iat', 0x8413d0, '', ("LoadIconA", "USER32.dll"), '-', 'hIcon в WNDCLASS', 'код'),
    (9, 'window', 'GetStockObject(DKGRAY_BRUSH=4)', 'iat', 0x841274, '', ("GetStockObject", "GDI32.dll"), '-', 'hbrBackground', 'код'),
    (10, 'window', 'WNDCLASS.lpfnWndProc = 0x404070, style=3', 'bytes', 0x404430, 'c7 44 24 18 70 40 40 00', "", '0x404070', 'WndProc: WM_DESTROY/ACTIVATE/PAINT/SETCURSOR (docs/MAINLOOP.md §2)', 'код'),
    (11, 'window', 'RegisterClassA("FSDSClass")', 'iat', 0x8413c4, '', ("RegisterClassA", "USER32.dll"), '-', '0 = провал, WinMain возвращает 0', 'код'),
    (12, 'window', 'имя класса', 'cstr', 0x4a5980, '', 'FSDSClass', '-', 'lpszClassName (и lpszMenuName — та же строка)', 'код'),
    (13, 'window', 'заголовок окна', 'cstr', 0x4a5968, '', 'FSDS Direct X Game', '-', 'lpWindowName', 'код'),
    (14, 'window', 'CreateWindowExA(0, "FSDSClass", "FSDS Direct X Game", WS_POPUP, 0,0,SM_CXSCREEN,SM_CYSCREEN)', 'iat', 0x8413bc, '', ("CreateWindowExA", "USER32.dll"), '-', '0 = провал', 'код'),
    (15, 'window', 'ShowWindow(SW_SHOWNORMAL=1) + UpdateWindow', 'iat', 0x8413b8, '', ("ShowWindow", "USER32.dll"), '-', 'окно видно до первого кадра', 'код'),
    (16, 'window', '[0x4bf294] = hwnd', 'bytes', 0x404498, '89 35 94 f2 4b 00', "", '0x4bf294', 'единственный hwnd: от него DirectDraw/DirectInput/DirectSound', 'код'),
    (17, 'window', 'GetSystemMetrics(SM_CXSCREEN/SM_CYSCREEN) — размер окна во весь экран', 'iat', 0x8413c0, '', ("GetSystemMetrics", "USER32.dll"), '-', 'окно = размер рабочего стола, дальше SetDisplayMode', 'код'),
    (18, 'window', 'GlobalMemoryStatus → [0x4bf290] ≈ (totalPhys-4МБ)/1МБ', 'iat', 0x841324, '', ("GlobalMemoryStatus", "KERNEL32.dll"), '0x4bf290', 'оценка памяти для кэшей (гипотеза: размеры текстур)', 'гипотеза'),
    (19, 'window', 'DialogBoxParamA(hInst, 0x66, hwnd, 0x404180, 0)', 'iat', 0x8413b0, '', ("DialogBoxParamA", "USER32.dll"), '-', 'только для /net (см. 0x4043c0..0x4043d7, флаг [esp+0xc])', 'код'),
    (20, 'window', 'MessageBoxA для любой ошибки инициализации (формат "%s:\\n%s")', 'iat', 0x8413d4, '', ("MessageBoxA", "USER32.dll"), '-', '0x4045e0: MessageBox + DestroyWindow + return 0', 'код'),
    (21, 'order', 'call 0x44a460 (game_preinit) — конфиг, CLI, палитра, звук', 'call', 0x4044e2, 'e8 79 5f 04 00', 0x44a460, '-', 'первым: нужен до выбора видеорежима', 'код'),
    (22, 'order', 'call 0x405fc0 (ddraw_preinit) — DirectDrawCreate + coop 0x53 + EnumDisplayModes', 'call', 0x4044f7, 'e8 c4 1a 00 00', 0x405fc0, '-', 'после preinit, до окна-зависимых буферов', 'код'),
    (23, 'order', 'call 0x404b00 (DirectInput init)', 'call', 0x40450d, 'e8 ee 05 00 00', 0x404b00, '0x4bfcf0/0x4bfcf4/0x4bfcf8', 'клавиатура, мышь, перечисление джойстиков', 'код'),
    (24, 'order', 'call 0x406a80 (ddraw_init) — SetDisplayMode + surface chain', 'call', 0x404539, 'e8 42 25 00 00', 0x406a80, '0x4c05c0..0x4c05e8', 'реальный видеорежим (с fallback 320x200x8)', 'код'),
    (25, 'order', 'call 0x4074a0 (timer thread) — 91 Гц тик + миксер звука', 'call', 0x404569, 'e8 32 2f 00 00', 0x4074a0, '0x4c0600/0x4c0604', 'фоновый поток стартует до первой игровой логики', 'код'),
    (26, 'order', 'call 0x44b200 (game_init) — фронтенд, интро, gamma, present', 'call', 0x40459c, 'e8 5f 6c 04 00', 0x44b200, '0x4bfce4', 'последним: требует уже готовых видео/звука/палитры', 'код'),
    (27, 'order', 'PeekMessageA/TranslateMessage/DispatchMessageA — цикл (docs/MAINLOOP.md §1)', 'iat', 0x8413e0, '', ("PeekMessageA", "USER32.dll"), '0x4a598c', 'после успешного app_init; WM_QUIT → shutdown', 'код'),
    (28, 'order', 'WaitMessage — окно неактивно ([0x4a598c] != 0)', 'iat', 0x8413dc, '', ("WaitMessage", "USER32.dll"), '0x4a598c', 'кадр не считается в фоне', 'код'),
    (29, 'config', 'лог: 0x493620() → 0x40c4f0 (GUNMETAL.LOG)', 'call', 0x44a49f, 'e8 4c 20 fc ff', 0x40c4f0, '0x4a69d8', 'первым в game_preinit: дальше все ошибки пишутся в лог', 'код'),
    (30, 'config', 'bigfile init 0x40d150 (контейнер .sbc)', 'call', 0x44a4a7, 'e8 a4 2c fc ff', 0x40d150, '0x4d2bd8/0x4d2dd8', 'ошибка → "Error initializing bigfile" 0x4abeb0', 'код'),
    (31, 'config', 'сброс состояния: [0x83d620]=[0x83d18c]=[0x83cb7c]=0', 'bytes', 0x44a4c9, '89 2d 20 d6 83 00 89 2d 8c d1', "", '0x818650/0x836ad0/0x83d620/0x83cb7c', '0x44a4bd..0x44a4d5, затем обнуление массива 0x81c250 (шаг 0x3510)', 'код'),
    (32, 'config', 'RNG-затравка: 0x480ef0()+0x40db00()+0x480e30() → 0x83d2d0/0x83d2d4', 'call', 0x44a528, 'e8 d3 35 fc ff', 0x40db00, '0x83d2d0/0x83d2d4', 'второе число = первое + 0x1001, оба != 0', 'код'),
    (33, 'config', 'открыть "GUNMETAL.CFG" (режим "rb")', 'push', 0x484dd2, '68 70 c3 4b 00', 0x4bc370, '0x4aac7c', 'в 0x484cf0 — самом начале game_preinit (0x44a518)', 'код'),
    (34, 'config', 'open() → read 0x2000 байт', 'call', 0x484e64, 'e8 77 8c f8 ff', 0x40dae0, '-', '0x40dae0=open, 0x40dc70=read, 0x40ddf0=write, 0x4035f0=close', 'код'),
    (35, 'config', 'заголовок файла и версия формата', 'cstr', 0x4bc380, '', 'GUNMETAL Configuration File. av1.93', '0x4aac7c', 'совпал → [0x4aac7c]=1 (конфиг валиден), иначе дефолты', 'код'),
    (36, 'config', 'нет файла → дефолты 320x200x8 + пустые биндинги', 'bytes', 0x44a4ee, 'c7 05 44 d9 83 00 40 01 00 00', "", '0x83d944/0x83d630/0x83d190', '0x44a4ee: mov [0x83d944],0x140 (и 200/8 следом)', 'код'),
    (37, 'config', 'ключи: "KEYCFG\\\\*.KEY" (FindFirstFileA)', 'push', 0x4852e7, '68 74 cc 4b 00', 0x4bcc74, '0x6c2160', 'перечисление профилей, до 15 штук', 'код'),
    (38, 'config', 'call 0x404750 — обёртка FindFirstFileA/FindNextFileA', 'call', 0x4852ec, 'e8 5f f4 f7 ff', 0x404750, '-', '0x84132c/0x841328', 'код'),
    (39, 'config', 'профиль: open("KEYCFG\\\\%s") + read 0x178 байт', 'call', 0x48533a, 'e8 a1 87 f8 ff', 0x40dae0, '0x106c2160+', 'имя профиля (9 байт) сравнивается с конфигом', 'код'),
    (40, 'config', 'сохранение: "GUNMETAL.CFG" в режиме "wb"', 'push', 0x485830, '68 f8 cc 4a 00', 0x4accf8, '-', '0x48583a open + 0x485855 write 0x2000', 'код'),
    (41, 'config', 'вызов config_load из game_preinit', 'call', 0x44a518, 'e8 d3 a7 03 00', 0x484cf0, '0x83d944..', 'до CLI-разбора и до DirectDraw', 'код'),
    (42, 'video', 'DirectDrawCreate(NULL, &0x4c05d8, NULL) через thunk 0x4092ce', 'call', 0x405fd9, 'e8 f0 32 00 00', 0x4092ce, '0x4c05d8', 'ошибка → "Unable to Pre Direct Draw Create"', 'код'),
    (43, 'video', 'IAT: DirectDrawCreate', 'iat', 0x841258, '', ("DirectDrawCreate", "DDRAW.dll"), '-', 'единственный потребитель — thunk 0x4092ce', 'код'),
    (44, 'video', 'QueryInterface(IID_IDirectDraw2, 0x4a1858) + Release старого', 'bytes', 0x406000, 'ff 11', "", '0x4c05d8', 'интерфейс IDirectDraw2 (методы +0x50/+0x54)', 'код'),
    (45, 'video', 'SetCooperativeLevel(hwnd, 0x53) — EXCLUSIVE|FULLSCREEN|ALLOWREBOOT|ALLOWMODEX', 'bytes', 0x406030, 'ff 51 50', "", '0x4c05d4', '0x53 пушится в 0x406026; hwnd = [0x4bf294]', 'код'),
    (46, 'video', 'EnumDisplayModes(3, 0, 0, cb=0x4065a0) — сбор режимов', 'bytes', 0x40605d, 'ff 51 20', "", '0x4bfd1c, 0x4c05bc', 'каждый режим: 5 dword {w,h,bpp,?,flags}; сортировка по площади', 'код'),
    (47, 'video', 'glide2x.dll (опция /glide, флаг 0x800 в [0x4a6170])', 'cstr', 0x4a61b8, '', 'glide2x.dll', '0x4a6170', 'LoadLibrary + glidelibinit; без 3dfx ветка пропускается', 'код'),
    (48, 'video', 'SetDisplayMode(w,h,bpp,0,0) — IDirectDraw+0x54', 'bytes', 0x406d1c, 'ff 51 54', "", '0x4c05c0..0x4c05d0', '0x140x0xc8x8 идёт особой веткой (/lowres)', 'код'),
    (49, 'video', 'DDSURFACEDESC: dwSize=0x6c, flags=0x21 (CAPS|BACKBUFFERCOUNT)', 'bytes', 0x406d6a, 'c7 44 24 1c 6c 00 00', "", '0x4c05dc', 'первичная поверхность', 'код'),
    (50, 'video', 'ddsCaps=0x218 (PRIMARYSURFACE|COMPLEX|FLIP), BackBufferCount=1', 'bytes', 0x406d7a, 'c7 84 24 84 00 00 00 18 02 00 00', "", '0x4c05dc', 'одна вложенная back-поверхность', 'код'),
    (51, 'video', 'CreateSurface → 0x4c05dc (IDirectDraw+0x18)', 'bytes', 0x406d8d, 'ff 51 18', "", '0x4c05dc', 'ошибка → "Unable to Create Surface"', 'код'),
    (52, 'video', 'GetAttachedSurface(DDSCAPS_BACKBUFFER=4) → 0x4c05e0 (surface+0x30)', 'bytes', 0x406db9, 'ff 51 30', "", '0x4c05e0', 'back buffer для кадра', 'код'),
    (53, 'video', 'Lock(back buffer) (surface+0x64), pitch → стек', 'bytes', 0x406a11, 'ff 51 64', "", '0x4c05e0', '0x4069f0: Lock/retry на DDERR_WASSTILLDRAWING', 'код'),
    (54, 'video', 'копирование софт-кадра 0x4c05e8 в back buffer + Unlock(+0x80)', 'bytes', 0x406a6a, 'ff 91 80 00 00 00', "", '0x4c05e8/0x4c05e0', '0x40c580 = построчный блит; 0x4c05d0&0x100 = удвоение строк', 'код'),
    (55, 'video', 'Flip(0,0) (surface+0x2c) с retry/restore', 'bytes', 0x4069ad, 'ff 51 2c', "", '0x4c05dc', 'DDERR_SURFACELOST 0x8876021c → Restore(+0x6c)', 'код'),
    (56, 'video', 'софт-кадр: [0x4c05e8] = malloc(w*h*bpp/8)', 'bytes', 0x406c4a, 'e8 21 bb ff ff', "", '0x4c05e8', '0x402770 — аллокатор; при 8bpp w*h, иначе w*h*4', 'код'),
    (57, 'audio', 'game_preinit → 0x40c420 → 0x404170 → 0x405cd0(hwnd)', 'call', 0x404176, 'e8 55 1b 00 00', 0x405cd0, '0x4bfd08/0x4bfd00', 'только если звук разрешён ([0x4aa384] != 0)', 'код'),
    (58, 'audio', 'DirectSoundCreate(NULL, &0x4bfd08, NULL) через thunk 0x4092c8', 'call', 0x405cdc, 'e8 e7 35 00 00', 0x4092c8, '0x4bfd08', 'ошибка → "Unable to directsoundcreate"', 'код'),
    (59, 'audio', 'IAT: DirectSoundCreate', 'iat', 0x841268, '', ("DirectSoundCreate", "DSOUND.dll"), '-', 'thunk 0x4092c8', 'код'),
    (60, 'audio', 'SetCooperativeLevel(hwnd, 3=DSSCL_EXCLUSIVE) (DS+0x18)', 'bytes', 0x405cfd, 'ff 51 18', "", '-', '0x6a 03 в 0x405cf7', 'код'),
    (61, 'audio', 'DSBUFFERDESC: flags=0x4001 (PRIMARYBUFFER), primary buffer → 0x4bfd0c', 'bytes', 0x405d40, 'c7 44 24 28 01 40 00 00', "", '0x4bfd0c', 'dwSize=0x14', 'код'),
    (62, 'audio', 'CreateSoundBuffer(primary) (DS+0x0c)', 'bytes', 0x405d4b, 'ff 51 0c', "", '0x4bfd0c', 'ошибка → "Unable to createsoundbuffer"', 'код'),
    (63, 'audio', 'WAVEFORMATEX: 32000 Гц, 2 канала, 16 бит, blockAlign=4, 128000 Б/с', 'bytes', 0x405db1, 'ff 51 38', "", '0x4bfd0c', 'сначала GetFormat(+0x14), потом SetFormat(+0x38) под себя', 'код'),
    (64, 'audio', 'стрим-буфер: DSBUFFERDESC flags=0x101e0, 0x10000 байт → 0x4bfd04', 'bytes', 0x405e69, 'ff 52 0c', "", '0x4bfd04', 'второй IDirectSound 0x4bfd00 (тоже coop 3)', 'код'),
    (65, 'audio', 'SetCurrentPosition(0)(+0x34), SetVolume(0)(+0x3c), Play(0,0,1) — зациклено', 'bytes', 0x405ebb, 'ff 52 30', "", '0x4bfd04', 'поток играет непрерывно, миксер пишет в Lock-область', 'код'),
    (66, 'audio', 'Lock(stream, 0, 0x10000) (buffer+0x2c) → GetCurrentPosition(+0x10)', 'bytes', 0x405f5c, 'ff 51 2c', "", '0x4bfd04', 'два региона (wrap)', 'код'),
    (67, 'audio', 'call 0x4092e0 — ассемблерный миксер (adpcmseg/mixasm*)', 'call', 0x405f8a, 'e8 51 33 00 00', 0x4092e0, '0x83cb80', 'пишет 16-бит стерео 32 кГц', 'код'),
    (68, 'audio', 'Unlock(buffer+0x4c) — конец порции', 'bytes', 0x405fae, 'ff 52 4c', "", '0x4bfd04', '0x405ed0 = shutdown: Stop(+0x48) + Release', 'код'),
    (69, 'audio', 'таблица подмен сэмплов "sounds\\\\sounds.sub" (0x40b510)', 'cstr', 0x4abb24, '', 'sounds\\sounds.sub', '0x4cf6b8', 'до 150 записей; ошибка не фатальна', 'код'),
    (70, 'input', 'DirectInputCreateA(hInst, 0x500, &0x4bfcf0, 0) через thunk 0x408fe8', 'call', 0x404b4a, 'e8 99 44 00 00', 0x408fe8, '0x4bfcf0', 'ошибка → "Direct Input Create failed"', 'код'),
    (71, 'input', 'IAT: DirectInputCreateA', 'iat', 0x841260, '', ("DirectInputCreateA", "DINPUT.dll"), '-', 'thunk 0x408fe8', 'код'),
    (72, 'input', 'CreateDevice(GUID_SysKeyboard, &0x4bfcf4) (DI+0x0c)', 'bytes', 0x404b72, 'ff 51 0c', "", '0x4bfcf4', 'клавиатура', 'код'),
    (73, 'input', 'SetDataFormat(c_dfDIKeyboard) (device+0x2c), формат 0x408fd0', 'bytes', 0x404b92, 'ff 52 2c', "", '0x4c05d4', '0x404b8a: push 0x408fd0', 'код'),
    (74, 'input', 'SetCooperativeLevel(hwnd, 6=FOREGROUND|NONEXCLUSIVE) (device+0x34)', 'bytes', 0x404bb0, 'ff 51 34', "", '0x4bfcf4', '0x6a 06 в 0x404baa; фон → Acquire/Unacquire по WM_ACTIVATE', 'код'),
    (75, 'input', 'SetProperty(DIPROP_BUFFERSIZE, 0x20) (device+0x18)', 'bytes', 0x404bf2, 'ff 52 18', "", '0x4bfcf4', 'буфер событий 32', 'код'),
    (76, 'input', 'CreateDevice(GUID_SysMouse, &0x4bfcf8) (DI+0x0c)', 'bytes', 0x404c20, 'ff 52 0c', "", '0x4bfcf8', 'если не /ignoremouse ([0x4bfcc8]==0)', 'код'),
    (77, 'input', 'SetDataFormat мыши = 0x407fb0 (device+0x2c)', 'bytes', 0x404c40, 'ff 51 2c', "", '0x4bfcf8', 'DIMOUSESTATE', 'код'),
    (78, 'input', 'SetCooperativeLevel мыши (5=FOREGROUND|EXCLUSIVE) (device+0x34)', 'bytes', 0x404c5e, 'ff 52 34', "", '0x4bfcf8', '0x6a 05 в 0x404c58; [0x4bfccc]=1', 'код'),
    (79, 'input', 'EnumDevices(DI8DEVCLASS_GAMECTRL=4, cb=0x404d00, 0, ATTACHEDONLY=1)', 'bytes', 0x404ce9, 'ff 51 10', "", '0x4bfca0, 0x4bfca8', 'до 8 устройств; /ignorejoystick → [0x4bfc94]', 'код'),
    (80, 'input', 'callback 0x404d00: CreateDevice(GUID_Joystick) → 0x4bfca8[i] (DI+0x0c)', 'bytes', 0x404d35, 'ff 51 0c', "", '0x4bfca8', 'QI на IDirectInputDevice2 (0x4a13b8), SetDataFormat(0x4092b0)', 'код'),
    (81, 'input', 'опрос клавиатуры: GetDeviceData(+0x28) в 0x405450', 'bytes', 0x405484, 'ff 51 28', "", '0x4bf2a8', '0x405450 — клавиатура/мышь; джойстик — 0x404880 (оси 0x50 байт)', 'код'),
    (82, 'input', 'WM_ACTIVATE → Acquire(+0x1c)/Unacquire(+0x20) всех устройств', 'bytes', 0x4059c0, '8b 44 24 08 56 85 c0', "", '0x4a598c, 0x4bfcf4/0x4bfcf8/0x4bfca8', '0x4059c0 вызывается из 0x4040e3', 'код'),
    (83, 'palette', 'загрузка FRONTEND/GAMEPAL.BMP → 0x83d320 (0x47efe0, 768 байт RGB>>2)', 'call', 0x44af88, 'e8 53 40 03 00', 0x47efe0, '0x83d320', '6-битные компоненты; ошибка → "Unable to load game palette"', 'код'),
    (84, 'palette', 'имена в bigfile: "frontend" + "gamepal"', 'cstr', 0x4abbf8, '', 'gamepal', '-', '0x4abbec = "frontend"; файл читается через .sbc-контейнер', 'код'),
    (85, 'palette', '[0x83d2c4] = &0x83d320 — текущая палитра', 'bytes', 0x44afb3, 'c7 05 c4 d2 83 00 20 d3 83 00', "", '0x83d2c4', 'переключается на waterpal/фейды', 'код'),
    (86, 'palette', 'FRONTEND/WATERPAL.BMP → 0x83d640, при ошибке копия gamepal', 'cstr', 0x4abbac, '', 'waterpal', '0x83d640', '0x41edd3(0x83d320,0x83d640) = копия 768 байт', 'код'),
    (87, 'palette', 'LOOK16: 256 dword в 0x83cb80 (4 уровня затенения в упаковке)', 'bytes', 0x44b017, '8b 9b 40 6e 4a 00', "", '0x83cb80/0x83cf80', 'источники 0x4a6a40 (R), 0x4a6e40 (B), 0x4a7240 (G), шаг 0x10, константа 0x2aa554aa', 'код'),
    (88, 'palette', 'LOOK16: 256 word RGB555 в 0x83cf80 + конец цикла по 0x83cf80', 'bytes', 0x44b06b, '81 ff 80 cf 83 00', "", '0x83cf80', '((g&0x3e)<<10)|(r<<5)|(b>>1)', 'код'),
    (89, 'palette', 'CreatePalette(DDPCAPS_8BIT|ALLOW256=0x44) → 0x4c05e4 (DD+0x14)', 'bytes', 0x4072b3, 'ff 52 14', "", '0x4c05e4', '0x407220(palette768): лениво, при первой смене палитры', 'код'),
    (90, 'palette', 'SetPalette(primary, palette) (surface+0x7c) + [0x4c05f8]=16', 'bytes', 0x4072c9, 'ff 52 7c', "", '0x4c05f8', 'первые 16 кадров палитра переустанавливается в present', 'код'),
    (91, 'timer', 'CreateThread(0, 10000, 0x4074e0, 0, 0, &0x4c0604) → [0x4c0600]', 'iatcall', 0x4074b5, 'ff 15 40 13 84 00', ("CreateThread", "KERNEL32.dll"), '0x4c0600/0x4c0604', '0x4074a9: push 0x4074e0; ошибка → "CreateThread Error"', 'код'),
    (92, 'timer', 'SetThreadPriority(handle, 0xf=THREAD_PRIORITY_TIME_CRITICAL)', 'iat', 0x84133c, '', ("SetThreadPriority", "KERNEL32.dll"), '0x4c0600', 'тик важнее рендера', 'код'),
    (93, 'timer', 'цикл потока: t=0x40db00(); Sleep(11 - (t-t0))', 'bytes', 0x407504, 'b9 0b 00 00 00', "", '0x4c0608', '0x4c0608=1 — выход; Sleep = 0x841344 (0x407519 call ebx)', 'код'),
    (94, 'timer', '0x40db00 — миллисекунды из GetLocalTime', 'call', 0x4074e4, 'e8 17 66 00 00', 0x40db00, '0x4d2df0/0x4d2df4', 'нет timeGetTime/QueryPerformanceCounter в импортах — только KERNEL32 time', 'код'),
    (95, 'timer', 'call 0x405f30 — подкормка звукового буфера (каждый 2-й тик)', 'call', 0x40751f, 'e8 0c ea ff ff', 0x405f30, '0x4bfd04', 'Lock/микс/Unlock в фоновом потоке', 'код'),
    (96, 'timer', 'call 0x44bdc0 — сервис тика: [0x5a4a74]++, таймеры слотов, 0x490b50', 'call', 0x40752d, 'e8 8e 48 04 00', 0x44bdc0, '0x5a4a74/0x5a49e0', 'глобальный счётчик кадров 91 Гц', 'код'),
    (97, 'timer', 'константа 91: cmp ecx,0x5b в 0x44db80 (часы из 91-Гц счётчика)', 'bytes', 0x44db86, '8b 54 24 04 03 ca', "", '0x5a4ae8/0x5a4ae4', '0x44db80 = секунды/минуты/часы, не сам тик', 'код'),
    (98, 'exit', 'WM_QUIT: [0x4c0608] = 1 — сигнал тик-потоку выйти', 'bytes', 0x407550, 'c7 05 08 06 4c 00 01 00 00 00', "", '0x4c0608', '0x407550 также закрывает лог/файлы', 'код'),
    (99, 'exit', '0x405ed0 — Stop(+0x48) + Release стрим-буфера и обоих IDirectSound', 'bytes', 0x405ee6, 'ff 51 48', "", '0x4bfd04/0x4bfd00', 'лог о незакрытых звуковых объектах ([0x4bfcfc])', 'код'),
    (100, 'exit', '0x406eb0 — Release primary/back/palette + free(0x4c05e8)', 'bytes', 0x406e65, 'ff 51 08', "", '0x4c05dc/0x4c05e4', '0x406e50 закрывает surface chain', 'код'),
    (101, 'exit', '0x405a40 — Unacquire+Release клавиатуры/мыши/джойстиков', 'bytes', 0x405a9e, 'ff 51 20', "", '0x4bfcf4/0x4bfcf8/0x4bfca8', 'порядок: звук → видео → ввод → лог', 'код'),
]
# --------------------------------------------------------------------------
PHASES = {
    "crt": "CRT / точка входа",
    "window": "Окно и WNDCLASS",
    "order": "Порядок подсистем",
    "config": "Конфиг: GUNMETAL.CFG + KEYCFG\\*.KEY",
    "video": "Видео: DirectDraw",
    "audio": "Звук: DirectSound",
    "input": "Ввод: DirectInput",
    "palette": "Палитра и LOOK16",
    "timer": "Таймеры и тик-поток",
    "exit": "Выход",
}


def _hex(text):
    return bytes(int(x, 16) for x in text.split())


def check(pe, steps):
    """Список расхождений между таблицей и PE (пусто — всё сходится)."""
    bad = []
    for (num, phase, title, kind, addr, expect, extra, _g, _e, _s) in steps:
        tag = "#%d %s @0x%x" % (num, kind, addr)
        if kind == "entry":
            if pe.entry_va != addr:
                bad.append("%s: AddressOfEntryPoint=0x%x, ожидался 0x%x"
                           % (tag, pe.entry_va, addr))
            got = pe.read(addr, len(_hex(expect)))
            if got != _hex(expect):
                bad.append("%s: пролог %r != %r" % (tag, got, _hex(expect)))
        elif kind in ("bytes", "call", "push", "iatcall"):
            want = _hex(expect)
            got = pe.read(addr, len(want))
            if got is None:
                bad.append("%s: адрес не отображён в файл" % tag)
                continue
            if got != want:
                bad.append("%s: байты %r != %r" % (tag, got, want))
                continue
            if kind == "call":
                if want[0] != 0xE8:
                    bad.append("%s: не e8, а %02x" % (tag, want[0]))
                    continue
                rel = struct.unpack("<i", want[1:5])[0]
                target = addr + 5 + rel
                if target != extra:
                    bad.append("%s: call → 0x%x, ожидался 0x%x"
                               % (tag, target, extra))
            elif kind == "push":
                if want[0] != 0x68:
                    bad.append("%s: не push imm32, а %02x" % (tag, want[0]))
                    continue
                value = struct.unpack("<I", want[1:5])[0]
                if value != extra:
                    bad.append("%s: push 0x%x, ожидался 0x%x"
                               % (tag, value, extra))
            elif kind == "iatcall":
                if want[:2] != b"\xff\x15":
                    bad.append("%s: не ff 15, а %r" % (tag, want[:2]))
                    continue
                slot = struct.unpack("<I", want[2:6])[0]
                if slot not in pe.imports:
                    bad.append("%s: слота 0x%x нет в таблице импорта"
                               % (tag, slot))
                    continue
                dll, name = pe.imports[slot]
                if not _same_import((name, dll), extra):
                    bad.append("%s: слот 0x%x = %s!%s, ожидался %s!%s"
                               % (tag, slot, dll, name, extra[1], extra[0]))
        elif kind == "iat":
            if addr not in pe.imports:
                bad.append("%s: адреса 0x%x нет в таблице импорта" % (tag, addr))
                continue
            dll, name = pe.imports[addr]
            if not _same_import((name, dll), extra):
                bad.append("%s: %s!%s, ожидался %s!%s"
                           % (tag, dll, name, extra[1], extra[0]))
        elif kind == "cstr":
            got = pe.cstr(addr)
            if got is None or not got.startswith(extra):
                bad.append("%s: строка %r != %r" % (tag, got, extra))
        else:
            bad.append("%s: неизвестный вид проверки" % tag)
    bad.extend(check_imports(pe))
    return bad


def _same_import(got, want):
    name, dll = got
    return name.lower() == want[0].lower() and dll.lower() == want[1].lower()


def check_imports(pe):
    """Отрицательные и «ровно один» проверки по таблице импорта."""
    bad = []
    names = {n.lower() for (_d, n) in pe.imports.values()}
    for n in NEGATIVE_IMPORTS:
        if n.lower() in names:
            bad.append("импорт %s есть, хотя время/звук сделаны вручную" % n)
    for dll, expect in SINGLE_PEER_IMPORTS.items():
        got = sorted(x for (d, x) in pe.imports.values() if d.lower() == dll.lower())
        if got != sorted(expect):
            bad.append("%s: импорты %s, ожидались %s" % (dll, got, expect))
    return bad


# --------------------------------------------------------------------------
# Вывод
# --------------------------------------------------------------------------
def wrap(text, width):
    words, lines, cur = text.split(), [], ""
    for w in words:
        if cur and len(cur) + 1 + len(w) > width:
            lines.append(cur)
            cur = w
        else:
            cur = (cur + " " + w).strip()
    if cur:
        lines.append(cur)
    return lines or [""]


def evidence(row):
    """Человекочитаемое доказательство шага."""
    _num, _ph, _t, kind, addr, expect, extra, _g, _e, _s = row
    if kind == "entry":
        return "точка входа PE + пролог %s" % expect
    if kind in ("bytes", "call", "push", "iatcall"):
        what = {"bytes": "байты", "call": "call →", "push": "push",
                "iatcall": "call через IAT"}[kind]
        if kind == "call":
            return "%s 0x%x (байты %s)" % (what, extra, expect)
        if kind == "push":
            return "%s 0x%x (байты %s)" % (what, extra, expect)
        if kind == "iatcall":
            return "%s %s!%s (байты %s)" % (what, extra[1], extra[0], expect)
        return "%s %s" % (what, expect)
    if kind == "iat":
        return "IAT 0x%x = %s!%s" % (addr, extra[1], extra[0])
    if kind == "cstr":
        return "строка %r" % extra
    return kind


def print_steps(steps, out=sys.stdout):
    phase = None
    for row in steps:
        num, ph, title, kind, addr, _expect, _extra, glob, effect, status = row
        if ph != phase:
            phase = ph
            out.write("\n== %s ==\n" % PHASES.get(ph, ph))
        lines = wrap(title, 74)
        out.write("#%-3d 0x%06x %-7s %s [%s]\n"
                  % (num, addr, kind, lines[0], status))
        for line in lines[1:]:
            out.write("              %-7s %s\n" % ("", line))
        for line in wrap(evidence(row), 96):
            out.write("              %s\n" % line)
        if glob != "-":
            out.write("              глобалы: %s\n" % glob)
        for line in wrap(effect, 96):
            out.write("              → %s\n" % line)
    out.write("\nвсего шагов: %d (код: %d, гипотеза: %d)\n"
              % (len(steps), sum(1 for s in steps if s[9] == "код"),
                 sum(1 for s in steps if s[9] != "код")))


def print_mermaid(steps, out=sys.stdout):
    """Диаграмма последовательности: одна стрелка на фазу (адреса из таблицы)."""
    out.write("```mermaid\nsequenceDiagram\n")
    out.write("    autonumber\n")
    out.write("    participant CRT as CRT 0x401000\n")
    out.write("    participant Win as WinMain 0x404680\n")
    out.write("    participant UI as Окно/USER32\n")
    out.write("    participant CFG as Конфиг\n")
    out.write("    participant DD as DirectDraw\n")
    out.write("    participant DI as DirectInput\n")
    out.write("    participant DS as DirectSound\n")
    out.write("    participant PAL as Палитра\n")
    out.write("    participant T as Тик-поток\n")
    to = {"crt": "CRT", "window": "UI", "config": "CFG", "video": "DD",
          "input": "DI", "audio": "DS", "palette": "PAL", "timer": "T",
          "order": "Win", "exit": "Win"}
    groups = []
    for row in steps:
        ph = row[1]
        if groups and groups[-1][0] == ph:
            groups[-1][1].append(row)
        else:
            groups.append((ph, [row]))
    for ph, rows in groups:
        first, last = rows[0][4], rows[-1][4]
        label = PHASES.get(ph, ph)
        if len(rows) > 1:
            label += " — %d записей, 0x%x..0x%x" % (len(rows), first, last)
        else:
            label += " (0x%x)" % first
        label = label.replace('"', "'")
        out.write("    Win->>%s: %s\n" % (to.get(ph, "Win"), label))
    out.write("    Win->>Win: цикл 0x4046be (docs/MAINLOOP.md)\n")
    out.write("    Win-->>CRT: WM_QUIT → shutdown 0x405ed0/0x406eb0/0x405a40\n")
    out.write("```\n")


# --------------------------------------------------------------------------
# Синтетический PE для --self-test
# --------------------------------------------------------------------------
SYNTH_TEXT_VA = 0x401000
SYNTH_IDATA_VA = 0x402000
SYNTH_STRING_VA = 0x402300
SYNTH_IAT_SLOT = 0x402060


def make_synthetic():
    """Маленький PE32: .text с тремя инструкциями и .idata с тремя импортами."""
    data = bytearray(0xA00)
    struct.pack_into("<H", data, 0x00, 0x5A4D)          # MZ
    struct.pack_into("<I", data, 0x3C, 0x40)           # e_lfanew
    data[0x40:0x44] = b"PE\0\0"
    struct.pack_into("<HHIIIHH", data, 0x44, 0x14C, 2, 0, 0, 0, 0xE0, 0x0102)
    opt = 0x58
    struct.pack_into("<H", data, opt, 0x10B)           # PE32
    struct.pack_into("<I", data, opt + 16, 0x1000)     # AddressOfEntryPoint
    struct.pack_into("<I", data, opt + 28, 0x400000)   # ImageBase
    struct.pack_into("<I", data, opt + 32, 0x1000)     # SectionAlignment
    struct.pack_into("<I", data, opt + 36, 0x200)      # FileAlignment
    struct.pack_into("<I", data, opt + 56, 0x4000)     # SizeOfImage
    struct.pack_into("<I", data, opt + 60, 0x400)      # SizeOfHeaders
    struct.pack_into("<H", data, opt + 68, 2)          # Subsystem GUI
    struct.pack_into("<I", data, opt + 92, 16)         # NumberOfRvaAndSizes
    struct.pack_into("<II", data, opt + 96 + 8, 0x2000, 0x60)  # импорты
    sh = opt + 0xE0
    # .text: RVA 0x1000, raw 0x200 @ 0x200
    struct.pack_into("<8sIIII", data, sh, b".text", 0x200, 0x1000, 0x200, 0x200)
    # .idata: RVA 0x2000, raw 0x400 @ 0x400
    struct.pack_into("<8sIIII", data, sh + 40, b".idata", 0x400, 0x2000, 0x400, 0x400)

    def put(va, blob):
        base_va, base_off = ((SYNTH_TEXT_VA, 0x200) if va < SYNTH_IDATA_VA
                             else (SYNTH_IDATA_VA, 0x400))
        off = base_off + (va - base_va)
        data[off:off + len(blob)] = blob

    put(0x401000, bytes.fromhex("55 8b ec"))
    put(0x401010, bytes.fromhex("e8 eb 00 00 00"))            # call 0x401100
    put(0x401020, struct.pack("<BI", 0x68, SYNTH_STRING_VA))  # push imm32
    put(0x401030, b"\xff\x15" + struct.pack("<I", SYNTH_IAT_SLOT))
    # в PE все ссылки внутри импорта — RVA (не VA)
    # ILT 0x2040/48/50, IAT 0x2060/64/68, имена DLL 0x2100/10/20
    struct.pack_into("<IIIII", data, 0x400, 0x2040, 0, 0, 0x2100, 0x2060)
    struct.pack_into("<IIIII", data, 0x414, 0x2048, 0, 0, 0x2110, 0x2064)
    struct.pack_into("<IIIII", data, 0x428, 0x2050, 0, 0, 0x2120, 0x2068)
    struct.pack_into("<IIIII", data, 0x43C, 0, 0, 0, 0, 0)      # terminator
    for i, hint in enumerate((0x213E, 0x215E, 0x217E)):
        struct.pack_into("<II", data, 0x440 + 4 * i, hint, 0)  # ILT
        struct.pack_into("<II", data, 0x460 + 4 * i, hint, 0)  # IAT
    put(0x402100, b"DDRAW.dll\0")
    put(0x402110, b"DINPUT.dll\0")
    put(0x402120, b"DSOUND.dll\0")
    put(0x40213E, b"\0\0DirectDrawCreate\0")
    put(0x40215E, b"\0\0DirectInputCreateA\0")
    put(0x40217E, b"\0\0DirectSoundCreate\0")
    put(SYNTH_STRING_VA, b"FSDSClass\0")
    return bytes(data)


SYNTH_STEPS = [
    (1, "crt", "синтетическая точка входа", "entry", 0x401000, "55 8b ec", "", "-", "-", "код"),
    (2, "crt", "синтетический call", "call", 0x401010, "e8 eb 00 00 00", 0x401100, "-", "-", "код"),
    (3, "window", "синтетический push строки", "push", 0x401020, "68 00 23 40 00", 0x402300, "-", "-", "код"),
    (4, "video", "синтетический вызов через IAT", "iatcall", 0x401030, "ff 15 60 20 40 00",
     ("DirectDrawCreate", "DDRAW.dll"), "-", "-", "код"),
    (5, "video", "синтетический слот IAT", "iat", 0x402060, "", ("DirectDrawCreate", "DDRAW.dll"),
     "-", "-", "код"),
    (6, "config", "синтетическая строка", "cstr", 0x402300, "", "FSDSClass", "-", "-", "код"),
]


def self_test(out=sys.stdout):
    fails = []

    def probe(name, data, steps, want_ok, expect_words=()):
        try:
            bad = check(PE(data), steps)
        except Exception as exc:                      # noqa: BLE001 - отчёт теста
            bad = ["исключение: %s" % exc]
        ok = not bad
        if ok != want_ok:
            fails.append(name)
        out.write("  [%s] %-44s проблем: %d\n" % ("ok" if ok == want_ok else "FAIL",
                                                 name, len(bad)))
        if want_ok and bad:
            for b in bad[:4]:
                out.write("        %s\n" % b)
        if not want_ok:
            joined = " ".join(bad)
            for word in expect_words:
                if word not in joined:
                    out.write("        (ожидалось упоминание %r)\n" % word)

    out.write("self-test: синтетический PE\n")
    synth = make_synthetic()
    probe("чистый синтетический PE", synth, SYNTH_STEPS, True)
    marker = PE(synth).va2off(0x401012)
    broken = bytearray(synth)
    broken[marker] ^= 0xFF
    probe("испорченный байт call", bytes(broken), SYNTH_STEPS, False, ["байты"])
    broken = bytearray(synth)
    idx = broken.find(b"DDRAW.dll")
    broken[idx + 5] = ord("X")
    probe("испорченное имя DLL", bytes(broken), SYNTH_STEPS, False, ["DDRAW"])
    broken = bytearray(synth)
    broken[PE(synth).va2off(SYNTH_IAT_SLOT):PE(synth).va2off(SYNTH_IAT_SLOT) + 4] = \
        struct.pack("<I", 0x2160)                      # IAT → имя DirectInput
    probe("подменённая запись IAT", bytes(broken), SYNTH_STEPS, False, ["DirectDraw"])
    broken = bytearray(synth)
    broken[PE(synth).va2off(SYNTH_STRING_VA)] = ord("Q")
    probe("испорченная строка", bytes(broken), SYNTH_STEPS, False, ["строка"])
    broken = bytearray(synth)
    broken[0x3C] = 0x00
    probe("битый заголовок (нет e_lfanew)", bytes(broken), SYNTH_STEPS, False,
          ["исключение"])

    out.write("self-test: реальный gundx.exe (%s)\n" % EXE)
    if not os.path.exists(EXE):
        out.write("  [skip] файла нет — проверяется только синтетика\n")
    else:
        with open(EXE, "rb") as fh:
            real = PE(fh.read())
        bad = check(real, STEPS)
        out.write("  [%s] %-44s проблем: %d\n"
                  % ("ok" if not bad else "FAIL", "таблица INIT по PE", len(bad)))
        out.write("        секций %d, импортов %d, entry 0x%x, подсистема %d\n"
                  % (len(real.sections), len(real.imports), real.entry_va,
                     real.subsystem))
        for b in bad[:10]:
            out.write("        %s\n" % b)
        if bad:
            fails.append("real exe")
    out.write("self-test: %s\n" % ("OK" if not fails else "FAIL " + ", ".join(fails)))
    return 0 if not fails else 1


def main(argv):
    if "--self-test" in argv:
        return self_test()
    if "--mermaid" in argv:
        print_mermaid(STEPS)
        return 0
    if "--verify" in argv:
        if not os.path.exists(EXE):
            sys.stderr.write("нет файла %s (задайте GUNMETAL_EXE)\n" % EXE)
            return 2
        with open(EXE, "rb") as fh:
            pe = PE(fh.read())
        bad = check(pe, STEPS)
        print_steps(STEPS)
        sys.stdout.write("\n--verify: записей %d, импортов в PE %d, entry 0x%x\n"
                         % (len(STEPS), len(pe.imports), pe.entry_va))
        if bad:
            for b in bad:
                sys.stdout.write("  РАСХОЖДЕНИЕ %s\n" % b)
            sys.stdout.write("--verify: FAIL (%d)\n" % len(bad))
            return 1
        sys.stdout.write("--verify: OK\n")
        return 0
    print_steps(STEPS)
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main(sys.argv[1:]))
    except FileNotFoundError as exc:
        sys.stderr.write("%s\n" % exc)
        raise SystemExit(2)
