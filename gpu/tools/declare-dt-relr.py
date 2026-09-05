#!/usr/bin/env python3
"""Declare GLIBC_ABI_DT_RELR in an ELF that uses DT_RELR but never says so.

glibc refuses to load an object carrying DT_RELR relocations unless it declares
a dependency on libc.so.6's GLIBC_ABI_DT_RELR version. The declaration exists so
such an object fails loudly on a glibc too old to apply DT_RELR, rather than
running with its relocations silently skipped. Objects built against a private
loader -- ChromeOS's Mali blob among them -- omit it and are rejected elsewhere.

The fix is pure bookkeeping: append a copy of .dynstr (plus the version name)
and a copy of .gnu.version_r (plus one Vernaux) to the file, map them through
the spare PT_NOTE header, and repoint DT_STRTAB/DT_VERNEED. Nothing existing is
moved or overwritten; the only casualty is the build-id program header, which
the loader never reads.
"""
import struct, sys

PT_LOAD, PT_NOTE, PT_DYNAMIC = 1, 4, 2
DT_STRTAB, DT_STRSZ, DT_RELR = 5, 10, 36
DT_VERNEED, DT_VERNEEDNUM = 0x6ffffffe, 0x6fffffff
VERSION = b"GLIBC_ABI_DT_RELR"
ALIGN = 0x10000


def elf_hash(name):
    h = 0
    for c in name:
        h = ((h << 4) + c) & 0xffffffff
        g = h & 0xf0000000
        h ^= g >> 24
        h &= ~g & 0xffffffff
    return h


def main(src, dst):
    d = bytearray(open(src, "rb").read())
    e_phoff, e_shoff = struct.unpack_from("<QQ", d, 0x20)
    e_phentsize, e_phnum = struct.unpack_from("<HH", d, 0x36)
    e_shentsize, e_shnum, e_shstrndx = struct.unpack_from("<HHH", d, 0x3a)

    phdr = lambda i: e_phoff + i * e_phentsize
    phs = [struct.unpack_from("<IIQQQQQQ", d, phdr(i)) for i in range(e_phnum)]

    def va_to_off(va):
        for t, _, off, p_va, _, filesz, _, _ in phs:
            if t == PT_LOAD and p_va <= va < p_va + filesz:
                return off + (va - p_va)
        sys.exit(f"vaddr 0x{va:x} is not in any loaded segment")

    dyn, o = {}, next(p[2] for p in phs if p[0] == PT_DYNAMIC)
    while True:
        tag, val = struct.unpack_from("<Qq", d, o)
        if tag == 0:
            break
        dyn.setdefault(tag, (val, o))
        o += 16
    if DT_RELR not in dyn:
        sys.exit("no DT_RELR: nothing to declare")

    strtab_va, strtab_dyn = dyn[DT_STRTAB]
    strtab_off, strsz = va_to_off(strtab_va), dyn[DT_STRSZ][0]
    verneed_off, verneed_num = va_to_off(dyn[DT_VERNEED][0]), dyn[DT_VERNEEDNUM][0]

    # Walk the Verneed list, keeping enough to rebuild it verbatim.
    entries, o = [], verneed_off
    for _ in range(verneed_num):
        ver, cnt, file_off, aux, nxt = struct.unpack_from("<HHIII", d, o)
        auxes, ao = [], o + aux
        for _ in range(cnt):
            h, flags, other, name, anext = struct.unpack_from("<IHHII", d, ao)
            auxes.append([h, flags, other, name])
            ao += anext
        name = bytes(d[strtab_off + file_off:d.index(b"\0", strtab_off + file_off)])
        entries.append({"ver": ver, "file": file_off, "name": name, "aux": auxes})
        if nxt == 0:
            break
        o += nxt

    # The Vernaux hash must match what the loader computes, so prove the hash
    # function against the entries the linker already wrote.
    for e in entries:
        for h, _, _, name in e["aux"]:
            s = bytes(d[strtab_off + name:d.index(b"\0", strtab_off + name)])
            assert elf_hash(s) == h, f"hash mismatch on {s}: 0x{h:x}"

    target = next((e for e in entries if e["name"] == b"libc.so.6"), None)
    if target is None:
        sys.exit("no libc.so.6 dependency to hang the version on")
    if any(d[strtab_off + a[3]:].startswith(VERSION + b"\0") for a in target["aux"]):
        sys.exit("already declared; nothing to do")

    spare = next((i for i, p in enumerate(phs) if p[0] == PT_NOTE), None)
    if spare is None:
        sys.exit("no spare program header to turn into a segment")

    # .dynstr is full, so republish it with the version name appended.
    new_name = strsz
    dynstr = d[strtab_off:strtab_off + strsz] + VERSION + b"\0"
    used = {a[2] & 0x7fff for e in entries for a in e["aux"]}
    target["aux"].append([elf_hash(VERSION), 0, max(used) + 1, new_name])

    # Rebuild the table contiguously: every Verneed, then every Vernaux.
    vn, vna = 16, 16
    total = len(entries) * vn + sum(len(e["aux"]) for e in entries) * vna
    verneed = bytearray(total)
    cursor = len(entries) * vn
    for i, e in enumerate(entries):
        at = i * vn
        struct.pack_into("<HHIII", verneed, at, e["ver"], len(e["aux"]), e["file"],
                         cursor - at, vn if i + 1 < len(entries) else 0)
        for j, (h, flags, other, name) in enumerate(e["aux"]):
            struct.pack_into("<IHHII", verneed, cursor, h, flags, other, name,
                             0 if j + 1 == len(e["aux"]) else vna)
            cursor += vna

    # Park both tables past the end of the file, mapped read-only well clear of
    # the highest address the object already claims.
    blob_off = (len(d) + 15) & ~15
    verneed_rel = (len(dynstr) + 7) & ~7
    blob = bytearray(verneed_rel + total)
    blob[:len(dynstr)] = dynstr
    blob[verneed_rel:] = verneed
    top = max(p[3] + p[6] for p in phs if p[0] == PT_LOAD)
    blob_va = blob_off + ((top - blob_off + 2 * ALIGN - 1) & ~(ALIGN - 1))

    d += b"\0" * (blob_off - len(d)) + blob
    struct.pack_into("<IIQQQQQQ", d, phdr(spare), PT_LOAD, 4, blob_off, blob_va,
                     blob_va, len(blob), len(blob), ALIGN)
    struct.pack_into("<q", d, strtab_dyn + 8, blob_va)
    struct.pack_into("<q", d, dyn[DT_STRSZ][1] + 8, len(dynstr))
    struct.pack_into("<q", d, dyn[DT_VERNEED][1] + 8, blob_va + verneed_rel)

    # Keep the section headers in step so readelf still agrees with the loader.
    shstr = struct.unpack_from("<Q", d, e_shoff + e_shstrndx * e_shentsize + 24)[0]
    moved = {b".dynstr": (blob_va, blob_off, len(dynstr)),
             b".gnu.version_r": (blob_va + verneed_rel, blob_off + verneed_rel, total)}
    for i in range(e_shnum):
        so = e_shoff + i * e_shentsize
        nm = struct.unpack_from("<I", d, so)[0]
        name = bytes(d[shstr + nm:d.index(b"\0", shstr + nm)])
        if name in moved:
            struct.pack_into("<QQQ", d, so + 16, *moved[name])

    open(dst, "wb").write(d)
    print(f"declared {VERSION.decode()} (index {target['aux'][-1][2]}) "
          f"at va 0x{blob_va:x}, {len(blob)} bytes appended")


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2])
