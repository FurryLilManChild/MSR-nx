# Switch build (devkitPro's devkitA64 Docker image)

Run from a folder (`W`) that holds `gen/` (generated C from tools/gen/generate.py):

1. `docker run --rm -v $W:/w -v <repo>/runtime:/r devkitpro/devkita64 bash /w/nxgen.sh`
   compiles the generated game units into `nxobj/` (about an hour on 2 cores; only changed units
   are rebuilt).
2. Shaders: `uam -s vert -o nxout/pica_vsh.dksh runtime/gpu/dk/pica_vsh.glsl`, the same with
   `-s frag` for pica_fsh.glsl, then `python3 mkshaders.py` writes `nxout/dk_shaders.h`.
3. `docker run ... bash /w/nxbuild.sh link` builds the runtime and links `nxout/msr.nro`.

The .nro holds translated game code: never commit it or anything in nxobj/ or nxout/.
On the SD card, `/switch/msr/` holds msr.nro, code.bin, exheader.bin and romfs.bin.
