"""A small .7z writer for the tests (Python's lzma does the compression): one folder (solid) or
one per file, LZMA / LZMA2 / copy, optional x86 BCJ, optional packed header.
usage: make7z.py out.7z method(lzma|lzma2|copy) [bcj] [solid|single] [encheader] dict_mb -- files... (relative names)"""
import lzma, struct, sys, zlib, os

def num(v):
    # 7z variable-length number
    for n in range(9):
        if v < (1 << (7 * (n + 1))) or n == 8:
            break
    if n == 0: return bytes([v])
    if n == 8: return b'\xff' + struct.pack('<Q', v)
    first = (0xFF << (8 - n)) & 0xFF
    hi = v >> (8 * n)
    return bytes([first | hi]) + (v & ((1 << (8 * n)) - 1)).to_bytes(n, 'little')

def coder_bytes(method, dict_size, bcj):
    out = b''
    coders = []
    if bcj: coders.append((b'\x03\x03\x01\x03', b''))
    if method == 'lzma':
        props = lzma._encode_filter_properties({'id': lzma.FILTER_LZMA1, 'dict_size': dict_size})
        coders.append((b'\x03\x01\x01', props))
    elif method == 'lzma2':
        props = lzma._encode_filter_properties({'id': lzma.FILTER_LZMA2, 'dict_size': dict_size})
        coders.append((b'\x21', props))
    else:
        coders.append((b'\x00', b''))
    out += num(len(coders))
    for cid, props in coders:
        f = len(cid) | (0x20 if props else 0)
        out += bytes([f]) + cid + (num(len(props)) + props if props else b'')
    if bcj: out += num(0) + num(1)        # bind: BCJ's input 0 <- coder 1's output (out index 1)
    return out

def compress(data, method, dict_size, bcj):
    filters = []
    if bcj: filters.append({'id': lzma.FILTER_X86})
    if method == 'lzma': filters.append({'id': lzma.FILTER_LZMA1, 'dict_size': dict_size})
    elif method == 'lzma2': filters.append({'id': lzma.FILTER_LZMA2, 'dict_size': dict_size})
    else: return data
    return lzma.compress(data, format=lzma.FORMAT_RAW, filters=filters)

def bitvec(bits):
    out = bytearray(); cur = 0; n = 0
    for b in bits:
        cur = (cur << 1) | (1 if b else 0); n += 1
        if n == 8: out.append(cur); cur = 0; n = 0
    if n: out.append(cur << (8 - n))
    return bytes(out)

def streams_info(pack_sizes, folders_coders, unpack_sizes_per_folder, sub_counts, sub_sizes, sub_crcs):
    s = b'\x06' + num(0) + num(len(pack_sizes)) + b'\x09' + b''.join(num(x) for x in pack_sizes) + b'\x00'
    s += b'\x07\x0b' + num(len(folders_coders)) + b'\x00' + b''.join(folders_coders)
    s += b'\x0c' + b''.join(b''.join(num(x) for x in us) for us in unpack_sizes_per_folder) + b'\x00'
    if sub_counts is not None:
        s += b'\x08'
        if any(c != 1 for c in sub_counts): s += b'\x0d' + b''.join(num(c) for c in sub_counts)
        sizes = []
        k = 0
        for c in sub_counts:
            sizes += sub_sizes[k:k + c - 1]; k += c
        if sizes: s += b'\x09' + b''.join(num(x) for x in sizes)
        s += b'\x0a\x01' + b''.join(struct.pack('<I', c) for c in sub_crcs) + b'\x00'
    return s + b'\x00'

def main():
    a = sys.argv[1:]; sep = a.index('--')
    out, method, opts, files = a[0], a[1], a[2:sep], a[sep + 1:]
    bcj = 'bcj' in opts; solid = 'single' not in opts; enc = 'encheader' in opts
    dict_size = int([o for o in opts if o.isdigit()][0]) << 20 if any(o.isdigit() for o in opts) else 1 << 24
    datas = [open(f, 'rb').read() for f in files]
    nonempty = [i for i, d in enumerate(datas) if d]
    packed = []; folders = []; unpack = []; counts = []; sizes = []; crcs = []
    groups = [nonempty] if solid else [[i] for i in nonempty]
    for g in groups:
        if not g: continue
        raw = b''.join(datas[i] for i in g)
        packed.append(compress(raw, method, dict_size, bcj))
        folders.append(coder_bytes(method, dict_size, bcj))
        unpack.append([len(raw), len(raw)] if bcj else [len(raw)])
        counts.append(len(g)); sizes += [len(datas[i]) for i in g]; crcs += [zlib.crc32(datas[i]) for i in g]
    body = b''.join(packed)
    hdr = b'\x01\x04' + streams_info([len(p) for p in packed], folders, unpack, counts, sizes, crcs)
    # files
    names = b'\x00' + b''.join(f.encode('utf-16-le') + b'\x00\x00' for f in files)
    fi = num(len(files))
    empty = [not d for d in datas]
    if any(empty):
        ev = bitvec(empty); fi += b'\x0e' + num(len(ev)) + ev
        ef = bitvec([True for d in datas if not d]); fi += b'\x0f' + num(len(ef)) + ef
    fi += b'\x11' + num(len(names)) + names + b'\x00'
    hdr += b'\x05' + fi + b'\x00'
    if enc:
        ph = lzma.compress(hdr, format=lzma.FORMAT_RAW, filters=[{'id': lzma.FILTER_LZMA1, 'dict_size': 1 << 20}])
        props = lzma._encode_filter_properties({'id': lzma.FILTER_LZMA1, 'dict_size': 1 << 20})
        coder = num(1) + bytes([3 | 0x20]) + b'\x03\x01\x01' + num(len(props)) + props
        eh = b'\x17' + b'\x06' + num(len(body)) + num(1) + b'\x09' + num(len(ph)) + b'\x00'
        eh += b'\x07\x0b' + num(1) + b'\x00' + coder + b'\x0c' + num(len(hdr)) + b'\x0a\x01' + struct.pack('<I', zlib.crc32(hdr)) + b'\x00\x00'
        body += ph; hdr = eh
    start = struct.pack('<QQI', len(body), len(hdr), zlib.crc32(hdr))
    with open(out, 'wb') as f:
        f.write(b'7z\xbc\xaf\x27\x1c\x00\x04' + struct.pack('<I', zlib.crc32(start)) + start + body + hdr)

main()
