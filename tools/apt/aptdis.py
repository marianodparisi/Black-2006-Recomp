"""Disassemble the ActionScript of BLACK's frontend APT bundles (game/export/frontend/*.bin).

usage:
  py -3 tools/apt/aptdis.py game/export/frontend/FEMain.bin                  list the bundle's movies
  py -3 tools/apt/aptdis.py game/export/frontend/FEMain.bin choosedifficulty  disassemble one movie's scripts

Reads only the user's own game files; nothing from them is stored in the repository.

Format, as found in BLACK's bundles:
  - The bundle starts with the movie names (NUL separated, from offset 488); the movies follow in the same order,
    each as an "Apt Data:7" block followed by its "Apt constant file" block.
  - Constant file: "Apt constant file\\x1a\\0\\0", u32 apt data offset, u32 count, u32 entries offset, then
    (type, value) u32 pairs. Type 1 is a string at value (relative to the constant file), 3 a special
    (undefined/null/true/false), 4 an integer, 5 a float, 7 a register, 8 an index into the script's
    ConstantPool.
  - Bytecode uses AVM1 (Flash ActionScript) opcode numbers. Opcodes below 0x80 take no arguments; the
    arguments of the others are aligned to 4 bytes:
      0x96 Push: count, pointer to `count` u32 constant indices;
      0x88 ConstantPool: count, pointer to `count` u32 constant indices (strings);
      0x99 Jump / 0x9D If: i32 offset from the end of the instruction;
      0x87 StoreRegister: u32 register;
      0x9B DefineFunction / 0x8E DefineFunction2: name pointer, parameter count, (registers,) parameter
      pointer, code size, the magic 0x98765432 0x12345678, then the body.
"""
import re
import struct
import sys


def movies(d):
    names, o = [], 488
    datas = [m.start() for m in re.finditer(rb"Apt Data:7", d)]
    consts = [m.start() for m in re.finditer(rb"Apt constant file", d)]
    while len(names) < len(datas):
        e = d.index(b"\0", o)
        names.append(d[o:e].decode("latin-1"))
        o = e + 1
        while d[o] == 0:
            o += 1
    return list(zip(names, datas, consts))


NOARG = {0x00: "end", 0x04: "nextFrame", 0x05: "prevFrame", 0x06: "play", 0x07: "stop", 0x0A: "add", 0x0B: "sub",
         0x0C: "mul", 0x0D: "div", 0x0E: "eq", 0x0F: "lt", 0x10: "and", 0x11: "or", 0x12: "not", 0x13: "strEq",
         0x14: "strLen", 0x17: "pop", 0x18: "toInt", 0x1C: "getVar", 0x1D: "setVar", 0x20: "setTarget2",
         0x21: "strAdd", 0x22: "getProp", 0x23: "setProp", 0x24: "cloneSprite", 0x25: "removeSprite", 0x26: "trace",
         0x27: "startDrag", 0x28: "endDrag", 0x34: "getTime", 0x3A: "delete", 0x3B: "delete2", 0x3C: "defineLocal",
         0x3D: "callFunction", 0x3E: "return", 0x3F: "mod", 0x40: "new", 0x41: "defineLocal2", 0x42: "initArray",
         0x43: "initObject", 0x44: "typeof", 0x45: "targetPath", 0x46: "enumerate", 0x47: "add2", 0x48: "lt2",
         0x49: "eq2", 0x4A: "toNumber", 0x4B: "toString", 0x4C: "dup", 0x4D: "swap", 0x4E: "getMember",
         0x4F: "setMember", 0x50: "inc", 0x51: "dec", 0x52: "callMethod", 0x53: "newMethod", 0x54: "instanceOf",
         0x55: "enumerate2", 0x60: "bitAnd", 0x61: "bitOr", 0x62: "bitXor", 0x63: "shl", 0x64: "shr", 0x65: "ushr",
         0x66: "strictEq", 0x67: "gt", 0x68: "strGt", 0x69: "extends"}


class Movie:
    def __init__(self, d, data_off, const_off):
        self.blk = d[data_off:const_off]
        self.cst = d[const_off:]
        count, entries = struct.unpack_from("<II", self.cst, 24)
        self.raw = [struct.unpack_from("<II", self.cst, entries + i * 8) for i in range(count)]
        self.pool = []

    def cstring(self, v):
        e = self.cst.index(b"\0", v)
        return self.cst[v:e].decode("latin-1")

    def bstring(self, ptr):
        e = self.blk.index(b"\0", ptr)
        return self.blk[ptr:e].decode("latin-1")

    def show(self, i):
        if i >= len(self.raw):
            return "?%d" % i
        t, v = self.raw[i]
        if t == 1:
            return repr(self.cstring(v))
        if t == 8:
            return repr(self.pool[v]) if v < len(self.pool) else "pool[%d]" % v
        if t == 7:
            return "r%d" % v
        if t == 4:
            return str(struct.unpack("<i", struct.pack("<I", v))[0])
        if t == 5:
            return "%g" % struct.unpack("<f", struct.pack("<I", v))[0]
        if t == 3:
            return ["undefined", "null", "true", "false"][v] if v < 4 else "t3:%d" % v
        return "t%d:%d" % (t, v)

    def pools(self):
        """Offsets of ConstantPool instructions whose entries are all strings (script starts)."""
        found = []
        blk = self.blk
        for i in range(len(blk) - 12):
            if blk[i] != 0x88:
                continue
            a = (i + 4) & ~3
            n, ptr = struct.unpack_from("<II", blk, a)
            if not (1 <= n <= 1000 and 0 < ptr < len(blk) - n * 4):
                continue
            idx = struct.unpack_from("<%dI" % n, blk, ptr)
            if all(j < len(self.raw) and self.raw[j][0] == 1 for j in idx):
                found.append(i)
        return found

    def dis(self, start, size, indent="  "):
        blk = self.blk
        pc, end = start, start + size
        out = []
        while pc < end and pc < len(blk):
            op = blk[pc]
            at = pc
            pc += 1
            if op == 0x9A:                           # GetURL2: no argument in this format
                out.append(f"{indent}{at:6d} getURL2")
                continue
            if op < 0x80:
                out.append(f"{indent}{at:6d} {NOARG.get(op, 'op%02X' % op)}")
                if op == 0x00 and size > 100000:
                    break
                continue
            pc = (pc + 3) & ~3
            if op == 0x96:
                n, ptr = struct.unpack_from("<II", blk, pc); pc += 8
                idx = struct.unpack_from("<%dI" % n, blk, ptr)
                out.append(f"{indent}{at:6d} push {', '.join(self.show(i) for i in idx)}")
            elif op in (0x99, 0x9D):
                off = struct.unpack_from("<i", blk, pc)[0]; pc += 4
                out.append(f"{indent}{at:6d} {'jump' if op == 0x99 else 'if'} -> {pc + off}")
            elif op in (0x9B, 0x8E):
                name_ptr, nparams = struct.unpack_from("<II", blk, pc)
                if op == 0x8E:
                    code = struct.unpack_from("<I", blk, pc + 16)[0]
                    pc += 20
                else:
                    code = struct.unpack_from("<I", blk, pc + 12)[0]
                    pc += 16
                pc += 8                              # magic
                name = self.bstring(name_ptr) if name_ptr else "<anonymous>"
                out.append(f"{indent}{at:6d} function {name}(params {nparams}) [{code} bytes]")
                out.extend(self.dis(pc, code, indent + "    "))
                pc += code
            elif op == 0x87:
                out.append(f"{indent}{at:6d} storeRegister r{struct.unpack_from('<I', blk, pc)[0]}"); pc += 4
            elif op in (0x8C, 0x8B):
                ptr = struct.unpack_from("<I", blk, pc)[0]; pc += 4
                out.append(f"{indent}{at:6d} {'gotoLabel' if op == 0x8C else 'setTarget'} {self.bstring(ptr)}")
            elif op == 0x83:
                url, target = struct.unpack_from("<II", blk, pc); pc += 8
                out.append(f"{indent}{at:6d} getURL {self.bstring(url)!r} {self.bstring(target)!r}")
            elif op in (0x81, 0x9F):
                out.append(f"{indent}{at:6d} gotoFrame {struct.unpack_from('<I', blk, pc)[0]}"); pc += 4
            elif op == 0x88:
                n, ptr = struct.unpack_from("<II", blk, pc); pc += 8
                self.pool = [self.cstring(self.raw[j][1]) for j in struct.unpack_from("<%dI" % n, blk, ptr)]
                out.append(f"{indent}{at:6d} constantPool {n} strings")
            else:
                out.append(f"{indent}{at:6d} op{op:02X} (unknown argument layout; stopped)")
                break
        return out


def main():
    d = open(sys.argv[1], "rb").read()
    ms = movies(d)
    if len(sys.argv) < 3:
        for name, data_off, const_off in ms:
            print(f"{name:24s} data {data_off:8d} constants {const_off:8d}")
        return
    for name, data_off, const_off in ms:
        if name.lower() == sys.argv[2].lower():
            m = Movie(d, data_off, const_off)
            for start in m.pools():
                print(f"; script at {start}")
                print("\n".join(m.dis(start, 1 << 30)))
                print()
            return
    sys.exit("no movie named " + sys.argv[2])


if __name__ == "__main__":
    main()
