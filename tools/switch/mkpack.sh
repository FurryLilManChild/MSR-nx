#!/bin/bash
# Builds shaders.pack from the configuration dumps vs.bin and fs.bin in the build folder, inside the
# devkitA64 container: /w the build folder, /r the repository's runtime folder, /t its tools folder.
# The pack and everything made on the way are game-derived: they stay out of the repository.
set -e
UAM=/opt/devkitpro/tools/bin/uam
cd /w
rm -rf pk_gen pk_dksh && mkdir -p pk_gen pk_dksh
python3 /t/shaders/pica2glsl.py vs.bin pk_gen > /dev/null
python3 /t/shaders/fsgen.py fs.bin /r/gpu/dk/pica_fsh.glsl pk_gen
for f in pk_gen/vs_*.glsl pk_gen/fs_*.glsl; do
  b=$(basename $f .glsl)
  case $b in vs_*) st=vert;; *) st=frag;; esac
  echo "$UAM -s $st -o pk_dksh/$b.dksh $f"
done | xargs -P 4 -I{} sh -c '{}'
python3 /t/shaders/mkpack.py pk_dksh shaders.pack
