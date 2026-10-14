#!/usr/bin/env python3
"""Generate BLACK locally from the user's own supported retail XBE.

No translated game code is included in this repository. This script replays the
runtime optimizations and title hooks used by the verified PC build.
"""
import argparse
import datetime
import hashlib
import json
import re
import shutil
import subprocess
import sys
from pathlib import Path

import black_release

# Hooks are written against the canonical (NTSC) layout. main() selects the
# local release; literal guest addresses in markers go through RELEASE.text.
RELEASE = black_release.NTSC
FUNCTION = re.compile(r"^void (\w+)\(void\)\n\{.*?^\}", re.M | re.S)
# These late-added wrappers retain the generator's original register model.
UNPROMOTED = {"xbe_entry_point", "sub_00102840_gen", "sub_00102D90_gen",
              "sub_000D2F40_gen", "sub_001314E0_gen", "sub_00114030_gen",
              "sub_0015A960_gen"}

INT_REGS = ['eax', 'ecx', 'edx', 'ebx', 'esi', 'edi', 'esp']
MM_REGS = ['mm%d' % i for i in range(8)]
CALL_TOKENS = ['RECOMP_ABI_CALL', 'RECOMP_ICALL_SAFE', 'RECOMP_ICALL', 'RECOMP_ITAIL']
COMMENT = re.compile(r'(/\*.*?\*/)')


def split_code(line):
    """Alternating [code, comment, code, ...] pieces."""
    return COMMENT.split(line)


def strip_comments(text):
    return re.sub(r'/\*.*?\*/', '', text, flags=re.S)


def matching_paren(s, start):
    depth = 0
    for i in range(start, len(s)):
        if s[i] == '(':
            depth += 1
        elif s[i] == ')':
            depth -= 1
            if depth == 0:
                return i
    return -1


def promote(lines, name, report):
    text = '\n'.join(lines)
    code = strip_comments(text)
    if re.search(r'__asm|setjmp|longjmp|\bswitch\b', code):
        report.append((name, 'skipped: unsupported construct'))
        return lines
    # direct global register references other than the indirect-call stack snapshot
    leftover = re.sub(r'uint32_t _icall_esp = g_esp;', '', code)
    if re.search(r'\bg_(eax|ecx|edx|ebx|esi|edi|esp|mm[0-7])\b', leftover):
        report.append((name, 'skipped: direct global register use'))
        return lines
    used_int = [r for r in INT_REGS if re.search(r'\b%s\b' % r, code)]
    used_mm = [r for r in MM_REGS if re.search(r'\b%s\b' % r, code)]
    if 'esp' not in used_int and re.search(r'_icall_esp', code):
        used_int.append('esp')
    if not used_int and not used_mm:
        report.append((name, 'unchanged: no registers'))
        return lines
    flush = ' '.join('g_%s = l_%s;' % (r, r) for r in used_int + used_mm)
    reload = ' '.join('l_%s = g_%s;' % (r, r) for r in used_int + used_mm)
    reg_re = re.compile(r'\b(%s)\b' % '|'.join(used_int + used_mm))
    calls = 0
    rets = 0
    out = []
    # the declaration goes after the opening brace
    brace_index = None
    for i, line in enumerate(lines):
        if line == '{':
            brace_index = i
            break
    if brace_index is None:
        report.append((name, 'skipped: no opening brace'))
        return lines
    for i, line in enumerate(lines):
        if i <= brace_index:
            out.append(line)
            if i == brace_index:
                decl = ' '.join('uint32_t l_%s = g_%s;' % (r, r) for r in used_int)
                if used_mm:
                    decl += ' ' + ' '.join('RecompMmx l_%s = g_%s;' % (r, r) for r in used_mm)
                out.append('    ' + decl)
            continue
        pieces = split_code(line)
        for k in range(0, len(pieces), 2):
            p = pieces[k]
            p = p.replace('uint32_t _icall_esp = g_esp;', 'uint32_t _icall_esp = l_esp;')
            p = reg_re.sub(lambda m: 'l_' + m.group(1), p)
            # calls: flush before, reload after (the whole call statement goes in its own block)
            pos = 0
            while True:
                found = None
                for tok in CALL_TOKENS:
                    j = p.find(tok + '(', pos)
                    if j >= 0 and (found is None or j < found[0]):
                        found = (j, tok)
                if not found:
                    break
                j, tok = found
                end = matching_paren(p, j + len(tok))
                if end < 0:
                    report.append((name, 'skipped: unbalanced call'))
                    return lines
                semi = end + 1
                while semi < len(p) and p[semi] == ' ':
                    semi += 1
                if semi >= len(p) or p[semi] != ';':
                    report.append((name, 'skipped: call not followed by a semicolon'))
                    return lines
                stmt = p[j:semi + 1]
                repl = '{ ' + flush + ' ' + stmt + ' ' + reload + ' }'
                p = p[:j] + repl + p[semi + 1:]
                pos = j + len(repl)
                calls += 1
            # bare direct calls: a tail jump compiles to `g_seh_ebp = ebp; sub_X(); return;`
            pos = 0
            while True:
                m = re.compile(r'\bsub_[0-9A-F]{8}(?:_gen)?\(\);').search(p, pos)
                if not m:
                    break
                repl = '{ ' + flush + ' ' + m.group(0) + ' ' + reload + ' }'
                p = p[:m.start()] + repl + p[m.end():]
                pos = m.start() + len(repl)
                calls += 1
            if re.search(r'\breturn;', p):
                n = len(re.findall(r'\breturn;', p))
                p = re.sub(r'\breturn;', '{ ' + flush + ' return; }', p)
                rets += n
            pieces[k] = p
        out.append(''.join(pieces))
    report.append((name, 'promoted %s%s; %d calls, %d returns' % (','.join(used_int), (' + ' + ','.join(used_mm)) if used_mm else '', calls, rets)))
    return out



BIN = {
    'PADDD': '_mm_add_epi32', 'PSUBD': '_mm_sub_epi32', 'PADDW': '_mm_add_epi16', 'PSUBW': '_mm_sub_epi16',
    'PAND': '_mm_and_si128', 'POR': '_mm_or_si128', 'PXOR': '_mm_xor_si128', 'PANDN': '_mm_andnot_si128',
    'PMADDWD': '_mm_madd_epi16', 'PUNPCKLWD': '_mm_unpacklo_epi16', 'PUNPCKLDQ': '_mm_unpacklo_epi32',
    'PUNPCKLBW': '_mm_unpacklo_epi8', 'PMULLW': '_mm_mullo_epi16',
}
SHIFT = {'PSRLD': '_mm_srli_epi32', 'PSLLD': '_mm_slli_epi32', 'PSRAD': '_mm_srai_epi32',
         'PSRLW': '_mm_srli_epi16', 'PSLLW': '_mm_slli_epi16', 'PSRAW': '_mm_srai_epi16'}
MMX_REG = re.compile(r'\b(eax|ecx|edx|ebx|esi|edi|ebp|esp)\b')


def mmx_reg(s):
    return MMX_REG.sub(lambda m: 'r_' + m.group(1), s)


def operand(x):
    x = x.strip()
    m = re.fullmatch(r'mm([0-7])', x)
    if m:
        return 'm%s' % m.group(1)
    m = re.fullmatch(r'MMX_MEM\((.*)\)', x)
    if m:
        return '_mm_loadl_epi64((const __m128i *)(mem + (%s)))' % mmx_reg(m.group(1))
    raise ValueError('operand ' + x)


def translate_mmx(line):
    s = re.sub(r'/\*.*?\*/', '', line).strip()
    if not s or s.startswith(('_fa', '_fb', '_cf', 'if ')) or s.endswith(':') or s.startswith('loc_'):
        return None
    m = re.fullmatch(r'mm([0-7]) = mm([0-7]);', s)
    if m:
        return 'm%s = m%s;' % m.groups()
    m = re.fullmatch(r'mm([0-7]) = (MMX_MEM\(.*\));', s)
    if m:
        return 'm%s = %s;' % (m.group(1), operand(m.group(2)))
    m = re.fullmatch(r'mm([0-7]) = MMX_FROM32\(MEM32\((.*)\)\);', s)
    if m:
        return 'm%s = _mm_cvtsi32_si128(*(const int32_t *)(mem + (%s)));' % (m.group(1), mmx_reg(m.group(2)))
    m = re.fullmatch(r'mm([0-7]) = MMX_PCMPEQB\(mm([0-7]), mm([0-7])\);', s)
    if m:
        return 'm%s = _mm_cmpeq_epi8(m%s, m%s);' % m.groups()
    m = re.fullmatch(r'mm([0-7]) = MMX_(PSRLD|PSLLD|PSRAD|PSRLW|PSLLW|PSRAW)\(mm([0-7]), (\d+)u\);', s)
    if m:
        return 'm%s = %s(m%s, %s);' % (m.group(1), SHIFT[m.group(2)], m.group(3), m.group(4))
    m = re.fullmatch(r'mm([0-7]) = MMX_(\w+)\(mm([0-7]), (.*)\);', s)
    if m and m.group(2) in BIN:
        return 'm%s = %s(m%s, %s);' % (m.group(1), BIN[m.group(2)], m.group(3), operand(m.group(4)))
    m = re.fullmatch(r'mm([0-7]) = MMX_PACKSSDW\(mm([0-7]), mm([0-7])\);', s)
    if m:
        return 'm%s = pack_ssdw(m%s, m%s);' % m.groups()
    m = re.fullmatch(r'mm([0-7]) = MMX_PACKUSWB\(mm([0-7]), mm([0-7])\);', s)
    if m:
        return 'm%s = pack_uswb(m%s, m%s);' % m.groups()
    m = re.fullmatch(r'mm([0-7]) = MMX_PUNPCKHDQ\(mm([0-7]), mm([0-7])\);', s)
    if m:
        return 'm%s = unpackhi_dq(m%s, m%s);' % m.groups()
    m = re.fullmatch(r'MMX_STORE\((.*), mm([0-7])\);', s)
    if m:
        return '_mm_storel_epi64((__m128i *)(mem + (%s)), m%s);' % (mmx_reg(m.group(1)), m.group(2))
    m = re.fullmatch(r'MEM32\((.*)\) = mm([0-7])\.ud\[0\];', s)
    if m:
        return '*(uint32_t *)(mem + (%s)) = (uint32_t)_mm_cvtsi128_si32(m%s);' % (mmx_reg(m.group(1)), m.group(2))
    m = re.fullmatch(r'(eax|ebx|ecx|edx|esi|edi) = mm([0-7])\.ud\[0\];', s)
    if m:
        return 'r_%s = (uint32_t)_mm_cvtsi128_si32(m%s);' % m.groups()
    m = re.fullmatch(r'MEM16\((.*)\) = LO16\((\w+)\);', s)
    if m:
        return '*(uint16_t *)(mem + (%s)) = (uint16_t)%s;' % (mmx_reg(m.group(1)), mmx_reg(m.group(2)))
    m = re.fullmatch(r'(eax|ebx|ecx|edx|esi|edi) = (.*);', s)
    if m and 'MMX' not in s and 'MEM' not in s:
        return 'r_%s = %s;' % (m.group(1), mmx_reg(m.group(2)))
    m = re.fullmatch(r'(eax|ebx|ecx|edx|esi|edi)\+\+;', s)
    if m:
        return 'r_%s++;' % m.group(1)
    raise ValueError(line)


def rewrite_functions(text, transform):
    return FUNCTION.sub(lambda m: transform(m[1], m[0]), text)


def promote_body(name, body):
    if name in UNPROMOTED:
        return body
    promoted = "\n".join(promote(body.split("\n"), name, []))
    # Preserve the two conditional leaf paths of the validated build.
    if name in {"sub_002400E0", "sub_00240137"}:
        promoted = re.sub(r"\{ (?:g_\w+ = l_\w+; )+(sub_[0-9A-F]{8}\(\);) (?:l_\w+ = g_\w+; )+\}", r"\1", promoted)
    return promoted


def repair_sse(text):
    compares = {"cmpltss": "_mm_cmplt_ss", "cmpnless": "_mm_cmpnle_ss",
                "cmpeqss": "_mm_cmpeq_ss", "cmpneqss": "_mm_cmpneq_ss",
                "cmpnltps": "_mm_cmpnlt_ps"}
    todo = re.compile(r"^(\s*)/\* TODO: (cmp\w+) (xmm\d), (xmm\d|dword ptr \[(?:esp|ebp) [+-] 0x[0-9A-Fa-f]+\]) \*/\s*$")
    reciprocal = re.compile(r"^(\s*)(xmm\d)\.f\[0\] = 1\.0f / (?:sqrtf\((xmm\d)\.f\[0\]\)|(xmm\d)\.f\[0\]); /\* (rsqrtps|rcpps) \(low lane; 4-lane model TODO\) \*/\s*$")
    lines = text.split("\n")
    for i, line in enumerate(lines):
        m = todo.match(line)
        if m:
            indent, op, dst, src = m.groups()
            if op not in compares:
                raise ValueError("Unsupported SSE comparison: " + line)
            if src.startswith("xmm"):
                rhs = src + ".m"
            else:
                regname, sign, offset = re.fullmatch(r"dword ptr \[(esp|ebp) ([+-]) (0x[0-9A-Fa-f]+)\]", src).groups()
                nearby = "\n".join(lines[max(0, i - 400):i])
                base = "l_" + regname if re.search(r"\bl_" + regname + r"\b", nearby) else regname
                rhs = f"XMM_SCALAR(MEMF({base} {sign} {offset})).m"
            lines[i] = f"{indent}{dst} = RecompXmmOf({compares[op]}({dst}.m, {rhs})); /* {op} {dst}, {src} (was a TODO) */"
            continue
        m = reciprocal.match(line)
        if m:
            indent, dst, rsqrt_src, rcp_src, op = m.groups()
            denominator = f"_mm_sqrt_ps({rsqrt_src}.m)" if rsqrt_src else f"{rcp_src}.m"
            lines[i] = f"{indent}{dst} = RecompXmmOf(_mm_div_ps(_mm_set1_ps(1.0f), {denominator})); /* {op}, all four lanes */"
    return "\n".join(lines)


def optimize_apu(text):
    text = text.replace("else { uint32_t _i; for (_i = 0; _i < l_ecx; _i++) MEM32(l_edi + _i*4) = MEM32(l_esi + _i*4); }",
                        "else { black_copy32_slow(l_edi, l_esi, l_ecx); }")
    read = re.compile(r"^(\s*)((?:l_[a-z]+|ebp)) = MEM32\((-\d+|0x[0-9A-Fa-f]+u?)\);(.*)$")
    write = re.compile(r"^(\s*)MEM32\((-\d+|0x[0-9A-Fa-f]+u?)\) = ([^;]*);(.*)$")
    def address(s):
        return int(s.rstrip("u"), 0) & 0xffffffff
    lines = []
    for line in text.split("\n"):
        m = read.match(line)
        if m and 0xFE800000 <= address(m[3]) <= 0xFE87FFFF:
            line = f"{m[1]}{m[2]} = APU_R32(0x{address(m[3]):08X}u);{m[4]}"
        else:
            m = write.match(line)
            if m and 0xFE800000 <= address(m[2]) <= 0xFE87FFFF and "MEM" not in m[3]:
                line = f"{m[1]}APU_W32(0x{address(m[2]):08X}u, {m[3]});{m[4]}"
        lines.append(line)
    return "\n".join(lines)


def replace_once(text, old, new):
    if text.count(old) != 1:
        raise ValueError(f"Expected one hook location, found {text.count(old)}: {old}")
    return text.replace(old, new, 1)


def audio_completion_queue(name, body):
    if name != "sub_0021EC97":
        return body
    start = "loc_0021EE37: ;\n"
    end = "    l_eax = MEM32(l_esi + 8);\n    MEM32(l_eax + 0x4B8)"
    replacement = "    l_ecx = black_audio_queue_completion(l_esi);\n"
    if body.count(start) != 1 or body.count(end) != 1:
        raise ValueError("Audio completion enqueue boundary changed")
    begin = body.index(start) + len(start)
    stop = body.index(end, begin)
    if body[begin:stop] == replacement:
        return body
    # Keep the native status write before this block, plus the work flags and
    # KeInsertQueueDpc after it. Only the embedded-list insertion is coalesced.
    required = ("l_ecx = MEM32(l_esi + 8);", "l_eax = l_esi + 0x54;",
                "l_ecx = l_ecx + 0x4A0;", "MEM32(l_eax) = l_ecx;",
                "l_ecx = MEM32(l_ecx + 4);", "MEM32(l_eax + 4) = l_ecx;",
                "MEM32(l_ecx) = l_eax;", "l_ecx = MEM32(l_eax);",
                "MEM32(l_ecx + 4) = l_eax;")
    insertion = body[begin:stop]
    operations = tuple(line.strip() for line in strip_comments(insertion).splitlines()
                       if line.strip() and not line.strip().startswith(
                           ("_fa =", "_fb =", "_fas =", "_fbs =")))
    if operations != required:
        raise ValueError("Audio completion link contract changed")
    return body[:begin] + replacement + body[stop:]


def weapon_runtime_hooks(name, body):
    # Keep the native reward byte intact. Silver affects bank keys, loading,
    # registration, unloading and weapon IDs; ammunition affects only its
    # native reserve-refill branches and HUD reads.
    silver_reads = {
        "sub_001B7390": ("SET_HI8(l_ecx, MEM8(l_eax + 0x5CB0));",),
        "sub_001D3790": ("SET_HI8(l_ecx, MEM8(l_eax + 0x5CB0));",),
        "sub_001D4F30": ("SET_LO8(l_ecx, MEM8(l_eax + 0x5CB0));",),
        "sub_001EBD60": ("SET_HI8(l_ecx, MEM8(ebp + 0x5CB0));",),
        "sub_001EDC90": ("SET_HI8(l_ecx, MEM8(ebp + 0x5CB0));",
                          "SET_LO8(l_eax, MEM8(ebp + 0x5CB0));",
                          "SET_HI8(l_eax, MEM8(ebp + 0x5CB0));"),
    }
    ammo_reads = {
        "sub_0012CEC0": ("SET_LO8(l_edx, MEM8(l_ecx + 0x5CB0));",),
        "sub_0013A920": ("SET_LO8(l_eax, MEM8(l_ecx + 0x5CB0));",),
        "sub_001EEFD0": ("SET_LO8(l_ecx, MEM8(l_edx + 0x5CB0));",
                          "SET_LO8(l_eax, MEM8(l_edx + 0x5CB0));"),
    }
    for reads, helper in ((silver_reads, "black_silver_weapon_flag"),
                          (ammo_reads, "black_infinite_ammo_flag")):
        for original in reads.get(name, ()):
            hooked = original.replace("MEM8(", helper + "(MEM8(").replace("));", ")));", 1)
            if hooked in body:
                if body.count(hooked) != 1 or original in body:
                    raise ValueError("Weapon runtime read contract changed: " + original)
            else:
                body = replace_once(body, original, hooked)
    return body


def title_hooks(name, body):
    body = audio_completion_queue(name, body)
    body = weapon_runtime_hooks(name, body)
    # Address metadata selects locations. Original instructions are generated
    # from the local XBE, rather than stored as copied function bodies here.
    if name == "sub_0007EA80":
        lines = body.split("\n")
        for i in range(len(lines) - 1, -1, -1):
            if "uint64_t _dividend =" in lines[i]:
                divide = next(line for line in lines[i:i + 8] if "_dividend /" in line)
                divisor = re.search(r"_dividend / (?:\(uint32_t\))?(l_\w+)", divide)[1]
                note = "hand patch, as below" if divisor == "l_edi" else "hand patch: a zero divisor faults the host (a console would have too); keep the value instead"
                lines.insert(i, f"    if ({divisor} == 0) {divisor} = 1; /* {note} */")
        body = "\n".join(lines)
    if name in {"sub_000C8F40", "sub_001CDA30"}:
        body = body.replace("if ((_fca <= _fcb))", "if (!(_fca > _fcb))")
    if name == "sub_001DAFF0":
        body = replace_once(body, "if ((_fca <= _fcb)) goto loc_001DB18D;", "if (!(_fca > _fcb)) goto loc_001DB18D;")
    if name == "sub_0016EA90":
        body = replace_once(body, RELEASE.text("MEMF({0x295CA8})"), "black_osc_factor()")
    if name == "sub_001BD3F0_gen":
        body, count = re.subn(r"(xmm[01]\.f\[0\] = xmm[01]\.f\[0\] \* )(MEMF\(l_esi \+ 0xCC\))", r"\1black_ctl_k(\2)", body)
        if count != 2:
            raise ValueError("Angular filter hook missing")
    if name == "sub_001F9130":
        label = "loc_001F91A5: ;\n"
        start = body.index(label) + len(label)
        end = body.index("\n\n", start)
        body = body[:start] + "    if (!black_idle_yield_skip()) {\n" + body[start:end] + "\n    }" + body[end:]
    if name == "sub_001EDC90":
        lines = body.split("\n")
        first = next(i for i, line in enumerate(lines) if "SET_LO8(l_edx," in line and "0x2020C" in line)
        second = next(i for i, line in enumerate(lines) if "SET_LO8(l_eax," in line and "0x2020E" in line)
        line = lines.pop(second)
        lines.insert(first + 1, line)
        lines.insert(first + 2, "    { uint8_t lv_ = LO8(l_edx), sg_ = LO8(l_eax); black_level_override(&lv_, &sg_); SET_LO8(l_edx, lv_); SET_LO8(l_eax, sg_); } /* lab hook, RECOMP_BLACK_LEVEL */")
        body = "\n".join(lines)
    if name in {"sub_00240683_gen", "sub_00241074"}:
        location = "002408D8" if name == "sub_00240683_gen" else "002411ED"
        label = f"loc_{location}: ;\n"
        start = body.index(label) + len(label)
        end = body.index("\n", start)
        line = body[start:end]
        if "if (_flags /* je: equal / zero */)" not in line:
            raise ValueError("XMV branch hook missing")
        line = line.replace("if (_flags /* je: equal / zero */)", "if (_fa == 0)")
        body = body[:start] + f"    /* Native JE at 0x{location}: both predecessors carry the coefficient sign in _fa. */\n" + line + body[end:]
    if name == "sub_0026D0C0":
        line = next(line for line in body.splitlines() if "MEM32(l_eax + 0x100410) = l_ecx;" in line)
        body = replace_once(body, line, line + "\n    MEM32(l_eax + 0x100410) = l_ecx & ~0x10000u;      /* the flush completes at once */")
    flips = {"sub_0026A420": [("0026A6D8", "l_eax", "4", "l_edi")],
             "sub_00276E80": [("00276F6C", "l_edx", "0x1C", "ebp"), ("00276F90", "l_eax", "0x14", "ebp")],
             "sub_00277020": [("00277106", "l_eax", "4", "ebp")]}
    for location, register, offset, value in flips.get(name, []):
        line = f"    MEM32({register} + {offset}) = {value};"
        start = body.index(f"loc_{location}: ;\n")
        next_label = re.search(r"^loc_", body[start + 1:], re.M)
        end = start + 1 + next_label.start() if next_label else len(body)
        segment = replace_once(body[start:end], line, line + f"\n    black_movie_tag_flip({register} + {offset});")
        body = body[:start] + segment + body[end:]
    if name == "sub_001314E0_gen":
        label = "loc_00132AB7: ;\n"
        start = body.index(label) + len(label)
        end = body.index("\n", start)
        body = body[:end] + "\n    black_pickup_text_ready(g_esi); /* bounded native pickup caption; guest state preserved */" + body[end:]
    body = weapon_trace_hooks(name, body)
    body = weapon_model_hooks(name, body)
    body = viewmodel_hooks(name, body)
    body = unsigned_division_hooks(name, body)
    body = test_weapon_hooks(name, body)
    body = scope_mask_boundary(name, body)
    return body


def scope_mask_boundary(name, body):
    """Retain the shape callback ABI and its native registration/dispatch route."""
    if name == "sub_00121CB0":
        for marker in (RELEASE.text("MEM32({0x562BD8}) = {0x121B50};"),
                       RELEASE.text("MEM32({0x562BA8}) = {0x121BC0};")):
            if body.count(marker) != 1:
                raise ValueError("Native shape/glyph callback registration changed")
        return body
    if name == "sub_00102840_gen":
        start, end = "loc_00102D59: ;", "loc_00102D67: ;"
        if body.count(start) != 1 or body.count(end) != 1:
            raise ValueError("Native shape callback consumer changed")
        branch = body[body.index(start):body.index(end)]
        markers = ("edx = MEM32(ebp + 0xC);", "eax = MEM32(ecx + 0x18);",
                   "PUSH32(esp, edx);", "PUSH32(esp, eax);",
                   RELEASE.text("_icall_target = MEM32({0x562BD8}); PUSH32(esp, {0x00102D67u});"))
        previous = -1
        for marker in markers:
            if branch.count(marker) != 1 or branch.index(marker) <= previous:
                raise ValueError("Native shape callback arguments/target changed")
            previous = branch.index(marker)
        return body
    if name != "sub_00121B50_gen":
        return body
    for marker in ("loc_00121B50: ;", "loc_00121B69: ;", "loc_00121B89: ;",
                   "loc_00121BA9: ;", "l_eax = MEM32(ebp + 0xC);",
                   RELEASE.text("RECOMP_ABI_CALL({0x000DA2B0u}, sub_000DA2B0)"),
                   RELEASE.text("RECOMP_ABI_CALL({0x000DA3A0u}, sub_000DA3A0)")):
        if body.count(marker) != 1:
            raise ValueError("Native scope leaf draw boundary changed")
    for operand in ("l_edx = MEM32(ebp + 8);", "l_ecx = l_eax + 0x3740;"):
        if body.count(operand) != 2:
            raise ValueError("Native scope geometry/affine ownership changed")
    return body


def weapon_trace_hooks(name, body):
    """Insert bounded, opt-in observations at recovered native drop boundaries.

    Kept separate so a private diagnostic build can add these hooks to a prior
    generated baseline without re-running or changing any game translation.
    """
    sites = {
        "sub_001E68B0": [("001E68B0", "1u, l_eax")],
        "sub_001E7090": [("001E7090", "2u, MEM32(l_esp + 4)")],
        "sub_001E0EB0": [("001E0EB0", "3u, l_eax"),
                          ("001E0FCE", "4u, l_esi")],
    }
    for label, arguments in sites.get(name, []):
        marker = f"loc_{label}: ;\n"
        hook = f"    black_weapon_trace({arguments});\n"
        if marker + hook not in body:
            body = replace_once(body, marker, marker + hook)
    render_sites = {
        "sub_001D25D0": ("001D25D0", "black_weapon_attachment_trace(1u, l_ecx);"),
        "sub_001C6150": ("001C6150", "black_weapon_attachment_trace(2u, l_ecx);"),
        "sub_001C5490": ("001C5490", "black_weapon_attachment_trace(3u, l_edi);"),
        "sub_0016B1E0": ("0016B1E0", "black_viewmodel_muzzle_gate(l_esi);"),
    }
    if name in render_sites:
        label, call = render_sites[name]
        marker = f"loc_{label}: ;\n"
        hook = f"    {call}\n"
        if marker + hook not in body:
            body = replace_once(body, marker, marker + hook)
    return body


def test_weapon_hooks(name, body):
    """Select a validated offline lab loadout before ordinary native creation."""
    if name == "sub_001EDC90":
        marker = "loc_001EDD8F: ;\n    MEM32(ebp + 0x5AF4) = l_eax;\n"
        hook = "    black_test_stream_unit(ebp); /* explicit offline lab checkpoint */\n"
        if body.count(marker) != 1:
            raise ValueError("Native streaming initialization boundary changed")
        if marker + hook not in body:
            body = replace_once(body, marker, marker + hook)
        return body
    if name != "sub_001B7800":
        return body
    marker = "loc_001B78A0: ;\n"
    original = "    l_edx = MEM32(ebp + 0x28);\n"
    hook = "    black_test_weapon_loadout(l_esi); /* explicit offline lab control */\n"
    if body.count(marker) != 1 or body.count(original) != 1:
        raise ValueError("Native player loadout initialization boundary changed")
    if marker + hook + original not in body:
        body = replace_once(body, marker + original, marker + hook + original)
    return body


def viewmodel_hooks(name, body):
    """Carry first-person ownership through native queues into constant upload."""
    sites = {
        "sub_0015A8F0": ("0015A8F0", "black_viewmodel_owner(0u);"),
        "sub_001D25D0": ("001D25D0", "black_viewmodel_owner(l_ecx);"),
        "sub_00172EB0": ("00172EB0", "uint32_t black_vm_previous = black_viewmodel_draw_begin(MEM32(l_esp + 20));"),
        "sub_000D4A50": ("000D4A50", "uint32_t black_vm_caller = MEM32(l_esp);"),
    }
    if name in sites:
        label, call = sites[name]
        marker = f"loc_{label}: ;\n"
        hook = f"    {call}\n"
        if marker + hook not in body:
            body = replace_once(body, marker, marker + hook)
    if name == "sub_00172EB0" and "black_viewmodel_draw_end(black_vm_previous);" not in body:
        lines = body.splitlines()
        returns = [line for line in lines if "return;" in line]
        if len(returns) != 1 or "/* ret 20 */" not in returns[0]:
            raise ValueError("First-person draw exit changed")
        body = replace_once(body, returns[0], "    black_viewmodel_draw_end(black_vm_previous);\n" + returns[0])
    if name == "sub_0016F610" and "black_viewmodel_constants(l_edx);" not in body:
        calls = [line for line in body.splitlines() if RELEASE.text("PUSH32(l_esp, {0x0016F680u})") in line]
        if len(calls) != 1 or "sub_00269930" not in calls[0]:
            raise ValueError("First-person projection upload changed")
        body = replace_once(body, calls[0], "    black_viewmodel_constants(l_edx);\n" + calls[0])
    if name == "sub_000D4A50" and "black_viewmodel_muzzle_constants(l_edx, black_vm_caller);" not in body:
        calls = [line for line in body.splitlines() if RELEASE.text("PUSH32(l_esp, {0x000D4AFDu})") in line]
        if len(calls) != 1 or "sub_00269930" not in calls[0]:
            raise ValueError("First-person muzzle projection upload changed")
        body = replace_once(body, calls[0], "    black_viewmodel_muzzle_constants(l_edx, black_vm_caller);\n" + calls[0])
    return body


def unsigned_division_hooks(name, body):
    """Restore the four omitted carry rotations in native unsigned CRT math.

    These helpers normalize a 64-bit divisor/numerator as paired SHR/RCRs.
    SHR already publishes _cf. The following SHR or OR overwrites all flags
    besides that consumed carry, so no other flag state needs reconstruction.
    Keep the original arithmetic correction, register saves and stack return.
    """
    if name not in {"sub_000A30B0", "sub_000A3380"}:
        return body
    if "int _cf = 0;" not in body:
        raise ValueError("Unsigned CRT helper no longer tracks carry")
    for register in ("ebx", "eax"):
        marker = f"    /* TODO: rcr {register}, 1 */"
        replacement = (
            f"    {{ uint32_t black_rcr_out = l_{register} & 1u; "
            f"l_{register} = (l_{register} >> 1) | ((uint32_t)(_cf & 1) << 31); "
            f"_cf = (int)black_rcr_out; }} /* native rcr {register}, 1 */")
        if replacement in body:
            if body.count(replacement) != 1 or marker in body:
                raise ValueError("Ambiguous unsigned CRT carry rotation")
            continue
        body = replace_once(body, marker, replacement)
    return body


def weapon_model_hooks(name, body):
    """Observe the original resolver result and its unchanged native slot write."""
    if name != "sub_001C5CD0":
        return body
    pairs = [
        ("loc_001C5CD0: ;\n", "    uint32_t black_model_name_low = MEM32(l_esp + 4), black_model_name_high = MEM32(l_esp + 8);\n"),
        ("loc_001C5D0F: ;\n", "    black_weapon_model_trace(1u, l_ebx, black_model_name_low, black_model_name_high, l_esi, l_edi, l_eax);\n"),
        ("    MEM32(l_ecx + 0xC) = l_eax;\n", "    black_weapon_model_trace(2u, l_ebx, black_model_name_low, black_model_name_high, l_esi, l_edi, l_eax);\n"),
    ]
    for marker, hook in pairs:
        if marker + hook not in body:
            if body.count(marker) != 1:
                raise ValueError("AI gun-model lookup boundary changed: " + marker)
            body = body.replace(marker, marker + hook)
    return body


def flatten_manual(source):
    text = source.read_text(encoding="utf-8")
    for include in re.findall(r'^#include "(black[^"\n]+\.inc)"', text, re.M):
        path = source.parent / include
        if path.is_file():
            text = text.replace(f'#include "{include}"', path.read_text(encoding="utf-8"))
        elif include == "black_xmv_idct.inc":
            text = text.replace(f'#include "{include}"', "extern void sub_002420C8_gen(void);")
        else:
            raise FileNotFoundError(path)
    for symbol in re.findall(r"^XMV_PART\((sub_[0-9A-F]{8}),", text, re.M):
        text += f"\nvoid {symbol}(void) {{}}\n"
    return text


def emit_idct(source, destination):
    match = next((m for m in FUNCTION.finditer(source) if m[1] == "sub_002420C8_gen"), None)
    if match is None:
        raise ValueError("Supported XMV IDCT function was not generated")
    # The SSE2 kernel is generated from the local translation. No original
    # instruction sequence is distributed with this generator.
    body = re.sub(r"\bl_(eax|ecx|edx|ebx|esi|edi|esp|mm[0-7])\b", r"\1", match[0])
    body = re.sub(r"STK(8|16|32)\(", r"MEM\1(", body)
    lines = body.splitlines()
    def loop(lo, hi):
        start = next(i for i, line in enumerate(lines) if line.startswith(lo + ":"))
        stop = next(i for i in range(start + 1, len(lines)) if lines[i].startswith(hi + ":"))
        return "\n".join("        " + translated for line in lines[start:stop]
                         if (translated := translate_mmx(line)) is not None)
    loop1 = loop("loc_002420ED", "loc_002422ED")
    loop2 = loop("loc_002422F8", "loc_0024252C")
    text = IDCT_PREFIX + loop1 + IDCT_MIDDLE + loop2 + IDCT_SUFFIX
    if not destination.is_file() or destination.read_text(encoding="utf-8") != text:
        destination.write_text(text, encoding="utf-8")


def audit_generated_sources(directory):
    """Summarize the exact transformed C and headers that the project compiles."""
    paths = sorted((*directory.glob("*.c"), *directory.glob("*.h")))
    if not paths:
        raise RuntimeError(f"No generated C or header files to audit in {directory}")

    digest = hashlib.sha256()
    files = []
    functions = todo_comments = failures = unresolved_sites = 0
    for path in paths:
        content = path.read_bytes()
        text = content.decode("utf-8")
        digest.update(path.name.encode("utf-8") + b"\0" + content + b"\0")
        files.append({
            "path": path.name,
            "bytes": len(content),
            "sha256": hashlib.sha256(content).hexdigest().upper(),
        })
        functions += len(re.findall(r"(?m)^void\s+\w+\s*\(void\)\s*\{", text))
        todo_comments += len(re.findall(r"(?m)^\s*/\* TODO:", text))
        failures += text.count("/* FAILED:")
        unresolved_sites += len(re.findall(
            r"(?m)^(?!\s*#define)\s*.*\bRECOMP_UNRESOLVED_STUB_HIT\(", text))

    return {
        "schema_version": 1,
        "source_sha256": digest.hexdigest().upper(),
        "source_file_count": len(files),
        "generated_function_definitions": functions,
        "remaining_todo_comments": todo_comments,
        "failed_translation_markers": failures,
        "unresolved_stub_trace_sites": unresolved_sites,
        "files": files,
    }


def audit_audio_completion_boundary(directory):
    text = "\n".join(path.read_text(encoding="utf-8")
                     for path in sorted(directory.glob("*.c")))
    contracts = (
        (r"\bl_ecx = black_audio_queue_completion\(l_esi\);", 1),
        (r"(?m)^void sub_0021C7EF_gen\(void\)\s*\{", 1),
        (r"(?m)^void sub_0021C7EF\(void\)\s*\{", 0),
        (RELEASE.text(r"RECOMP_ABI_CALL\({0x0021C7EFu}, sub_0021C7EF\)"), 1),
        (RELEASE.text(r"\{ {0x0021C7EFu}, \(recomp_func_t\)sub_0021C7EF \}"), 1),
    )
    for pattern, count in contracts:
        if len(re.findall(pattern, text)) != count:
            raise ValueError("Audio completion wrapper/enqueue binding changed: " + pattern)


def write_post_transform_audit(generated, output, raw_summary_path):
    """Persist final-source metrics and link them to the raw recompiler summary."""
    audit = audit_generated_sources(output)
    if audit["source_sha256"] != audit_generated_sources(generated)["source_sha256"]:
        raise RuntimeError("Copied generated sources differ from transformed generator output")
    raw_summary_bytes = raw_summary_path.read_bytes()
    raw_summary = json.loads(raw_summary_bytes)
    raw_archive_path = raw_summary_path.with_name(
        raw_summary_path.stem + ".raw" + raw_summary_path.suffix)
    # Keep the exact bytes whose digest the audit records. The original path
    # below becomes the augmented summary, so it cannot serve as this archive.
    with raw_archive_path.open("xb") as archive:
        archive.write(raw_summary_bytes)
    audit["raw_recompiler_summary"] = {
        "path": str(raw_archive_path),
        "sha256": hashlib.sha256(raw_summary_bytes).hexdigest().upper(),
        "unimplemented_instruction_sites": sum(
            len(addresses) for addresses in raw_summary.get("unimplemented", {}).values()),
    }
    audit_path = output / "post_transform_audit.json"
    audit_path.write_text(json.dumps(audit, indent=2) + "\n", encoding="utf-8")
    raw_summary["summary_stage"] = "raw_recompiler_before_black_transforms"
    raw_summary["post_transform_audit"] = {
        "path": str(audit_path),
        "source_sha256": audit["source_sha256"],
        "source_file_count": audit["source_file_count"],
        "generated_function_definitions": audit["generated_function_definitions"],
        "remaining_todo_comments": audit["remaining_todo_comments"],
        "failed_translation_markers": audit["failed_translation_markers"],
        "unresolved_stub_trace_sites": audit["unresolved_stub_trace_sites"],
    }
    raw_summary_path.write_text(json.dumps(raw_summary, indent=2) + "\n", encoding="utf-8")
    return audit


def release_seeds(source, destination):
    """The repository's seed functions, at the local release's addresses."""
    if RELEASE.canonical:
        return source
    seeds = json.loads(source.read_text(encoding="utf-8"))
    for seed in seeds:
        seed["start"] = "0x%08X" % RELEASE.to_guest(int(seed["start"], 16))
    destination.write_text(json.dumps(seeds, indent=2) + "\n", encoding="utf-8")
    return destination


def canonicalize_generated(directory):
    """Rename generated sub_X/loc_X symbols to canonical (NTSC) addresses.

    Hooks and recomp_manual.c select functions and labels by these names.
    Literal guest addresses in the code are left at the release's values.
    """
    if RELEASE.canonical:
        return
    for path in sorted((*directory.glob("*.c"), *directory.glob("*.h"))):
        text = path.read_text(encoding="utf-8")
        renamed = RELEASE.rename_symbols(text)
        if renamed != text:
            path.write_text(renamed, encoding="utf-8")


def write_release_header(destination):
    text = RELEASE.header()
    if not destination.is_file() or destination.read_text(encoding="utf-8") != text:
        destination.write_text(text, encoding="utf-8")


def command(toolkit, module, arguments):
    print(f"Running {module}", flush=True)
    subprocess.run([sys.executable, "-m", module, *map(str, arguments)], cwd=toolkit, check=True)


def merge_icall_feedback(toolkit, feedback_db, feedback_dumps):
    """Merge observations, treating an empty runtime dump as a harmless no-op."""
    if not feedback_dumps:
        return False
    # Use the runtime's parser so truncation and zero flags have exactly the
    # same meaning here as in its merge command. Keep toolkit imports isolated
    # from this title's Python modules, and propagate genuine parser failures.
    probe = subprocess.run(
        [sys.executable, "-c",
         "import json, sys; "
         "from tools.recomp.icall_feedback import parse_dump; "
         "print(json.dumps(any(parse_dump(path) for path in sys.argv[1:])))",
         *map(str, feedback_dumps)],
        cwd=toolkit, check=True, stdout=subprocess.PIPE, text=True)
    if not json.loads(probe.stdout):
        print("No runtime indirect-call observations to merge", flush=True)
        return False
    command(toolkit, "tools.recomp.icall_feedback",
            ["--db", feedback_db, "merge", *feedback_dumps])
    return True


def run_disasm(toolkit, xbe, analysis, output, seed_files=None):
    arguments = [xbe, "--analysis-json", analysis, "-o", output, "--force"]
    if seed_files is not None:
        if isinstance(seed_files, (str, Path)):
            seed_files = [seed_files]
        for seed_file in seed_files:
            arguments.extend(["--seed-functions", seed_file])
    command(toolkit, "tools.disasm", arguments)


IDCT_PREFIX = '/* Locally generated XMV SSE2 kernel. Do not distribute this generated file. */\n#include <emmintrin.h>\n\nextern void sub_002420C8_gen(void);\nextern __declspec(thread) uint32_t g_ebp;\nextern __declspec(thread) uint32_t g_seh_ebp;\n\nstatic int black_idct_mode = -1;                 /* 0 generated, 1 native, 2 verify */\nstatic unsigned black_idct_verified, black_idct_bad;\n\n/* MMX pack/unpack on the low 64 bits of an SSE register */\nstatic __forceinline __m128i idct_pack_ssdw(__m128i a, __m128i b) { __m128i x = _mm_unpacklo_epi64(a, b); return _mm_packs_epi32(x, x); }\nstatic __forceinline __m128i idct_pack_uswb(__m128i a, __m128i b) { __m128i x = _mm_unpacklo_epi64(a, b); return _mm_packus_epi16(x, x); }\nstatic __forceinline __m128i idct_unpackhi_dq(__m128i a, __m128i b) { return _mm_unpacklo_epi32(_mm_srli_epi64(a, 32), _mm_srli_epi64(b, 32)); }\n#define pack_ssdw idct_pack_ssdw\n#define pack_uswb idct_pack_uswb\n#define unpackhi_dq idct_unpackhi_dq\n\ntypedef struct IdctOut { uint32_t eax, ecx, edx; } IdctOut;\n\n/* sp = guest esp at entry (the return address). Returns the registers the generated code leaves behind. */\nstatic IdctOut idct_native(uint32_t sp)\n{\n    uint8_t *mem = (uint8_t *)(uintptr_t)g_xbox_mem_offset;\n    uint32_t r_ebp = sp - 4;                              /* push ebp; mov ebp, esp */\n    uint32_t arg_dst = GUEST32(sp + 4), arg_stride = GUEST32(sp + 8), arg_coef = GUEST32(sp + 12);\n    uint32_t scratch = (r_ebp - 268) & 0xFFFFFFE0u;\n    uint32_t r_eax = 0, r_ebx = 0, r_ecx, r_edx, r_esi, r_edi;\n    __m128i m0 = _mm_setzero_si128(), m1 = m0, m2 = m0, m3 = m0, m4 = m0, m5 = m0, m6 = m0, m7 = m0;\n    IdctOut out;\n\n    r_esi = arg_coef;\n    r_edi = scratch;\n    for (r_ecx = 0xFFFFFFFCu; (int32_t)r_ecx < 0; ) {\n'

IDCT_MIDDLE = '\n    }\n    r_ecx = 0;\n    r_esi = scratch;\n    r_edi = arg_dst;\n    r_edx = arg_stride;\n    for (;;) {\n'

IDCT_SUFFIX = '\n        if (r_ecx == 8) break;\n    }\n    (void)r_ebx;\n    out.eax = r_eax; out.ecx = r_ecx; out.edx = r_edx;\n    return out;\n}\n\nstatic void idct_run_native(uint32_t sp)\n{\n    IdctOut o = idct_native(sp);\n    g_eax = o.eax; g_ecx = o.ecx; g_edx = o.edx;\n    g_ebp = sp - 4; g_seh_ebp = sp - 4;                   /* the frame the generated code leaves published */\n    g_esp = sp + 16;                                      /* ret 12 */\n}\n\n/* The dispatcher the profile wrapper calls. */\nstatic void idct_dispatch(void)\n{\n    uint32_t sp = g_esp;\n    if (black_idct_mode < 0) {\n        const char *e = getenv("RECOMP_BLACK_NATIVE_IDCT");\n        black_idct_mode = e && !strcmp(e, "verify") ? 2 : (e && *e == \'0\') ? 0 : 1;\n        fprintf(stderr, "[BLACK] XMV inverse DCT: %s\\n",\n                black_idct_mode == 0 ? "generated code" : black_idct_mode == 1 ? "native SSE2" : "native, verified against the generated code");\n    }\n    if (black_idct_mode == 2 && black_idct_verified < 3000) {\n        uint8_t *mem = (uint8_t *)(uintptr_t)g_xbox_mem_offset;\n        uint32_t dst = GUEST32(sp + 4), stride = GUEST32(sp + 8), row;\n        uint8_t before[8][8], mine[8][8];\n        uint32_t s_eax = g_eax, s_ecx = g_ecx, s_edx = g_edx, s_ebp = g_ebp, s_seh = g_seh_ebp, s_esp = g_esp;\n        uint32_t n_eax, n_ecx, n_edx, n_ebp, n_seh, n_esp;\n        int bad = 0;\n        for (row = 0; row < 8; row++) memcpy(before[row], mem + dst + row * stride, 8);\n        idct_run_native(sp);\n        for (row = 0; row < 8; row++) memcpy(mine[row], mem + dst + row * stride, 8);\n        n_eax = g_eax; n_ecx = g_ecx; n_edx = g_edx; n_ebp = g_ebp; n_seh = g_seh_ebp; n_esp = g_esp;\n        for (row = 0; row < 8; row++) memcpy(mem + dst + row * stride, before[row], 8);\n        g_eax = s_eax; g_ecx = s_ecx; g_edx = s_edx; g_ebp = s_ebp; g_seh_ebp = s_seh; g_esp = s_esp;\n        sub_002420C8_gen();\n        for (row = 0; row < 8; row++) if (memcmp(mem + dst + row * stride, mine[row], 8)) bad = 1;\n        if (n_eax != g_eax || n_ecx != g_ecx || n_edx != g_edx || n_ebp != g_ebp || n_seh != g_seh_ebp || n_esp != g_esp) bad |= 2;\n        black_idct_verified++;\n        if (bad) {\n            black_idct_bad++;\n            if (black_idct_bad <= 8)\n                fprintf(stderr, "[BLACK] XMV IDCT verify: %s differ (call %u; eax %08X/%08X ecx %08X/%08X edx %08X/%08X ebp %08X/%08X esp %08X/%08X)\\n",\n                        bad & 1 ? "bytes" : "registers", black_idct_verified, n_eax, g_eax, n_ecx, g_ecx, n_edx, g_edx, n_ebp, g_ebp, n_esp, g_esp);\n        }\n        if (black_idct_verified == 3000 || black_idct_verified == 1)\n            fprintf(stderr, "[BLACK] XMV IDCT verify: %u calls, %u mismatching\\n", black_idct_verified, black_idct_bad);\n        return;\n    }\n    if (black_idct_mode == 0) { sub_002420C8_gen(); return; }\n    idct_run_native(sp);\n}\n'

def main():
    root = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--toolkit", type=Path, default=root / "third_party/xboxrecomp")
    parser.add_argument("--xbe", type=Path, default=root / "game/default.xbe")
    parser.add_argument("--output", type=Path, default=root / "src/recomp/gen")
    parser.add_argument("--icall-db", type=Path,
                        default=root / "local/icall_targets.json",
                        help="cumulative runtime ICALL feedback database")
    parser.add_argument("--icall-dump", type=Path, action="append", default=None,
                        help="runtime ICALL dump to merge before generation; may be repeated")
    args = parser.parse_args()
    toolkit, xbe, output = args.toolkit.resolve(), args.xbe.resolve(), args.output.resolve()
    if not xbe.is_file():
        parser.error("Place the supported original Xbox BLACK files in game/ first")
    global RELEASE
    release = black_release.detect(hashlib.sha256(xbe.read_bytes()).hexdigest())
    if release is None:
        parser.error("The XBE does not match a supported retail release")
    RELEASE = release
    print(f"Detected the {release.name.upper()} release", flush=True)
    work = root / ".work" / ("generate-" + datetime.datetime.now(datetime.timezone.utc).strftime("%Y%m%dT%H%M%S.%fZ"))
    work.mkdir(parents=True)
    analysis = work / "xbe-analysis.json"
    disasm, identified, abi = work / "disasm", work / "func-id", work / "abi"
    manual = work / "manual-scan.c"
    # The recompiler skips and wraps functions by address, so it must see the
    # local release's addresses. Generated names are made canonical below.
    manual.write_text(RELEASE.guest_symbols(flatten_manual(root / "src/recomp_manual.c")),
                      encoding="utf-8")
    command(toolkit, "tools.xbe_parser", [xbe, "--json", analysis])
    run_disasm(toolkit, xbe, analysis, disasm)
    feedback_db = args.icall_db.resolve()
    if not RELEASE.canonical and feedback_db == (root / "local/icall_targets.json").resolve():
        # Observed targets are guest addresses; keep each release's apart.
        feedback_db = feedback_db.with_name(f"icall_targets.{RELEASE.name}.json")
    feedback_dumps = [path.resolve() for path in (args.icall_dump or [])]
    default_dump = root / "local/icall_targets.dump"
    if default_dump.is_file() and default_dump not in feedback_dumps:
        feedback_dumps.append(default_dump)
    merge_icall_feedback(toolkit, feedback_db, feedback_dumps)
    seed_files = []
    known_seeds = root / "scripts/icall_seed_functions.json"
    if known_seeds.is_file():
        seed_files.append(release_seeds(known_seeds, work / "known-seeds.json"))
    if feedback_db.is_file():
        seeds = work / "icall-seeds.json"
        command(toolkit, "tools.recomp.icall_feedback",
                ["--db", feedback_db,
                 "--functions", disasm / "functions.json",
                 "seeds", "--out", seeds, "--xbe", xbe,
                 "--analysis-json", analysis])
        if json.loads(seeds.read_text(encoding="utf-8")):
            seed_files.append(seeds)
    if seed_files:
        run_disasm(toolkit, xbe, analysis, disasm, seed_files)
    command(toolkit, "tools.func_id", [xbe, "--functions", disasm / "functions.json", "--strings", disasm / "strings.json", "--xrefs", disasm / "xrefs.json", "-o", identified])
    command(toolkit, "tools.abi_analysis", [xbe, "--disasm-dir", disasm, "--func-id-dir", identified, "--output-dir", abi])
    generated = work / "gen"
    command(toolkit, "tools.recomp", [xbe, "--all", "--split", "1000", "--game-name", "BLACK", "--disasm-dir", disasm, "--func-id-dir", identified, "--abi-dir", abi, "--gen-dir", generated, "--exclude-manual", manual, "--output-dir", work / "summary"])
    canonicalize_generated(generated)
    chunks = sorted(generated.glob("recomp_[0-9]*.c"))
    emit_idct("\n".join(path.read_text(encoding="utf-8") for path in chunks), root / "src/black_xmv_idct.inc")
    declarations = "\nextern float black_osc_factor(void);\nextern float black_ctl_k(float);\nextern void black_movie_tag_flip(uint32_t);\nextern void black_pickup_text_ready(uint32_t);\nextern void black_weapon_trace(uint32_t, uint32_t);\nextern void black_weapon_model_trace(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t);\nextern void black_weapon_attachment_trace(uint32_t, uint32_t);\nextern void black_viewmodel_muzzle_gate(uint32_t);\nextern void black_test_weapon_loadout(uint32_t);\nextern void black_viewmodel_owner(uint32_t);\nextern uint32_t black_viewmodel_draw_begin(uint32_t);\nextern void black_viewmodel_draw_end(uint32_t);\nextern void black_viewmodel_constants(uint32_t);\nextern void black_viewmodel_muzzle_constants(uint32_t, uint32_t);\n"
    declarations += "extern int black_silver_weapon_flag(int);\n"
    declarations += "extern int black_infinite_ammo_flag(int);\n"
    declarations += "\nextern void black_test_stream_unit(uint32_t);\n"
    declarations += "\nextern uint32_t black_audio_queue_completion(uint32_t);\n"
    for chunk in chunks:
        text = chunk.read_text(encoding="utf-8")
        text = rewrite_functions(text, promote_body)
        text = repair_sse(text)
        text = optimize_apu(text)
        text = rewrite_functions(text, title_hooks)
        text = replace_once(text, '#include <math.h>\n', '#include <math.h>\n' + declarations)
        chunk.write_text(text, encoding="utf-8")
    audit_audio_completion_boundary(generated)
    output.mkdir(parents=True, exist_ok=True)
    stale = set(output.glob("*.c")) - {output / path.name for path in generated.glob("*.c")}
    if stale:
        raise RuntimeError("Unexpected old generated source files; preserve them outside src/recomp/gen before retrying")
    for path in generated.iterdir():
        if path.is_file():
            destination = output / path.name
            if not destination.is_file() or destination.read_bytes() != path.read_bytes():
                shutil.copyfile(path, destination)

    write_release_header(output.parent / "black_release_gen.h")
    write_post_transform_audit(generated, output, work / "summary" / "summary.json")
    print(f"Generated {len(chunks)} source chunks from your XBE. Local analysis: {work}", flush=True)


if __name__ == "__main__":
    main()




