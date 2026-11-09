// MSR-nx: PICA vertex shader configuration to GLSL (the C version of tools/shaders/pica2glsl.py).
#pragma once
#include <stdint.h>
typedef uint32_t u32;

typedef struct {
    const u32* code;     // 4096 program words
    const u32* swz;      // 128 operand descriptors
    u32 entry, outmask, total;
    u32 outmap[7];
} VsConfig;

// The vertex shader source for uam, or null (with *why) when the program needs the interpreter.
// The caller frees the string.
char* vsgen_glsl(const VsConfig* cfg, const char** why);
