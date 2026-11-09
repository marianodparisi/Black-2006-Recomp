#!/usr/bin/env python3
"""Check the repository's translated guest addresses against a local XBE.

Every canonical (NTSC) address written as BLACK_VA(...) in the hand-written C,
or as a {0x...} placeholder in generate.py's hook markers, is mapped to the
XBE's release and checked statically:

* code: the target must be a return address (right after a call) or a
  function start (after int3/nop padding or a ret);
* data: the release's code must use the translated address as an instruction
  operand. When it also uses the untranslated value, the address must sit
  between verified neighbours in the same section, or be used within a few
  instructions of a verified address (as one expression, e.g. the zoom
  formula reads 1.0 and k together).

Nothing is executed. Usage: verify_release.py [--xbe game/default.xbe]
Exit status 1 when an address fails.
"""
import argparse
import bisect
import hashlib
import re
import struct
from collections import Counter, defaultdict
from pathlib import Path

import capstone
from capstone import x86

import black_release

ROOT = Path(__file__).resolve().parents[1]


class Xbe:
    def __init__(self, data):
        self.data = data
        base = struct.unpack_from("<I", data, 0x104)[0]
        count, headers = struct.unpack_from("<II", data, 0x11C)
        self.sections = []
        for i in range(count):
            _, va, vsize, raw, rsize, name = struct.unpack_from("<IIIIII", data, headers - base + i * 0x38)
            label = data[name - base:name - base + 16].split(b"\0")[0].decode("ascii", "replace")
            self.sections.append((label, va, vsize, raw, rsize))

    def section(self, va):
        for s in self.sections:
            if s[1] <= va < s[1] + s[2]:
                return s
        return None

    def raw(self, va):
        s = self.section(va)
        if s and va < s[1] + s[4]:
            return s[3] + va - s[1]
        return None


CODE_SECTIONS = {".text", "DSOUND", "WMADEC", "XMV", "XGRPH", "D3D", "XPP", "DOLBY"}


def operand_references(xbe):
    """Absolute operand addresses in every code section: address -> positions.

    A position is the index of the instruction in one linear sweep, so nearby
    positions are nearby instructions.
    """
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
    md.detail = True
    md.skipdata = True
    refs = defaultdict(list)
    position = 0
    for name, va, _, raw, rsize in xbe.sections:
        if name not in CODE_SECTIONS:
            continue
        position += 1000  # sections are not adjacent instructions
        for ins in md.disasm(xbe.data[raw:raw + rsize], va):
            position += 1
            if ins.id == 0:
                continue
            for op in ins.operands:
                if op.type == x86.X86_OP_MEM and op.mem.disp > 0x10000:
                    refs[op.mem.disp & 0xFFFFFFFF].append(position)
                elif op.type == x86.X86_OP_IMM and op.imm > 0x10000:
                    refs[op.imm & 0xFFFFFFFF].append(position)
    return refs


def used_together(refs, address, partner, window=8):
    partner_positions = refs.get(partner, [])
    return any(abs(a - b) <= window for a in refs.get(address, []) for b in partner_positions)


def code_kind(xbe, va):
    r = xbe.raw(va)
    if r is None:
        return None
    d = xbe.data
    if d[r - 5] == 0xE8:
        return "return address after call"
    for size in (2, 3, 6, 7):  # call r/m32 with no, 8-bit or 32-bit displacement / SIB
        if d[r - size] == 0xFF and (d[r - size + 1] >> 3) & 7 == 2:
            return "return address after call"
    if d[r] not in (0xCC, 0x90, 0x00) and (d[r - 1] in (0xCC, 0x90, 0xC3) or d[r - 3] == 0xC2):
        return "function start"
    return None


def repository_addresses():
    """Canonical address -> files that use it."""
    found = defaultdict(set)
    for path in [ROOT / "src/recomp_manual.c", *sorted((ROOT / "src").glob("black_*.inc"))]:
        if path.name == "black_xmv_idct.inc":
            continue  # generated from the local XBE at its real addresses
        for match in re.finditer(r"BLACK_VA\(\s*0x([0-9A-Fa-f]+)", path.read_text(encoding="utf-8")):
            found[int(match[1], 16)].add(path.name)
    for match in re.finditer(r"\{0x([0-9A-Fa-f]+)u?\}", (ROOT / "scripts/generate.py").read_text(encoding="utf-8")):
        found[int(match[1], 16)].add("generate.py")
    return found


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--xbe", type=Path, default=ROOT / "game/default.xbe")
    parser.add_argument("--verbose", action="store_true", help="list every address, not only problems")
    args = parser.parse_args()
    data = args.xbe.read_bytes()
    release = black_release.detect(hashlib.sha256(data).hexdigest())
    if release is None:
        parser.error("not a supported BLACK XBE")
    xbe = Xbe(data)
    refs = operand_references(xbe)
    addresses = repository_addresses()

    results = {}
    for canonical in sorted(addresses):
        guest = release.to_guest(canonical)
        section = xbe.section(guest)
        name = section[0] if section else "?"
        here = len(refs.get(guest, []))
        there = len(refs.get(canonical, [])) if guest != canonical else 0
        if canonical < black_release.CANONICAL_TEXT_END:
            kind = code_kind(xbe, guest)
            results[canonical] = ("ok", f"{name} {kind}") if kind else ("fail", f"{name}: not a call return or function start")
        elif name in CODE_SECTIONS and not here and code_kind(xbe, guest):
            results[canonical] = ("ok", f"{name} {code_kind(xbe, guest)}")
        elif here and not there:
            results[canonical] = ("ok", f"{name} {here} operand refs")
        elif here:
            results[canonical] = ("ambiguous", f"{name} {here} refs here, {there} at the untranslated value")
        elif there:
            results[canonical] = ("fail", f"{name}: no refs here, {there} at the untranslated value")
        else:
            results[canonical] = ("unreferenced", f"{name}: no exact operand refs (range start or field)")

    # An ambiguous address is settled by verified neighbours on both sides in
    # the same section: one layout shift covers everything between them.
    verified = sorted(c for c, (state, _) in results.items() if state == "ok")
    for canonical, (state, text) in list(results.items()):
        if state not in ("ambiguous", "unreferenced"):
            continue
        i = bisect.bisect_left(verified, canonical)
        low = verified[i - 1] if i else None
        high = verified[i] if i < len(verified) else None
        same = lambda other: other is not None and xbe.section(release.to_guest(other)) == xbe.section(release.to_guest(canonical))
        partner = next((v for v in verified if v != canonical and
                        used_together(refs, release.to_guest(canonical), release.to_guest(v))), None)
        if same(low) and same(high):
            results[canonical] = ("ok", f"{text}; between verified 0x{low:06X} and 0x{high:06X}")
        elif partner is not None:
            results[canonical] = ("ok", f"{text}; used next to verified 0x{partner:06X}")
        elif state == "ambiguous":
            results[canonical] = ("warn", f"{text}; no verified neighbour on both sides")

    counts = Counter(state for state, _ in results.values())
    print(f"{release.name.upper()} XBE: {len(results)} translated addresses, " +
          ", ".join(f"{n} {s}" for s, n in sorted(counts.items())))
    for canonical, (state, text) in sorted(results.items()):
        if args.verbose or state != "ok":
            files = ",".join(sorted(addresses[canonical]))
            print(f"{state:12} 0x{canonical:06X} -> 0x{release.to_guest(canonical):06X}  {text}  [{files}]")
    return 1 if counts.get("fail") else 0


if __name__ == "__main__":
    raise SystemExit(main())
