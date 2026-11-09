#!/bin/bash
# Builds the Switch runtime and links msr.nro (inside the devkitA64 container).
set -e
export DEVKITPRO=/opt/devkitpro
export PATH=$DEVKITPRO/devkitA64/bin:$DEVKITPRO/tools/bin:$PATH
cd /w
mkdir -p nxrt nxout
ARCH="-march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIE"
CFLAGS="-O2 $ARCH -ffunction-sections -fdata-sections -fno-strict-aliasing -ffp-contract=off -Wall -Wno-unused -Wno-format-truncation -D__SWITCH__ -I$DEVKITPRO/libnx/include -I/r/cpu -I/r/os -I/r/gpu -Inxout"
for f in cpu/dispatch.c os/log.c os/mem.c os/svc.c os/kernel.c os/boot.c os/hle_apt.c os/hle_hid_gsp.c os/hle_fs_cfg.c os/sched.c os/hle_dsp.c os/dsp_audio.c gpu/pica_cmd.c gpu/pica_shader.c gpu/sw_raster.c gpu/sw_light.c gpu/dk/dk_pica.c switch/main_nx.c; do
  if [ "$f" = "gpu/dk/dk_pica.c" ]; then while [ ! -f nxout/dk_shaders.h ]; do sleep 1; done; fi
  aarch64-none-elf-gcc $CFLAGS -c /r/$f -o nxrt/$(basename $f .c).o
done
echo RUNTIME-OK
if [ "$1" = "link" ]; then
  # the shader pack (made from game data, never committed) goes inside msr.nro when there is one
  PACKOBJ=""
  if [ -f shaders.pack ]; then
    printf '.section .rodata.msr_pack,"a"\n.global msr_pack_data\n.global msr_pack_end\n.balign 256\nmsr_pack_data:\n.incbin "/w/shaders.pack"\nmsr_pack_end:\n' > nxout/pack_blob.S
    aarch64-none-elf-gcc $ARCH -c nxout/pack_blob.S -o nxout/pack_blob.o
    PACKOBJ=nxout/pack_blob.o
  fi
  aarch64-none-elf-gcc -specs=$DEVKITPRO/libnx/switch.specs $ARCH -Wl,--gc-sections -o nxout/msr.elf nxrt/*.o nxobj/*.o $PACKOBJ -L$DEVKITPRO/libnx/lib -ldeko3d -lnx -lm
  nacptool --create "Metroid Samus Returns NX" "MSR-nx" "0.1-test4b" nxout/msr.nacp
  elf2nro nxout/msr.elf nxout/msr.nro --nacp=nxout/msr.nacp
  ls -la nxout/msr.nro
fi
