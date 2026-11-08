// MSR-nx: the PICA200 GPU state, shared by the command processor and the renderers.
//
// Register meanings follow 3dbrew "GPU/Internal Registers" and "GPU/Shader Instruction Set", with
// fearkov/zakuro (MIT) zakuro-gpu as a second reference. The code here is ours.
#pragma once
#include "msr_cpu.h"

#define PICA_REGS 0x300

// Physical addresses the GPU sees, mapped to where that memory sits in the guest window.
// VRAM 0x18000000 (6 MiB) is at 0x1F000000; FCRAM 0x20000000 is the linear heap at 0x14000000.
MSR_INLINE u32 pica_vaddr(u32 paddr) {
    if (paddr >= 0x18000000u && paddr < 0x18600000u) return paddr - 0x18000000u + 0x1F000000u;
    if (paddr >= 0x20000000u && paddr < 0x28000000u) return paddr - 0x20000000u + 0x14000000u;
    return 0;   // nothing the probe maps
}
MSR_INLINE u8* pica_ptr(u32 paddr) {
    u32 v = pica_vaddr(paddr);
    return v ? g_mem + v : 0;
}

typedef struct { float x, y, z, w; } Vec4;

// One programmable shader unit (vertex or geometry).
typedef struct {
    u32 program[4096];
    u32 swizzle[128];
    Vec4 f[96];            // float uniforms c0..c95
    u32 program_pos, swizzle_pos;
    u32 f_index, f_pos, f_wide, f_stage[4];
} ShaderUnit;

typedef struct {
    u32 reg[PICA_REGS];
    ShaderUnit vs, gs;
    Vec4 fixed_attr[16];      // default attribute values (GPUREG_FIXEDATTRIB)
    u32 fixed_index, fixed_pos, fixed_stage[3];
    Vec4 immediate[16];       // immediate-mode vertex being gathered
    // statistics for the probe
    unsigned lists, draws, draws_indexed, vertices, gs_draws, immediate_vertices;
} Pica;

extern Pica g_pica;

// One vertex after the vertex shader, as the rasterizer takes it.
typedef struct {
    Vec4 pos;
    Vec4 quat;
    Vec4 color;
    float uv0[2], uv1[2], uv2[2], uv0w;
    float view[3];
} PicaVertex;

void pica_run_list(u32 vaddr, u32 size);
void pica_load_attributes(u32 index, Vec4* in);

// GPU renderer hooks (the Switch's deko3d renderer); unset in the probe, which draws on the CPU.
extern int (*pica_draw_hook)(int indexed);                  // returns 1 if it drew
extern void (*pica_sync_hook)(u32 vaddr, u32 size);         // before the CPU reads guest memory the GPU may have drawn
extern void (*pica_invalidate_hook)(u32 vaddr, u32 size);   // after the CPU wrote guest memory the GPU may hold
extern void (*pica_fill_hook)(u32 start, u32 end, u32 value, u32 width);
extern float pica_lut[24][256][2];
extern unsigned pica_lut_version, pica_program_version;
float pica_f24(u32 v);
// Runs the vertex shader for one vertex: inputs v0..v15, writes the outputs o0..o15.
void pica_shader_run(ShaderUnit* u, const u32* reg_block, const Vec4* in, Vec4* out);
// Maps shader outputs to vertex attributes with GPUREG_SH_OUTMAP.
void pica_map_outputs(const Vec4* out, PicaVertex* v);

// The renderer: called with the register file at each draw.
void pica_draw_triangles(const PicaVertex* v, unsigned count);   // count is a multiple of 3
void pica_display_transfer(u32 in_v, u32 out_v, u32 in_dim, u32 out_dim, u32 flags);
void pica_texture_copy(u32 in_v, u32 out_v, u32 size, u32 in_gap, u32 out_gap, u32 flags);
void pica_frame_shown(int screen, u32 left_vaddr, u32 stride, u32 format);
