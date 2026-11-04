#!/usr/bin/env python3
"""
Assembles the lines of an ARM corpus file with clang (the integrated assembler) and returns the
bytes of every line. No objcopy is needed: the .o file is an ELF, parsed here.

Corpus line syntax (tests/arm/semantics/corpus/*.s):
    <instruction>                 // @feature: sve   // @state: x1=4100008000 nzcv=4
  * Blank lines and lines that start with `//`, `@` or `#` are comments.
  * `;` separates several instructions of one line (a T32 `it` block: `it eq ; addeq r0, r1, r2`):
    the bytes of the line are the bytes of all of them, run as one unit.
  * `// @feature: <llvm feature>` is informative, `// @state: ` adds directed states (see
    states_arm.py), `// @mode: sm` runs the line in streaming mode, `// @mode: sve` in SVE mode.
"""
import os
import re
import struct
import subprocess
import tempfile

A64_TARGET = ['-target', 'aarch64-none-elf',
              '-march=armv9.4-a+sve2+sme2+sme-f64f64+sme-i16i64+sve2-aes+sve2-bitperm+sve2-sha3'
              '+sve2-sm4+fp16+bf16+i8mm+sha3+sm4+aes+sha2+crc+lse+rcpc+f32mm+f64mm+mops+cssc+rcpc3+lse128+faminmax+lut+ssbs+flagm+pauth+fp16fml'
              '+lsui+ls64+hbc+wfxt+pcdphint+predres']
A32_TARGET = ['-target', 'armv8.2a-none-eabi', '-march=armv8.2-a+fp16+dotprod+crypto',
              '-mfpu=crypto-neon-fp-armv8']
T32_TARGET = A32_TARGET
HEADERS = {
    'a64': '.text\n',
    'a32': '.syntax unified\n.arm\n.text\n',
    't32': '.syntax unified\n.thumb\n.text\n',
}


class AsmError(Exception):
    pass


def split_comment(raw):
    """(instruction text, comment) of a corpus line."""
    text, sep, comment = raw.partition('//')
    if not sep and '@' in text:                  # the AArch32 comment character
        text, sep, comment = raw.partition('@')
        # `@feature` style annotations are after `//`, a bare `@` starts a comment only at the start
    return text.strip(), comment.strip()


def parse_corpus(path):
    """Returns the list of {'no', 'text', 'comment', 'tags'} of a corpus file."""
    lines = []
    with open(path, encoding='utf-8') as f:
        for no, raw in enumerate(f, 1):
            raw = raw.rstrip('\n')
            if not raw.strip() or raw.lstrip().startswith(('//', '@', '#')):
                continue
            text, comment = split_comment(raw)
            if not text:
                continue
            tags = {}
            for m in re.finditer(r'@(feature|state|mode):\s*([^@]*)', comment):
                tags.setdefault(m.group(1), []).append(m.group(2).strip())
            lines.append({'no': no, 'text': text, 'comment': comment, 'tags': tags})
    return lines


def _elf_text_and_symbols(blob):
    """(the .text bytes, {symbol name: offset in .text}) of an ELF relocatable file (32 or 64 bit
    little endian)."""
    if blob[:4] != b'\x7fELF':
        raise AsmError('not an ELF file')
    is64 = blob[4] == 2
    if is64:
        shoff, = struct.unpack_from('<Q', blob, 0x28)
        shentsize, shnum, shstrndx = struct.unpack_from('<HHH', blob, 0x3A)
    else:
        shoff, = struct.unpack_from('<I', blob, 0x20)
        shentsize, shnum, shstrndx = struct.unpack_from('<HHH', blob, 0x2E)
    sections = []
    for i in range(shnum):
        off = shoff + i * shentsize
        if is64:
            name, typ, flags, addr, offset, size, link, info, align, entsize = \
                struct.unpack_from('<IIQQQQIIQQ', blob, off)
        else:
            name, typ, flags, addr, offset, size, link, info, align, entsize = \
                struct.unpack_from('<IIIIIIIIII', blob, off)
        sections.append((name, typ, offset, size, link, entsize))
    shstr = sections[shstrndx]

    def sname(n):
        start = shstr[2] + n
        return blob[start:blob.index(b'\0', start)].decode()

    text_index = None
    for i, s in enumerate(sections):
        if sname(s[0]) == '.text':
            text_index = i
    if text_index is None:
        raise AsmError('no .text section')
    text = blob[sections[text_index][2]:sections[text_index][2] + sections[text_index][3]]
    symbols = {}
    for s in sections:
        if s[1] == 2:                               # SHT_SYMTAB
            strtab = sections[s[4]]
            count = s[3] // s[5]
            for k in range(count):
                o = s[2] + k * s[5]
                if is64:
                    nm, info, other, shndx, value, size = struct.unpack_from('<IBBHQQ', blob, o)
                else:
                    nm, value, size, info, other, shndx = struct.unpack_from('<IIIBBH', blob, o)
                if shndx == text_index:
                    start = strtab[2] + nm
                    symbols[blob[start:blob.index(b'\0', start)].decode()] = value & ~1
    return text, symbols


def assemble(arch, texts, clang='clang', extra=()):
    """Assemble the instruction texts of `arch` ('a64', 'a32', 't32'). Returns a list of
    (hex or None, error or None) in the order of `texts`. A text with `;` is several
    instructions (their bytes are joined)."""
    target = {'a64': A64_TARGET, 'a32': A32_TARGET, 't32': T32_TARGET}[arch]
    results = [None] * len(texts)
    # One file with a symbol before every line. A line that doesn't assemble makes clang fail
    # for the whole file: the failing lines (reported with their line number) are replaced by
    # `nop` and the file is assembled again
    bad = {}
    for _ in range(64):
        src = [HEADERS[arch]]
        ranges = []
        number = HEADERS[arch].count('\n')
        for i, t in enumerate(texts):
            body = 'nop\n' if i in bad else t.replace(';', '\n') + '\n'
            first = number + 2                          # after the label line
            number += 1 + body.count('\n')
            ranges.append((first, number))
            src.append('arm_line_%d:\n%s' % (i, body))
        src.append('arm_line_end:\n')
        with tempfile.TemporaryDirectory() as tmp:
            s_path, o_path = os.path.join(tmp, 'a.s'), os.path.join(tmp, 'a.o')
            with open(s_path, 'w') as f:
                f.write(''.join(src))
            p = subprocess.run([clang] + target + list(extra) + ['-c', s_path, '-o', o_path],
                               capture_output=True, text=True)
            if p.returncode == 0:
                blob = open(o_path, 'rb').read()
                break
            offenders = 0
            for m in re.finditer(r'a\.s:(\d+):\d+: error: (.*)', p.stderr):
                lineno = int(m.group(1))
                for i, (first, last) in enumerate(ranges):
                    if first <= lineno <= last and i not in bad:
                        bad[i] = m.group(2).strip()
                        offenders += 1
                        break
            if not offenders:
                raise AsmError(p.stderr[:400])
    else:
        raise AsmError('too many assembler errors')
    text, symbols = _elf_text_and_symbols(blob)
    offsets = [symbols['arm_line_%d' % i] for i in range(len(texts))] + [symbols['arm_line_end']]
    for i in range(len(texts)):
        if i in bad:
            results[i] = (None, bad[i])
        else:
            results[i] = (text[offsets[i]:offsets[i + 1]].hex(), None)
    return results
