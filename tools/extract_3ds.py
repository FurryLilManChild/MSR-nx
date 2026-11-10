#!/usr/bin/env python3
"""MSR-nx: makes the three files the port reads (code.bin, exheader.bin, romfs.bin) from your own
decrypted cartridge dump of Metroid: Samus Returns (USA), with nothing but Python 3.

    python extract_3ds.py "Metroid - Samus Returns (USA) (En,Fr,Es).3ds" out_folder

The dump must be decrypted (the NCCH "NoCrypto" flag set), as GodMode9 makes them with the
"decrypt" option. The files written are checked against the ones the port was made from.
"""
import hashlib
import os
import struct
import sys

MEDIA = 0x200
EXPECTED = {   # sha256 of the files from the USA cartridge (docs/progress-log.md, 2026-11-07)
    'code.bin': '669090e6caeef405aaf4d2b884749460f8f0b868ecd158c7f1943ac235c102c6',
    'exheader.bin': '1ebde6cc7304affc99417af4fd9cd748057d0e2ba38e8d32650042801c322fa6',
    'romfs.bin': '4e1884ae07f531023f75a00e1d5b7c2c57badd2ca18708829dac451fd46a79df',
}


def fail(msg):
    print('error: ' + msg)
    sys.exit(1)


def blz_decompress(data):
    """The 3DS executable's backward LZ compression (the .code file of the ExeFS)."""
    if len(data) < 8:
        return data
    off_size, addl = struct.unpack_from('<II', data, len(data) - 8)
    if addl == 0:
        return data
    comp_len = off_size & 0xFFFFFF
    header_len = off_size >> 24
    out = bytearray(data) + bytearray(addl)
    src = len(data) - header_len
    dst = len(out)
    end = len(data) - comp_len
    while src > end:
        src -= 1
        flags = data[src]
        for bit in range(8):
            if src <= end:
                break
            if flags & (0x80 >> bit):
                src -= 2
                seg = data[src] | (data[src + 1] << 8)
                length = (seg >> 12) + 3
                disp = (seg & 0x0FFF) + 3
                for _ in range(length):
                    dst -= 1
                    out[dst] = out[dst + disp]
            else:
                src -= 1
                dst -= 1
                out[dst] = data[src]
    return bytes(out)


def main():
    if len(sys.argv) != 3:
        print(__doc__)
        sys.exit(1)
    rom, out_dir = sys.argv[1], sys.argv[2]
    os.makedirs(out_dir, exist_ok=True)
    with open(rom, 'rb') as f:
        ncsd = f.read(0x200)
        if ncsd[0x100:0x104] != b'NCSD':
            fail('not a .3ds cartridge dump (no NCSD header)')
        part0, part0_size = struct.unpack_from('<II', ncsd, 0x120)
        base = part0 * MEDIA
        f.seek(base)
        ncch = f.read(0x200)
        if ncch[0x100:0x104] != b'NCCH':
            fail('the first partition is not an NCCH')
        flags7 = ncch[0x188 + 7]
        if not flags7 & 0x04:
            fail('the dump is encrypted; make a decrypted dump (GodMode9: "decrypt") and try again')
        title = struct.unpack_from('<Q', ncch, 0x118)[0]
        print('title %016X, product %s' % (title, ncch[0x150:0x160].rstrip(b'\0').decode('ascii', 'replace')))
        exefs_off, exefs_size = struct.unpack_from('<II', ncch, 0x1A0)
        romfs_off, romfs_size = struct.unpack_from('<II', ncch, 0x1B0)

        # exheader.bin: the extended header and the access descriptor that follows it
        f.seek(base + 0x200)
        exh = f.read(0x800)
        compressed = exh[0xD] & 1

        # code.bin: .code from the ExeFS, decompressed
        f.seek(base + exefs_off * MEDIA)
        exefs_hdr = f.read(0x200)
        code = None
        for k in range(10):
            name, off, size = struct.unpack_from('<8sII', exefs_hdr, k * 16)
            if name.rstrip(b'\0') == b'.code':
                f.seek(base + exefs_off * MEDIA + 0x200 + off)
                code = f.read(size)
        if code is None:
            fail('no .code in the ExeFS')
        if compressed:
            print('decompressing the code...')
            code = blz_decompress(code)

        files = {'code.bin': code, 'exheader.bin': exh}
        for name, data in files.items():
            with open(os.path.join(out_dir, name), 'wb') as o:
                o.write(data)

        # romfs.bin: the RomFS as it is on the cartridge
        print('copying the RomFS (%d MB)...' % (romfs_size * MEDIA >> 20))
        f.seek(base + romfs_off * MEDIA)
        left = romfs_size * MEDIA
        h = hashlib.sha256()
        with open(os.path.join(out_dir, 'romfs.bin'), 'wb') as o:
            while left:
                chunk = f.read(min(left, 8 << 20))
                if not chunk:
                    fail('the dump ends inside the RomFS')
                o.write(chunk)
                h.update(chunk)
                left -= len(chunk)

    sums = {'code.bin': hashlib.sha256(code).hexdigest(), 'exheader.bin': hashlib.sha256(exh).hexdigest(), 'romfs.bin': h.hexdigest()}
    ok = True
    for name in ('code.bin', 'exheader.bin', 'romfs.bin'):
        good = sums[name] == EXPECTED[name]
        ok = ok and good
        print('%-13s %s' % (name, 'checks out' if good else 'DIFFERENT from the supported USA cartridge (%s)' % sums[name]))
    if not ok:
        fail('this is not the supported version of the game (USA cartridge); the port may not run')
    print('done: copy code.bin, exheader.bin and romfs.bin to sdmc:/switch/msr/')


if __name__ == '__main__':
    main()
