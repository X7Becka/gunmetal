#!/usr/bin/env python3
"""Emulate the original player integrator of ``gundx.exe``.

Why an emulator: the retention coefficient ``k`` (``player+0x81bfb8``) is read
in exactly one place -- the horizontal integrator loop of ``FUN_00443690``
(``0x444476``..``0x4444d5``).  Reading the listing proves the *shape* of the
formula (``v <- v*k + dv``), but it cannot prove the order of the floating
point operations, the number of iterations or the way the displacement is
accumulated.  Running the original bytes does.

The loop (verbatim, ``objdump -D -M intel`` of the shipped binary)::

    0x444476  mov  eax, esi                ; eax = param_2 (ticks of the batch)
    0x444478  fld  qword ptr [ebx+0x81bd90]   ; vx
    0x44447e  fmul qword ptr [ebx+0x81bfb8]   ; vx * k
    0x444484  dec  eax
    0x444485  fadd qword ptr [esp+0x30]      ; + dvx
    0x444489  fstp qword ptr [ebx+0x81bd90]   ; vx = vx*k + dvx
    0x44448f  fld  qword ptr [ebx+0x81bfb8]   ; k
    0x444495  fmul qword ptr [ebx+0x81bd98]   ; vy * k
    0x44449b  fadd qword ptr [esp+0x28]      ; + dvy
    0x44449f  fld  st(0)
    0x4444a1  fxch st(3)
    0x4444a3  fadd st, st(1)
    0x4444a5  fxch st(3)
    0x4444a7  fstp qword ptr [ebx+0x81bd98]   ; vy = vy*k + dvy
    0x4444ad  fld  qword ptr [ebx+0x81be28]   ; vz
    0x4444b3  fmul qword ptr ds:0x4a2368]     ; vz * -0.85
    0x4444b9  fxch st(2)
    0x4444bb  fadd qword ptr [ebx+0x81bd90]   ; displacement_x += vx
    0x4444c1  fxch st(1)
    0x4444c3  fxch st(2)
    0x4444c5  fsubr st, st(4)                 ; 0.15*pitch + 0.85*vz
    0x4444c7  fxch st(1)
    0x4444c9  fxch st(2)
    0x4444cb  fxch st(1)
    0x4444cd  fstp qword ptr [ebx+0x81be28]   ; vz = 0.85*vz + 0.15*pitch
    0x4444d3  fstp st(0)
    0x4444d5  jne  0x444478

So one batch of ``param_2`` ticks does

    vx' = vx*k + dvx          vy' = vy*k + dvy          vz' = 0.85*vz + 0.15*pitch
    displacement_x += vx'    displacement_y += vy'

with ``dvx``/``dvy`` already divided by ``param_2`` by the caller
(``0x44444c``/``0x44445e``).  The ``0.15*pitch`` term is prepared before the
loop: ``0x444468`` multiplies the clamped look angle (``+-pi/12``) by ``0.15``
(``0x4a2360``); the vertical retention ``-0.85`` is the constant ``0x4a2368``.

This module runs those bytes and checks the result against a closed form:

    v(n)      = k^n * v0 + dv * (1 - k^n) / (1 - k)
    disp(n)   = v0 * k*(1 - k^n)/(1 - k) + dv * (n - k*(1 - k^n)/(1 - k)) / (1 - k)

Usage::

    python3 tools/re/player_integrator.py             # table + closed form
    python3 tools/re/player_integrator.py --json      # machine readable
    python3 tools/re/player_integrator.py --selftest   # exit code only

The game is only read.
"""

from __future__ import annotations

import argparse
import json
import math
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import exelib  # noqa: E402  (path set above)

# --- addresses of the original ------------------------------------------------
LOOP_VA = 0x444476          # `mov eax, esi`
AFTER_LOOP_VA = 0x4444D7    # `fstp qword ptr [esp+0x40]` (first instruction
                            #  after the `jne`)
STOP_VA = 0x4444DF          # `fstp st(0)` -- the accumulators are stored

# player fields (EBX is the player struct, the loop uses EBX+offset)
OFF_VX = 0x81BD90          # horizontal velocity, world units per tick
OFF_VY = 0x81BD98
OFF_VZ = 0x81BE28          # vertical channel
OFF_K = 0x81BFB8           # retention coefficient of the chassis

# stack slots of the caller frame
STACK_DVY = 0x28           # [esp+0x28] = dvy
STACK_DVX = 0x30           # [esp+0x30] = dvx
STACK_DISP_Y = 0x38        # [esp+0x38] = displacement_y accumulator
STACK_DISP_X = 0x40        # [esp+0x40] = displacement_x accumulator

# constants read by the loop
CONST_MINUS_085 = 0x4A2368  # -0.85 (vertical retention)
CONST_015 = 0x4A2360        # 0.15 (the look angle is scaled by it)

MEM_SIZE = 0x1000000        # 16 MiB, enough for the player block at 0x81bd90
PLAYER_BASE = 0x0           # EBX = 0 -> the player block sits at its offsets
STACK_TOP = 0x7FF000


class EmuError(RuntimeError):
    pass


class Fpu:
    """The x87 register stack (8 slots, ST(0) is the top).

    Only double precision is modelled: the original keeps intermediates in the
    80-bit extended format, this module rounds every operation to binary64.
    The difference is at most one ulp and only for inexact products, so the
    test vectors below use dyadic values where both models agree bit for bit.
    """

    def __init__(self) -> None:
        self.st: list[float] = []

    def push(self, value: float) -> None:
        if len(self.st) == 8:
            self.st.pop()          # ST(7) is overwritten by a push
        self.st.insert(0, value)

    def pop(self) -> float:
        if not self.st:
            raise EmuError("x87 stack underflow")
        return self.st.pop(0)

    def top(self) -> float:
        if not self.st:
            raise EmuError("x87 stack underflow")
        return self.st[0]

    def fxch(self, index: int) -> None:
        if index >= len(self.st):
            raise EmuError("fxch st(%d) on a short stack" % index)
        self.st[0], self.st[index] = self.st[index], self.st[0]


def load_image() -> bytearray:
    """Map the PE sections of ``gundx.exe`` into a flat memory image.

    The virtual addresses are the ones the game uses, so ``memory[0x4a2368]``
    is the ``-0.85`` constant and ``memory[0x444476]`` is the first
    instruction of the integrator loop.
    """
    data = exelib.data()
    mem = bytearray(MEM_SIZE)
    for name, vma, _vsize, rawsize, rawoff in exelib.virtual_sections():
        if rawsize:
            chunk = data[rawoff:rawoff + rawsize]
            mem[vma:vma + len(chunk)] = chunk
    return mem


class Cpu:
    """Just enough x86-32 to run the integrator loop of ``gundx.exe``."""

    def __init__(self, memory: bytearray) -> None:
        self.mem = memory
        self.regs = {"eax": 0, "ebx": 0, "ecx": 0, "edx": 0,
                     "esp": 0, "ebp": 0, "esi": 0, "edi": 0}
        self.eflags_zf = 0
        self.fpu = Fpu()
        self.eip = 0

    # --- memory ---------------------------------------------------------
    def rd32(self, addr: int) -> int:
        return int.from_bytes(self.mem[addr:addr + 4], "little")

    def wr32(self, addr: int, value: int) -> None:
        self.mem[addr:addr + 4] = (value & 0xFFFFFFFF).to_bytes(4, "little")

    def rd64(self, addr: int) -> float:
        (value,) = struct.unpack_from("<d", self.mem, addr)
        return value

    def wr64(self, addr: int, value: float) -> None:
        struct.pack_into("<d", self.mem, addr, value)

    # --- modrm ----------------------------------------------------------
    def _modrm(self, addr: int):
        """Decode the ModRM byte at ``addr``.

        Returns ``(kind, reg, rm, length)``; ``kind`` is ``"mem"`` or ``"reg"``.
        For ``"mem"`` the third value is the effective address.
        """
        modrm = self.mem[addr]
        mod = modrm >> 6
        reg = (modrm >> 3) & 7
        rm = modrm & 7
        length = 1
        if mod == 3:
            return "reg", reg, rm, length

        base = None
        index = None
        scale = 1
        disp = 0
        if rm == 4:  # SIB
            sib = self.mem[addr + length]
            length += 1
            scale = 1 << (sib >> 6)
            idx = (sib >> 3) & 7
            base_reg = sib & 7
            if idx != 4:
                index = idx
            if base_reg == 5 and mod == 0:
                disp = struct.unpack_from("<i", self.mem, addr + length)[0]
                length += 4
            else:
                base = base_reg
        elif rm == 5 and mod == 0:
            disp = struct.unpack_from("<i", self.mem, addr + length)[0]
            length += 4
        else:
            base = rm

        if base is None:
            ea = 0
        else:
            ea = self.regs[("eax", "ecx", "edx", "ebx",
                            "esp", "ebp", "esi", "edi")[base]]
        if index is not None:
            ea += self.regs[("eax", "ecx", "edx", "ebx",
                             "esp", "ebp", "esi", "edi")[index]] * scale
        if mod == 1:
            disp = struct.unpack_from("<b", self.mem, addr + length)[0]
            length += 1
        elif mod == 2:
            disp = struct.unpack_from("<i", self.mem, addr + length)[0]
            length += 4
        return "mem", reg, (ea + disp) & 0xFFFFFFFF, length

    # --- run ------------------------------------------------------------
    def run(self, start: int, stop: int, limit: int = 1_000_000) -> None:
        self.eip = start
        steps = 0
        while self.eip != stop:
            self.step()
            steps += 1
            if steps > limit:
                raise EmuError("step limit at %08x" % self.eip)

    def step(self) -> None:
        eip = self.eip
        op = self.mem[eip]
        n = 1
        fpu = self.fpu

        if op == 0x8B:  # mov r32, r/m32
            kind, reg, where, mlen = self._modrm(eip + 1)
            n += mlen
            if kind != "reg":
                raise EmuError("mov with a memory source at %08x" % eip)
            self._set_reg(reg, self.regs[_REG[where]])
        elif op == 0x89:  # mov r/m32, r32
            kind, reg, where, mlen = self._modrm(eip + 1)
            n += mlen
            if kind != "reg":
                raise EmuError("mov with a memory destination at %08x" % eip)
            self._set_reg(where, self.regs[_REG[reg]])
        elif op == 0x48:  # dec eax
            value = (self.regs["eax"] - 1) & 0xFFFFFFFF
            self.regs["eax"] = value
            self.eflags_zf = 1 if value == 0 else 0
        elif op == 0x75:  # jne rel8
            rel = struct.unpack_from("<b", self.mem, eip + 1)[0]
            if not self.eflags_zf:
                self.eip = (eip + 2 + rel) & 0xFFFFFFFF
                return
            n += 1
        elif op == 0xDD:
            kind, reg, where, mlen = self._modrm(eip + 1)
            n += mlen
            if kind == "reg":
                # DD D0+i FST st(i), DD D8+i FSTP st(i), DD C0+i FLD st(i)
                if reg == 0:                      # FLD st(i)
                    fpu.push(fpu.st[where])
                elif reg == 3:                    # FSTP st(i)
                    fpu.st.pop(where)
                else:
                    raise EmuError("unsupported DD /%d at %08x" % (reg, eip))
            elif reg == 0:                        # FLD m64fp
                fpu.push(self.rd64(where))
            elif reg == 3:                        # FSTP m64fp
                self.wr64(where, fpu.pop())
            else:
                raise EmuError("unsupported DD /%d at %08x" % (reg, eip))
        elif op == 0xD9:
            kind, reg, where, mlen = self._modrm(eip + 1)
            n += mlen
            if kind != "reg":
                raise EmuError("unsupported D9 memory form at %08x" % eip)
            if reg == 0:                          # FLD st(i)
                fpu.push(fpu.st[where])
            elif reg == 1:                        # FXCH st(i)
                fpu.fxch(where)
            else:
                raise EmuError("unsupported D9 /%d at %08x" % (reg, eip))
        elif op == 0xDC:
            kind, reg, where, mlen = self._modrm(eip + 1)
            n += mlen
            if kind != "mem":
                raise EmuError("unsupported DC register form at %08x" % eip)
            value = self.rd64(where)
            if reg == 0:                          # FADD m64fp
                fpu.st[0] = fpu.top() + value
            elif reg == 1:                        # FMUL m64fp
                fpu.st[0] = fpu.top() * value
            else:
                raise EmuError("unsupported DC /%d at %08x" % (reg, eip))
        elif op == 0xD8:
            kind, reg, where, mlen = self._modrm(eip + 1)
            n += mlen
            if kind != "reg":
                raise EmuError("unsupported D8 memory form at %08x" % eip)
            if reg == 0:                          # FADD st(0), st(i)
                fpu.st[0] = fpu.top() + fpu.st[where]
            elif reg == 5:                        # FSUBR st(0), st(i)
                fpu.st[0] = fpu.st[where] - fpu.top()
            else:
                raise EmuError("unsupported D8 /%d at %08x" % (reg, eip))
        else:
            raise EmuError("opcode %02x at %08x" % (op, eip))

        self.eip = (eip + n) & 0xFFFFFFFF

    def _set_reg(self, index: int, value: int) -> None:
        self.regs[_REG[index]] = value & 0xFFFFFFFF


_REG = ("eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi")


class Result:
    def __init__(self, vx, vy, vz, disp_x, disp_y):
        self.vx = vx
        self.vy = vy
        self.vz = vz
        self.displacement_x = disp_x
        self.displacement_y = disp_y

    def as_dict(self) -> dict:
        return {
            "vx": self.vx,
            "vy": self.vy,
            "vz": self.vz,
            "displacement_x": self.displacement_x,
            "displacement_y": self.displacement_y,
        }


def check_image(memory: bytearray) -> list[str]:
    """Guard against emulating something else than the original loop.

    The first 22 bytes of the loop are listed verbatim in the module
    docstring; the two constants are read by the loop itself.
    """
    problems = []
    expected = bytes.fromhex(
        "8bc6"                      # mov eax, esi
        "dd8390bd8100"              # fld  qword ptr [ebx+0x81bd90]
        "dc8bb8bf8100"              # fmul qword ptr [ebx+0x81bfb8]
        "48"                        # dec  eax
        "dc442430"                  # fadd qword ptr [esp+0x30]
        "dd9b90bd8100")             # fstp qword ptr [ebx+0x81bd90]
    got = bytes(memory[LOOP_VA:LOOP_VA + len(expected)])
    if got != expected:
        problems.append("loop bytes at 0x%08x are %s" % (LOOP_VA, got.hex(" ")))
    for va, want in ((CONST_MINUS_085, -0.85), (CONST_015, 0.15)):
        (value,) = struct.unpack_from("<d", memory, va)
        if value != want:
            problems.append("constant 0x%08x is %r, expected %r" % (va, value, want))
    return problems


def emulate(vx0: float, vy0: float, vz0: float, k: float,
            dvx: float, dvy: float, pitch: float, ticks: int) -> Result:
    """Run the original loop and return the resulting state."""
    image = load_image()
    cpu = Cpu(image)

    # The player block: EBX points at it, the loop uses EBX+offset.
    cpu.regs["ebx"] = PLAYER_BASE
    cpu.wr64(PLAYER_BASE + OFF_VX, vx0)
    cpu.wr64(PLAYER_BASE + OFF_VY, vy0)
    cpu.wr64(PLAYER_BASE + OFF_VZ, vz0)
    cpu.wr64(PLAYER_BASE + OFF_K, k)

    # The caller frame: the two deltas and the two displacement accumulators.
    esp = STACK_TOP
    cpu.regs["esp"] = esp
    cpu.wr64(esp + STACK_DVY, dvy)
    cpu.wr64(esp + STACK_DVX, dvx)
    cpu.wr64(esp + STACK_DISP_Y, 0.0)
    cpu.wr64(esp + STACK_DISP_X, 0.0)

    # The x87 stack the caller leaves behind: ST(0) = displacement_x,
    # ST(1) = displacement_y, ST(2) = 0.15*pitch (prepared at 0x444468 with
    # the constant 0x4a2360), ST(3) = an older value of the caller that the
    # loop carries along.
    scale = cpu.rd64(CONST_015)
    cpu.fpu.push(0.0)                 # ST(3)
    cpu.fpu.push(scale * pitch)       # ST(2)
    cpu.fpu.push(0.0)                 # ST(1) = displacement_y
    cpu.fpu.push(0.0)                 # ST(0) = displacement_x

    # param_2 (ticks of the batch) in ESI, the loop copies it into EAX.
    cpu.regs["esi"] = ticks & 0xFFFFFFFF

    cpu.run(LOOP_VA, AFTER_LOOP_VA)
    # the two stores that publish the accumulators
    cpu.run(AFTER_LOOP_VA, STOP_VA)

    return Result(
        cpu.rd64(PLAYER_BASE + OFF_VX),
        cpu.rd64(PLAYER_BASE + OFF_VY),
        cpu.rd64(PLAYER_BASE + OFF_VZ),
        cpu.rd64(esp + STACK_DISP_X),
        cpu.rd64(esp + STACK_DISP_Y),
    )


def closed_form(v0: float, k: float, dv: float, ticks: int):
    """The integrator written as a formula (independent of the emulation)."""
    if k == 1.0:
        v = v0 + dv * ticks
        disp = v0 * ticks + dv * ticks * (ticks + 1) / 2.0
        return v, disp
    kn = k ** ticks
    geom = k * (1.0 - kn) / (1.0 - k)
    v = kn * v0 + dv * (1.0 - kn) / (1.0 - k)
    disp = v0 * geom + dv * (ticks - geom) / (1.0 - k)
    return v, disp


def model(vx0, vy0, k, dvx, dvy, ticks):
    """A plain re-implementation of the loop (no machine code)."""
    vx, vy = vx0, vy0
    dx = dy = 0.0
    for _ in range(ticks):
        vx = vx * k + dvx
        vy = vy * k + dvy
        dx += vx
        dy += vy
    return vx, vy, dx, dy


CASES = [
    # (name, vx0, vy0, k, dvx, dvy, ticks)
    ("dyadic-decay", 1.0, -0.5, 0.5, 0.0, 0.0, 8),
    ("dyadic-full-gas", 0.0, 0.0, 0.5, 0.25, -0.125, 10),
    ("dyadic-brake", 2.0, 1.0, 0.25, -0.5, 0.75, 6),
    ("dyadic-quarter", 0.125, 0.375, 0.75, 1.5, -0.25, 5),
    ("light-chassis", 0.0, 0.0, 0.9, 0.6775, 0.0, 22),
    ("heavy-chassis", 0.0, 0.0, 0.96, 1.355, -0.25, 45),
]


def run_cases():
    out = []
    for name, vx0, vy0, k, dvx, dvy, ticks in CASES:
        res = emulate(vx0, vy0, 0.0, k, dvx, dvy, 0.0, ticks)
        mx, my, mdx, mdy = model(vx0, vy0, k, dvx, dvy, ticks)
        cxv, cdx = closed_form(vx0, k, dvx, ticks)
        cyv, cdy = closed_form(vy0, k, dvy, ticks)
        out.append({
            "name": name,
            "input": {"vx0": vx0, "vy0": vy0, "k": k, "dvx": dvx,
                      "dvy": dvy, "ticks": ticks},
            "emulated": res.as_dict(),
            "model": {"vx": mx, "vy": my, "displacement_x": mdx,
                      "displacement_y": mdy},
            "closed_form": {"vx": cxv, "vy": cyv, "displacement_x": cdx,
                            "displacement_y": cdy},
        })
    return out


def format_table(rows) -> str:
    lines = []
    lines.append("%-18s %5s %8s %-22s %-22s %-22s" %
                 ("case", "ticks", "k", "emulated vx/vy",
                  "model vx/vy", "closed form vx/vy"))
    for row in rows:
        e = row["emulated"]
        m = row["model"]
        c = row["closed_form"]
        lines.append("%-18s %5d %8.3f %11.9g/%-10.9g %11.9g/%-10.9g %11.9g/%-10.9g" % (
            row["name"], row["input"]["ticks"], row["input"]["k"],
            e["vx"], e["vy"], m["vx"], m["vy"], c["vx"], c["vy"]))
    lines.append("")
    lines.append("%-18s %-24s %-24s %-24s" %
                 ("case", "displacement_x (emu)", "displacement_y (emu)",
                  "dx/dy closed form"))
    for row in rows:
        e = row["emulated"]
        c = row["closed_form"]
        lines.append("%-18s %24.12g %24.12g %12.9g/%-12.9g" % (
            row["name"], e["displacement_x"], e["displacement_y"],
            c["displacement_x"], c["displacement_y"]))
    return "\n".join(lines)


def max_deviation(rows):
    """Largest relative deviation between the emulation and the model."""
    worst = 0.0
    for row in rows:
        e = row["emulated"]
        m = row["model"]
        c = row["closed_form"]
        for key in ("vx", "vy", "displacement_x", "displacement_y"):
            for other in (m, c):
                ref = other[key]
                scale = max(abs(ref), 1e-12)
                worst = max(worst, abs(e[key] - ref) / scale)
    return worst


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--json", action="store_true",
                        help="machine readable output")
    parser.add_argument("--selftest", action="store_true",
                        help="exit non zero when the model and the emulation "
                             "disagree")
    args = parser.parse_args(argv)

    problems = check_image(load_image())
    if problems:
        for line in problems:
            print("image check failed: %s" % line, file=sys.stderr)
        return 2

    rows = run_cases()
    if args.json:
        print(json.dumps({"cases": rows,
                          "max_relative_deviation": max_deviation(rows)},
                         indent=2))
    else:
        print(format_table(rows))
        print()
        print("max relative deviation (emulation vs model / closed form): %.3g"
              % max_deviation(rows))

    if args.selftest:
        return 0 if max_deviation(rows) < 1e-12 else 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
