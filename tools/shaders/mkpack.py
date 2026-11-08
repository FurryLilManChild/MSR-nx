#!/usr/bin/env python3
"""MSR-nx: packs translated vertex shaders (.dksh files named vs_<key>.dksh) into shaders.pack.

Layout (read by runtime/gpu/dk/dk_pica.c, dkr_load_pack): "MSRVSPK1", u32 count, u32 0, then
count entries {u64 key, u32 offset, u32 size} sorted by key, then each .dksh at its offset,
256-byte aligned (deko3d's shader code alignment). The pack is built from game data: it goes to
the SD card, never into the repository.
"""
import glob
import os
import struct
import sys

def main():
    if len(sys.argv) != 3:
        raise SystemExit('usage: mkpack.py <dir with vs_*.dksh> <shaders.pack>')
    items = []
    for p in glob.glob(os.path.join(sys.argv[1], 'vs_*.dksh')):
        key = int(os.path.basename(p)[3:19], 16)
        items.append((key, open(p, 'rb').read()))
    items.sort()
    head = 16 + 16 * len(items)
    pos = (head + 255) & ~255
    entries, blobs = [], []
    for key, data in items:
        entries.append(struct.pack('<QII', key, pos, len(data)))
        pad = ((pos + len(data) + 255) & ~255) - (pos + len(data))
        blobs.append(data + b'\0' * pad)
        pos += len(data) + pad
    out = b'MSRVSPK1' + struct.pack('<II', len(items), 0) + b''.join(entries)
    out += b'\0' * (((head + 255) & ~255) - head) + b''.join(blobs)
    open(sys.argv[2], 'wb').write(out)
    print('%d shaders, %d bytes' % (len(items), len(out)))

if __name__ == '__main__':
    main()
