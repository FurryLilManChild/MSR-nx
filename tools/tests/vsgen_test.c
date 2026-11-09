// MSR-nx: checks runtime/gpu/vsgen.c against tools/shaders/pica2glsl.py. Writes vs_<key>.glsl for
// every configuration in a dump into a folder; tools/tests/vsgen_test.sh compares the two folders.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "vsgen.h"

int main(int argc, char** argv) {
    if (argc < 3) { fprintf(stderr, "usage: vsgen_test <dump> <out dir>\n"); return 1; }
    FILE* f = fopen(argv[1], "rb");
    if (!f) return 1;
    static u32 rec[13 + 4096 + 128];
    int n = 0, bad = 0;
    while (fread(rec, 4, 13 + 4096 + 128, f) == 13 + 4096 + 128) {
        VsConfig c;
        c.code = rec + 13;
        c.swz = rec + 13 + 4096;
        c.entry = rec[3];
        c.outmask = rec[4];
        c.total = rec[5];
        memcpy(c.outmap, rec + 6, sizeof c.outmap);
        unsigned long long key = rec[1] | ((unsigned long long)rec[2] << 32);
        const char* why = 0;
        char* s = vsgen_glsl(&c, &why);
        if (!s) { printf("  %016llx: %s\n", key, why); bad++; continue; }
        char path[512];
        snprintf(path, sizeof path, "%s/vs_%016llx.glsl", argv[2], key);
        FILE* o = fopen(path, "w");
        fputs(s, o);
        fclose(o);
        free(s);
        n++;
    }
    printf("%d translated, %d left to the interpreter\n", n, bad);
    return 0;
}
