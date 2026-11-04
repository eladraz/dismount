#!/usr/bin/env python3
"""
Assembles the lines of a RISC-V corpus file with llvm-mc (Homebrew llvm 23) and returns the bytes of
every line. The .o file is an ELF, parsed here (no objcopy needed). dismount's own RISC-V assembler
can replace llvm-mc when it is merged (see TEST_PLAN.md).

Corpus line syntax (tests/riscv/semantics/corpus/*.s):
    <instruction>             # @feature: zba   # @state: x10=80100100 f1=7ff8000000000000
  * Blank lines and lines that start with `#` or `//` are comments. Comments after an instruction
    start with `#` or `//`; the tags are `@feature:` (informative), `@state:` (directed values for
    the random states, see states_rv.py), `@mode:` (vector, cfi), `@oracle:` (the oracles which run it),
    `@word:` (the 32 bit encoding in hex, for the reserved encodings llvm-mc refuses to assemble, such as
    a vector destination that overlaps a source: the text is for the state generator and the report).
  * `;` separates several instructions of one line (`lui a0, 1 ; addi a0, a0, 2`): the bytes of the
    line are the bytes of all of them, run as one unit.
  * The instructions are not compressed by the assembler (`.option norvc`); a `c.` instruction is
    assembled as itself.
"""
import os
import re
import struct
import subprocess
import tempfile

LLVM_MC = '/opt/homebrew/opt/llvm/bin/llvm-mc'
FEATURES = ('+m,+a,+f,+d,+c,+v,+zba,+zbb,+zbs,+zbc,+zbkb,+zbkc,+zbkx,+zknd,+zkne,+zknh,+zksed,+zksh,'
            '+zkr,+zfa,+zfh,+zfhmin,+zfbfmin,+zvfbfmin,+zvfbfwma,+zvfh,+zicond,+zcb,+zcmop,+zimop,+zawrs,'
            '+zacas,+zabha,+zicbom,+zicboz,+zicbop,+zihintpause,+zihintntl,+zvbb,+zvbc,+zvkb,+zvkg,'
            '+zvkned,+zvknha,+zvknhb,+zvksed,+zvksh,+zvkt,+experimental-zicfilp,+experimental-zicfiss,'
            '+zicsr,+zifencei,+zvl128b,+zvl256b,+zvl512b,+zve64d')
EXTRA = {64: '', 32: ',+zcf'}
TRIPLE = {64: 'riscv64', 32: 'riscv32'}


class AsmError(Exception):
    pass


def split_comment(raw):
    """(instruction text, comment) of a corpus line."""
    m = re.search(r'(#|//)', raw)
    if not m:
        return raw.strip(), ''
    return raw[:m.start()].strip(), raw[m.end():].strip()


def parse_corpus(path):
    """Returns the list of {'no', 'text', 'comment', 'tags'} of a corpus file."""
    lines = []
    with open(path, encoding='utf-8') as f:
        for no, raw in enumerate(f, 1):
            raw = raw.rstrip('\n')
            if not raw.strip() or raw.lstrip().startswith(('#', '//')):
                continue
            text, comment = split_comment(raw)
            if not text:
                continue
            tags = {}
            for m in re.finditer(r'@(feature|state|mode|oracle|word):\s*([^@]*)', comment):
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
        return b'', {}
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
                    symbols[blob[start:blob.index(b'\0', start)].decode()] = value
    return text, symbols


def _assemble_group(xlen, texts, header):
    """One llvm-mc run for the `texts` of one group. Returns [(hex or None, error or None)]."""
    results = [None] * len(texts)
    bad = {}
    for _ in range(64):
        src = [header]
        ranges = []
        number = header.count('\n')
        for i, t in enumerate(texts):
            body = 'nop\n' if i in bad else t.replace(';', '\n') + '\n'
            first = number + 2
            number += 1 + body.count('\n')
            ranges.append((first, number))
            src.append('rv_line_%d:\n%s' % (i, body))
        src.append('rv_line_end:\n')
        with tempfile.TemporaryDirectory() as tmp:
            s_path, o_path = os.path.join(tmp, 'a.s'), os.path.join(tmp, 'a.o')
            with open(s_path, 'w') as f:
                f.write(''.join(src))
            p = subprocess.run([LLVM_MC, '-triple=' + TRIPLE[xlen], '-mattr=' + FEATURES + EXTRA[xlen],
                                '-filetype=obj', s_path, '-o', o_path], capture_output=True, text=True)
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
    offsets = [symbols['rv_line_%d' % i] for i in range(len(texts))] + [symbols['rv_line_end']]
    for i in range(len(texts)):
        if i in bad:
            results[i] = (None, bad[i])
        else:
            results[i] = (text[offsets[i]:offsets[i + 1]].hex(), None)
    return results


def assemble(xlen, texts):
    """Assemble the instruction texts for RV`xlen`. Returns a list of (hex or None, error or None)
    in the order of `texts`. The `c.` instructions are assembled with the compressed extension on,
    the others with `.option norvc` (no automatic compression)."""
    results = [None] * len(texts)
    plain = [i for i, t in enumerate(texts) if not t.lstrip().startswith('c.')]
    comp = [i for i, t in enumerate(texts) if t.lstrip().startswith('c.')]
    for idxs, header in ((plain, '.text\n.option norvc\n'), (comp, '.text\n')):
        if not idxs:
            continue
        for i, r in zip(idxs, _assemble_group(xlen, [texts[i] for i in idxs], header)):
            results[i] = r
    return results
