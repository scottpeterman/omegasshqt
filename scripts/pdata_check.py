#!/usr/bin/env python3
# scripts/pdata_check.py
#
# Answers one question about a Go c-archive on Windows: are the .pdata
# RUNTIME_FUNCTION entries in go.o sorted by function start address?
#
# MSVC's linker requires that ordering and rejects the object outright when it
# is wrong:
#
#     go.o : fatal error LNK1223: invalid or corrupt file:
#            file contains invalid .pdata contributions
#
# The error names no entry and no function, so there is nothing in it to act
# on. This prints the ordering, which is the missing half.
#
# The addresses are not in the object. Every BeginAddress field in .pdata is
# zero and the real target lives in the relocation over it, so a naive hexdump
# shows a sorted array of zeroes and tells you nothing. This resolves each
# entry through its relocation to the symbol's section offset, and sorts on
# that -- which is what the linker does before checking.
#
# Reads a .lib (extracts go.o from it) or a go.o directly. Standard library
# only, no toolchain needed:
#
#     python scripts\pdata_check.py build\omegassh.lib

import struct
import sys


def extract_go_o(blob):
    """Pull the go.o member out of a COFF archive; pass an object through."""
    if not blob.startswith(b"!<arch>\n"):
        return blob, "(object file)"
    pos = 8
    longnames = b""
    while pos + 60 <= len(blob):
        header = blob[pos:pos + 60]
        name = header[0:16].decode("latin-1").rstrip()
        size = int(header[48:58].decode("latin-1").strip() or "0")
        body = blob[pos + 60:pos + 60 + size]
        if name == "//":
            longnames = body
        elif name.startswith("/") and name[1:].isdigit():
            off = int(name[1:])
            end = longnames.find(b"/", off)
            name = longnames[off:end].decode("latin-1")
        name = name.rstrip("/")
        if name == "go.o":
            return body, name
        pos += 60 + size + (size & 1)
    raise SystemExit("no go.o member in this archive")


def sections_and_symbols(obj):
    machine, nsec, _, psym, nsym, optsz, _ = struct.unpack_from("<HHIIIHH", obj, 0)
    if machine != 0x8664:
        raise SystemExit("not an x86-64 object (machine 0x%04x)" % machine)
    base = 20 + optsz
    secs = {}
    for i in range(nsec):
        raw = obj[base + i * 40:base + (i + 1) * 40]
        name = raw[:8].rstrip(b"\0").decode("latin-1")
        _, _, rawsz, rawptr, relptr, _, nrel, _, _ = struct.unpack_from(
            "<IIIIIIHHI", raw, 8)
        secs[name] = (rawsz, rawptr, relptr, nrel)
    syms = []
    i = 0
    while i < nsym:
        entry = obj[psym + i * 18:psym + i * 18 + 18]
        value, secnum, _, _, naux = struct.unpack_from("<IhHBB", entry, 8)
        syms.append((value, secnum))
        for _ in range(naux):
            syms.append((0, 0))
        i += 1 + naux
    return secs, syms


def main(path):
    obj, member = extract_go_o(open(path, "rb").read())
    secs, syms = sections_and_symbols(obj)
    if ".pdata" not in secs:
        raise SystemExit("no .pdata section in %s" % member)

    rawsz, rawptr, relptr, _ = secs[".pdata"]
    count = rawsz // 12
    if count == 0:
        raise SystemExit(".pdata is empty")

    # Three relocations per entry: BeginAddress, EndAddress, UnwindInfo.
    # Only the first is needed; index them by the offset they patch.
    relocs = {}
    for i in range(count * 3):
        va, symidx, _ = struct.unpack_from("<IIH", obj, relptr + i * 10)
        relocs.setdefault(va, symidx)

    resolved = []
    for i in range(count):
        symidx = relocs.get(i * 12)
        if symidx is None:
            raise SystemExit("entry %d has no BeginAddress relocation" % i)
        value, secnum = syms[symidx]
        resolved.append((secnum, value))

    keys = [v for _, v in resolved]
    bad = [i for i in range(count - 1) if keys[i] > keys[i + 1]]
    sections = sorted({s for s, _ in resolved})

    print("member:              %s" % member)
    print("RUNTIME_FUNCTIONs:   %d" % count)
    print("target sections:     %s" % ", ".join(str(s) for s in sections))
    print("out-of-order pairs:  %d of %d" % (len(bad), count - 1))
    if not bad:
        print()
        print("SORTED. MSVC's ordering requirement is met, so LNK1223 on this")
        print("object is coming from something other than entry order.")
        return 0

    print()
    print("UNSORTED -- this is what LNK1223 is complaining about.")
    print("first few inversions (index: this > next):")
    for i in bad[:10]:
        print("  %6d: 0x%08x > 0x%08x" % (i, keys[i], keys[i + 1]))
    return 1


if __name__ == "__main__":
    if len(sys.argv) != 2:
        raise SystemExit("usage: pdata_check.py <archive.lib | go.o>")
    sys.exit(main(sys.argv[1]))