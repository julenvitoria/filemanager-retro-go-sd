#!/usr/bin/env python3
"""Append the File Manager overlays to the packed homebrew (FileManager.bin).

Layout (little-endian):
    [GWHB container from pack_homebrew.py]
    [overlay 0 bytes][overlay 1 bytes]...      (4-byte aligned, BSS not stored)
    footer: magic 'FMOV', version 1, count, then 4 x {file offset, load size, total size}

The firmware only loads the GWHB payload; src/ovl.c reads the footer from its own .bin.
"""
import argparse
import struct
import subprocess
import sys

MAGIC = 0x564F4D46  # "FMOV"
VERSION = 1
MAX = 4
FOOTER = struct.calcsize("<3I") + MAX * struct.calcsize("<3I")


def symbols(nm, elf):
    out = subprocess.run([nm, elf], capture_output=True, text=True, check=True).stdout
    syms = {}
    for line in out.splitlines():
        parts = line.split()
        if len(parts) == 3:
            syms[parts[2]] = int(parts[0], 16)
    return syms


def strip_old(data):
    if len(data) >= FOOTER:
        magic, ver, n = struct.unpack_from("<3I", data, len(data) - FOOTER)
        if magic == MAGIC and ver == VERSION and n <= MAX:
            offs = [struct.unpack_from("<I", data, len(data) - FOOTER + 12 + i * 12)[0] for i in range(n)]
            offs = [o for o in offs if o]
            if offs:
                return data[:min(offs)]
    return data


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--bin", required=True, help="packed FileManager.bin (modified in place)")
    ap.add_argument("--elf", required=True)
    ap.add_argument("--nm", default="arm-none-eabi-nm")
    ap.add_argument("overlay", nargs="+", help="name=blob.bin, in overlay id order")
    a = ap.parse_args()

    syms = symbols(a.nm, a.elf)
    start = syms["__FM_OVL_START__"]
    room = syms["__FM_OVL_END__"] - start
    data = bytearray(strip_old(open(a.bin, "rb").read()))
    entries = []
    for spec in a.overlay:
        name, path = spec.split("=", 1)
        load = syms[f"__fm_ovl_{name}_load"] - start
        total = syms[f"__fm_ovl_{name}_end"] - start
        blob = open(path, "rb").read()
        if len(blob) < load:
            sys.exit(f"overlay {name}: {path} is {len(blob)} bytes, expected at least {load}")
        if total > room:
            sys.exit(f"overlay {name}: {total} bytes, window is {room}")
        while len(data) % 4:
            data.append(0)
        entries.append((len(data), load, total))
        data += blob[:load]
        print(f"  overlay {name:6s} {load:6d} bytes in the file, {total:6d} in RAM (window {room})")
    while len(data) % 4:
        data.append(0)
    footer = struct.pack("<3I", MAGIC, VERSION, len(entries))
    for i in range(MAX):
        footer += struct.pack("<3I", *(entries[i] if i < len(entries) else (0, 0, 0)))
    data += footer
    open(a.bin, "wb").write(data)


if __name__ == "__main__":
    main()
