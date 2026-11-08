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
  aarch64-none-elf-gcc -specs=$DEVKITPRO/libnx/switch.specs $ARCH -Wl,--gc-sections -o nxout/msr.elf nxrt/*.o nxobj/*.o -L$DEVKITPRO/libnx/lib -ldeko3d -lnx -lm
  nacptool --create "Metroid Samus Returns NX" "MSR-nx" "0.1-test1" nxout/msr.nacp
  elf2nro nxout/msr.elf nxout/msr.nro --nacp=nxout/msr.nacp
  ls -la nxout/msr.nro
fi
