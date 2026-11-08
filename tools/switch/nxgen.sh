#!/bin/bash
# Compiles the generated game units for the Switch (aarch64) inside the devkitA64 container.
export PATH=/opt/devkitpro/devkitA64/bin:$PATH
cd /w
FLAGS="-O2 -march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIE -ffunction-sections -fdata-sections -w -fno-strict-aliasing -ffp-contract=off -D__SWITCH__ -I/r/cpu -I/r/os -Igen"
n=0; total=$(ls gen/unit_*.c gen/table.c | wc -l)
for f in gen/unit_*.c gen/table.c; do
  b=$(basename $f .c)
  if [ -f nxobj/$b.o ] && [ nxobj/$b.o -nt $f ]; then n=$((n+1)); continue; fi
  echo "aarch64-none-elf-gcc $FLAGS -c $f -o nxobj/$b.o && echo built $b"
done | xargs -P 2 -I{} sh -c '{}'
echo NXGEN-DONE
