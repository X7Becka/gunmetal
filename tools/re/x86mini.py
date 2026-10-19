"""Minimal x86 (32-bit protected mode) interpreter — just enough to run the
Miles/Gunmetal ADPCM decoders that live in ``gundx.exe``'s ``adpcmseg`` section.

Why an emulator: the three decoders (0x852b40 / 0x852c00 / 0x852cc0 for 4/2/1
bits per code and 0x852d80 for the SOU2 bitstream) *patch their own
instructions* with a table pointer (``mov [0x852ba3], edi``), so a static
disassembly is misleading.  Running the real bytes gives the exact algorithm
and lets us port it afterwards.

Only the instructions that occur in those routines are implemented; anything
else raises with the address, which is what we want while reverse engineering.
"""

from __future__ import annotations

import struct

MEM_SIZE = 0x1000000  # 16 MiB of flat address space, enough for the image


class UnknownOpcode(RuntimeError):
    pass


def _s32(v: int) -> int:
    v &= 0xFFFFFFFF
    return v - 0x100000000 if v & 0x80000000 else v


def _s8(v: int) -> int:
    v &= 0xFF
    return v - 0x100 if v & 0x80 else v


class CPU:
    REG32 = ("eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi")

    def __init__(self, image: bytes | None = None, base: int = 0, size: int = MEM_SIZE):
        self.mem = bytearray(size)
        if image:
            self.mem[base:base + len(image)] = image
        self.r = {name: 0 for name in self.REG32}
        self.eflags = {name: 0 for name in ("zf", "cf", "of", "sf")}
        self.eip = 0
        self.steps = 0

    # --- memory ---------------------------------------------------------
    def rd8(self, addr: int) -> int:
        return self.mem[addr & 0xFFFFFF]

    def rd16(self, addr: int) -> int:
        return int.from_bytes(self.mem[addr:addr + 2], "little")

    def rd32(self, addr: int) -> int:
        return int.from_bytes(self.mem[addr:addr + 4], "little")

    def wr8(self, addr: int, value: int) -> None:
        self.mem[addr & 0xFFFFFF] = value & 0xFF

    def wr16(self, addr: int, value: int) -> None:
        self.mem[addr:addr + 2] = (value & 0xFFFF).to_bytes(2, "little")

    def wr32(self, addr: int, value: int) -> None:
        self.mem[addr:addr + 4] = (value & 0xFFFFFFFF).to_bytes(4, "little")

    def read(self, addr: int, size: int) -> bytes:
        return bytes(self.mem[addr:addr + size])

    def load(self, addr: int, data: bytes) -> None:
        self.mem[addr:addr + len(data)] = data

    # --- helpers --------------------------------------------------------
    def get(self, name: str) -> int:
        return self.r[name]

    def set(self, name: str, value: int) -> None:
        self.r[name] = value & 0xFFFFFFFF

    def _set_reg(self, index: int, value: int) -> None:
        self.r[self.REG32[index]] = value & 0xFFFFFFFF

    def _reg(self, index: int) -> int:
        return self.r[self.REG32[index]]

    def _set_flags_zn(self, value: int, size_bits: int = 32) -> None:
        mask = (1 << size_bits) - 1
        v = value & mask
        self.eflags["zf"] = 1 if v == 0 else 0
        self.eflags["sf"] = 1 if (v >> (size_bits - 1)) & 1 else 0

    # --- ModRM ----------------------------------------------------------
    def _modrm(self, addr: int) -> tuple[str, int, int, int]:
        """Decode ``[addr]`` -> (kind, reg, rm_addr_or_reg, length).

        kind is ``"reg"`` or ``"mem"``; for ``"reg"`` the third value is the
        register index, for ``"mem"`` it is the effective address.
        """
        modrm = self.rd8(addr)
        mod = modrm >> 6
        reg = (modrm >> 3) & 7
        rm = modrm & 7
        length = 1

        if mod == 3:
            return "reg", reg, rm, length

        base = None
        index = None
        scale = 0
        disp = 0

        if rm == 4:  # SIB
            sib = self.rd8(addr + length)
            length += 1
            scale = 1 << (sib >> 6)
            idx = (sib >> 3) & 7
            base_r = sib & 7
            if idx != 4:
                index = idx
            if base_r == 5 and mod == 0:
                disp = struct.unpack("<i", self.read(addr + length, 4))[0]
                length += 4
            else:
                base = base_r
        elif rm == 5 and mod == 0:
            disp = struct.unpack("<i", self.read(addr + length, 4))[0]
            length += 4
        else:
            base = rm

        if base is not None:
            ea = self._reg(base)
        else:
            ea = 0
        if index is not None:
            ea += self._reg(index) * scale
        if mod == 1:
            disp = _s8(self.rd8(addr + length))
            length += 1
        elif mod == 2:
            disp = struct.unpack("<i", self.read(addr + length, 4))[0]
            length += 4

        return "mem", reg, (ea + disp) & 0xFFFFFFFF, length

    # --- run ------------------------------------------------------------
    def call(self, addr: int, args: list[int], stack_top: int = 0x800000,
             limit: int = 2_000_000) -> int:
        """Call ``addr`` with cdecl ``args``; returns eax."""
        self.set("esp", stack_top)
        for value in reversed(args):
            self.set("esp", self.get("esp") - 4)
            self.wr32(self.get("esp"), value)
        self.set("esp", self.get("esp") - 4)
        self.wr32(self.get("esp"), 0xDEADBEEF)
        self.eip = addr
        self.steps = 0
        while self.eip != 0xDEADBEEF:
            self.step()
            self.steps += 1
            if self.steps > limit:
                raise RuntimeError("step limit at eip=%08x" % self.eip)
        return self.get("eax")

    def step(self) -> None:
        eip = self.eip
        op = self.rd8(eip)
        n = 1

        if op == 0x90:  # nop
            pass
        elif 0x50 <= op <= 0x57:  # push r32
            self.set("esp", self.get("esp") - 4)
            self.wr32(self.get("esp"), self._reg(op - 0x50))
        elif 0x58 <= op <= 0x5F:  # pop r32
            self._set_reg(op - 0x58, self.rd32(self.get("esp")))
            self.set("esp", self.get("esp") + 4)
        elif op == 0x68:  # push imm32
            self.set("esp", self.get("esp") - 4)
            self.wr32(self.get("esp"), self.rd32(eip + 1))
            n += 4
        elif op == 0x6A:  # push imm8
            self.set("esp", self.get("esp") - 4)
            self.wr32(self.get("esp"), _s8(self.rd8(eip + 1)) & 0xFFFFFFFF)
            n += 1
        elif op == 0xE8:  # call rel32
            rel = struct.unpack("<i", self.read(eip + 1, 4))[0]
            self.set("esp", self.get("esp") - 4)
            self.wr32(self.get("esp"), eip + 5)
            self.eip = (eip + 5 + rel) & 0xFFFFFFFF
            return
        elif op == 0xE9:  # jmp rel32
            rel = struct.unpack("<i", self.read(eip + 1, 4))[0]
            self.eip = (eip + 5 + rel) & 0xFFFFFFFF
            return
        elif op == 0xEB:  # jmp rel8
            self.eip = (eip + 2 + _s8(self.rd8(eip + 1))) & 0xFFFFFFFF
            return
        elif 0x70 <= op <= 0x7F:  # jcc rel8
            if self._cond(op - 0x70):
                self.eip = (eip + 2 + _s8(self.rd8(eip + 1))) & 0xFFFFFFFF
                return
            n += 1
        elif op == 0x0F and self.rd8(eip + 1) in (0x84, 0x85):  # jz/jnz rel32
            rel = struct.unpack("<i", self.read(eip + 2, 4))[0]
            if self._cond(self.rd8(eip + 1) - 0x80):
                self.eip = (eip + 6 + rel) & 0xFFFFFFFF
                return
            n += 5
        elif op == 0xC3:  # ret
            self.eip = self.rd32(self.get("esp"))
            self.set("esp", self.get("esp") + 4)
            return
        elif op in (0x89, 0x8B, 0x88, 0x8A):
            kind, reg, where, mlen = self._modrm(eip + 1)
            n += mlen
            if op == 0x89:
                if kind == "mem":
                    self.wr32(where, self._reg(reg))
                else:
                    self._set_reg(where, self._reg(reg))
            elif op == 0x8B:
                if kind == "mem":
                    self._set_reg(reg, self.rd32(where))
                else:
                    self._set_reg(reg, self._reg(where))
            elif op == 0x88:
                if kind == "mem":
                    self.wr8(where, self._reg(reg) & 0xFF)
                else:
                    self._set_reg8(where, self._reg(reg) & 0xFF)
            else:  # 0x8A
                if kind == "mem":
                    self._set_reg8(reg, self.rd8(where))
                else:
                    self._set_reg8(reg, self._get_reg8(where))
        elif op == 0xA0:  # mov al, moffs32
            self._set_reg8(0, self.rd8(self.rd32(eip + 1)))
            n += 4
        elif op == 0xA1:  # mov eax, moffs32
            self.set("eax", self.rd32(self.rd32(eip + 1)))
            n += 4
        elif op == 0xA3:  # mov moffs32, eax
            self.wr32(self.rd32(eip + 1), self.get("eax"))
            n += 4
        elif op in (0x01, 0x03, 0x29, 0x2B, 0x31, 0x33):
            kind, reg, where, mlen = self._modrm(eip + 1)
            n += mlen
            if kind == "mem":
                other = self.rd32(where)
            else:
                other = self._reg(where)
            if op in (0x01, 0x03):
                if op == 0x01:  # add r/m32, r32
                    lhs = self.rd32(where) if kind == "mem" else self._reg(where)
                    value = (lhs + self._reg(reg)) & 0xFFFFFFFF
                    if kind == "mem":
                        self.wr32(where, value)
                    else:
                        self._set_reg(where, value)
                else:  # add r32, r/m32
                    value = (self._reg(reg) + other) & 0xFFFFFFFF
                    self._set_reg(reg, value)
                self._set_flags_zn(value)
            elif op in (0x29, 0x2B):
                if op == 0x29:
                    a, b = (self.rd32(where) if kind == "mem" else self._reg(where)), self._reg(reg)
                    value = (a - b) & 0xFFFFFFFF
                    if kind == "mem":
                        self.wr32(where, value)
                    else:
                        self._set_reg(where, value)
                else:
                    value = (self._reg(reg) - other) & 0xFFFFFFFF
                    self._set_reg(reg, value)
                self._set_flags_zn(value)
            else:  # xor
                if op == 0x31:
                    a = self.rd32(where) if kind == "mem" else self._reg(where)
                    value = a ^ self._reg(reg)
                    if kind == "mem":
                        self.wr32(where, value)
                    else:
                        self._set_reg(where, value)
                else:
                    value = self._reg(reg) ^ other
                    self._set_reg(reg, value)
                self._set_flags_zn(value)
        elif op in (0x83, 0x81, 0xC1, 0xD1, 0xD3, 0xC7, 0x85, 0x39, 0x3B, 0x3D, 0x05, 0x25, 0x2D, 0x35):
            self._alu(eip, op)
            n = self._alu_len + 1
        elif 0x40 <= op <= 0x47:  # inc r32
            value = (self._reg(op - 0x40) + 1) & 0xFFFFFFFF
            self._set_reg(op - 0x40, value)
            self._set_flags_zn(value)
        elif 0x48 <= op <= 0x4F:  # dec r32
            value = (self._reg(op - 0x48) - 1) & 0xFFFFFFFF
            self._set_reg(op - 0x48, value)
            self._set_flags_zn(value)
        else:
            raise UnknownOpcode("opcode %02x at %08x: %s"
                                % (op, eip, self.read(eip, 8).hex(" ")))

        self.eip = (eip + n) & 0xFFFFFFFF

    # 8-bit register helpers (al/cl/dl/bl = eax/ecx/edx/ebx low bytes)
    def _get_reg8(self, index: int) -> int:
        if index < 4:
            return self._reg(index) & 0xFF
        return (self._reg(index - 4) >> 8) & 0xFF

    def _set_reg8(self, index: int, value: int) -> None:
        if index < 4:
            self._set_reg(index, (self._reg(index) & 0xFFFFFF00) | (value & 0xFF))
        else:
            reg = index - 4
            self._set_reg(reg, (self._reg(reg) & 0xFFFF00FF) | ((value & 0xFF) << 8))

    def _cond(self, code: int) -> bool:
        zf, sf, of, cf = (self.eflags["zf"], self.eflags["sf"],
                          self.eflags["of"], self.eflags["cf"])
        return {
            0x0: of, 0x1: not of, 0x2: cf, 0x3: not cf,
            0x4: zf, 0x5: not zf, 0x6: cf or zf, 0x7: not (cf or zf),
            0x8: sf, 0x9: not sf, 0xA: sf != of, 0xB: sf == of,
            0xC: zf or (sf != of), 0xD: not (zf or (sf != of)),
            0xE: zf or (sf != of), 0xF: not (zf or (sf != of)),
        }[code]

    _alu_len = 0

    def _alu(self, eip: int, op: int) -> None:
        """0x83/0x81 group, shifts, mov imm32, test, cmp."""
        eip += 1
        length = 0
        if op in (0xC1, 0xD1, 0xD3, 0xC7):
            kind, reg, where, mlen = self._modrm(eip)
            length += mlen
            if op == 0xC7:
                imm = self.rd32(eip + length)
                length += 4
                if kind == "mem":
                    self.wr32(where, imm)
                else:
                    self._set_reg(where, imm)
            else:
                if op == 0xC1:
                    count = self.rd8(eip + length) & 0x1F
                    length += 1
                elif op == 0xD1:
                    count = 1
                else:
                    count = self.get("ecx") & 0x1F
                value = self.rd32(where) if kind == "mem" else self._reg(where)
                if reg == 4:  # shl
                    result = (value << count) & 0xFFFFFFFF
                    self.eflags["cf"] = 1 if count and ((value >> (32 - count)) & 1) else 0
                elif reg == 5:  # shr
                    result = (value >> count) & 0xFFFFFFFF
                    self.eflags["cf"] = 1 if count and ((value >> (count - 1)) & 1) else 0
                elif reg == 7:  # sar
                    sv = _s32(value) >> count
                    result = sv & 0xFFFFFFFF
                else:
                    raise UnknownOpcode("shift /%d at %08x" % (reg, eip - 1))
                self._set_flags_zn(result)
                if kind == "mem":
                    self.wr32(where, result)
                else:
                    self._set_reg(where, result)
            self._alu_len = length
            return

        if op in (0x85, 0x39, 0x3B):
            kind, reg, where, mlen = self._modrm(eip)
            length += mlen
            a = self.rd32(where) if kind == "mem" else self._reg(where)
            b = self._reg(reg)
            if op == 0x85:
                self._set_flags_zn(a & b)
            else:
                self._set_flags_zn((a - b) & 0xFFFFFFFF)
                self.eflags["cf"] = 1 if a < b else 0
            self._alu_len = length
            return

        if op in (0x3D, 0x05, 0x25, 0x2D, 0x35):
            imm = self.rd32(eip)
            length += 4
            a = self.get("eax")
            if op == 0x3D:
                self._set_flags_zn((a - imm) & 0xFFFFFFFF)
                self.eflags["cf"] = 1 if a < imm else 0
            elif op == 0x05:
                a = (a + imm) & 0xFFFFFFFF
                self.set("eax", a)
                self._set_flags_zn(a)
            elif op == 0x25:
                a = a & imm
                self.set("eax", a)
                self._set_flags_zn(a)
            elif op == 0x2D:
                a = (a - imm) & 0xFFFFFFFF
                self.set("eax", a)
                self._set_flags_zn(a)
                self.eflags["cf"] = 1 if self.get("eax") + imm > 0xFFFFFFFF else 0
            else:
                a = a ^ imm
                self.set("eax", a)
                self._set_flags_zn(a)
            self._alu_len = length
            return

        # 0x83 / 0x81 groups
        kind, reg, where, mlen = self._modrm(eip)
        length += mlen
        if op == 0x83:
            imm = _s8(self.rd8(eip + length))
            length += 1
        else:
            imm = struct.unpack("<i", self.read(eip + length, 4))[0]
            length += 4
        value = self.rd32(where) if kind == "mem" else self._reg(where)
        if reg == 0:  # add
            result = (value + imm) & 0xFFFFFFFF
        elif reg == 4:  # and
            result = value & (imm & 0xFFFFFFFF)
        elif reg == 5:  # sub
            result = (value - imm) & 0xFFFFFFFF
            self.eflags["cf"] = 1 if value < (imm & 0xFFFFFFFF) else 0
        elif reg == 7:  # cmp
            result = (value - imm) & 0xFFFFFFFF
            self.eflags["cf"] = 1 if value < (imm & 0xFFFFFFFF) else 0
            self._set_flags_zn(result)
            self._alu_len = length
            return
        else:
            raise UnknownOpcode("alu /%d at %08x" % (reg, eip - 1))
        self._set_flags_zn(result)
        if kind == "mem":
            self.wr32(where, result)
        else:
            self._set_reg(where, result)
        self._alu_len = length
