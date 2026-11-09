#!/bin/bash
# MSR-nx: builds libuam.a, uam compiled for the Switch, into the build folder given as $1.
# uam (https://github.com/devkitPro/uam, zlib license; mesa parts MIT) is pinned below. Its build
# generates C++ sources with python (mako), bison and flex: that part runs on the host with meson;
# the generated and original sources are then compiled for the Switch in the devkitA64 container,
# with the same flags as the host build plus the Switch's, and uam_iface.cpp added.
# Needs: git, meson, ninja, bison, flex, python3 with mako, docker with devkitpro/devkita64.
set -e
OUT=$(realpath "$1")
HERE=$(dirname "$(realpath "$0")")
COMMIT=5a5afc2bae8b55409ab36ba45be63fcb73f68993
SRC=$OUT/uam-src
if [ ! -d "$SRC" ]; then git clone -q https://github.com/devkitPro/uam.git "$SRC"; fi
git -C "$SRC" checkout -q -f $COMMIT
cp "$HERE/uam_iface.cpp" "$SRC/source/uam_iface.cpp"
[ -d "$SRC/build" ] || meson setup "$SRC/build" "$SRC" > /dev/null
ninja -C "$SRC/build" > /dev/null
python3 - "$SRC" <<'PY'
import json, os, shlex, sys
src = sys.argv[1]
cc = json.load(open(os.path.join(src, 'build/compile_commands.json')))
sw = ('-march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIE -D__SWITCH__ '
      '-I/opt/devkitpro/libnx/include -fdata-sections -w').split()
jobs = []
iface = None
for e in cc:
    a = shlex.split(e['command'])
    if e['file'].endswith('source/main.cpp'):
        iface = a   # the same flags compile uam_iface.cpp
        continue
    out, skip, o = [], 0, None
    for i, x in enumerate(a):
        if skip:
            skip -= 1
            continue
        if x in ('-MQ', '-MF'):
            skip = 1
            continue
        if x == '-MD' or x.startswith('-fdiagnostics'):
            continue
        if x == '-o':
            o = a[i + 1]
            skip = 1
            continue
        out.append(x)
    comp = 'aarch64-none-elf-g++' if out[0] in ('c++', 'g++') else 'aarch64-none-elf-gcc'
    jobs.append(' '.join([comp] + [shlex.quote(x) for x in out[1:]] + sw + ['-o', '/u/nxobj/' + os.path.basename(o)]))
out = []
skip = 0
for i, x in enumerate(iface):
    if skip:
        skip -= 1
        continue
    if x in ('-MQ', '-MF', '-o'):
        skip = 1
        continue
    if x == '-MD' or x.startswith('-fdiagnostics') or x.endswith('main.cpp'):
        continue
    out.append(x)
jobs.append(' '.join(['aarch64-none-elf-g++'] + [shlex.quote(x) for x in out[1:]] + sw + ['-o', '/u/nxobj/uam_iface.o', '-c', '../source/uam_iface.cpp']))
open(os.path.join(src, 'jobs.txt'), 'w').write('\n'.join(jobs) + '\n')
PY
docker run --rm -v "$SRC":/u devkitpro/devkita64 bash -c '
export PATH=/opt/devkitpro/devkitA64/bin:$PATH
mkdir -p /u/nxobj && cd /u/build
xargs -P 4 -I{} -d "\n" sh -c "{} || echo FAILED: {}" < /u/jobs.txt
rm -f /u/libuam.a && aarch64-none-elf-ar rcs /u/libuam.a /u/nxobj/*.o'
cp "$SRC/libuam.a" "$OUT/libuam.a"
ls -la "$OUT/libuam.a"
